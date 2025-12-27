#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

#define	MAXLINE 8192
#define MAXARGS 128

static void eval(char *cmdline);
static int builtin_command(char **argv);
static int parseline(char *buf, char **argv);
static void cd(char **argv);
static void export(char **argv);
static char *extract_pwd(char *pwd);

int main() {

    char cmdline[MAXLINE];
    char *username = getenv("USER");
    char *hostname = getenv("HOSTNAME");
    char *pwd;
    char homedir[2] = "~";

    while (1) {
        pwd = extract_pwd(getenv("PWD"));
        if (!strcmp(pwd, username))
            pwd = &homedir;
        printf("%s@%s: %s %% ", username, hostname, pwd);
        fgets(cmdline, MAXLINE, stdin);
        if (feof(stdin))
            continue;

        eval(cmdline);
    }
}

static char *extract_pwd(char *pwd) {
    char *ret_ptr = pwd;
    int i = strlen(pwd);

    while (ret_ptr[i] != '/') {
        i--;
    }

    return ret_ptr + i;
}

static void eval(char *cmdline) {

}

static int builtin_command(char **argv) {
    if (!strcmp(argv[0], "exit"))
        exit(0);
    if (!strcmp(argv[0], "&"))
        return 1;
    if (!strcmp(argv[0], "cd"))
        cd(argv);
    if (!strcmp(argv[0], "export"))
        export(argv);
}