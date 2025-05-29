#include <boost/asio.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using boost::asio::ip::tcp;


std::string html_escape(const std::string& src) {
    std::string out;
    out.reserve(src.size()*2);
    for (char c : src) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            case '\n': out += "&NewLine;"; break;
            case '\r': break;
            default:   out += c;
        }
    }
    return out;
}

// Insert the server response into the corresponding <pre id='sX'>
void output_shell(int id, const std::string& msg) {
    std::cout << "<script>document.getElementById('s" << id
              << "').innerHTML += '" << html_escape(msg)
              << "';</script>\n" << std::flush;
}
// Add the client command in bold and insert
void output_cmd(int id, const std::string& cmd) {
    std::cout << "<script>document.getElementById('s" << id
              << "').innerHTML += '<b>"
              << html_escape(cmd)
              << "</b>';</script>\n" << std::flush;
}

//target server ds
struct Target { 
    std::string h,p,f; 
};

//QUERY_STRING, inclusing sh/sp、 h0..h4、p0..p4、f0..f4
struct QueryData {
    std::string sh, sp;
    std::vector<Target> targets;
};
QueryData parse_query() {
    QueryData q;
    const char* qs = std::getenv("QUERY_STRING");
    if (!qs) return q;
    // 先拆 key=value
    std::map<std::string,std::string> kv;
    std::string s(qs), token;
    std::istringstream ss(s);
    while (std::getline(ss, token, '&')) {
        auto pos = token.find('=');
        if (pos==std::string::npos) continue;
        kv[token.substr(0,pos)] = token.substr(pos+1);
    }
    // 讀 SOCKS 伺服器設定
    if (kv.count("sh")) q.sh = kv["sh"];
    if (kv.count("sp")) q.sp = kv["sp"];
    // 讀最多 5 個目標
    for (int i = 0; i < 5; i++) {
        std::string hk = "h"+std::to_string(i);
        std::string pk = "p"+std::to_string(i);
        std::string fk = "f"+std::to_string(i);
        if (kv.count(hk) && kv.count(pk) && kv.count(fk)) {
            q.targets.push_back({kv[hk], kv[pk], kv[fk]});
        }
    }
    return q;
}

// ---------- Session ----------
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::io_context& io, int id,
            std::string sh, std::string sp,
            std::string th, std::string tp,
            std::string file)
      : resolver_(io), socket_(io), id_(id),
        sh_(std::move(sh)), sp_(std::move(sp)),
        th_(std::move(th)), tp_(std::move(tp)),
        file_(std::move(file))
    {}

    void start() {
        cmdfile_.open("test_case/" + file_);
        if (!cmdfile_) return;

        // connect to SOCKS server (sh_, sp_)
        auto self = shared_from_this();
        resolver_.async_resolve(sh_, sp_,
            [this, self](auto ec, auto results){
                if(!ec){
                    boost::asio::async_connect(socket_, results,
                    [this, self](auto ec2, auto){
                        if( !ec2) do_socks_handshake();
                    });
                }
            });
    }

private:
    //SOCKS
    void do_socks_handshake() {
        auto self = shared_from_this();
        // resolve target ip
        resolver_.async_resolve(th_, tp_,
          [this,self](auto ec, auto results){
            if (ec || results.empty()) return;
            auto addr_bytes = results.begin()->endpoint()
                                  .address().to_v4().to_bytes();
            uint16_t port = static_cast<uint16_t>(std::stoi(tp_));
            // build SOCKS4 CONNECT request
            std::vector<unsigned char> req = {
                0x04,       // VN = 4
                0x01,       // CD = 1 (CONNECT)
                uint8_t(port>>8), uint8_t(port&0xFF),
                addr_bytes[0], addr_bytes[1],
                addr_bytes[2], addr_bytes[3],
                0x00        // USERID = "" + NULL
            };
            boost::asio::async_write(socket_,
              boost::asio::buffer(req),
              [this,self](auto ec3, std::size_t){
                if (!ec3) read_socks_reply();
            });
        });
    }

    // 2. read 8 bytes of SOCKS4_REPLY
    void read_socks_reply() {
        auto self = shared_from_this();
        boost::asio::async_read(socket_,
          boost::asio::buffer(reply_),
          [this,self](auto ec, std::size_t){
            // reply_[1] == 0x5A (90) means Accept
            if (!ec && reply_[1]==0x5A) {
                // 握手成功，開始原本的讀寫機制
                do_read();
            }
            // 握手失敗則不動作
        });
    }

    // original procedure in Project4 
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(buf_),
            [this, self](auto ec, std::size_t n) {
                if (!ec) {
                    std::string data(buf_.data(), n);
                    output_shell(id_, data); //output to web
                    if(data.find("% ") != std::string::npos)
                        send_next();
                    do_read();
                }
            });
    }

    void send_next() {
        if (!std::getline(cmdfile_, next_)) return;
        next_ += "\n";
        output_cmd(id_, next_); //output to web
        auto self = shared_from_this();
        boost::asio::async_write(socket_, //output to RWG
            boost::asio::buffer(next_),
            [this, self](auto, std::size_t) {});
    }

    tcp::resolver                   resolver_;
    tcp::socket                     socket_;
    int                              id_;
    std::string                      sh_, sp_;    // SOCKS server
    std::string                      th_, tp_;    // target host/port
    std::string                      file_;       // test case file
    std::ifstream                    cmdfile_;
    std::array<char,4096>            buf_;
    std::array<unsigned char,8>      reply_;
    std::string                      next_;
};

int main() {
    // HTTP header + HTML table 
    std::cout << "Content-Type: text/html\r\n\r\n";
    std::cout << R"(
      <!DOCTYPE html><html><head><meta charset="UTF-8">
      <title>NP Project5 Console</title>
      <link rel="stylesheet"
        href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css">
      <style>
        body { background:#212529; color:#e5e5e5;
               font-family: 'Source Code Pro', monospace; }
        table { width:100%; }
        b { color:#01b468; }
        pre { margin:0; white-space:pre-wrap; color:#f1f3f5; }
      </style>
    </head><body>
    <table class="table table-dark table-bordered">
      <thead><tr>)";

    // parse QUERY_STRING
    auto q = parse_query();
    // header：host:port (in Project4)
    for (auto& t : q.targets)
        std::cout << "<th>"<<t.h<<":"<<t.p<<"</th>";
    std::cout << "</tr></thead><tbody><tr>";
    // body： <pre id="s0">…</pre>
    for (size_t i = 0; i < q.targets.size(); i++)
        std::cout << "<td><pre id=\"s"<<i<<"\"></pre></td>";
    std::cout << "</tr></tbody></table>\n" << std::flush;

    // Session
    boost::asio::io_context io;
    for (size_t i = 0; i < q.targets.size(); ++i) {
        auto& t = q.targets[i];
        std::make_shared<Session>(
            io, static_cast<int>(i),
            q.sh, q.sp,
            t.h, t.p, t.f
        )->start();
    }
    io.run();
    return 0;
}
