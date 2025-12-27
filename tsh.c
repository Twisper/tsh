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

int main() {

    char cmdline[MAXLINE];

    while (1) {
        printf("/>");
        fgets(cmdline, MAXLINE, stdin);
        if (feof(stdin))
            continue;
    }
}