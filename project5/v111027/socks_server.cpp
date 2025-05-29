// socks_server.cpp
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <iostream>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <unistd.h>      // fork
#include <sys/wait.h>    // waitpid

using boost::asio::ip::tcp;

//Firewall Rule
struct FirewallRule{
    bool isConnect;  // true=CONNECT, false=BIND
    std::regex pattern; // compiled from wildcard
};

std::vector<FirewallRule> load_firewall_rules(const std::string& file="socks.conf") {
    std::vector<FirewallRule> rules;
    std::ifstream in(file);
    std::string line;
    std::regex rx(R"(permit\s+([cb])\s+([\d\.\*]+))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, rx)) {
            bool isC = (m[1] == "c");
            std::string pat = m[2];
            std::string cre = "^" +
                std::regex_replace(pat, std::regex(R"(\*)"), R"(\d+)") +
                "$";
            rules.push_back({ isC, std::regex(cre) });
        }
    }
    return rules;
}

bool check_firewall(const std::vector<FirewallRule>& rules,
                    bool isConnect,
                    const std::string& dest_ip) {
    for (auto& r : rules)
        if (r.isConnect == isConnect && std::regex_match(dest_ip, r.pattern))
            return true;
    return false;
}

//Session: handle one client, SOCKS4/4A 
class Session : public std::enable_shared_from_this<Session> {
public:
    static std::shared_ptr<Session> create(boost::asio::io_context& ctx) {
        return std::shared_ptr<Session>(new Session(ctx));
    }
    tcp::socket& socket() { return client_sock_; }
    void start() { read_header(); }

private:
    explicit Session(boost::asio::io_context& ctx)
      : client_sock_(ctx),
        remote_sock_(ctx),
        resolver_(ctx),
        bind_acceptor_(nullptr),
        socks4a_(false)
    {}

    void read_header() { //VN(1byte), CD(1byte), DSTPORT(2bytes), DSTIP(4bytes)
        auto self = shared_from_this();
        boost::asio::async_read(client_sock_,
            boost::asio::buffer(header_),
            [this,self](auto ec, auto){
                if (!ec) parse_header();
            });
    }

    void parse_header() {
        version_   = header_[0];
        command_   = header_[1];
        dest_port_ = (header_[2]<<8) | header_[3];
        ipb_       = { header_[4],header_[5],header_[6],header_[7] };
        socks4a_   = (ipb_[0]==0 && ipb_[1]==0 && ipb_[2]==0 && ipb_[3]!=0);
        read_userid();
    }

    void read_userid() {
        auto self = shared_from_this();
        boost::asio::async_read_until(client_sock_, buf_, '\0',
            [this,self](auto ec, auto){
                if (ec) return;
                std::istream is(&buf_);
                std::getline(is, userid_, '\0');
                if (socks4a_) read_domain();
                else         resolve_and_handle();
            });
    }

    void read_domain() {
        auto self = shared_from_this();
        boost::asio::async_read_until(client_sock_, buf_, '\0',
            [this,self](auto ec, auto){
                if (ec) return;
                std::istream is(&buf_);
                std::getline(is, domain_, '\0');
                resolve_and_handle();
            });
    }

    void resolve_and_handle() {
        // Build dest_ip_ early so header_msg_ can include it even on Reject
        if (socks4a_) {
            dest_ip_ = domain_;
        } else {
            dest_ip_ = std::to_string(ipb_[0])+"."+
                       std::to_string(ipb_[1])+"."+
                       std::to_string(ipb_[2])+"."+
                       std::to_string(ipb_[3]);
        }
        prepare_header();

        if (socks4a_) {
            // async resolve domain
            auto self = shared_from_this();
            resolver_.async_resolve(domain_, std::to_string(dest_port_),
                [this,self](auto ec, auto results){
                    if (ec || results.empty()) {
                        send_reply(91,"Reject");
                    } else {
                        ep_to_connect_ = (*results.begin()).endpoint();
                        // override dest_ip_ to actual IP in header
                        dest_ip_ = ep_to_connect_.address().to_string();
                        prepare_header(); 
                        handle_request();
                    }
                });
        } else {
            ep_to_connect_ = tcp::endpoint(
                boost::asio::ip::make_address(dest_ip_), dest_port_);
            handle_request();
        }
    }

    void prepare_header() {
        std::string cmd = (command_==1 ? "CONNECT" : "BIND");
        header_msg_ =
            "<S_IP>: "   + client_sock_.remote_endpoint().address().to_string() + "\n"
          + "<S_PORT>: " + std::to_string(client_sock_.remote_endpoint().port())   + "\n"
          + "<D_IP>: "   + dest_ip_                                             + "\n"
          + "<D_PORT>: " + std::to_string(dest_port_)                           + "\n"
          + "<Command>: " + cmd + "\n";
    }

    void handle_request() {
        if (version_!=4 || (command_!=1 && command_!=2)) {
            send_reply(91,"Reject");
            return;
        }

        auto rules = load_firewall_rules();
        bool ok = check_firewall(rules, command_==1, dest_ip_);
        if (!ok) {
            send_reply(91,"Reject");
            return;
        }

        if (command_==1) do_connect();
        else             do_bind();
    }

    void do_connect() {
        auto self = shared_from_this();
        remote_sock_.async_connect(ep_to_connect_,
            [this,self](auto ec){
                if (ec) send_reply(91,"Reject");
                else {
                    send_reply(90,"Accept");
                    start_relay();
                }
            });
    }

    void do_bind() {
        bind_acceptor_ = std::make_unique<tcp::acceptor>(client_sock_.get_executor());
        bind_acceptor_->open(tcp::v4());
        bind_acceptor_->set_option(tcp::acceptor::reuse_address(true));
        bind_acceptor_->bind(tcp::endpoint(tcp::v4(), 0));  // port=0 → system allocate
        bind_acceptor_->listen();

        // port（host order）
        uint16_t port = bind_acceptor_->local_endpoint().port();

        //first reply, reply code=90，port use host-order，IP is 0.0.0.0
        send_reply(90, "Accept", port);
        // (send_reply: port>>8/port&0xFF change to network-order byte)

        // sync accept, wait remote
        bind_acceptor_->accept(remote_sock_);
        bind_acceptor_->close();

        // second response, code + port
        send_reply(90, "Accept", port);

        // std::cout << header_msg_
        //         << "<Reply>: Accept\n\n";
        start_relay();
    }

    void send_reply(uint8_t code, const std::string& txt,
                    uint16_t port=0,
                    const std::array<uint8_t,4>& ip={{0,0,0,0}})
    {
        std::array<uint8_t,8> rep{};
        rep[0]=0; rep[1]=code;
        rep[2]=uint8_t(port>>8);
        rep[3]=uint8_t(port&0xFF);
        for(int i=0;i<4;++i) rep[4+i]=ip[i];

        auto self = shared_from_this();
        boost::asio::async_write(client_sock_,
            boost::asio::buffer(rep),
            [this,self,txt](auto,auto){
                std::cout<< header_msg_
                         << "<Reply>: "<<txt<<"\n\n";
                if(txt=="Reject")
                    client_sock_.close();
            });
    }
    
    void start_relay() {
        do_read_client();
        do_read_remote();
    }
    void do_read_client() {
        auto self = shared_from_this();
        client_sock_.async_read_some(boost::asio::buffer(buf_c_),
            [this,self](auto ec,size_t n){
                if(!ec) {
                    boost::asio::async_write(remote_sock_,
                        boost::asio::buffer(buf_c_,n),
                        [this,self](auto ec2,auto){
                            if(!ec2) do_read_client();
                        });
                }
            });
    }
    void do_read_remote() {
        auto self = shared_from_this();
        remote_sock_.async_read_some(boost::asio::buffer(buf_r_),
            [this,self](auto ec,size_t n){
                if(!ec) {
                    boost::asio::async_write(client_sock_,
                        boost::asio::buffer(buf_r_,n),
                        [this,self](auto ec2,auto){
                            if(!ec2) do_read_remote();
                        });
                }
            });
    }

    tcp::socket                         client_sock_;
    tcp::socket                         remote_sock_;
    tcp::resolver                       resolver_;
    tcp::endpoint                       ep_to_connect_;
    std::unique_ptr<tcp::acceptor>      bind_acceptor_;
    std::array<uint8_t,8>               header_;
    std::array<uint8_t,4>               ipb_;
    uint8_t                             version_{}, command_{};
    uint16_t                            dest_port_{};
    bool                                socks4a_;
    std::string                         userid_, domain_, dest_ip_, header_msg_;
    boost::asio::streambuf              buf_;
    std::array<char,4096>               buf_c_, buf_r_;
};

// async server with fork 
class Server{
public:
    Server(boost::asio::io_context& ctx, unsigned short port)
      : io_ctx_(ctx),
        acceptor_(ctx, tcp::endpoint(tcp::v4(), port))
    {
        start_accept();
    }

private:
    void start_accept(){
        auto session = Session::create(io_ctx_);
                acceptor_.async_accept(session->socket(),
            [this, session](boost::system::error_code ec){
                if (!ec) {
                    io_ctx_.notify_fork(boost::asio::io_context::fork_prepare);
                    pid_t pid = fork();
                    if (pid > 0) {
                        io_ctx_.notify_fork(boost::asio::io_context::fork_parent);
                        session->socket().close();
                        while (waitpid(-1, nullptr, WNOHANG) > 0) {}
                        start_accept();
                    }
                    else if (pid == 0) {
                        io_ctx_.notify_fork(boost::asio::io_context::fork_child);
                        acceptor_.close();
                        session->start();
                    }
                } else {
                    start_accept();
                }
            });
    }

    boost::asio::io_context& io_ctx_;
    tcp::acceptor            acceptor_;
};

int main(int argc, char* argv[]) {
    signal(SIGCHLD, SIG_IGN);
    if (argc!=2) {
        std::cerr<<"Usage: "<<argv[0]<<" <port>\n";
        return 1;
    }
    unsigned short port = static_cast<unsigned short>(std::stoi(argv[1]));

    boost::asio::io_context io_ctx;
    Server server(io_ctx, port);
    io_ctx.run();
    return 0;
}