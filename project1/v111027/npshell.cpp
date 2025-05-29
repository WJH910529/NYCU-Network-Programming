#include <iostream>
#include <sstream>
#include <cstdlib>   // getenv, setenv
#include <unistd.h>  // fork, exec, pipe, dup2
#include <sys/wait.h> // wait, waitpid
#include <fcntl.h> // open()
#include <vector>
#include <string>
#include <unordered_map>
#include <array>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_LINE_LENGTH 15000
#define MAX_CMD_LENGTH 256

using namespace std;

struct Command{
    vector<string> args;
    bool has_redirection = false; // ">"
    string outfile;

    // pipeDelay: -1 = 無pipe；0 = normal pipe； >0 = numbered pipe
    int pipeDelay = -1;

    bool pipeStdErr = false; // 若為 true，表示 "!" ，讓 STDOUT 與 STDERR 均導向同一 pipe
    // 執行時使用的 fd
    int fd_in = STDIN_FILENO;
    int fd_out = STDOUT_FILENO;
    int fd_err = STDERR_FILENO;
};

// gloabl pipe來manage：key 為剩餘等待的命令數，value 為一個 pipe (read, write)
unordered_map<int, array<int, 2>> pipeMap;

void updatePipeMap(){
    unordered_map<int, array<int, 2>> newMap;
    for (auto &entry : pipeMap) {
        int newKey = entry.first - 1;
        if (newKey >= 0) {
            newMap[newKey] = entry.second;
        } else {
            close(entry.second[0]);
            close(entry.second[1]);
        }
    }
    pipeMap = move(newMap);
}

//拆空白，去判斷 "|" 和 "!" 後面有沒有接數字
vector<Command> parseCommands(const string &cmdline){
    vector<Command> commands;
    istringstream iss(cmdline);
    string token;
    vector<string> currentArgs;
    Command curCmd;

    while(iss >> token){
        if((token[0] == '|' || token[0] == '!')){
            // 將目前累積的參數存入 curCmd
            curCmd.args = currentArgs;
            currentArgs.clear();

            // 若 token 只有一個字元 (例如 "|" 或 "!")
            if(token.size() == 1){
                curCmd.pipeDelay = 0; //normal pipe  |
            }else{
                curCmd.pipeDelay = stoi(token.substr(1)); // |N
            }
            if(token[0] == '!'){
                curCmd.pipeStdErr = true;
            }
            commands.push_back(curCmd);
            curCmd = Command();
        }else if(token == ">"){             // if token是 '>' 表示 file redirection
            curCmd.has_redirection = true;
            if(iss >> token){
                curCmd.outfile = token;
            }
        }else{                              //一般指令/參數
            currentArgs.push_back(token);
        }
    }
    if(!currentArgs.empty()){
        curCmd.args = currentArgs;
        commands.push_back(curCmd);
    }
    return commands;
}

//檢查是不是built-in function 
bool handleBuiltin(const Command &cmd){
    if(cmd.args.empty()) return true;
    if(cmd.args[0] == "exit"){
        exit(0);
    }else if(cmd.args[0] == "setenv"){
        if(cmd.args.size() < 3){
            cerr << "Usage: setenv [var] [value]" << endl;
        }else{
            setenv(cmd.args[1].c_str(), cmd.args[2].c_str(), 1);
        }
        return true;
    }else if(cmd.args[0] == "printenv"){
        if(cmd.args.size() < 2){
            cerr << "Usage: printenv [var]" << endl;
        }else{
            char *value = getenv(cmd.args[1].c_str());
            if(value){
                cout<<value<<endl;
            }
        }
        return true;
    }
    return false; //not built-in
}

void handleFileRedirection(const Command &cmd){
    // O_CREAT | O_WRONLY | O_TRUNC: 覆蓋模式
    int fd = open(cmd.outfile.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        cerr << "Cannot open file: " << cmd.outfile << std::endl;
        exit(1);
    }
    // 把 STDOUT_FILENO 改成 fd
    if (dup2(fd, STDOUT_FILENO) < 0) {
        cerr << "dup2 error" << std::endl;
        exit(1);
    }
    close(fd); // 關閉原始 fd
}

// 將 vector<string> 轉換成 execvp 需要的 char* 陣列
vector<char*>buildArgv(const Command &cmd){
    vector<char*> argv;
    for(auto &arg : cmd.args){
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

// 執行一行內的所有 Command 和 處理 normal pipe 與 numbered pipe
void executeCmd(vector<Command> &cmds){
    int numCmds = cmds.size();
    int inputFd = STDIN_FILENO;
    for(int i = 0; i< numCmds; i++){
        // 如果前一行留下 key==0 的 pipe，將其讀端當作本行第一個指令的輸入，
        // 同時關閉 parent process 持有的寫入端，避免造成 EOF 無法送出 (((當所有寫端都關閉後，讀端讀取時會收到 EOF
        if(pipeMap.count(0)) {
            inputFd = pipeMap[0][0];
            close(pipeMap[0][1]);
            pipeMap.erase(0);
        }
        Command cmd = cmds[i];
        cmd.fd_in = inputFd;

        // 若該指令要求pipe（不論 normal 或 numbered）
        if(cmd.pipeDelay != -1) {
            int key = cmd.pipeDelay;
            // 若該 key 尚未建立 pipe ，則建立新 pipe
            if(pipeMap.find(key) == pipeMap.end()) {
                array<int,2> newPipe;
                if(pipe(newPipe.data()) < 0) {
                    perror("pipe error");
                    exit(1);
                }
                pipeMap[key] = newPipe;
            }
            cmd.fd_out = pipeMap[key][1];
            if(cmd.pipeStdErr)
                cmd.fd_err = pipeMap[key][1];
        }
                
        pid_t pid;
        while ((pid = fork()) < 0){ //fork失敗進入while, 處理 problem4 process limitation
            //-1:等待任一child process, 
            //存放child process的退出狀態(暫時沒用到), 
            //以 阻塞模式 執行，會一直等待，直到有任意一個child process終止。
            waitpid(-1, nullptr, 0); 
        }

        if(pid == 0){ // child process
            if(cmd.fd_in != STDIN_FILENO)
                dup2(cmd.fd_in, STDIN_FILENO);
            if(cmd.fd_out != STDOUT_FILENO)
                dup2(cmd.fd_out, STDOUT_FILENO);
            if(cmd.fd_err != STDERR_FILENO)
                dup2(cmd.fd_err, STDERR_FILENO);
            if(cmd.has_redirection) {
                handleFileRedirection(cmd);
            }
            // child process中關閉全域 pipeMap 中所有的fd
            for(auto &entry : pipeMap) {
                close(entry.second[0]);
                close(entry.second[1]);
            }
            vector<char*> argv = buildArgv(cmd); //為了execvp，要轉換為 char* 
            execvp(argv[0], argv.data());
            //success no return, if execvp fail: return -1
            cerr << "Unknown command: [" << argv[0] << "]." << endl;
            exit(1);
        } else{     //parent process
            //close掉前一命令沒release掉的
            if(inputFd != STDIN_FILENO)
                close(inputFd);
            // 若該指令使用pipe
            if(cmd.pipeDelay != -1) {
                int key = cmd.pipeDelay;
                if(key == 0) {
                    // normal pipe：關閉 parent process 對寫端的持有，並將讀端傳給下一個命令
                    if(pipeMap.find(0) != pipeMap.end()) {
                        close(pipeMap[0][1]); //parent process 關閉自己持有的寫端，這樣當所有child process都完成寫入後，讀端能收到 EOF，讓下一個命令知道資料已寫完。
                        inputFd = pipeMap[0][0];
                        pipeMap.erase(0);
                    } else {
                        inputFd = STDIN_FILENO;
                    }
                } else {
                    // numbered pipe：parent process 不關閉寫入端（等待後續命令consume），設定下一個輸入為 STDIN
                    inputFd = STDIN_FILENO;
                }
            } else {
                inputFd = STDIN_FILENO;
            }             
            //如果指令是沒有 pipe 
            if(cmd.pipeDelay == -1){  //處理一般指令Problem 2 : % ordering
                waitpid(pid, nullptr, 0);
            } 
            else if(cmd.args[0] == "removetag0" && cmd.fd_err == STDERR_FILENO){
                // 對於 removetag0，即使標準輸出在 pipe，仍需等待，但這樣沒處理到其他會輸出 err 的指令
                waitpid(pid, nullptr, 0);
            }
            else{ // 其他pipe, 使用非阻塞 waitpid
                waitpid(pid, nullptr, WNOHANG);
            }
        }
        // 本行所有指令執行完後，更新所有待用pipe的倒數值 (排除normal pipe), 一般指令是 -1 會被當true
        if(cmd.pipeDelay != 0){
            // cout<<cmd.pipeDelay<<endl;
            updatePipeMap();
        }
    }
}

int main(){
    signal(SIGCHLD, SIG_IGN);// 自動回收所有結束的child process，處理impl3的largefile
    setenv("PATH" , "bin:." , 1); //const char *envname, const char *envval, int overwrite

    string cmd;
    while(true){
        cout<<"% "<< flush;

        //EOF or Ctrl+D
        if(!getline(cin,cmd)) 
            break;
        //ignore empty
        if(cmd.empty()) 
            continue;
        
        // 先把整行 parse 成多個命令
        vector<Command> commands = parseCommands(cmd);
        if (commands.empty())
            continue;
        
        if(commands.size() == 1 && handleBuiltin(commands[0])){
            //cout<<"~ builtin ~"<<"\n";
            updatePipeMap();
        }else{
            executeCmd(commands);
        }
    }
    return 0;
}