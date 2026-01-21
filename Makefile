CC = gcc
CFLAGS = -Wall -Wextra -I/opt/homebrew/opt/readline/include -L/opt/homebrew/opt/readline/lib -lreadline

all: myshell

debug: CFLAGS += -g -DDEBUG
debug: tsh

tsh: tsh.c
	$(CC) $(CFLAGS) tsh.c -o tsh

clean:
	rm -f tsh