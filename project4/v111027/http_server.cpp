#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <cstring>
#include <regex>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sstream>
#include <cerrno>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

constexpr size_t max_length = 8192;          // 足夠放下完整 HTTP header
constexpr char CRLF[]        = "\r\n";
constexpr char HEADER_END[]  = "\r\n\r\n";

void reap_child(int) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
}

class session : public std::enable_shared_from_this<session>
{
  public:
    session(tcp::socket socket)
        : socket_(std::move(socket)){}

    void start()
    {
        do_read();
    }

  private:
  // asynchoronus read until \r\n\r\n
    void do_read()
    {
        auto self(shared_from_this());
        socket_.async_read_some(boost::asio::buffer(data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length)
            {
                if (!ec)
                {
                    request_ += std::string(data_, length);
                    if(request_.find(HEADER_END) != std::string::npos){
                        handle_request();
                    } else {
                        do_read(); // not finish reading yet. 
                    }
                }
            });
    }

    void handle_request(){
        //method uri protcol
        std::istringstream iss(request_);
        std::string method, uri, protocol;
        iss >> method >> uri >> protocol; //this will left \r\n

        //to eat left "\r\n"
        std::string dummy;
        std::getline(iss, dummy); // read '\r' to dummuy, and getline discards the terminator '\n'

        if(method != "GET"){ send_400(); return; }
        if (!std::regex_match(uri, std::regex(R"(/[\w\-.]+\.cgi(\?.*)?)"))) {
            send_404(); return;
        }

        //host header
        std::string line, host_header;
        while( std::getline(iss, line) && line != "\r"){
            if (line.rfind("Host:", 0) == 0){
                host_header = line.substr(5);
                //erase space and \r
                host_header.erase(0, host_header.find_first_not_of(" \t"));
                host_header.erase(host_header.find_last_not_of("\r\n")+1);
            }
        }

        //URI / Query
        std::string cgi_path = "." + uri;
        std::string query_string;
        auto qpos = uri.find('?');
        if(qpos != std::string::npos){
            cgi_path = "." + uri.substr(0, qpos); //"." + /xxx.cgi
            query_string = uri.substr(qpos + 1);
        }

        // HTTP response header
        auto self(shared_from_this());
        const std::string resp_header = "HTTP/1.1 200 OK\r\n";

        boost::asio::async_write(socket_, boost::asio::buffer(resp_header),
                          [this, self, protocol, uri, query_string,
                           host_header, cgi_path](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                launch_cgi(protocol, uri, query_string,
                           host_header, cgi_path);
            }
        });
    }

    void launch_cgi(const std::string& protocol,
                    const std::string& uri,
                    const std::string& query,
                    const std::string& host,
                    const std::string& path){
        //fork
        pid_t pid;
        while ((pid = fork()) < 0) {
            waitpid(-1, nullptr, 0);
        }
        if(pid == 0){ //child
            dup2(socket_.native_handle(), STDOUT_FILENO);
            dup2(socket_.native_handle(), STDERR_FILENO);

            setenv("REQUEST_METHOD", "GET", 1);
            setenv("REQUEST_URI", uri.c_str(), 1);
            setenv("QUERY_STRING", query.c_str(), 1);
            setenv("SERVER_PROTOCOL", protocol.c_str(), 1);
            setenv("HTTP_HOST", host.c_str(), 1);

            auto local_ep = socket_.local_endpoint();
            auto remote_ep = socket_.remote_endpoint();
            setenv("SERVER_ADDR",
                    local_ep.address().to_string().c_str(), 1);
            setenv("SERVER_PORT",
                    std::to_string(local_ep.port()).c_str(), 1);
            setenv("REMOTE_ADDR",
                    remote_ep.address().to_string().c_str(), 1);
            setenv("REMOTE_PORT",
                    std::to_string(remote_ep.port()).c_str(), 1);       
            
            socket_.close();

            setenv("PATH", "/bin:/usr/bin", 1); //set PATH to search /bin first, then /usr/bin

            //exec cgi
            execlp(path.c_str(), path.c_str(), nullptr);
            // if exec fail
            std::cerr << "Exec error: " << strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
        //parent
        socket_.close(); 
    }

    //error message to send and handler
    void send_common(const std::string& header) {
        auto self(shared_from_this());
        boost::asio::async_write(socket_, boost::asio::buffer(header),
                          [this, self](boost::system::error_code, std::size_t) {
            socket_.close();
        });
    }
    void send_400() { send_common("HTTP/1.1 400 Bad Request\r\n\r\n"); }
    void send_404() { send_common("HTTP/1.1 404 Not Found\r\n\r\n"); }

    tcp::socket socket_;
    char data_[max_length];
    std::string request_;
};

class server
{
public:
  server(boost::asio::io_context& io_context, short port)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
  {
    do_accept();
  }

private:
  void do_accept()
  {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket)
        {
          if (!ec)
          {
            std::make_shared<session>(std::move(socket))->start();
          }

          do_accept();
        });
  }

  tcp::acceptor acceptor_;
};

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 2)
    {
      std::cerr << "Usage: ./http_server <port>\n";
      return 1;
    }

    signal(SIGCHLD, reap_child);
    
    boost::asio::io_context io_context;

    server s(io_context, std::atoi(argv[1]));

    io_context.run();
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}