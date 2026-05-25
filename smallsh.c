// need to expose additional POSIX features to use sigaction
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#define MAX_LINE_LENGTH 2048
#define MAX_ARGS 512

// set the maximum number of background pids we can store at once to 100 -
//  this should be plenty for this purpose, but could be expanded to 
//  support an arbitrary number by using a linked list
#define MAX_BACKGROUND_PROCESSES 100

// maintain global variables to hold global status information that is used by 
//  the built in 'status' command
int last_exit_status = 0; // exit status of last foreground process
int last_term_signal = 0; // terminating signal of last foreground process
int terminated_by_signal = 0; // bool - was the last foreground process terminated by a signal?

// maintain a global background pids array that we use to check the status of 
//  all currently running background processes once per shell loop cycle
pid_t background_pids[MAX_BACKGROUND_PROCESSES];
int num_background_processes = 0;

// boolean variable corresponding to whether or not we are in foreground only
//  mode - modified by SIGTSTP handler in the shell process
volatile sig_atomic_t foreground_only_mode = 0;
// boolean variable corresponding to whether or not we have a pending
//  mode change message to print when appropriate
volatile sig_atomic_t pending_message = 0;
// boolean variable corresponding to whether or not a foreground process is
//  currently running (used to decide SIGTSTP signal handling behavior)
volatile sig_atomic_t fg_process_running = 0;

struct command {
    char* args[MAX_ARGS + 1]; // holds up to the max number of args and a null terminator
    int argc; // number of arguments
    char* input_file;
    char* output_file;
    int background; // 0 - foreground, 1 - background
};

// free memory allocated in the command struct
void free_command(struct command* command)
{
    // free all arg strings
    for (int i = 0; i < command->argc; i++)
    {
        free(command->args[i]);
    }
    // free input file string if it exists
    if (command->input_file != NULL)
    {
        free(command->input_file);
    }
    // free output file string if it exists
    if (command->output_file != NULL)
    {
        free(command->output_file);
    }
}

void print_prompt()
{
    printf(": ");
    fflush(stdout);
}

char* get_input()
{
    while (1)
    {
        // getline vars
        char* line = NULL;
        size_t buffer_size = 0;
        ssize_t length = 0;

        length = getline(&line, &buffer_size, stdin);

        // EOF or getline error
        if (length == -1)
        {
            free(line);
            exit(0);
        }

        // replace trailing newline character with null byte
        if (line[length - 1] == '\n')
        {
            line[length - 1] = '\0';
            length--;
        }

        // check that the input line does not exceed the max number of chars.
        //  If it does, discard line, print message and reprompt
        if (length > MAX_LINE_LENGTH)
        {
            printf("Input exceeds %d characters, discarding.\n: ", MAX_LINE_LENGTH);
            fflush(stdout);

            free(line);
            line = NULL;
            length = 0;

            continue;
        }

        return line;
    }
}

// check if the input line is blank
int is_blank_line(char* line)
{
    return line[0] == '\0';
}

// check if the input line is a comment
int is_comment(char* line)
{
    return line[0] == '#';
}

// expand instances of $$ to into the PID of smallsh
char* expand_pid(char* line)
{
    pid_t pid = getpid();

    char pid_str[20]; // allocate 20 bytes to hold pid (can hold up 19 digits
                      // + null byte to support 64-bit integer (may be overkill))
    sprintf(pid_str, "%d", pid); // populate pid_str with pid

    int pid_len = strlen(pid_str);
    int line_len = strlen(line);

    /* maximum length that the expanded string can be is when every two 
        characters become pid_len characters, we will allocate extra
        memory to ensure we do not overflow the buffer, and act as if every
        character in the input is replaced with the pid (this can be optimized)*/
    int max_length = pid_len * line_len + 1;

    char* expanded_line = malloc(max_length); // string to store result

    int i = 0; // input string index
    int j = 0; // output string index

    // loop through input line and expand instances of "$$"
    while (line[i] != '\0')
    {
        // found "$$", replace with pid
        if (line[i] == '$' && line[i+1] == '$')
        {
            // loop through pid and copy over to result string
            for (int k = 0; k < pid_len; k++)
            {
                expanded_line[j++] = pid_str[k];
            }

            i += 2;
        }
        // normal char, copy over
        else
        {
            expanded_line[j++] = line[i++];
        }
    }

    expanded_line[j] = '\0'; // add null terminator
    return expanded_line;
}

// initialize command structure to empty values
void init_command(struct command* cmd)
{
    // initialize argv pointers to all be null
    for (int i = 0; i < MAX_ARGS + 1; i++)
    {
        cmd->args[i] = NULL;
    }
    // no args yet;
    cmd->argc = 0;
    // no input/output defined yet
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    // run in the foreground by default
    cmd->background = 0;
}

// process the command string token by token, and store token in a token array
//  to be post-processed later
int tokenize_str(char* command_str, char *token_array[MAX_ARGS + 10])
{
    // track total num of tokens in the command
    int num_tokens = 0;

    // for use with strtok_r
    char* save_ptr;

    // process token by token (delimited by ' '), and add the corresponding
    //  string to the token array
    char* token = strtok_r(command_str, " ", &save_ptr);
    while (token != NULL)
    {
        // ensure we do not overflow the buffer, check for too many tokens 
        //  on an improperly formatted input
        if (num_tokens >= MAX_ARGS + 10)
        {
            printf("Too many tokens, maximum number of args is: %d\n", MAX_ARGS);
            fflush(stdout);
            break;
        }

        // copy token string over to token array
        token_array[num_tokens] = strdup(token);
        // increase token count by one and get next token
        num_tokens++;
        token = strtok_r(NULL, " ", &save_ptr);
    }

    // return total number of tokens
    return num_tokens;
}

void parse_command(char* command_str, struct command* command)
{
    // initialize an array to hold all tokens from the command string.
    //  Give this array size MAX_ARGS + 10 to ensure that there are no
    //  overflow problems. (In theory the total number of tokens is: 
    //  MAX_ARGS + 5, since we can have >, <, input_file, output_file, 
    //  and & that are not args, but we set the size to MAX_ARGS + 10 to 
    //  pad slightly, ensuring we avoid any edge-case errors)
    char* token_array[MAX_ARGS + 10];
    // populate token array with all of the " " delimitted tokens in command_str
    //  and store the number of tokens
    int num_tokens = tokenize_str(command_str, token_array);

    // loop through all tokens and classify what type of token it is
    for (int i = 0; i < num_tokens; i++)
    {
        // if we see the "<" character, copy the next token to the 
        //  input file variable
        if (strcmp(token_array[i], "<") == 0 && i < num_tokens - 1)
        {
            command->input_file = strdup(token_array[i+1]);
            // skip over next token (filename)
            i++;
        }
        // if we see the ">" character, copy the next token to the
        //  output file variable
        else if (strcmp(token_array[i], ">") == 0 && i < num_tokens - 1)
        {
            command->output_file = strdup(token_array[i+1]);
            // skip over next token (filename)
            i++;
        }
        // if we see the "&" character as the last token, set the background
        //  flag to true
        else if (strcmp(token_array[i], "&") == 0 && i == num_tokens - 1)
        {
            // only set background to true if foreground only mode is off
            if (!foreground_only_mode)
            {
                command->background = 1;
            }
        }
        // otherwise we are looking at a normal argument, add it to args array
        //  and increment argc
        else
        {
            // ensure we do not exceed the maximum number of args
            if (command->argc >= MAX_ARGS)
            {
                printf("Too many arguments passed (max: %d)\n", MAX_ARGS);
                fflush(stdout);
                break;
            }

            command->args[command->argc++] = strdup(token_array[i]);
        }
    }

    // ensure a null byte follows all arguments in args array
    command->args[command->argc] = NULL;

    // free memory allocated in token array
    for (int i = 0; i < num_tokens; i++)
    {
        free(token_array[i]);
    }
}

// check if command is a built in command
int is_built_in(struct command* command)
{
    // check if the first argument (command name) is one of the 
    //  built in commands
    return (strcmp(command->args[0], "exit") == 0 ||
            strcmp(command->args[0], "cd") == 0 ||
            strcmp(command->args[0], "status") == 0);
}

// do status command
void execute_status()
{
    // if last foreground process was terminated by a signal, print which
    //  signal it was
    if (terminated_by_signal)
    {
        printf("terminated by signal %d\n", last_term_signal);
    }
    // otherwise, print exit status
    else
    {
        printf("exit value %d\n", last_exit_status);
    }
    fflush(stdout);
}

// do cd command
void execute_cd(struct command* command)
{
    // no arguments supplied, go to directory in HOME env variable
    if (command->argc == 1)
    {
        // load HOME env variable 
        char* home = getenv("HOME");

        // change directory to home, and print an error if one occurs
        if (chdir(home) != 0)
        {
            perror("cd");
        }
    }
    // path supplied, change directory to that path
    else
    {
        // attempt to change directory to the path specified in the first
        //  argument, and print an error if one occurs
        if (chdir(command->args[1]) != 0)
        {
            perror("cd");
        }
    }
}

// do exit command - exit cleanly by killing all running processes
void execute_exit(struct command* command)
{
    // terminate all running background processes
    for (int i = 0; i < num_background_processes; i++)
    {
        // terminate each background process gracefully
        kill(background_pids[i], SIGTERM);
    }
    // reap all terminated processes to avoid zombie processes
    for (int i = 0; i < num_background_processes; i++)
    {
        // cleanup pid
        waitpid(background_pids[i], NULL, 0);
    }

    // cleanup memory allocated in command
    free_command(command);

    exit(0);
}

// given a built in command, decide which it is, then execute
void execute_built_in_command(struct command* command)
{
    // command is status
    if (strcmp(command->args[0], "status") == 0)
    {
        execute_status();
    }
    // command is cd
    else if (strcmp(command->args[0], "cd") == 0)
    {
        execute_cd(command);
    }
    // only built in command left is exit
    else
    {
        execute_exit(command);
    }
}

// if applicable, redirect stdin to the file specified in command's
//  input_file field
void redirect_input(struct command* command)
{
    if (command->input_file != NULL)
    {
        // open input file for reading
        int source_fd = open(command->input_file, O_RDONLY);
        
        // failed to open input file, print error and exit
        if (source_fd < 0)
        {
            printf("cannot open %s for input\n", command->input_file);
            fflush(stdout);
            free_command(command);
            exit(1);
        }

        // attempt to redirect stdin (fd 0) to the input file file 
        //  descriptor (source_fd), print an error and exit on failure
        if (dup2(source_fd, 0) < 0)
        {
            perror("dup2");
            free_command(command);
            exit(1);
        }

        // no longer need source_fd open
        close(source_fd);
    }
}

// if applicable, redirect stdout to the file specified in command's
//  input_file field
void redirect_output(struct command* command)
{
    if (command->output_file != NULL)
    {
        // open output file for writing (create it if it doesn't exist,
        // truncate if it does exist already, set permissions to
        // owner: read/write, group/others: read only)
        int destination_fd = open(command->output_file, 
            O_WRONLY | O_CREAT | O_TRUNC, 0644);

        // failed to open output file, print error and exit
        if (destination_fd < 0)
        {
            printf("cannot open %s for output\n", command->output_file);
            fflush(stdout);
            free_command(command);
            exit(1);
        }

        // attempt to redirect stdout (fd 1) to the output file file 
        //  descriptor (destination_fd), print an error and exit on failure
        if (dup2(destination_fd, 1) < 0)
        {
            perror("dup2");
            free_command(command);
            exit(1);
        }

        // no longer need destination_fd open
        close(destination_fd);
    }
}

// if either of command's input/output file fields are empty, redirect
//  background process' stdin and stdout to /dev/null
void redirect_backgroud_io(struct command* command)
{
    // redirect input to dev/null
    if (command->background && command->input_file == NULL)
    {
        // open dev/null for reading
        int dev_null_fd = open("/dev/null", O_RDONLY);
        // redirect stdin to dev/null
        dup2(dev_null_fd, 0);
        close(dev_null_fd);
    }

    // redirect output to dev/null
    if (command->background && command->output_file == NULL)
    {
        // open dev/null for writing
        int dev_null_fd = open("/dev/null", O_WRONLY);
        // redirect stdout to dev/null
        dup2(dev_null_fd, 1);
        close(dev_null_fd);
    }
}

// set up and run child process command
void do_child_process(struct command* command)
{
    // input redirection
    redirect_input(command);

    // output redirection
    redirect_output(command);

    // for background processes, if there is no input/output file
    //  provided, we want to redirect input/output to dev/null
    redirect_backgroud_io(command);

    // command->args[0] is the command name, replace current process with 
    //  the corresponding command to run
    execvp(command->args[0], command->args);

    // since execvp replaces the running process, this code will only get
    //  reached if execvp fails - print an error and exit the process
    perror(command->args[0]);
    free_command(command);
    exit(1);
}

// print message to the user that the foreground only mode has been 
//  turned on/off
void print_mode_change_message()
{
    // foreground only mode is on - tell user
    if (foreground_only_mode)
    {
        printf("\nEntering foreground-only mode (& is now ignored)\n");
    }
    // foreground only mode is off - tell user
    else
    {
        printf("\nExiting foreground-only mode\n");
    }
    fflush(stdout);
}

// run foreground process and update information about that process' status
//  upon completion
void do_foreground_process(pid_t pid)
{
    // variable to hold child process status
    int child_status;

    // set running fg process flag (used by SIGTSTP handler)
    fg_process_running = 1;

    // wait for child process to change state, and store child process'
    //  exit status
    waitpid(pid, &child_status, 0);

    // unset running fg process flag (process has finished)
    fg_process_running = 0;

    // if a message about changing foreground-only mode state should be 
    //  printed, do so (a SIGTSTP signal was sent while the foreground
    //  process was running)
    if (pending_message)
    {
        print_mode_change_message();
        // message no longer pending
        pending_message = 0;
    }

    // child process exited normally
    if (WIFEXITED(child_status))
    {
        // update last process exit status variable accordingly
        last_exit_status = WEXITSTATUS(child_status);
        // process was not terminated by signal
        terminated_by_signal = 0;
    }
    // child processed terminated from signal
    else if (WIFSIGNALED(child_status))
    {
        // update which signal terminated child processes
        last_term_signal = WTERMSIG(child_status);
        // process was terminated by signal
        terminated_by_signal = 1;

        // print message to user
        printf("terminated by signal %d\n", last_term_signal);
        fflush(stdout);
    }
}

// update information relating to currently running background process
void do_background_process(pid_t pid)
{
    // check that we are not going to overflow our background_pids array
    if (num_background_processes < MAX_BACKGROUND_PROCESSES)
    {
        // store pid in the background processes array and increment
        //  number of background processes
        background_pids[num_background_processes++] = pid;
    }
    // too many background processes are being submitted at once for 
    //  us to keep track of, do not store this pid and print an error
    //  message to the user
    else
    {
        printf("Too many background processes are running currently "
            "(more than %d) - the information for the most recent "
            "command will not be printed upon completion.\n", 
            MAX_BACKGROUND_PROCESSES);
        fflush(stdout);
    }

    // print pid message to user
    printf("background pid is %d\n", pid);
    fflush(stdout);
}

// if the child process is a foreground process, wait for it to finish
//  then process it. If the child process is a background process,
//  do not wait for it
void do_parent_process(struct command* command, pid_t pid)
{
    // foreground process - wait for child process to finish, then 
    //  update global status variables accordingly
    if (!command->background)
    {
        do_foreground_process(pid);
    }
    // background process - do not wait for child process to finish,
    //  we also do not need to update any status variables
    else 
    {
        do_background_process(pid);
    }
}

// set up the desired child process behavior for when it receives a SIGINT
//  signal (foreground -> default behavior, background -> ignore)
void configure_child_sigint_behavior(int background_process)
{
    // initialize empty sigaction struct for SIGNT (CTRL-C) signal
    struct sigaction SIGINT_action = {0};

    // foreground child process -> restore default SIGINT behavior
    //  (child process will terminate itself when receiving SIGINT signal)
    if (!background_process)
    {
        SIGINT_action.sa_handler = SIG_DFL;
    }
    // background child process -> still ignore SIGINT signals
    else
    {
        SIGINT_action.sa_handler = SIG_IGN;
    }

    SIGINT_action.sa_flags = 0; // no extra flags

    // tell the OS to use the behavior defined in SIGIN_action for SIGINT signals
    //  (IGN if background process, DFL if foreground process)
    sigaction(SIGINT, &SIGINT_action, NULL);
}

// set up the desired child process behavior for when it receives a SIGTSTP
//  signal (ignore SIGTSTP signals)
void configure_child_sigtstp_behavior()
{
    // initialize empty sigaction struct for SIGTSTP (CTRL-Z) signal
    struct sigaction SIGTSTP_action = {0};

    // child processes should ignore SIGTSTP (CTRL-Z) signals (no handler)
    SIGTSTP_action.sa_handler = SIG_IGN;
    SIGTSTP_action.sa_flags = 0; // no extra flags

    // tell the OS to use the behavior defined in SIGTSTP_action for SIGTSTP signals
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

// create a child process to run the external command in, set up proper signal
//  behavior, then run the command
void execute_external_command(struct command* command)
{
    // fork here to use child process to run command
    pid_t pid = fork();

    // error with fork, print and exit
    if (pid < 0)
    {
        perror("fork");
        exit(1);
    }
    // child process, redirect input/output, then execute external command 
    //  here with execvp
    else if (pid == 0)
    {
        // configure how we want child processes to behave when they 
        //  receive SIGINT / SIGTSTP signals
        configure_child_sigint_behavior(command->background);
        configure_child_sigtstp_behavior();

        do_child_process(command);
    }
    // parent process, handle foreground/background process functionality 
    else
    {
        do_parent_process(command, pid);
    }
}

// decide if the command is built in or external, then execute the 
//  appropriate command
void execute_command(struct command* command)
{
    // built in command
    if (is_built_in(command))
    {
        execute_built_in_command(command);
    }
    // external command
    else
    {
        execute_external_command(command);
    }
}

// if any background processes have completed, remove them from the
//  array and print a message to the user
void check_for_completed_background_proceses()
{
    int background_status;

    for (int i = 0; i < num_background_processes; i++)
    {
        // check if the background process has finished, but do not wait
        //  until it does. If the background process has finished, its exit
        //  status will be stored in background_status. If the process has 
        //  finished, result will be > 0
        pid_t result = waitpid(background_pids[i], &background_status, WNOHANG);

        // background process has terminated, handle properly
        if (result > 0)
        {
            // exited normally
            if (WIFEXITED(background_status))
            {
                // print message to user with exit value
                printf("background pid %d is done: exit value %d\n",
                    background_pids[i], WEXITSTATUS(background_status));
                fflush(stdout);
            }
            // terminated by signal
            else if (WIFSIGNALED(background_status))
            {
                // print message to user with signal number
                printf("background pid %d is done: terminated by signal %d\n",
                    background_pids[i], WTERMSIG(background_status));
                fflush(stdout);
            }

            // remove pid from running background processes array
            for (int j = i; j < num_background_processes - 1; j++)
            {
                // replace each pid with the one immediately following it
                background_pids[j] = background_pids[j+1];
            }
            // removed a pid, decrement the total number of processes
            num_background_processes--;

            // keep index aligned after shifting follwing pids back by one
            i--;
        }
    }
}

// shell loop -> loop infinitely and prompt the user to enter commands
//  until they exit the program
void run_shell_loop()
{
    while (1)
    {
        // check if any currently running background processes have completed
        //  before getting user input
        check_for_completed_background_proceses();

        // check if we need to print a message (from SIGTSTP signal), and
        //  print it if so
        if (pending_message)
        {
            print_mode_change_message();
            // message no longer pending
            pending_message = 0;
        }

        // print ": " to indicate command prompt
        print_prompt();

        // user's input
        char* input_line = get_input();

        // skip blank line/comment
        if (is_blank_line(input_line) || is_comment(input_line))
        {
            free(input_line);
            continue;
        }
        
        // expand all instances of "$$" to the current process' pid
        char* expanded_line = expand_pid(input_line);

        // allocate a command struct on the stack, and populate it properly
        //  based on the command string by parsing argument by argument
        struct command command;
        init_command(&command);
        parse_command(expanded_line, &command);

        // after parsing command, we no longer need the command string, free 
        //  the expanded and unexpanded lines
        free(expanded_line);
        free(input_line);

        // execute the command the user entered
        execute_command(&command);

        // free memory allocated in command struct
        free_command(&command);
    }
}

// SIGTSTP signal handler -> properly toggle foreground only mode, and print
//  messages to the user
void handle_sigtstp(int signal)
{
    // initialize message strings to be printed when entering/exiting
    //  foreground-only mode
    const char enter_msg[] = 
    "\nEntering foreground-only mode (& is now ignored)\n: ";
    const char exit_msg[] =
    "\nExiting foreground-only mode\n: ";

    // toggle foreground only mode
    foreground_only_mode = !foreground_only_mode;

    // if a foreground process is currently running, we do not want to print
    //  the message yet -> set pending message flag for printing later 
    //  (when fg process finishes)
    if (fg_process_running)
    {
        pending_message = 1;
    }
    // otherwise, we are sitting on a prompt -> print message to the user
    //  immediately
    else
    {
        // just entered foreground only mode
        if (foreground_only_mode)
        {
            // use write to print out the message instead of printf - write
            //  is async-signal safe, printf is not
            write(STDOUT_FILENO, enter_msg, sizeof(enter_msg) - 1);
        }
        // just left foreground only mode
        else
        {
            write(STDOUT_FILENO, exit_msg, sizeof(exit_msg) - 1);
        }
    }
}

// configure the behavior that we want the signals SIGINT and SIGTSTP to have
//  for the shell process (this behavior will be overwritten in child
//  processes)
void configure_signal_behavior()
{
    // initialize empty sigaction structs for SIGNT (CTRL-C) and SIGTSTP
    //  (CTRL-Z) signals
    struct sigaction SIGINT_action = {0};
    struct sigaction SIGTSTP_action = {0};

    // shell process should ignore SIGINT (CTRL-C) signals (no handler)
    SIGINT_action.sa_handler = SIG_IGN;
    SIGINT_action.sa_flags = 0; // no extra flags

    // tell the OS to use the behavior defined in SIGINT_action for SIGINT signals
    sigaction(SIGINT, &SIGINT_action, NULL);

    // shell responds to SIGTSTP signals with behavior defined in handle_SIGTSTP
    SIGTSTP_action.sa_handler = handle_sigtstp;
    // temporarily block all other incoming signals while handler runs
    sigfillset(&SIGTSTP_action.sa_mask);
    // set flag to restart interrupted syscalls after handler completes
    SIGTSTP_action.sa_flags = SA_RESTART;
    
    // tell the OS to use the behavior defined in SIGTSTP_action for SIGTSTP signals
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

// configure signal behavior, then begin the shell prompting loop, prompt 
//  infinitely until the user wants to exit
int main(int argc, char* argv[])
{
    // configure the behavior for SIGINT and SIGTSTP signals (for shell)
    configure_signal_behavior();
    run_shell_loop();
    return(0);
}
