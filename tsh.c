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
#define MAXDIRLEN 64

extern char **environ;

static void eval(char *cmdline);
static int builtin_command(char **argv);
static int parseline(char *buf, char **argv);
static void export(char **argv);
static char *extract_pwd(char *pwd);

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

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL)
        return;

    if (!builtin_command(argv)) {
        if ((pid = fork()) == 0) {
            if (execve(argv[0], argv, environ) < 0) {
                printf("%s: Command not found.\n", argv[0]);
                exit(0);
            }
        }

        if (!bg) {
            int status;
            if (waitpid(pid, &status, 0) < 0)
                fprintf(stderr, "waitfg: waitpid error");
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
        if (chdir(argv[1]) != 0)
            fprintf(stderr, "ERROR: directory not found.\n");
        else {
            char cwd[1024];
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
        //export(argv);
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
    if (!strcmp(argv[0], "kill")) {
        
        return 1;
    }
    if (!strcmp(argv[0], "unset")) {
        
        return 1;
    }
    return 0;
}