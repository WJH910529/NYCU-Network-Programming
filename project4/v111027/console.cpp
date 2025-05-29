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

void output_shell(int id, const std::string& msg) {
    std::cout << "<script>document.getElementById('s" << id
              << "').innerHTML += '" << html_escape(msg)
              << "';</script>\n" << std::flush;
}

void output_cmd(int id, const std::string& cmd) {
    std::cout << "<script>document.getElementById('s" << id
              << "').innerHTML += '<b>"
              << html_escape(cmd)
              << "</b>';</script>\n" << std::flush;
}

// ---------- Session ----------
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::io_context& io, int id,
            std::string host, std::string port, std::string file)
        : resolver_(io), socket_(io),
          id_(id), host_(std::move(host)), port_(std::move(port)),
          file_(std::move(file)) {}

    void start() {
        cmdfile_.open("test_case/" + file_);
        if (!cmdfile_) return;
        auto self = shared_from_this();
        resolver_.async_resolve(host_, port_,
            [this, self](auto ec, auto results){
                if(!ec){
                    boost::asio::async_connect(socket_, results,
                    [this, self](auto ec2, auto){
                        if( !ec2) do_read();
                    });
                }
            });
    }

private:
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

    tcp::resolver resolver_;
    tcp::socket   socket_;
    std::array<char, 4096> buf_;
    int id_;
    std::string host_, port_, file_;
    std::ifstream cmdfile_;
    std::string next_;
};

//QUERY_STRING
struct Target { std::string h,p,f; };
std::vector<Target> parse_query() {
    const char* qs = std::getenv("QUERY_STRING");
    std::vector<Target> res(5);
    if (!qs) return {};
    //kv_re = key-value regular expression
    std::regex kv_re(R"(([hpf])(\d)=(.*?)(&|$))");
    std::cmatch m;
    const char* cur = qs;
    while( std::regex_search(cur, m, kv_re)){
        char ch = m[1].str()[0];
        int idx = m[2].str()[0] - '0';
        std::string val = m[3];
        if( idx>=0 && idx<5 ){
            if(ch =='h') res[idx].h = val;
            else if (ch == 'p') res[idx].p = val;
            else if(ch == 'f') res[idx].f = val;
        }
        cur = m.suffix().first;
    }
    std::vector<Target> out;
    for(auto& t: res)
        if(!t.h.empty() && !t.p.empty() && !t.f.empty())
            out.push_back(t);
    return out;
}

int main() {
    //HTTP header
    std::cout << "Content-Type: text/html\r\n\r\n";

    //link info
    auto targets = parse_query();

    //generate table frame
    std::cout << R"(
    <!DOCTYPE html><html>
    <head><meta charset="UTF-8">
      <title>NP Project4 Console</title>
      <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css">
      <style>
      body { background:#212529; color:#e5e5e5; font-family: 'Source Code Pro', monospace; color:#ccc;}
      table { width:100%; }
      b { color:#01b468; }
      pre   { margin:0; white-space:pre-wrap; color:#f1f3f5; } 
      </style>
    </head>
    <body><table class="table table-dark table-bordered"><thead><tr>)";

    for (auto& t:targets)
        std::cout << "<th>" << t.h << ":" << t.p << "</th>";
    std::cout << "</tr></thead><tbody><tr>";
    for (size_t i=0;i<targets.size();++i)
        std::cout << "<td><pre id=\"s" << i << "\"></pre></td>";
    std::cout << "</tr></tbody></table>\n" << std::flush;

    boost::asio::io_context io;
    for (size_t i=0; i<targets.size(); ++i) {
        auto& t = targets[i];
        std::make_shared<Session>(io, static_cast<int>(i),
                                  t.h, t.p, t.f)->start();
    }
    io.run();
    return 0;
}
