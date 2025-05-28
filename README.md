"# NYCU-Network-Programming [NP project]"

## Homework 0: Context-Switch Analysis
Analyze a given C++ program that two processes run concurrently, reading and updating a sequence number in a file. For each of nine marked context-switch points, identify which lines have completed and which have not, and explain your reasoning based solely on the provided interleaved output.

## Project 1: NPShell

Project 1: NPShell

How to compile & exec
```
make           # builds `npshell`
make clean     # removes `npshell`
./npshell      # starts your custom shell
```

Implement a Unix-style shell named npshell in C/C++ supporting:
External commands via exec*

Ordinary pipes: cmd | cmd

Numbered pipes: cmd |N and cmd !N, forwarding stdout and/or stderr to the Nth next command

File redirection: cmd > file

Use only unnamed pipes, fork/exec, and wait/waitpid, handling large outputs and process limits correctly.