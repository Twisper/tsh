#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <fcntl.h>

#define	MAXLINE 8192
#define MAXARGS 128
#define PATHDIRLEN 64
#define MAXDIRLEN 4096
#define MAXJOBS 16
#define MAXCMDLINE 512
#define MAXPIPESCOUNT 32

#ifdef DEBUG
#define LOG(fmt, ...) fprintf(stderr, "\n[LOG] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif

extern char **environ;

typedef enum {UNDEF, FG, BG, STOPPED} job_state_t;
typedef enum reason {NONE, FINISHED, SIGNAL, FREEZED} reason_t;
typedef struct _states{
    volatile unsigned int is_edited : 1;
    volatile unsigned int reason : 2;
} states_t;
typedef struct _job {
    pid_t pgid;
    size_t jid;
    volatile job_state_t state;
    states_t flags;
    int running_count;
    int stopped_count;
    char cmdline[MAXCMDLINE];
    int pids[MAXPIPESCOUNT];
} job_t;

typedef struct _command_t {
    char *argv[MAXARGS];
    char *infile;
    char *outfile;
    int append;
    int pipe_fd_in;
    int pipe_fd_out;
    pid_t pgid;
} command_t;

size_t jobs_count;

job_t jobs[MAXJOBS];

static void eval(char *cmdline);
static int builtin_command(char **argv);
static int parseline(char *buf, command_t *command, int last);
static char *extract_pwd(char *pwd);
static int add_job(pid_t pgid, job_state_t state, char *cmdline, int total_commands, pid_t *pids);
static pid_t delete_job(job_t *job_to_del);
static job_t *get_job_by_jid(size_t jid);
static job_t *get_job_by_pid(pid_t pgid);
static job_t *parse_arg(char *arg);
static void waitfg(pid_t pgid);
static void reason_print(void);
static void string_copy(char *oldstr, char *newstr);
static pid_t execute(command_t *command);

void sigchld_handler(int sig) {
    int status;
    int old_errno = errno;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        job_t *curr_job = get_job_by_pid(pid);
        if (curr_job == NULL) continue;
        if (WIFEXITED(status)) {
            curr_job->running_count -= 1;
            if (curr_job->running_count == 0) {
                curr_job->flags.is_edited = 1;
                curr_job->flags.reason = FINISHED;
                for (int i = 0; i < MAXPIPESCOUNT; i++) {
                    if (curr_job->pids[i] == pid) {
                        curr_job->pids[i] = 0;
                        break;
                    }
                }
            }
        }
        else if (WIFSIGNALED(status)) {
            curr_job->running_count -= 1;
            if (curr_job->running_count == 0) {
                curr_job->flags.is_edited = 1;
                curr_job->flags.reason = SIGNAL;
                for (int i = 0; i < MAXPIPESCOUNT; i++) {
                    if (curr_job->pids[i] == pid) {
                        curr_job->pids[i] = 0;
                        break;
                    }
                }
            }
        }
        else if (WIFSTOPPED(status)) {
            curr_job->stopped_count += 1;
            curr_job->running_count -= 1;
            if (curr_job->running_count == 0) {
                curr_job->flags.is_edited = 1;
                curr_job->flags.reason = FREEZED;
                curr_job->state = STOPPED;
            }
        }
    }
    errno = old_errno;
}

int main() {

    char prompt[1024];
    char *username = getenv("USER");
    char *hostname = getenv("HOSTNAME");
    char *pwd;
    char homedir[2] = "~";
    char *input;
    
    for (int i = 0; i < MAXJOBS; i++) {
        jobs[i].jid = (size_t)(i + 1);
        jobs[i].state = UNDEF;
        jobs[i].flags.is_edited = 0;
        jobs[i].flags.reason = NONE;
        jobs[i].running_count = 0;
        jobs[i].stopped_count = 0;
        memset(jobs[i].pids, 0, MAXPIPESCOUNT * sizeof(pid_t));
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

        if (!input) {
            int flag;
            for (int i = 0; i < MAXJOBS; i++) {
                if (jobs[i].state != UNDEF)
                    flag = 1;
            }
            if (flag)
                continue;
            else
                break;
        }

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

    char buf[2*MAXLINE];
    char *curr_command, *curr_pipe;
    int bg = -1;
    int old_fd = -1;
    int fds[2];
    int total_commands = 0;
    pid_t pids[MAXPIPESCOUNT];
    command_t command;
    sigset_t mask, prev_mask;
    pid_t pid, firstpid = -1;

    command.append = 0;
    command.infile = NULL;
    command.outfile = NULL;
    command.pipe_fd_in = -1;
    command.pipe_fd_out = -1;

    memset(pids, 0, MAXPIPESCOUNT * sizeof(pid_t));

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_SETMASK, &mask, &prev_mask);

    string_copy(cmdline, buf);

    curr_command = buf;

    int i = 0;
        
    while ((curr_pipe = strchr(curr_command, '|'))) {

        *curr_pipe = '\0';

        command.append = 0;
        command.infile = NULL;
        command.outfile = NULL;
        command.pipe_fd_in = -1;
        command.pipe_fd_out = -1;
        command.pgid = firstpid;

        total_commands++;

        parseline(curr_command, &command, 0);
        if (command.argv[0] == NULL)
            return;

        if (old_fd != -1) {
            command.pipe_fd_in = old_fd;
        }

        pipe(fds);
        command.pipe_fd_out = fds[1];
        old_fd = fds[0];

        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
            
        pid = execute(&command);

        pids[total_commands-1] = pid;
            
        if (i == 0) {
            firstpid = pid;
            i++;
        }
            
        curr_command = curr_pipe + 1;
    }

    command.append = 0;
    command.infile = NULL;
    command.outfile = NULL;
    command.pipe_fd_in = -1;
    command.pipe_fd_out = -1;
    command.pgid = firstpid;
    total_commands++;

    bg = parseline(curr_command, &command, 1);

    if (!builtin_command(command.argv)) {

        if (command.argv[0] == NULL)
            return;
            
        if (old_fd != -1)
            command.pipe_fd_in = old_fd;

        pid = execute(&command);
        pids[total_commands-1] = pid;

        if (!bg) {
            tcsetpgrp(STDIN_FILENO, firstpid);
            add_job(firstpid, FG, cmdline, total_commands, pids);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            waitfg(firstpid);
            tcsetpgrp(STDIN_FILENO, getpgrp());
            reason_print();
        } else {
            add_job(firstpid, BG, cmdline, total_commands, pids);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
        }
    }
}

static pid_t execute(command_t *command) {

    pid_t pid;
    char *path = getenv("PATH");
    char *pathdir;
    char *execute_dir = NULL;
    char dir_with_path[MAXDIRLEN];
    sigset_t mask;

    size_t pathlen = strlen(path);
    char path_copy[pathlen+1];

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
        
    if (jobs_count == 16) {
        fprintf(stderr, "ERROR: Сouldn't launch more jobs, wait for current jobs to end.\n");
        return -1;
    }
    if (strchr(command->argv[0], '/') == NULL) {
        strcpy(path_copy, path);
        pathdir = strtok(path_copy, ":");
        while (pathdir != NULL){
            snprintf(dir_with_path, sizeof(dir_with_path), "%s/%s", pathdir, command->argv[0]);
            if (access(dir_with_path, X_OK) == 0) {
                execute_dir = dir_with_path;
                break;
            }
            pathdir = strtok(NULL, ":");
        }
    } else
        execute_dir = command->argv[0];

    if ((pid = fork()) == 0) {
        if (command->pgid == -1) {
            if (setpgid(0, 0) < 0) {
                perror("setpgid failed");
                exit(1);
            }
        } else
            setpgid(0, command->pgid);

        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);

        if ((command->infile != NULL) && (command->pipe_fd_in == -1)) {
            int fd_in = open(command->infile, O_RDONLY);
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }

        if ((command->outfile != NULL) && (command->pipe_fd_out == -1)) {
            int flags = O_WRONLY | O_CREAT;
            if (command->append) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }
            int fd_out = open(command->outfile, flags, 0644);
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }

        if (command->pipe_fd_in != -1) {
            dup2(command->pipe_fd_in, STDIN_FILENO);
        }

        if (command->pipe_fd_out != -1) {
            dup2(command->pipe_fd_out, STDOUT_FILENO);
        }

        if (execve(execute_dir, command->argv, environ) < 0) {
            printf("%s: Command not found.\n", command->argv[0]);
            exit(0);
        }
    }
    if (pid > 0) {
        if (command->pgid == -1)
            setpgid(pid, pid);
        else
            setpgid(pid, command->pgid);

        if (command->pipe_fd_in != -1) {
            close(command->pipe_fd_in);
        }

        if (command->pipe_fd_out != -1) {
            close(command->pipe_fd_out);
        }
    }
    return pid;
}

static void string_copy(char *oldstr, char *newstr) {
    while (*oldstr) {
        if (*oldstr == '>' && *(oldstr+1) == '>') {
            *newstr++ = ' ';
            *newstr++ = '>';
            *newstr++ = '>';
            *newstr++ = ' ';
            oldstr += 2;       
            continue;
        }

        if (*oldstr == '<' || *oldstr == '>' || *oldstr == '|') {
            *newstr++ = ' ';
            *newstr++ = *oldstr;
            *newstr++ = ' ';
            oldstr++;
            continue;
        }

        *newstr++ = *oldstr++;
    }
    *newstr = ' ';
    *(newstr+1) = '\0';
}

static int parseline(char *buf, command_t *command, int last) {

    char *delim;
    int argc;
    int bg;
    int expect_infile = 0, expect_outfile = 0;

    while (*buf && (*buf == ' '))
        buf++;

    argc = 0;

    while ((delim = strchr(buf, ' '))) {
        *delim = '\0';
        if (strcmp(buf, "<") == 0) {
            expect_infile = 1;
        }
        else if (strcmp(buf, ">") == 0) {
            expect_outfile = 1;
            command->append = 0;
        }
        else if (strcmp(buf, ">>") == 0) {
            expect_outfile = 1;
            command->append = 1;
        }
        else {
            if (expect_infile) {
                command->infile = buf;
                expect_infile = 0;
            } else if (expect_outfile) {
                command->outfile = buf;
                expect_outfile = 0;
            } else {
                command->argv[argc++] = buf;
            }
        }
        
        buf = delim + 1;
        while (*buf && (*buf == ' ')) buf++;
    }
    command->argv[argc] = NULL;

    if (argc == 0) return 1;

    if (((bg = (*(command->argv[argc-1]) == '&')) != 0)) {
        command->argv[--argc] = NULL;
        if (!last && (bg == 1))
            bg = -1;
    }

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
                printf("[%ld] (%d) %s %s\n", jobs[i].jid, jobs[i].pgid, state, jobs[i].cmdline);
            }
        }
        return 1;
    }
    if (!strcmp(argv[0], "fg")) {
        job_t *job = parse_arg(argv[1]);
        if ((job == NULL) || (job->state == UNDEF)) {
            fprintf(stderr, "fg: No such job\n");
            return 1;
        }
        
        if (jobs->state == STOPPED) {
            job->running_count = job->stopped_count;
            job->stopped_count = 0;
        }
        job->state = FG;
        job->flags.is_edited = 0;
        job->flags.reason = NONE;
        tcsetpgrp(STDIN_FILENO, job->pgid);
        kill(-(job->pgid), SIGCONT);

        waitfg(job->pgid);
        tcsetpgrp(STDIN_FILENO, getpgrp());
        reason_print();   
        return 1;
    }
    if (!strcmp(argv[0], "bg")) {
        job_t *job = parse_arg(argv[1]);
        if ((job == NULL) || (job->state == UNDEF)) {
            fprintf(stderr, "bg: No such job\n");
            return 1;
        }

        if (job->state == STOPPED) {
            job->running_count = job->stopped_count;
            job->stopped_count = 0;
        }
        job->state = BG;
        job->flags.is_edited = 0;
        job->flags.reason = NONE;
        kill(-(job->pgid), SIGCONT);
        return 1;
    }
    if (!strcmp(argv[0], "kill")) {
        job_t *job = parse_arg(argv[1]);
        if ((job == NULL) || (job->state == UNDEF)) {
            fprintf(stderr, "kill: No such job\n");
            return 1;
        }
        kill(-(job->pgid), SIGINT);
        return 1;
    }
    return 0;
}

void waitfg(pid_t pgid) {
    sigset_t empty_mask, mask, prev_mask;
    job_t *curr_job = get_job_by_pid(pgid);

    sigemptyset(&empty_mask);
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    sigprocmask(SIG_BLOCK, &mask, &prev_mask);
    while (curr_job->state == FG && curr_job->running_count > 0 && curr_job->flags.is_edited == 0) {
        sigsuspend(&empty_mask);
    }
    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
}

static job_t *parse_arg(char *arg) {
    if (arg == NULL) return NULL;

    if (arg[0] == '%') {
        int jid = atoi(&arg[1]);
        return get_job_by_jid(jid);
    } else {
        pid_t pid = atoi(arg);
        return get_job_by_pid(pid);
    }
}

static job_t *get_job_by_jid(size_t jid) {
    for (int i = 0; i < 16; i++) {
        if (jobs[i].jid == jid)
            return &jobs[i];
    }
    return NULL;
}

job_t *get_job_by_pid(pid_t pid) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pgid == pid) return &jobs[i];
        if (jobs[i].state == UNDEF) continue;
        for (int j = 0; j < MAXPIPESCOUNT; j++) {
            if (jobs[i].pids[j] == pid) return &jobs[i];
        }
    }
    return NULL;
}

static int add_job(pid_t pgid, job_state_t state, char *cmdline, int total_commands, pid_t *pids) {
    for (int i = 0; i < 16; i++) {
        if (jobs[i].state == UNDEF) {
            job_t *curr_job = (jobs+i);
            curr_job->pgid = pgid;
            curr_job->state = state;
            snprintf(curr_job->cmdline, MAXCMDLINE, "%s", cmdline);
            curr_job->flags.is_edited = 0;
            curr_job->flags.reason = NONE;
            curr_job->running_count = total_commands;
            curr_job->stopped_count = 0;
            memcpy(curr_job->pids, pids, MAXPIPESCOUNT*sizeof(pid_t));
            jobs_count++;
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

    job_to_del->pgid = -1;
    job_to_del->state = UNDEF;
    memset(job_to_del->cmdline, 0, MAXCMDLINE);
    job_to_del->flags.is_edited = 0;
    job_to_del->flags.reason = NONE;
    memset(job_to_del->pids, 0, MAXPIPESCOUNT * sizeof(pid_t));
    jobs_count--;
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
                pid_t curr_pgid = jobs[i].pgid;
                if ((stop_reason == FINISHED)  && (jobs[i].running_count == 0)) {
                    if (jobs[i].state == BG) {
                        printf("Job [%ld] (%d) is finished\n", curr_jid, curr_pgid);
                    }
                    delete_job(jobs+i);
                }
                else if ((stop_reason == SIGNAL) && (jobs[i].running_count == 0)) {
                    if (jobs[i].state == BG) {
                        printf("\nJob [%ld] (%d) was stopped by signal\n", curr_jid, curr_pgid);
                    }
                    delete_job(jobs+i);
                }
                else if ((stop_reason == FREEZED) && (jobs[i].running_count == 0)) {
                    if (jobs[i].state == BG) {
                        printf("\nJob [%ld] (%d) was stopped by signal\n", curr_jid, curr_pgid);
                    }
                }
                jobs[i].flags.is_edited = 0;
                jobs[i].flags.reason = NONE;
            }
        }
    }
    sigprocmask(SIG_UNBLOCK, &mask_chld, NULL);
}