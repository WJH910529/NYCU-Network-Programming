#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstring> //memset
#include "npshell.h"  // npshell header
#define QLEN_LISTEN_BACKLOG 50
using namespace std;

void sigchld_handler(int signo) {
    (void)signo;  // signo is unused
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
        cout << "A client disconnected." << endl;
    }
}

int passiveTCP(int port){
    int sockfd;
    struct sockaddr_in serv_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0){
        perror("socket error");
        exit(1);
    }
    int opt = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0){
        perror("setsockopt error");
        exit(1);
    }
    // memset((char *)&serv_addr, 0, sizeof(serv_addr));
    ////cs.pdf use this function, bzero and memset have the same function in this usage, they both initialize a piece of memory to 0
    bzero((char *)&serv_addr, sizeof(serv_addr)); 
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if(bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
        perror("bind error");
        exit(1);
    }
    if(listen(sockfd, QLEN_LISTEN_BACKLOG) < 0){
        perror("listen error");
        exit(1);
    }
    return sockfd;
}

int main(int argc, char *argv[]){
    // 設定 SIGCHLD handler
    signal(SIGCHLD, sigchld_handler);

    int port;
    if(argc > 1){
        port = atoi(argv[1]);
    }else{
        port = 7001; //deafult
    }

    cout<<"[Port]: "<< port << endl;
    int msock = passiveTCP(port);
    int ssock;
    struct sockaddr_in cli_addr; // the from address of a client 
    int addr_len = sizeof(cli_addr); //from-address length

    while(true){
        ssock = accept(msock, (struct sockaddr *)&cli_addr, (socklen_t*)&addr_len);
        if(ssock < 0){
            perror("accept error");
            continue;
        }
        cout << "New Connection from " << inet_ntoa(cli_addr.sin_addr)<< ":" << ntohs(cli_addr.sin_port) << endl;

        //fork 
        pid_t pid = fork();
        if(pid < 0){
            perror("fork error");
            close(ssock);
            continue;
        }
        if(pid > 0){
            //parent: close client socket，wait for others
            close(ssock);
        }else{
            //child
            close(msock); // child process doesn't need to listen with msock
            dup2(ssock, STDIN_FILENO);
            dup2(ssock, STDOUT_FILENO);
            dup2(ssock, STDERR_FILENO);
            close(ssock);
            //call npshell's shell() to handle client command
            shell();
            exit(0);
        }
    }
    close(msock);
    return 0;
}


