#ifndef NP_MULTI_PROC_H
#define NP_MULTI_PROC_H

#include <iostream>
#include <sstream>
#include <vector>
#include <array>
#include <string>
#include <cstring>      // memset, strcmp
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>

using namespace std;

#define MAX_CLIENTS 30
#define MAX_LINE_LENGTH 15000
#define MAX_CMD_LENGTH 256
#define MAX_NAME_LEN 21
#define MAX_IP_LEN 16
#define SHM_KEY 1127
#define PERM 0666

// 用於命令解析的結構
struct Command {
    vector<string> args;
    bool has_redirection = false;
    string outfile;
    int pipeDelay = -1;    // -1: no pipe; 0: ordinary pipe; >0: numbered pipe
    bool pipeStdErr = false;

    // user pipe 旗標
    bool userPipeOut = false;   // 命令中 ">n"
    int  userPipeOutTarget = -1;
    bool userPipeIn = false;      // 命令中 "<n"
    int  userPipeInSource = -1;

    // 執行時使用的fd
    int fd_in = STDIN_FILENO;
    int fd_out = STDOUT_FILENO;
    int fd_err = STDERR_FILENO;
};

// 客戶端本地資料（在child process中，不放在share memory中）
struct Client {
    int id;                // 使用者 ID: 1 ~ MAX_CLIENTS
    int sockfd;            // 此連線的 socket（dup 到 STDIN／STDOUT）
    string ip;
    int port;
    string name;           // 預設 "(no name)"
    unordered_map<string, string> env; // 例如 PATH
    unordered_map<int, array<int,2>> numberedPipes;
    pid_t pid;             // 本child process pid（用於發signal）
};

// 共享記憶體中client資料（用 C 字串存放）
struct SharedClient {
    int used;                   // 0 表示空閒，1 表示使用中
    int id;
    char ip[MAX_IP_LEN];        // IP address
    int port;
    char name[MAX_NAME_LEN];    // 使用者名稱
    pid_t pid;                  // 用戶子進程 PID
    char message[1024];         // 訊息緩衝區（廣播或點對點訊息）
};

struct SharedClients {
    SharedClient clients[MAX_CLIENTS];
};

// Function Prototypes
void broadcastMessage(const string &msg);
void sendToClient(int clientId, const string &msg);
int assignClientId(SharedClients *shmClients);
void updateSharedClient(SharedClients *shmClients, const Client &client);
void removeSharedClient(SharedClients *shmClients, int clientId);

vector<Command> parseCommandLine(const string &line);
vector<char*> buildArgv(const Command &cmd);
void updateNumberedPipes(Client* client);
bool handleBuiltin(Client* client, const vector<string> &tokens, SharedClients *shmClients);
void executeCommandLine(int sockfd, Client* client, const string &line, SharedClients *shmClients);

void cleanup(int signo);
void cleanupUserPipes(int userId);

// user pipe 接收相關全域變數 和 signal處理
extern unordered_map<int, int> pendingUserPipeFD; // key: sender id, value: FIFO 的 read fd
extern int g_myClientId; // 每個 child process 保存自己的 ID

#endif
