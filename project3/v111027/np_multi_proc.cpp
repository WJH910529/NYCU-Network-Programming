#include "np_multi_proc.h"

//Global Data Definitions
int shm_id;
SharedClients *shmClients;

// FIFO 目錄, 用於 user pipe
const string FIFO_DIR = "user_pipe/";

// 取得 FIFO 檔名 (格式： FIFO_DIR/pipe_<src>_<dst>)
string getFifoName(int src, int dst) {
    return FIFO_DIR + "pipe_" + to_string(src) + "_" + to_string(dst);
}

//global data for receiver user pipe
unordered_map<int, int> pendingUserPipeFD; // key: sender id, value: FIFO 讀取 fd
int g_myClientId = 0;  // 每個 child process 中保存自己的用戶 id

//SIGUSR1: 讀取 share memory 中自己的訊息並寫到 STDOUT_FILENO
void sigusr1_handler(int signum) {
    (void)signum;  //  標記為已使用，消除 warning
    pid_t mypid = getpid();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (shmClients->clients[i].used && shmClients->clients[i].pid == mypid) {
            if (strlen(shmClients->clients[i].message) > 0) {
                write(STDOUT_FILENO, shmClients->clients[i].message, strlen(shmClients->clients[i].message));
                memset(shmClients->clients[i].message, 0, sizeof(shmClients->clients[i].message));
            }
            break;
        }
    }
    signal(SIGUSR1, sigusr1_handler);
}


//SIGUSR2：接收端通知自己要打開 FIFO 的讀取端
void sigusr2_handler(int signum){
    (void)signum; 
    for(int sender = 1; sender <= MAX_CLIENTS; sender++){
        string fifoName = getFifoName(sender, g_myClientId);
        if(access(fifoName.c_str(), F_OK) == 0) {
            int fd = open(fifoName.c_str(), O_RDONLY | O_NONBLOCK);
            if(fd >= 0) {
                if(pendingUserPipeFD.find(sender) == pendingUserPipeFD.end()) {
                    pendingUserPipeFD[sender] = fd;
                }
            }
        }
        signal(SIGUSR2, sigusr2_handler);
    }
}

void cleanup(int signo) {
    (void)signo; 
    // 先把所有 client 的 user‑pipe FIFO 刪一遍
    for (int id = 1; id <= MAX_CLIENTS; ++id) {
        cleanupUserPipes(id);
    }
    // 刪空之後再移除整個 user_pipe 目錄
    rmdir(FIFO_DIR.c_str());

    // 再 detach 共享記憶體
    if (shmdt(shmClients) < 0) perror("shmdt");
    // 再標記整塊 shared memory 要被移除
    if (shmctl(shm_id, IPC_RMID, NULL) < 0) perror("shmctl IPC_RMID");
    exit(0);
}

//清除與特定 userId 相關的 FIFO 檔案
void cleanupUserPipes(int userId) {
    // userId 當作 sender，檢查所有可能的 receiver
    for (int dst = 1; dst <= MAX_CLIENTS; dst++) {
        string fifo = getFifoName(userId, dst);
        unlink(fifo.c_str());
    }
    // userId 當作 receiver ，檢查所有可能的 sender
    for (int src = 1; src <= MAX_CLIENTS; src++) {
        string fifo = getFifoName(src, userId);
        unlink(fifo.c_str());
    }
    // 從 pendingUserPipeFD 中移除與 userId 相關的記錄
    pendingUserPipeFD.erase(userId);
    for (auto it = pendingUserPipeFD.begin(); it != pendingUserPipeFD.end(); ) {
        if (it->first == userId) {
            close(it->second);
            it = pendingUserPipeFD.erase(it);
        } else {
            ++it;
        }
    }
}

void removeSharedClient(SharedClients *shmClients, int clientId) {
    // 清除與該用戶相關的 FIFO 檔案
    cleanupUserPipes(clientId);

    int idx = clientId - 1;
    shmClients->clients[idx].used = 0;
    memset(shmClients->clients[idx].ip, 0, MAX_IP_LEN);
    memset(shmClients->clients[idx].name, 0, MAX_NAME_LEN);
    shmClients->clients[idx].port = 0;
    shmClients->clients[idx].pid = 0;
    memset(shmClients->clients[idx].message, 0, sizeof(shmClients->clients[idx].message));
}

void broadcastMessage(const string &msg) {
    for(int i=0; i < MAX_CLIENTS; i++){
        if (shmClients->clients[i].used) {
            strncat(shmClients->clients[i].message, msg.c_str(), 
                    sizeof(shmClients->clients[i].message) - strlen(shmClients->clients[i].message) - 1);
            kill(shmClients->clients[i].pid, SIGUSR1);
        }
    }
}

void send2TargetClient(int clientId, const string &msg) {
    int idx = clientId - 1;
    if (shmClients->clients[idx].used) {
        strncat(shmClients->clients[idx].message, msg.c_str(),
                sizeof(shmClients->clients[idx].message) - strlen(shmClients->clients[idx].message) - 1);
        kill(shmClients->clients[idx].pid, SIGUSR1);
    }
}

int assignClientId(SharedClients *shmClients) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (shmClients->clients[i].used == 0) {
            return i + 1; // 1-indexed
        }
    }
    return -1;
}

void updateSharedClient(SharedClients *shmClients, const Client &client) {
    int idx = client.id - 1;
    shmClients->clients[idx].used = 1;
    shmClients->clients[idx].id = client.id;
    strncpy(shmClients->clients[idx].ip, client.ip.c_str(), MAX_IP_LEN - 1);
    shmClients->clients[idx].ip[MAX_IP_LEN - 1] = '\0';
    shmClients->clients[idx].port = client.port;
    strncpy(shmClients->clients[idx].name, client.name.c_str(), MAX_NAME_LEN - 1);
    shmClients->clients[idx].name[MAX_NAME_LEN - 1] = '\0';
    shmClients->clients[idx].pid = client.pid;
}

vector<Command> parseCommandLine(const string &line) {
    vector<Command> cmds;
    istringstream iss(line);
    string token;
    vector<string> currentArgs;
    Command curCmd;

    while (iss >> token) {
        // check for pipe or !-pipe
        if (token[0] == '|' || token[0] == '!') {
            curCmd.args = currentArgs;
            currentArgs.clear();
            if(token.size() == 1) {
                curCmd.pipeDelay = 0;  // ordinary pipe
            } else {
                curCmd.pipeDelay = stoi(token.substr(1)); // numbered pipe
            }
            if(token[0] == '!') {
                curCmd.pipeStdErr = true; 
            }
            cmds.push_back(curCmd);
            curCmd = Command();
        }
        // user pipe out
        else if (token[0] == '>' && token.size() > 1 && isdigit(token[1])) {
            curCmd.userPipeOut = true;
            curCmd.userPipeOutTarget = stoi(token.substr(1));
        }
        // user pipe in
        else if (token[0] == '<' && token.size() > 1 && isdigit(token[1])) {
            curCmd.userPipeIn = true;
            curCmd.userPipeInSource = stoi(token.substr(1));
        }
        // redirection to file
        else if (token == ">") {
            curCmd.has_redirection = true;
            if (iss >> token) {
                curCmd.outfile = token;
            }
        } 
        else {
            currentArgs.push_back(token);
        }
    }
    // last command
    if (!currentArgs.empty()) {
        curCmd.args = currentArgs;
        cmds.push_back(curCmd);
    }
    return cmds;
}

vector<char*> buildArgv(const Command &cmd) {
    vector<char*> argv;
    for (auto &arg : cmd.args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

void updateNumberedPipes(Client* client) {
    unordered_map<int, array<int,2>> newMap;
    for (auto &entry : client->numberedPipes) {
        int newKey = entry.first - 1;
        if (newKey >= 0) {
            newMap[newKey] = entry.second;
        } else {
            // close and remove old pipe
            close(entry.second[0]);
            close(entry.second[1]);
        }
    }
    client->numberedPipes = move(newMap);
}

bool handleBuiltin(Client* client, const vector<string> &tokens, SharedClients *shmClients) {
    if(tokens.empty())return true;
    string cmd = tokens[0];

    if(cmd == "exit"){
        removeSharedClient(shmClients, client->id);
        string logoutMsg = "*** User '" + client->name + "' left. ***\n";
        broadcastMessage(logoutMsg);
        if (shmdt(shmClients) < 0) perror("shmdt in builtin exit");
        exit(0);
    }
    else if(cmd == "setenv"){
        if (tokens.size() < 3){
            string usage = "Usage: setenv [var] [value]\n";
            write(STDOUT_FILENO, usage.c_str(), usage.size());
        } else {
            client->env[tokens[1]] = tokens[2];
        }
        return true;
    }
    else if(cmd == "printenv"){
        if(tokens.size() < 2){
            string usage = "Usage: printenv [var]\n";
            write(STDOUT_FILENO, usage.c_str(), usage.size());
        } else {
            auto it = client->env.find(tokens[1]);
            if(it != client->env.end()){
                string out = client->env[tokens[1]] + "\n";
                write(STDOUT_FILENO, out.c_str(), out.size());
            }
        }
        return true;
    }
    else if(cmd == "who"){
        string out = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if(shmClients->clients[i].used) {
                out += to_string(shmClients->clients[i].id) + "\t" +
                       string(shmClients->clients[i].name) + "\t" +
                       string(shmClients->clients[i].ip) + ":" +
                       to_string(shmClients->clients[i].port);
                if(shmClients->clients[i].id == client->id)
                    out += "\t<-me";
                out += "\n";
            }
        }
        write(STDOUT_FILENO, out.c_str(), out.size());
        return true;
    }
    else if(cmd == "tell"){
        if (tokens.size() < 3) return true;
        int targetId = stoi(tokens[1]);
        if(targetId < 1 || targetId > MAX_CLIENTS || !shmClients->clients[targetId-1].used) {
            string err = "*** Error: user #" + to_string(targetId) + " does not exist yet. ***\n";
            write(STDOUT_FILENO, err.c_str(), err.size());
        }
        else{
            //gather all the rest as message
            string msg;
            for(size_t i = 2; i < tokens.size(); i++){
                msg += tokens[i] + " ";
            }
            string fullMsg = "*** " + string(shmClients->clients[client->id-1].name) + " told you ***: " + msg + "\n";
            send2TargetClient(targetId, fullMsg);
        }
        return true;
    }
    else if (cmd == "yell"){
        //gather all tokens after "yell"
        string msg;
        for(size_t i = 1; i < tokens.size(); i++){
            msg += tokens[i] + " ";
        }
        string fullMsg = "*** " + string(shmClients->clients[client->id-1].name) + " yelled ***: " + msg + "\n";
        broadcastMessage(fullMsg);
        return true;
    }
    else if (cmd == "name"){
        if(tokens.size() < 2) return true;
        string newName = tokens[1];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if(shmClients->clients[i].used && string(shmClients->clients[i].name) == newName) {
                string err = "*** User '" + newName + "' already exists. ***\n";
                write(STDOUT_FILENO, err.c_str(), err.size());
                return true;
            }
        }
        // rename
        strncpy(shmClients->clients[client->id-1].name, newName.c_str(), MAX_NAME_LEN-1);
        shmClients->clients[client->id-1].name[MAX_NAME_LEN-1] = '\0';
        client->name = newName;
        string note = "*** User from " + client->ip + ":" + to_string(client->port) +
                      " is named '" + newName + "'. ***\n";
        broadcastMessage(note);
        return true;
    }

    //not a built-in command
    return false; 
}

void executeCommandLine(int sockfd, Client* client, const string &line, SharedClients *shmClients) {
    // Quick check for an empty or whitespace line
    istringstream pre(line);
    string firstToken;
    pre >> firstToken;
    if(firstToken.empty()) {
        // user input 空行, 啥都不做 送回%
        write(sockfd, "% ", 2);
        return;
    }

    set<string> builtins ={
        "exit","setenv","printenv","who","tell","yell","name"
    };
    //---if it's built-in func. handle that then return---
    if(builtins.count(firstToken)){
        vector<string> tokens;
        tokens.push_back(firstToken);
        string tmp;
        while(pre >> tmp){
            tokens.push_back(tmp);
        }
        handleBuiltin(client, tokens, shmClients);
        updateNumberedPipes(client); //built in function also need to count one line, so need to update numberedpipe
        write(sockfd, "% ", 2);
        return;
    }

    //------Not a built-in. Parse for user pipes, umbered pipes, normal pipes, etc.
    vector<Command> cmds = parseCommandLine(line);
    if(cmds.empty()){
        write(sockfd, "% ", 2);
        return;
    }
    
    //check if there's a leftover numberd pipe(key==0) from previous commands
    int inputFd = STDIN_FILENO;
    auto it = client->numberedPipes.find(0);
    if(it != client->numberedPipes.end()){
        inputFd = it->second[0]; //read
        close(it->second[1]); //close write
        client->numberedPipes.erase(0);
    }

    //run each command in cmds
    int numCmds = cmds.size();
    for(int i=0; i<numCmds; i++){
        if(client->numberedPipes.count(0)){
        // 如果前一行留下 key==0 的 pipe，將其讀端當作本行第一個指令的輸入，
        // 同時關閉 parent process 持有的寫入端，避免造成 EOF 無法送出 (((當所有寫端都關閉後，讀端讀取時會收到 EOF
            inputFd = client->numberedPipes[0][0];
            close(client->numberedPipes[0][1]);
            client->numberedPipes.erase(0);
        }
        Command &cmd = cmds[i];
        cmd.fd_in = inputFd;

        //if the command has a numbered pipe
        if (cmd.pipeDelay != -1){
            int key = cmd.pipeDelay;
            if(client->numberedPipes.find(key) == client->numberedPipes.end()){
                //create new
                array<int,2> newPipe;
                if(pipe(newPipe.data()) < 0){
                    perror("pipe error");
                    exit(1);
                }
                client->numberedPipes[key] = newPipe;
            }
            // cout<<"i am key: "<< key<<endl;
            cmd.fd_out = client->numberedPipes[key][1];
            if(cmd.pipeStdErr){
                cmd.fd_err = client->numberedPipes[key][1];
            }
        }
        
        //user pipe in
        if(cmd.userPipeIn){
            int srcId = cmd.userPipeInSource;
            if(srcId < 1 || srcId > MAX_CLIENTS || !shmClients->clients[srcId - 1].used){
                //user doesn't exist
                string err = "*** Error: user #" + to_string(srcId) + " does not exist yet. ***\n";
                write(STDOUT_FILENO, err.c_str(), err.size());
                int devnull = open("/dev/null", O_RDONLY);
                cmd.fd_in = devnull;
            }else{
                //check fifo exist or not
                string fifoName = getFifoName(srcId, client->id);
                if(access(fifoName.c_str(), F_OK) != 0) {  //F_OK means if file exists return 0; else return -1
                    string err = "*** Error: the pipe #" + to_string(srcId) + "->#" +
                                to_string(client->id) + " does not exist yet. ***\n";
                    write(STDOUT_FILENO, err.c_str(), err.size());
                    int devnull = open("/dev/null", O_RDONLY);
                    cmd.fd_in = devnull;
                }else{
                    int fd;
                    auto it = pendingUserPipeFD.find(srcId);

                    if (it != pendingUserPipeFD.end()) {
                        fd = it->second; // read fd of FIFO
                        pendingUserPipeFD.erase(it);
                        // 把用 nonblock open 的 read fd 切成 block
                        int flags = fcntl(fd, F_GETFL, 0);
                        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                    } else {
                        //blocking open
                        fd = open(fifoName.c_str(), O_RDONLY);
                        if (fd < 0) {
                            string err = "*** Error: failed to open the pipe #"
                                       + to_string(srcId) + "->#" + to_string(client->id)
                                       + ". ***\n";
                            write(STDOUT_FILENO, err.c_str(), err.size());
                            cmd.fd_in = open("/dev/null", O_RDONLY);
                            // 不要繼續走下面的 broadcast/unlink
                            fd = -1;
                        }
                    }
                    if (fd >= 0) {
                        cmd.fd_in = fd;
                        string bmsg = "*** " + string(shmClients->clients[client->id-1].name)
                                    + " (#" + to_string(client->id) + ") just received from "
                                    + string(shmClients->clients[srcId-1].name)
                                    + " (#" + to_string(srcId) + ") by '" + line + "' ***\n";
                        broadcastMessage(bmsg);
                        unlink(fifoName.c_str());
                    }
                }
            }
        }

        //user pipe out
        if (cmd.userPipeOut){
            int dstId = cmd.userPipeOutTarget;
            if(dstId < 1 || dstId > MAX_CLIENTS || !shmClients->clients[dstId-1].used){
                string err = "*** Error: user #" + to_string(dstId) + " does not exist yet. ***\n";
                write(STDOUT_FILENO, err.c_str(), err.size());
                int devnull = open("/dev/null", O_RDWR );
                cmd.fd_out = devnull;
            } else {
                string fifoName = getFifoName(client->id, dstId);
                if(access(fifoName.c_str(), F_OK) == 0) {
                    string err = "*** Error: the pipe #" + to_string(client->id) + "->#" +
                                 to_string(dstId) + " already exists. ***\n";
                    write(STDOUT_FILENO, err.c_str(), err.size());
                    int devnull = open("/dev/null", O_RDWR);
                    cmd.fd_out = devnull;
                } else {
                    if(mkfifo(fifoName.c_str(), 0600) < 0) {
                        perror("mkfifo error");
                        exit(1);
                    }
                    // 通知接收端先建立非阻塞讀端
                    kill(shmClients->clients[dstId-1].pid, SIGUSR2);

                    // 此處以 blocking 模式開啟 FIFO 寫端
                    cmd.fd_out = open(fifoName.c_str(), O_WRONLY);
                    if(cmd.fd_out < 0){
                        string err = "*** Error: the pipe #" + to_string(client->id) + "->#" +
                                     to_string(dstId) + " cannot be opened for writing. ***\n";
                        write(STDOUT_FILENO, err.c_str(), err.size());
                        int devnull = open("/dev/null", O_RDWR);
                        cmd.fd_out = devnull;
                    } else {
                        string bmsg = "*** " + string(shmClients->clients[client->id-1].name) +
                                      " (#" + to_string(client->id) + ") just piped '" + line +
                                      "' to " + string(shmClients->clients[dstId-1].name) +
                                      " (#" + to_string(dstId) + ") ***\n";
                        broadcastMessage(bmsg);
                    }
                }
            }
        }

        //fork and execute
        pid_t pid;
        while((pid = fork()) < 0){
            waitpid(-1, nullptr, 0);
        }
        if(pid == 0){
            //child
            setenv("PATH", client->env["PATH"].c_str(), 1); //const char *envname, const char *envval, int overwrite
            
            //redirect stdin
            if(cmd.fd_in == STDIN_FILENO){
                dup2(client->sockfd, STDIN_FILENO);
            }else{
                dup2(cmd.fd_in, STDIN_FILENO);
                close(cmd.fd_in);  
            }

            //handle stdout & stderr
            int out_fd = cmd.fd_out;
            int err_fd = cmd.fd_err;
            
            //if has file redirction
            if(cmd.has_redirection){
                int fd = open(cmd.outfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    perror("open outfile error");
                    exit(1);
                }
                out_fd = fd;
            }
            if(out_fd == STDOUT_FILENO){
                dup2(client->sockfd, STDOUT_FILENO);
            }else{
                dup2(out_fd, STDOUT_FILENO);
                close(out_fd);
            }

            if (err_fd == STDERR_FILENO) {
                dup2(client->sockfd, STDERR_FILENO);
            } else {
                dup2(err_fd, STDERR_FILENO);
                close(err_fd);
            }

            //close all pipes that are not needed
            for(auto &np: client->numberedPipes){
                close(np.second[0]);
                close(np.second[1]);
            }
            
            //exec
            vector<char*> argv = buildArgv(cmd);
            if (execvp(argv[0], argv.data()) < 0) {
                string err = "Unknown command: [" + string(argv[0]) + "].\n";
                write(STDERR_FILENO, err.c_str(), err.size());
                exit(1);
            }
        }else{
            //parent
            if(cmd.fd_in != STDIN_FILENO) 
                close(cmd.fd_in);

            if(cmd.userPipeOut) {
                // 如果這是 user pipe 輸出命令，在父進程中立即關閉 write FD，
                // 這樣當child process結束時，FIFO 寫端就不會阻塞接收端
                close(cmd.fd_out);
            }

            inputFd = STDIN_FILENO;
            //it's an ordinary pipe
            if (cmd.pipeDelay == 0 && client->numberedPipes.find(0) != client->numberedPipes.end()) {
                inputFd = client->numberedPipes[0][0];
                close(client->numberedPipes[0][1]);
                client->numberedPipes.erase(0);
            }
            if(cmd.pipeDelay == -1 && !cmd.userPipeOut){
                //wait for child to finish if no pipe to next
                waitpid(pid, nullptr, 0);
            }
            else if(cmd.args[0] == "removetag0" && cmd.fd_err == STDERR_FILENO){
                waitpid(pid, nullptr, 0);
            } 
            else {
                // do not block ( including userpipe
                waitpid(pid, nullptr, WNOHANG);
            }
        }
        if(cmd.pipeDelay != 0){
            //all commands done, update numbered pipes
            updateNumberedPipes(client);
        }
    }
    // send %
    if(client->sockfd >= 0){
        write(STDOUT_FILENO, "% ", 2);
    }
}

// Passive TCP 建立
int passiveTCP(int port) {
    int sockfd;
    struct sockaddr_in serv_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0) {
        perror("socket error");
        exit(1);
    }
    int opt = 1;
    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0){
        perror("setsockopt error");
        exit(1);
    }
    bzero((char *)&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if(bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
        perror("bind error");
        exit(1);
    }
    if(listen(sockfd, 50) < 0){
        perror("listen error");
        exit(1);
    }
    return sockfd;
}

int main(int argc, char *argv[]){
    int port = (argc > 1) ? atoi(argv[1]) : 7001;

    shm_id = shmget(SHM_KEY, sizeof(SharedClients), PERM | IPC_CREAT);
    if(shm_id < 0){
        perror("shmget error");
        exit(1);
    }

    //shmat: void *shmat(int shm_id, const void *shm_addr, int shmflg); 
    //shm_addr is NULL which means let the system auto select
    shmClients = (SharedClients *) shmat(shm_id, NULL, 0);
    if(shmClients == (void*) -1){
        perror("shmat error");
        exit(1);
    }
    for(int i = 0; i < MAX_CLIENTS; i++){
        shmClients->clients[i].used = 0;
        memset(shmClients->clients[i].ip, 0, MAX_IP_LEN);
        memset(shmClients->clients[i].name, 0, MAX_NAME_LEN);
        shmClients->clients[i].port = 0;
        shmClients->clients[i].pid = 0;
        memset(shmClients->clients[i].message, 0, sizeof(shmClients->clients[i].message));
    }

    mkdir(FIFO_DIR.c_str(), 0777);

    int msock = passiveTCP(port);
    cout<<"[Port]: "<< port << endl;

    signal(SIGCHLD, SIG_IGN);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGUSR2, sigusr2_handler);

    signal(SIGINT,  cleanup);
    signal(SIGQUIT, cleanup);
    signal(SIGTERM, cleanup);

    string welcomeMsg = 
    "****************************************\n"
    "** Welcome to the information server. **\n"
    "****************************************\n";

    while(true){
        struct sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);
        int csock = accept(msock, (struct sockaddr *)&cli_addr, &clilen);
        if(csock < 0){
            perror("accept error");
            continue;
        }
        cout << "New Connection from " << inet_ntoa(cli_addr.sin_addr)
             << ":" << ntohs(cli_addr.sin_port) << endl;
        pid_t pid = fork();
        if(pid < 0){
            perror("fork error");
            continue;
        } else if(pid == 0){
            close(msock);

            Client client;
            client.sockfd = csock;
            client.ip = inet_ntoa(cli_addr.sin_addr);
            client.port = ntohs(cli_addr.sin_port);
            client.name = "(no name)";
            client.env["PATH"] = "bin:.";
            client.pid = getpid();
            client.numberedPipes.clear();

            //[debug] let server to monitor the message from cleint
            int saved_stdout = dup(STDOUT_FILENO);

            dup2(csock, STDIN_FILENO);
            dup2(csock, STDOUT_FILENO);
            dup2(csock, STDERR_FILENO);

            int cid = assignClientId(shmClients);
            if(cid < 0){
                string err = "Too many users. Connection refused.\n";
                write(csock, err.c_str(), err.size());
                close(csock);
                exit(1);
            }
            client.id = cid;
            updateSharedClient(shmClients, client);

            g_myClientId = client.id;

            write(csock, welcomeMsg.c_str(), welcomeMsg.size());

            string loginMsg = "*** User '" + client.name + "' entered from " +
                              client.ip + ":" + to_string(client.port) + ". ***\n";
            broadcastMessage(loginMsg);

            write(csock, "% ", 2);

            char buf[MAX_LINE_LENGTH];
            while(true) {
                memset(buf, 0, sizeof(buf));
                int n = read(csock, buf, sizeof(buf)-1);
                if(n < 0) {
                    if(errno == EINTR) continue;
                    break; //other error, be considered as client disconnect
                } else if(n == 0) {
                    break;
                } else {
                    string input(buf);
                    input.erase(input.find_last_not_of("\r\n") + 1);

                    //[debug] let server to monitor the message from cleint
                    string DebungOnServer = "ID " + to_string(cid) + ": " + input + "\n"; 
                    write(saved_stdout, DebungOnServer.c_str(), DebungOnServer.size());
                    //------
                    executeCommandLine(csock, &client, input, shmClients);
                }
            }
            close(csock);
            removeSharedClient(shmClients, client.id);
            string logoutMsg = "*** User '" + client.name + "' left. ***\n";
            broadcastMessage(logoutMsg);
            exit(0);
        } else{
            close(csock);
        }
    }
    // shmdt(shmClients);
    // shmctl(shm_id, IPC_RMID, NULL);
    return 0;
}
