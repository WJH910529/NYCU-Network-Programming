#ifndef NP_SINGLE_PROC_H
#define NP_SINGLE_PROC_H

#include <iostream>
#include <sstream>
#include <vector>
#include <array>
#include <string>
#include <cstring> //memset
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

using namespace std;

#define MAX_CLIENTS 30
#define MAX_LINE_LENGTH 15000
#define MAX_CMD_LENGTH 256

struct Command{
    vector<string> args;
    bool has_redirection = false;
    string outfile;
    int pipeDelay = -1;    // -1: no pipe; 0: ordinary pipe; >0: numbered pipe
    bool pipeStdErr = false;

    // user pipe
    bool userPipeOut = false;   // >n
    int  userPipeOutTarget = -1;
    bool userPipeIn = false;    // <n
    int  userPipeInSource = -1;

    // 執行時使用的 fd
    int fd_in = STDIN_FILENO;
    int fd_out = STDOUT_FILENO;
    int fd_err = STDERR_FILENO;
};

struct Client{
    int id;
    int sockfd;
    string ip;
    int port;
    string name;
    
    // store environment variables
    unordered_map<string, string> env;
    // numbered pipes
    unordered_map<int, array<int,2>> numberedPipes;
};

//---------Function Prototypes---------

void broadcastMessage(const string &msg);

// Send a message to a specific client
void sendToClient(Client* client, const string &msg);

// Return a pointer to the client of a given ID, or nullptr if not found
Client* getClientById(int id);

// Assign the smallest available user ID in [1..MAX_CLIENTS]
int assignClientId();

// Close and remove a client from all data structures
void closeAndRemoveClient(Client* dc);

vector<Command> parseCommandLine(const string &line);

// for execvp
vector<char*> buildArgv(const Command &cmd);

// Decrement all numbered pipes by 1, close/erase expired ones
void updateNumberedPipes(Client* client);

// Process built-in commands: exit, setenv, printenv, who, tell, yell, name.
bool handleBuiltin(Client* client, const vector<string> &tokens);

void executeCommandLine(Client* client, const string &line);

#endif