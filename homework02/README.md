````markdown
# Homework 2: Pipe Flow

## Introduction

In this homework, you will implement a custom process chaining system similar to Linux shell pipelines. You will create a program named `flow.c` that interprets a `.flow` file, describing processes and how their outputs and inputs are connected.

You can complete this homework on Linux, WSL (Windows Subsystem for Linux), or Mac. Standard libraries are allowed. Working with a partner is also permitted.

---

## Examples in Bash

### Simple Pipe

```bash
ls | wc
````

Counts the number of files, words, and characters in the current directory.

### Output Redirection

```bash
ls > result.txt
wc < result.txt
```

Writes the output of `ls` to a file and reads it with `wc`.

### Error Redirection

```bash
mkdir a | wc      # Only stdout is piped
mkdir a |& wc     # Both stdout and stderr are piped
mkdir a 2>&1 | wc # Alternative way to redirect stderr to stdout
```

### Complex Example

```bash
echo "f o o" > foo.txt
(cat foo.txt; cat foo.txt | sed 's/o/u/g') | wc
```

Combines multiple commands and pipes their outputs sequentially.

---

## .flow Language Specification

### Core Components

* **node**: Represents a process.

  ```
  node=<name>
  command=<command_string>
  ```

* **pipe**: Connects output of one component to input of another.

  ```
  pipe=<name>
  from=<source_name>
  to=<destination_name>
  ```

* **concatenate**: Executes multiple components sequentially.

  ```
  concatenate=<name>
  parts=<number_of_parts>
  part_0=<component_name>
  part_1=<component_name>
  ...
  ```

* **stderr**: Captures standard error from a node.

  ```
  stderr=<name>
  from=<node_name>
  ```

### Extra Credit Components

* **file**: Specifies a file as input or output.

  ```
  file=<name>
  name=<filename>
  ```

---

## Example `.flow` Files

### Simple Pipe (`filecount.flow`)

```text
node=list_files
command=ls

node=word_count
command=wc

pipe=doit
from=list_files
to=word_count
```

Run with:

```bash
./flow filecount.flow doit
```

### Complex Example (`complicated.flow`)

```text
node=cat_foo
command=cat foo.txt

node=sed_o_u
command=sed 's/o/u/g'

pipe=foo_to_fuu
from=cat_foo
to=sed_o_u

concatenate=foo_then_fuu
parts=2
part_0=cat_foo
part_1=foo_to_fuu

node=word_count
command=wc

pipe=shenanigan
from=foo_then_fuu
to=word_count
```

Run with:

```bash
./flow complicated.flow shenanigan
```

### Error Handling Example

```text
node=mkdir_attempt
command=mkdir a

node=word_count
command=wc

stderr=stdout_to_stderr_for_mkdir
from=mkdir_attempt

pipe=catch_errors
from=stdout_to_stderr_for_mkdir
to=word_count
```

### File Handling Example (Extra Credit)

```text
node=read_file
command=cat

file=input_file
name=result.txt

node=word_count
command=wc

pipe=read_pipe
from=input_file
to=read_file

pipe=process_pipe
from=read_pipe
to=word_count
```

---

## Instructions

1. Write a program `flow.c` that reads a `.flow` file.
2. Execute the commands defined in the file, connecting their inputs and outputs according to the flow graph.
3. The program should take two arguments: the flow file and the name of the final action to execute.

Example:

```bash
./flow filecount.flow doit
```

---

## FAQ

* **Do you need to implement all redirections?**
  No. All redirections can be handled using pipes.

* **How to do `ls > foo.txt`?**
  Use `tee`:

  ```text
  node=list_dir
  command=ls

  node=tee_to_foo
  command=tee foo.txt

  pipe=ls_to_foo
  from=list_dir
  to=tee_to_foo
  ```

* **How to do input redirection (`wc < foo.txt`)?**
  Equivalent flow:

  ```text
  node=cat_foo
  command=cat foo.txt

  node=word_count
  command=wc

  pipe=cat_to_wc
  from=cat_foo
  to=word_count
  ```
