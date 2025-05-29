// cgi_server.cpp
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <regex>
#include <sstream>
#include <fstream>
#include <array>
#include <functional>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

// 最大支援 5 組 Session
constexpr int MAX_SESSIONS = 5;
// 可選主機清單
const std::vector<std::string> HOSTS = []{
    std::vector<std::string> v;
    for(int i=1;i<=12;++i)
        v.push_back("nplinux"+std::to_string(i)+".cs.nycu.edu.tw");
    return v;
}();
// 可選測試檔案清單
const std::vector<std::string> TEST_FILES = {
    "t1.txt","t2.txt","t3.txt","t4.txt","t5.txt"
};

// HTML escape，用於安全地送入 innerHTML
static std::string html_escape(const std::string& src) {
    std::string out; out.reserve(src.size()*2);
    for(char c: src) {
        switch(c) {
            case '&':  out += "&amp;";      break;
            case '<':  out += "&lt;";       break;
            case '>':  out += "&gt;";       break;
            case '"':  out += "&quot;";     break;
            case '\'': out += "&#39;";      break;
            case '\n': out += "&NewLine;";  break;
            case '\r': break;
            default:   out += c;
        }
    }
    return out;
}

// 解析 QUERY_STRING 中的 h0..h4, p0..p4, f0..f4
struct Target { std::string host, port, file; };
static std::vector<Target> parse_query(const std::string& qs) {
    std::vector<Target> tmp(MAX_SESSIONS);
    std::regex kv_re(R"(([hpf])(\d)=(.*?)(&|$))");
    std::cmatch m;
    const char* cur = qs.c_str();
    while(std::regex_search(cur, m, kv_re)) {
        char key = m[1].str()[0];
        int idx  = m[2].str()[0] - '0';
        std::string val = m[3];
        if(idx>=0 && idx<MAX_SESSIONS) {
            if(key=='h') tmp[idx].host = val;
            if(key=='p') tmp[idx].port = val;
            if(key=='f') tmp[idx].file = val;
        }
        cur = m.suffix().first;
    }
    std::vector<Target> out;
    for(auto& t: tmp)
        if(!t.host.empty() && !t.port.empty() && !t.file.empty())
            out.push_back(t);
    return out;
}

class HttpSession;
using HttpSessionPtr = std::shared_ptr<HttpSession>;

// 處理單線程 HTTP client 連線
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket sock, boost::asio::io_context& io)
      : socket_(std::move(sock))
      , strand_(socket_.get_executor())
      , io_ctx_(io)
    {}

    // 開始讀 HTTP 請求
    void start() { do_read_header(); }

    // 提供給 RemoteSession 呼叫，將 script push 回 browser
    void deliver(const std::string& msg) {
        auto self = shared_from_this();
        boost::asio::async_write(socket_,
            boost::asio::buffer(msg),
            boost::asio::bind_executor(strand_,
            [self](auto,auto){})
        );
    }

private:
    tcp::socket socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::io_context& io_ctx_;
    std::array<char, 8192> buffer_;
    std::string request_;

    // 非同步讀到 header 結尾
    void do_read_header() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(buffer_),
        boost::asio::bind_executor(strand_,
        [this,self](boost::system::error_code ec, std::size_t n){
            if(ec) return;
            request_.append(buffer_.data(), n);
            if(request_.find("\r\n\r\n") != std::string::npos)
                handle_request();
            else
                do_read_header();
        }));
    }

    // 處理 GET /panel.cgi or /console.cgi?...
    void handle_request() {
        std::istringstream iss(request_);
        std::string method, uri, proto;
        iss >> method >> uri >> proto;
        if(method != "GET") {
            send_response("HTTP/1.1 400 Bad Request\r\n\r\n");
            return;
        }

        // 讀 Host header
        std::string line, host_hdr;
        std::getline(iss, line); // skip rest of request line
        while(std::getline(iss, line) && line != "\r") {
            if(line.rfind("Host:",0) == 0) {
                host_hdr = line.substr(5);
                auto p = host_hdr.find_first_not_of(" \t");
                host_hdr = host_hdr.substr(p);
                while(!host_hdr.empty() && (host_hdr.back()=='\r' || host_hdr.back()=='\n'))
                    host_hdr.pop_back();
            }
        }

        // 分離 path 和 query
        std::string path = uri, query;
        auto qp = uri.find('?');
        if(qp != std::string::npos) {
            path  = uri.substr(0, qp);
            query = uri.substr(qp+1);
        }

        if(path == "/panel.cgi") {
            handle_panel();
        }
        else if(path == "/console.cgi") {
            handle_console(query);
        }
        else {
            send_response("HTTP/1.1 404 Not Found\r\n\r\n");
        }
    }

// panel.cgi：輸出完整表單，並關 socket
void handle_panel() {
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: text/html\r\n\r\n"
        << R"(
<!DOCTYPE html><html lang="en"><head>
  <meta charset="UTF-8"><title>NP Project4 Panel</title>
  <link rel="stylesheet"
    href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css">
  <style>*{font-family:'Source Code Pro',monospace;}</style>
</head><body class="bg-secondary pt-5">
  <form action="console.cgi" method="GET">
    <table class="table mx-auto bg-light" style="width: inherit">
      <thead class="thead-dark">
        <tr><th>#</th><th>Host</th><th>Port</th><th>Input File</th></tr>
      </thead><tbody>)";

    for(int i = 0; i < MAX_SESSIONS; ++i) {
        oss << "<tr><th>Session " << (i+1) << "</th><td>"
               R"(<div class="input-group"><select name="h)" << i << R"(" class="custom-select"><option></option>)";

        for (auto& fqdn : HOSTS) {
            auto pos = fqdn.find('.');
            std::string prefix = (pos == std::string::npos ? fqdn : fqdn.substr(0, pos));
            oss << "<option value=\"" << fqdn << "\">" << prefix << "</option>";
        }

        oss << R"(</select><div class="input-group-append">
                     <span class="input-group-text">.cs.nycu.edu.tw</span>
                   </div></div></td>
                   <td><input name="p)" << i << R"(" type="text"
                     class="form-control" size="5"/></td>
                   <td><select name="f)" << i << R"(" class="custom-select">
                     <option></option>)";

        for (auto& f : TEST_FILES) {
            oss << "<option value=\"" << f << "\">" << f << "</option>";
        }

        oss << R"(</select></td></tr>)";
    }

    oss << R"(
      <tr><td colspan="3"></td>
          <td><button type="submit"
            class="btn btn-info btn-block">Run</button>
          </td>
      </tr>
    </tbody></table>
  </form>
</body></html>)";

    send_response(oss.str());
}


    // console.cgi：先寫出 table skeleton，但不關 socket
    void handle_console(const std::string& qs) {
        auto targets = parse_query(qs);
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/html\r\n\r\n"
            << R"(
<!DOCTYPE html><html><head>
  <meta charset="UTF-8"><title>NP Project4 Console</title>
  <link rel="stylesheet"
    href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css">
  <style>body{background:#212529;color:#e5e5e5;
           font-family:'Source Code Pro',monospace;}
         table{width:100%;}b{color:#01b468;}
         pre{margin:0;white-space:pre-wrap;color:#f1f3f5;}</style>
</head><body>
<table class="table table-dark table-bordered">
  <thead><tr>)";
        for(size_t i=0; i<targets.size(); ++i)
            oss << "<th>" << targets[i].host << ":" << targets[i].port << "</th>";
        oss << R"(</tr></thead><tbody><tr>)";
        for(size_t i=0; i<targets.size(); ++i)
            oss << "<td><pre id=\"s" << i << "\"></pre></td>";
        oss << R"(</tr></tbody></table>
        </body></html>)";                     

        // 非同步寫 skeleton，**不**關 socket
        auto self = shared_from_this();
        std::string skeleton = oss.str();
        boost::asio::async_write(socket_,
            boost::asio::buffer(skeleton),
            boost::asio::bind_executor(strand_,
            [this,self,targets = std::move(targets)]
            (boost::system::error_code ec, std::size_t){
                if(ec) return;
                // skeleton 寫完後，啟動各遠端 Session
                for(size_t i=0; i<targets.size(); ++i) {
                    std::make_shared<RemoteSession>(
                        io_ctx_, int(i), shared_from_this(), targets[i]
                    )->start();
                }
            }));
    }

    // 真正要結束連線時呼叫：送完整 HTTP + 關 socket
    void send_response(const std::string& s) {
        auto self = shared_from_this();
        boost::asio::async_write(socket_,
            boost::asio::buffer(s),
            boost::asio::bind_executor(strand_,
            [self](auto,auto){
                self->socket_.close();
            }));
    }

    // 負責單一遠端連線，並注入 <script> 回 Browser
    class RemoteSession : public std::enable_shared_from_this<RemoteSession> {
    public:
        RemoteSession(boost::asio::io_context& io, int id,
                      HttpSessionPtr parent, const Target& tgt)
          : resolver_(io), socket_(io),
            id_(id), parent_(parent),
            host_(tgt.host), port_(tgt.port),
            cmdfile_("test_case/"+tgt.file)
        {}
        void start() {
            if(!cmdfile_) return;
            auto self = shared_from_this();
            resolver_.async_resolve(host_, port_,
            [this,self](auto ec, auto eps){
                if(ec) return;
                boost::asio::async_connect(socket_, eps,
                [this,self](auto ec2, auto){
                    if(!ec2) do_read();
                });
            });
        }
    private:
        tcp::resolver        resolver_;
        tcp::socket          socket_;
        int                  id_;
        HttpSessionPtr       parent_;
        std::string          host_, port_;
        std::ifstream        cmdfile_;
        std::string          next_;
        std::array<char,4096> buf_;

        void do_read() {
            auto self = shared_from_this();
            socket_.async_read_some(boost::asio::buffer(buf_),
            [this,self](auto ec, std::size_t n){
                if(ec) return;
                std::string data(buf_.data(), n);
                send_to_client(data, false);
                if(data.find("% ") != std::string::npos)
                    send_next();
                do_read();
            });
        }
        void send_next() {
            if(!std::getline(cmdfile_, next_)) return;
            next_ += "\n";
            send_to_client(next_, true);
            auto self = shared_from_this();
            boost::asio::async_write(socket_,
                boost::asio::buffer(next_),
                [this,self](auto,auto){});
        }
        void send_to_client(const std::string& txt, bool is_cmd) {
            std::string esc = html_escape(txt);
            std::ostringstream js;
            js << "<script>document.getElementById('s"
               << id_ << "').innerHTML += '";
            if(is_cmd) js << "<b>";
            js << esc;
            if(is_cmd) js << "</b>";
            js << "';</script>\n";
            parent_->deliver(js.str());
        }
    }; // end RemoteSession

}; // end HttpSession

int main(int argc, char* argv[]){
    if(argc != 2){
        std::cerr << "Usage: cgi_server.exe <port>\n";
        return 1;
    }
    try {
        boost::asio::io_context io;
        tcp::acceptor acceptor(io,tcp::endpoint(tcp::v4(), std::atoi(argv[1])));
        std::function<void()> do_accept;
        do_accept = [&](){
            acceptor.async_accept(
            [&](boost::system::error_code ec, tcp::socket sock){
                if(!ec)
                    std::make_shared<HttpSession>(
                        std::move(sock), io
                    )->start();
                do_accept();
            });
        };
        do_accept();
        io.run();
    }
    catch(std::exception& e){
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}
