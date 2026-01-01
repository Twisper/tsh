#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>

#define	MAXLINE 8192
#define MAXARGS 128
#define PATHDIRLEN 64
#define MAXDIRLEN 128
#define MAXJOBS 16

extern char **environ;

typedef enum {UNDEF, FG, BG, STOPPED} job_state_t;

struct job_t {
    pid_t pid;
    size_t jid;
    job_state_t state;
    char cmdline[MAXLINE];
};

size_t jobs_count;

struct job_t jobs[MAXJOBS];

static void eval(char *cmdline);
static int builtin_command(char **argv);
static int parseline(char *buf, char **argv);
static void export(char **argv);
static char *extract_pwd(char *pwd);
static void add_job(pid_t pid, job_state_t state, char *cmdline);
static pid_t delete_job(pid_t pid, size_t jid);
static void edit_job_state(pid_t pid, size_t jid, job_state_t state);

int main() {

    char cmdline[MAXLINE];
    char prompt[1024];
    char *username = getenv("USER");
    char *hostname = getenv("HOSTNAME");
    char *cwd[1024];
    char *pwd;
    char homedir[2] = "~";
    char *input;

    using_history();

    while (1) {
        if ((pwd = extract_pwd(getenv("PWD"))) == NULL)
            pwd = homedir;
        snprintf(prompt, sizeof(prompt), "%s@%s: %s %% ", username, hostname, pwd);
        input = readline(prompt);

        if (!input) break;

        if (*input) add_history(input);

        eval(input);

        free(input);
    }
}

static char *extract_pwd(char *pwd) {
    char *ret_ptr = pwd;
    char *home = getenv("HOME");
    int i = strlen(pwd);

    if (!strcmp(pwd, home))
        return NULL;

    while ((ret_ptr[--i] != '/') && (i > 0))
        ;

    return ret_ptr + i;
}

static void eval(char *cmdline) {

    char *argv[MAXARGS];
    char buf[MAXLINE];
    int bg;
    pid_t pid;
    char *path = getenv("PATH");
    char *pathdir;
    char *execute_dir = NULL;
    char dir_with_path[MAXDIRLEN];
    
    size_t pathlen = strlen(path);
    char path_copy[pathlen];

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL)
        return;

    if (!builtin_command(argv)) {
        if (jobs_count == 16) {
            fprintf(stderr, "ERROR: Сouldn't launch more jobs, wait for current jobs to end.\n");
            return;
        }
        if (strchr(argv[0], '/') == NULL) {
            strcpy(path_copy, path);
            pathdir = strtok(path_copy, ":");
            while (pathdir != NULL){
                snprintf(dir_with_path, sizeof(dir_with_path), "%s/%s", pathdir, argv[0]);
                if (access(dir_with_path, X_OK) == 0) {
                    execute_dir = dir_with_path;
                    break;
                }
                pathdir = strtok(NULL, ":");
            }
        } else
            execute_dir = argv[0];
        if ((pid = fork()) == 0) {
            setpgid(0, 0);
            if (execve(execute_dir, argv, environ) < 0) {
                printf("%s: Command not found.\n", argv[0]);
                exit(0);
            }
        }
        tcsetpgrp(STDIN_FILENO, pid);
        if (!bg) {
            int status;
            //add_job(pid, FG, cmdline);
            waitpid(pid, &status, WUNTRACED);
            //if (WIFEXITED(status)) 
                //delete_job(pid, NULL);
            //else if (WIFSTOPPED(status))
                //edit_job_state(pid, NULL, STOPPED);
        } else {
            //add_job(pid, BG, cmdline);
        }
    }
    return;
}

static int parseline(char *buf, char **argv) {

    char *delim;
    int argc;
    int bg;

    while (*buf && (*buf == ' '))
        buf++;

    argc = 0;
    while ((delim = strchr(buf, ' '))) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' '))
            buf++;
    }
    if (*buf != '\0') {
        argv[argc++] = buf;
    }
    argv[argc] = NULL;

    if (argc == 0) return 1;

    if ((bg = (*argv[argc-1] == '&')) != 0)
        argv[--argc] = NULL;

    return bg;
}

static int builtin_command(char **argv) {
    if (!strcmp(argv[0], "exit"))
        exit(0);
    if (!strcmp(argv[0], "&"))
        return 1;
    if (!strcmp(argv[0], "cd")) {
        char cwd[1024];
        if (argv[1] == NULL) {
            char *home = getenv("HOME");
            chdir(home);
            setenv("PWD", home, 1);
            return 1;
        }
        if (chdir(argv[1]) != 0)
            fprintf(stderr, "ERROR: directory not found.\n");
        else {
            getcwd(cwd, sizeof(cwd));
            setenv("PWD", cwd, 1);
        }
        return 1;
    }
    if (!strcmp(argv[0], "pwd")) {
        printf("%s\n", getenv("PWD"));
        return 1;
    }
    if (!strcmp(argv[0], "export")) {
        char *envvar = strtok(argv[1], "=");
        char *newval = strtok(NULL, "=");
        if ((envvar != NULL) && (newval != NULL)) {
            if (setenv(envvar, newval, 1) != 0) {
                fprintf(stderr, "ERROR: couldn't set %s variable", envvar);
            }
        }
        return 1;
    }
    if (!strcmp(argv[0], "history")) {
        HIST_ENTRY **hist = history_list();
        if (hist) {
            printf("History of your commands:\n");
            for (int i = 0; hist[i]; i++) {
                printf("%d: %s\n", i, hist[i]->line);
            }
        }
        return 1;
    }
    if (!strcmp(argv[0], "unset")) {
        unsetenv(argv[1]);
        return 1;
    }
    if (!strcmp(argv[0], "jobs")) {
        char *state;
        for (int i = 0; i < 16; i++) {
            if (jobs[i].state == STOPPED) state = "Suspended";
            else state = "Running";
            printf("[%ld] %d %s %s", jobs[i].jid, jobs[i].pid, state, jobs[i].cmdline);
        }
        return 1;
    }
    if (!strcmp(argv[0], "fg")) {
        
        return 1;
    }
    if (!strcmp(argv[0], "bg")) {
        
        return 1;
    }
    if (!strcmp(argv[0], "kill")) {
        //pid_t pid = delete_job(NULL, atoi(argv[1]));
        //kill(pid, SIGINT);
        return 1;
    }
    return 0;
}