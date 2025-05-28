
# NYCU-Network-Programming [NP project]
  2025 NYCU 113-2 網路程式設計 Network Programming

If it helps you, please click Star.🌟

## Homework 0: Context-Switch Analysis

Analyze a given C++ program that two processes run concurrently, reading and updating a sequence number in a file. For each of nine marked context-switch points, identify which lines have completed and which have not, and explain your reasoning based solely on the provided interleaved output.

  

## Project 1: NPShell

  

Project 1: NPShell


**How to compile & exec**

```bash

make  # builds `npshell`
./npshell  # starts your custom shell

```


Implement a Unix-style shell named npshell in C/C++ supporting:

- External commands via `exec*`

- Ordinary pipes: `cmd | cmd`

- Numbered pipes: `cmd |N `and `cmd !N`, forwarding stdout and/or stderr to the Nth next command

- File redirection: `cmd > file`

Use only unnamed pipes, `fork/exec`, and `wait/waitpid`, handling large outputs and process limits correctly.

## Project 2: RWG Server (Single-Process Concurrent)

**How to compile & exec**

```bash
make
./np_simple <port>       # single-client shell server
./np_single_proc <port>   # multi-user RWG server
```

Build two TCP servers in C/C++:

-   `np_simple`: single-client, connection-oriented shell server (access via telnet).
    
-   `np_single_proc`: multi-user “Remote Working Ground” chat system with:
    
    -   Login/logout broadcasts and welcome banner
        
    -   Per-user environment variables and prompts
        
    -   Built-ins: `who`, `name`, `tell`, `yell`
        
    -   User pipes: send stdout between users with `>n` and `<n`, including error handling and broadcast notifications
        

All I/O multiplexing must use `select` in a single process.

## Project 3: RWG Server (Multi-Process)

**How to compile & exec**

```bash
make
./np_multi_proc <port>
```

Re-implement Project 2 using a concurrent model:

-   Fork one process per client
    
-   Use FIFOs (named pipes) for user-to-user communication
    
-   Use shared memory to broadcast login/logout messages and maintain global state
    

The behavior and commands are identical to Project 2; only the IPC mechanisms differ.

## Project 4: HTTP Server & CGI

**How to compile & exec**

```bash
# Part 1 (Linux)
make part1
./http_server <port>     # listens on port

# Part 2 (Windows)
make part2
# Build with MinGW to produce cgi_server.exe
```

Create a remote-batch system using Boost.Asio in C++:

1.  **Part 1 (Linux)**
    
    -   `http_server`: asynchronous HTTP/1.1 server handling `GET /<cgi>.cgi?...`, sets CGI environment variables, and spawns the corresponding `.cgi` program
        
    -   `console.cgi`: reads up to five remote shell hosts/ports/files from `QUERY_STRING`, connects via Boost.Asio, drives the NP Project 2 shell by sending a line at each `%` prompt, and streams I/O back to the browser
        
2.  **Part 2 (Windows)**
    
    -   `cgi_server.exe`: merges the server and CGI applications into a single process, delivering the same HTML form and interactive console under MinGW on Windows

## Project 5: SOCKS4 Server & CGI Proxy

**How to compile & exec**

```bash
make # it will generate socks_server pj5.cgi
./socks_server <port>     # starts SOCKS 4/4A proxy on port
# Deploy pj5.cgi under your HTTP server's CGI directory
```

Implement a SOCKS 4/4A proxy and related CGI front-ends in C/C++:

1.  **SOCKS 4 server**
    
    -   Support CONNECT (`CD=1`) and BIND (`CD=2`), including DNS resolution for SOCKS4A
        
    -   Enforce firewall rules from `socks.conf`
        
    -   Log each request with source/destination IPs and ports, command type, and accept/reject status
        
2.  **CGI Proxy Extension**
    
    -   `pj5.cgi`: modified `console.cgi` that reads a backend SOCKS host/port (`sh`, `sp`) from the query string and tunnels all shell connections through your SOCKS server
        
    -   `panel_socks.cgi`: HTML form to drive `pj5.cgi`
        
3.  **Firewall**

    -   Deny all traffic by default; allow only flows matching the CONNECT/BIND rules specified in socks.conf. The pattern *.*.*.* permits all connections.