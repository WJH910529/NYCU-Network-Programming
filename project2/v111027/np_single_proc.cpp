#include "np_single_proc.h"
#define QLEN_LISTEN_BACKLOG 50

//Global Data Definitions
vector<Client*> clients;
map<pair<int,int>, array<int,2>> userPipes; // // (sender,reciver) (read,write)
fd_set afds;
int fdmax;

void broadcastMessage(const string &msg) {
    for (auto c : clients) {
        if (c->sockfd >= 0) {
            write(c->sockfd, msg.c_str(), msg.size());
        }
    }
}

void sendToClient(Client* client, const string &msg) {
    if(client->sockfd >= 0){
        write(client->sockfd, msg.c_str(), msg.size());
    }
}

Client* getClientById(int id) {
    for(auto c : clients){
        if(c->id == id){
            return c;
        }
    }
    return nullptr;
}

int assignClientId() {
    set<int> used;
    for(auto c : clients){
        used.insert(c->id);
    }
    for(int i = 1; i <= MAX_CLIENTS; i++){
        if (used.count(i) == 0){
            return i;
        }
    }
    return -1;
}

void closeAndRemoveClient(Client* dc) {
    //broadcast user leaving
    string logoutMsg = "*** User '" + dc->name + "' left. ***\n";
    broadcastMessage(logoutMsg);
    
    //[debug] disconnect message
    cout << "User " << dc->id << " (" << dc->name << ") "<< "IP: " << dc->ip << " has disconnect." << endl;

    // remove from select set
    FD_CLR(dc->sockfd, &afds);
    if(dc->sockfd == fdmax){
        while(fdmax >=0 && !FD_ISSET(fdmax, &afds)){
            fdmax--;
        }
    }
    close(dc->sockfd);
    dc->sockfd = -1;

    //remove all user-pipes related to this client
    vector<pair<int,int>> toRemove;
    for(auto &up : userPipes){
        if(up.first.first == dc->id || up.first.second == dc->id){
            close(up.second[0]);
            close(up.second[1]);
            toRemove.push_back(up.first);
        }
    }
    for(auto &key: toRemove){
        userPipes.erase(key);
    }

    //remove from clients vector
    for (auto it = clients.begin(); it != clients.end(); ++it){
        if(*it == dc){
            clients.erase(it);
            break;
        }
    }
    delete dc;
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

bool handleBuiltin(Client* client, const vector<string> &tokens) {
    if(tokens.empty())return true;
    string cmd = tokens[0];

    if(cmd == "exit"){
        closeAndRemoveClient(client);
        return true;
    }
    else if(cmd == "setenv"){
        if (tokens.size() < 3){
            sendToClient(client, "Usage: setenv [var] [value]\n");
        } else {
            client->env[tokens[1]] = tokens[2];
        }
        return true;
    }
    else if(cmd == "printenv"){
        if(tokens.size() < 2){
            sendToClient(client, "Usage: printenv [var]\n");
        } else {
            auto it = client->env.find(tokens[1]);
            if(it != client->env.end()){
                sendToClient(client, it->second + "\n");
            }
        }
        return true;
    }
    else if(cmd == "who"){
        vector<Client*> onlineClients;
        for(auto c : clients){
            if(c->sockfd >= 0){
                onlineClients.push_back(c);
            }
        }
        //ascending order
        sort(onlineClients.begin(), onlineClients.end(), [](const Client* a, const Client* b){
            return a->id < b->id;
        });

        //Show all user info
        string out = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
        for (auto c : onlineClients){
            out += to_string(c->id) + "\t" + c->name + "\t"
                + c->ip + ":" + to_string(c->port);
            if (c == client) out += "\t<-me";
            out += "\n";
        }
        sendToClient(client, out);
        return true;
    }
    else if(cmd == "tell"){
        if (tokens.size() < 3) return true;
        int targetId = stoi(tokens[1]);
        Client* target = getClientById(targetId);
        if(!target || target->sockfd < 0){
            sendToClient(client, 
                "*** Error: user #" + to_string(targetId) + " does not exist yet. ***\n");
        }
        else{
            //gather all the rest as message
            string msg;
            for(size_t i = 2; i < tokens.size(); i++){
                msg += tokens[i] + " ";
            }
            string fullMsg = "*** " + client->name + " told you ***: " + msg + "\n";
            sendToClient(target, fullMsg);
        }
        return true;
    }
    else if (cmd == "yell"){
        //gather all tokens after "yell"
        string msg;
        for(size_t i = 1; i < tokens.size(); i++){
            msg += tokens[i] + " ";
        }
        string fullMsg = "*** " + client->name + " yelled ***: " + msg + "\n";
        broadcastMessage(fullMsg);
        return true;
    }
    else if (cmd == "name"){
        if(tokens.size() < 2) return true;
        string newName = tokens[1];
        //check duplication
        for(auto c : clients){
            if (c->name == newName){
                sendToClient(client, "*** User '" + newName + "' already exists. ***\n");
                return true;
            }
        }
        // rename
        client->name = newName;
        string note = "*** User from " + client->ip + ":" + to_string(client->port) 
        + " is named '" + newName + "'. ***\n";
        broadcastMessage(note);
        return true;
    }

    //not a built-in command
    return false; 
}

void executeCommandLine(Client* client, const string &line) {
    // Quick check for an empty or whitespace line
    istringstream pre(line);
    string firstToken;
    pre >> firstToken;
    if(firstToken.empty()) {
        // user input 空行, 啥都不做 送回%
        sendToClient(client, "% ");
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
        handleBuiltin(client, tokens);
        updateNumberedPipes(client); //built in function also need to count one line, so need to update numberedpipe
        //if client still alive
        if(client->sockfd >= 0){
            sendToClient(client, "% ");
        }
        return;
    }

    //------Not a built-in. Parse for user pipes, umbered pipes, normal pipes, etc.
    vector<Command> cmds = parseCommandLine(line);
    if(cmds.empty()){
        sendToClient(client,"% ");
        return;
    }
    
    //check if there's a leftover numberd pipe(key==0) from previous commands
    int inputFd = STDIN_FILENO;
    // auto it = client->numberedPipes.find(0);
    // if(it != client->numberedPipes.end()){
    //     inputFd = it->second[0];
    //     close(it->second[1]);
    //     client->numberedPipes.erase(0);
    // }

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
            Client* sc = getClientById(srcId);
            if(!sc || sc->sockfd < 0){
                //user doesn't exist
                string err = "*** Error: user #" + to_string(srcId) + " does not exist yet. ***\n";
                sendToClient(client, err);
                int devnull = open("/dev/null", O_RDONLY);
                cmd.fd_in = devnull;
            }else{
                auto key = make_pair(srcId, client->id);
                if (userPipes.find(key) == userPipes.end()){
                    //pipe doesn't exist
                    string err = "*** Error: the pipe #" 
                               + to_string(srcId) + "->#" 
                               + to_string(client->id) 
                               + " does not exist yet. ***\n";
                    sendToClient(client, err);
                    int devnull = open("/dev/null", O_RDONLY);
                    cmd.fd_in = devnull;
                } else {
                    // pipe exists
                    cmd.fd_in = userPipes[key][0];
                    close(userPipes[key][1]);
                    // broadcast
                    string bmsg = "*** " + client->name + " (#" + to_string(client->id)
                                 + ") just received from " 
                                 + sc->name + " (#" + to_string(srcId) 
                                 + ") by '" + line + "' ***\n";
                    broadcastMessage(bmsg);
                    userPipes.erase(key);
                }
            }
        }

        //user pipe out
        if (cmd.userPipeOut){
            int dstId = cmd.userPipeOutTarget;
            Client* tc = getClientById(dstId);
            if(!tc || tc->sockfd < 0){
                string err = "*** Error: user #" + to_string(dstId) + " does not exist yet. ***\n";
                sendToClient(client, err);
                // int devnull = open("/dev/null", O_WRONLY);
                // cmd.fd_out = devnull;
                int devnull = open("/dev/null", O_RDWR );
                cmd.fd_out = devnull;
            } else {
                auto key = make_pair(client->id, dstId);
                if (userPipes.find(key) != userPipes.end()){
                    //pipe already exists
                    string err = "*** Error: the pipe #" + to_string(client->id)
                    + "->#" + to_string(dstId) + " already exists. ***\n";
                    sendToClient(client, err);
                    int devnull = open("/dev/null", O_WRONLY);
                    cmd.fd_out = devnull;
                } else{
                    //create new
                    array<int,2> newPipe;
                    if(pipe(newPipe.data()) < 0){
                        perror("pipe error");
                        exit(1);
                    }
                    
                    // int flags = fcntl(newPipe[0], F_GETFL, 0);
                    // fcntl(newPipe[0], F_SETFL, flags | O_NONBLOCK);
                    // flags = fcntl(newPipe[1], F_GETFL, 0);
                    // fcntl(newPipe[1], F_SETFL, flags | O_NONBLOCK);

                    userPipes[key] = newPipe;
                    cmd.fd_out = newPipe[1];
                    //broadcast
                    string bmsg = "*** " + client->name + " (#" + to_string(client->id)
                                    + ") just piped '" + line + "' to " 
                                    + tc->name + " (#" + to_string(dstId) + ") ***\n";
                    broadcastMessage(bmsg);
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
            }

            if (err_fd == STDERR_FILENO) {
                dup2(client->sockfd, STDERR_FILENO);
            } else {
                dup2(err_fd, STDERR_FILENO);
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
        sendToClient(client, "% ");
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
    ////cs.pdf use this function
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
    int port;
    if(argc > 1) {
        port = atoi(argv[1]);
    }else{
        port = 7001; //default
    }

    int msock = passiveTCP(port);
    cout<<"[Port]: "<< port << endl;

    signal(SIGCHLD,SIG_IGN);

    FD_ZERO(&afds);
    FD_SET(msock, &afds);
    fdmax = msock;

    string welcomeMsg = 
    "****************************************\n"
    "** Welcome to the information server. **\n"
    "****************************************\n";

    while(true){
        fd_set rfds = afds;
        int err,state;
        do{
            state = select(fdmax+1, &rfds, nullptr, nullptr, nullptr);
            if(state < 0) err = errno;
        }while((state < 0) && (err == EINTR)); // Interrupted system call. handle "Select May Failed problem."

        for(int fd=0; fd <= fdmax; fd++){
            if(FD_ISSET(fd, &rfds)){
                if(fd == msock){
                    // new connection
                    struct sockaddr_in cli_addr;
                    int addr_len = sizeof(cli_addr); //from-address length
                    int csock = accept(msock, (struct sockaddr *)&cli_addr, (socklen_t*)&addr_len);

                    //[debug] new connection
                    cout << "New Connection from " << inet_ntoa(cli_addr.sin_addr)<< ":" << ntohs(cli_addr.sin_port) << endl;
                    
                    if(csock < 0){
                        perror("accept error");
                        continue;
                    }
                    //new Client
                    Client* c = new Client();
                    c->sockfd = csock;
                    c->id = assignClientId();
                    c->ip = inet_ntoa(cli_addr.sin_addr);
                    c->port = ntohs(cli_addr.sin_port);
                    c->name = "(no name)";
                    c->env["PATH"] = "bin:.";

                    clients.push_back(c);

                    FD_SET(csock, &afds);
                    if(csock > fdmax) fdmax = csock;

                    //welcome + broadcast Login + prompt
                    sendToClient(c, welcomeMsg);
                    string longinMsg =
                    "*** User '" + c->name + "' entered from " + c->ip + ":" + to_string(c->port) + ". ***\n";
                    broadcastMessage(longinMsg);

                    sendToClient(c, "% ");
                }else{
                    //existing Client
                    char buf[MAX_LINE_LENGTH];
                    memset(buf, 0, sizeof(buf));
                    int n = read(fd, buf, sizeof(buf)-1);
                    if(n < 0){
                        if(errno == EINTR){
                            continue;
                        }
                        else{
                            // other error are treated as closing connection
                            Client* dc = nullptr;
                            for(auto cc : clients) {
                                if(cc->sockfd == fd) {
                                    dc = cc;
                                    break;
                                }
                            }
                            if(dc) {
                                closeAndRemoveClient(dc);
                            }
                            continue;     
                        }
                    }else if(n==0){
                        // cout <<"n should be 0 from read(fd,buf,size): " << n <<endl;
                        //client disconnected
                        Client* dc = nullptr;
                        for(auto cc : clients){
                            if(cc->sockfd == fd){
                                dc = cc;
                                break;
                            }
                        } 
                        if(dc){
                            closeAndRemoveClient(dc);
                        }
                    }else{
                        //[debug]
                        #ifdef DEBUG
                        cout <<"n=read(fd,buf,size): " << n <<endl;
                        #endif
                        
                        // read a Line .. parse
                        string input(buf);
                        //remove \r\n
                        input.erase(input.find_last_not_of("\r\n") + 1);
                        Client* cur = nullptr;
                        for(auto cc : clients){
                            if(cc->sockfd == fd){
                                cur = cc;
                                break;
                            }
                        }
                        if(cur){
                            //[debug] print id and command
                            cout << "ID " << cur->id << ": " << input << endl;
                            executeCommandLine(cur, input);
                        }
                    }
                }
            }
        }
    }
    return 0;
}


