# Homework 2 — Flow Execution Engine

## Overview
This program reads a `.flow` file and executes commands defined as nodes, pipes, concatenations, stderr links, and file components — simulating a mini shell pipeline system.

It uses standard Unix calls (`fork()`, `pipe()`, `dup2()`, `exec()`, `waitpid()`) instead of `system()` and matches shell-style output formatting.

---

## Design & Implementation
- Parses `.flow` files with components:
  - **node=** → defines a command to run  
  - **pipe=** → connects `from` → `to` (can be node or file)  
  - **concatenate=** → merges multiple parts sequentially  
  - **file=** → represents a file as source/destination  
  - **stderr=** → redirects errors from a node to another target  
- Ignores comments starting with `#` or `//`.  
- Detects cyclic dependencies and exits cleanly with an error message.  
- Provides clear error handling for missing files.  

---

## Build
```bash
gcc -Wall -Wextra -O2 -Wno-stringop-truncation -o flow flow.c

---

## Run Examples

# Basic test
./flow filecount.flow doit

# Combined pipe
./flow complicated.flow shenanigan

# File redirection
./flow your_tests.flow save_ls

Other .flow files such as stderr.flow and fileio.flow can be used for additional testing.

---

## Extra Credit
save_ls → Demonstrates file sink redirection (writes output to file).
stderr_pipeline → Shows error stream redirection.

---

## Assumptions
Input files follow assignment format exactly.
No use of system().
Works in any standard Linux environment (tested on Kali Linux).
Missing files trigger clear error messages.
Cyclic dependencies are detected and handled gracefully.
