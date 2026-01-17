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
#define MAXCMDLINE 512

extern char **environ;

typedef enum {UNDEF, FG, BG, STOPPED} job_state_t;
typedef enum reason {NONE, FINISHED, SIGNAL, FREEZED} reason_t;
typedef struct _states{
    volatile unsigned int is_edited : 1;
    volatile unsigned int reason : 2;
} states_t;
typedef struct _job {
    pid_t pid;
    size_t jid;
    volatile job_state_t state;
    states_t flags;
    char cmdline[MAXCMDLINE];
} job_t;

size_t jobs_count;

job_t jobs[MAXJOBS];

static void eval(char *cmdline);
static int builtin_command(char **argv);
static int parseline(char *buf, char **argv);
static void export(char **argv);
static char *extract_pwd(char *pwd);
static int add_job(pid_t pid, job_state_t state, char *cmdline);
static pid_t delete_job(job_t *job_to_del);
static job_t *get_job(pid_t pid, size_t jid);
static job_t *parse_arg(char *arg);
static void waitfg(pid_t pid);
static void reason_print(void);

void sigchld_handler(int sig) {
    int status;
    int old_errno = errno;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        job_t *curr_job = get_job(pid, 0);
        size_t curr_jid = curr_job->jid;
        if (WIFEXITED(status)) {
            curr_job->flags.is_edited = 1;
            curr_job->flags.reason = FINISHED;
        }
        else if (WIFSIGNALED(status)) {
            curr_job->flags.is_edited = 1;
            curr_job->flags.reason = SIGNAL;
        }
        else if (WIFSTOPPED(status)) {
            curr_job->flags.is_edited = 1;
            curr_job->flags.reason = FREEZED;
            curr_job->state = STOPPED;
        }
    }
}

int main() {

    char cmdline[MAXLINE];
    char prompt[1024];
    char *username = getenv("USER");
    char *hostname = getenv("HOSTNAME");
    char *cwd[1024];
    char *pwd;
    char homedir[2] = "~";
    char *input;
    
    for (int i = 1; i <= 16; i++) {
        jobs[i-1].jid = (size_t)i;
        jobs[i-1].state = UNDEF;
        jobs[i].flags.is_edited = 0;
        jobs[i].flags.reason = NONE;
    }

    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);

    using_history();

    while (1) {
        if ((pwd = extract_pwd(getenv("PWD"))) == NULL)
            pwd = homedir;
        
        reason_print();
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
    sigset_t mask, prev_mask;
    
    size_t pathlen = strlen(path);
    char path_copy[pathlen];

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

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
        
        sigprocmask(SIG_BLOCK, &mask, &prev_mask);
        if ((pid = fork()) == 0) {
            setpgid(0, 0);
            sigprocmask(SIG_UNBLOCK, &prev_mask, NULL);
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            if (execve(execute_dir, argv, environ) < 0) {
                printf("%s: Command not found.\n", argv[0]);
                exit(0);
            }
        }
        if (!bg) { 
            int status;
            tcsetpgrp(STDIN_FILENO, pid);
            add_job(pid, FG, cmdline);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            waitfg(pid);
            tcsetpgrp(STDIN_FILENO, getpgrp());
            reason_print();
        } else {
            add_job(pid, BG, cmdline);
            sigprocmask(SIG_UNBLOCK, &prev_mask, NULL);
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
            if (jobs[i].state != UNDEF) {
                if (jobs[i].state == STOPPED) state = "suspended";
                else state = "running";
                printf("[%ld] (%d) %s %s\n", jobs[i].jid, jobs[i].pid, state, jobs[i].cmdline);
            }
        }
        return 1;
    }
    if (!strcmp(argv[0], "fg")) {
        job_t *job = parse_arg(argv[1]);
        if (job == NULL) {
            fprintf(stderr, "fg: No such job\n");
            return 1;
        }
        
        job->state = FG;
        tcsetpgrp(STDIN_FILENO, job->pid);
        kill(job->pid, SIGCONT);
        waitfg(job->pid);
        if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0) {
            perror("tcsetpgrp failed");
        }
        reason_print();   
        return 1;
    }
    if (!strcmp(argv[0], "bg")) {
        job_t *job = parse_arg(argv[1]);
        if (job == NULL) return 1;

        job->state = BG;
        kill(job->pid, SIGCONT);
        return 1;
    }
    if (!strcmp(argv[0], "kill")) {
        job_t *job = parse_arg(argv[1]);
        if (job == NULL) { return 1; }
        
        kill(job->pid, SIGINT);
        return 1;
    }
    return 0;
}

void waitfg(pid_t pid) {
    sigset_t empty_mask;
    job_t *curr_job = get_job(pid, 0);

    sigemptyset(&empty_mask);

    while (curr_job != NULL && curr_job->state == FG && curr_job->flags.is_edited == 0) {
        sigsuspend(&empty_mask);
    }
}

static job_t *parse_arg(char *arg) {
    if (arg == NULL) return NULL;

    if (arg[0] == '%') {
        int jid = atoi(&arg[1]);
        return get_job(-1, jid);
    } else {
        pid_t pid = atoi(arg);
        return get_job(pid, 0);
    }
}

static job_t *get_job(pid_t pid, size_t jid) {
    if ((pid != -1) && (jid == 0)) {
        for (int i = 0; i < 16; i++) {
            if (jobs[i].pid == pid)
                return (jobs+i);
        }
    } else if ((pid == -1) && (jid != 0)) {
        for (int i = 0; i < 16; i++) {
            if (jobs[i].jid == jid)
                return (jobs+i);
        }
    }
    return NULL;
}

static int add_job(pid_t pid, job_state_t state, char *cmdline) {
    for (int i = 0; i < 16; i++) {
        if (jobs[i].state == UNDEF) {
            job_t *curr_job = (jobs+i);
            curr_job->pid = pid;
            curr_job->state = state;
            strcpy(curr_job->cmdline, cmdline);
            curr_job->flags.is_edited = 0;
            curr_job->flags.is_edited = NONE;
            return 0;
        }      
    }
    return -1;
}

static pid_t delete_job(job_t *job_to_del) {

    if (job_to_del == NULL) {
        fprintf(stderr, "ERROR: No such job to delete found");
        return -1;
    }

    job_to_del->pid = -1;
    job_to_del->state = UNDEF;
    memset(job_to_del->cmdline, 0, MAXCMDLINE);
    job_to_del->flags.is_edited = 0;
    job_to_del->flags.is_edited = NONE;
    return 0;
}

static void reason_print() {
    sigset_t mask_chld;

    sigemptyset(&mask_chld);
    sigaddset(&mask_chld, SIGCHLD);

    sigprocmask(SIG_BLOCK, &mask_chld, NULL);
    for (int i = 0; i < 16; i++) {
        if (jobs[i].state != UNDEF) {
            if (jobs[i].flags.is_edited == 1) {
                reason_t stop_reason = jobs[i].flags.reason;
                size_t curr_jid = jobs[i].jid;
                pid_t curr_pid = jobs[i].pid;
                if (stop_reason == FINISHED) {
                    printf("Job [%ld] (%d) is finished\n", curr_jid, curr_pid);
                    delete_job(jobs+i);
                }
                else if (stop_reason == SIGNAL) {
                    printf("Job [%ld] (%d) terminated by signal\n", curr_jid, curr_pid);
                    delete_job(jobs+i);
                }
                else if (stop_reason == FREEZED) {
                    printf("Job [%ld] (%d) stopped by signal\n", curr_jid, curr_pid);
                }
                jobs[i].flags.is_edited = 0;
                jobs[i].flags.reason = NONE;
            }
        }
    }
    sigprocmask(SIG_UNBLOCK, &mask_chld, NULL);
}