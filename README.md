# Small Shell

Small Shell is a lightweight Unix-style command-line shell implemented in C. This project features command parsing, process creation and management, input/output redirection, background execution, and POSIX signal handling.

The shell provides an interactive command prompt and supports executing both built-in and external commands through child processes.

https://github.com/user-attachments/assets/61cc605b-958d-4146-b3ed-17d729d64256

## Features

* Command Execution: Execute external commands using `fork()` and `execvp()`, with support for command-line arguments.
* Built-in Commands: Use `cd`, `status`, and `exit` to manage the working directory, inspect process status, and terminate the shell.
* I/O Redirection: Redirect standard input and output using `<` and `>`, implemented via `open()`, and `dup2()`.
* Background Processes: Launch commands asynchronously using `&`. Consistently tracks background process IDs, and reports completion status without blocking the shell.
* Process Management: Use `waitpid()` to synchronize foreground execution, monitor background processes, and collect process exit statuses.
* POSIX Signal Handling: Configure `SIGINT` and `SIGTSTP` behavior using `sigaction()`, supporting "foreground-only mode" toggling and context-dependent signal handling.
* Input Handling: Parse command-line input and ignore blank lines and comments.
* Variable Expansion: Expand `$$` to the shell’s process ID within command input.
* Resource Management: Manage dynamically allocated command data and file descriptors throughout command execution and cleanup.

## Technical Highlights

### Process Creation and Execution

External commands run in child processes created with `fork()` and executed with `execvp()`. The parent shell distinguishes between foreground and background execution, waiting for foreground processes while allowing background processes to run concurrently with user interaction.

### Signal Handling

The shell uses `sigaction()` to implement multiple context-dependent POSIX siganl behaviors:

* The shell ignores `SIGINT`, allowing it to remain interactive when Ctrl+C is pressed.
* Foreground child processes use the default `SIGINT` behavior.
* The shell handles `SIGTSTP` to toggle foreground-only mode.
* Child processes ignore `SIGTSTP`.
* Signal handlers use `write()` for immediate terminal output, avoiding non-async-signal-safe functions such as `printf()` within the handler.

### Background Process Management

Background process IDs are maintained in a dedicated array. The shell periodically checks their status with `waitpid(..., WNOHANG)`, reports completed processes, and removes their IDs from the active process list.

### File Descriptor Redirection

The shell supports input and output redirection for external commands. It opens the specified files and uses `dup2()` to replace the child’s standard input or output. Background processes without explicit redirection have their corresponding standard streams redirected to `/dev/null`.

## Building and Running

Compile the source file with a C compiler on a POSIX-compatible system:

```bash
gcc -o smallsh smallsh.c
```

Run the shell:

```bash
./smallsh
```

Enter commands at the `:` prompt. Use `exit` to terminate the shell.

## Usage

### Built-in Commands

| Command | Description |
| :--- | :--- |
| cd | Change to the home directory. |
| cd <directory> | Change to the specified directory. |
| status | Display the exit status of the last foreground process. |
| exit | Terminate the shell and clean up tracked background processes. |

### External Commands

Run external programs with arguments as you would in a conventional Unix shell.

### Input and Output Redirection

Use < to redirect standard input and > to redirect standard output respectively.

### Background Execution

Append `&` to run a command in the background.

The shell returns to the prompt without waiting for the background process to finish and reports its process ID. Completion information is printed upon background process termination.

### Foreground-Only Mode

Press Ctrl+Z to toggle foreground-only mode. While this mode is enabled, commands execute in the foreground even if their input ends with `&`.

### Process ID Expansion

Use `$$` to insert the shell’s process ID into a command.
