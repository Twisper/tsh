CC = gcc
CFLAGS = -Wall -Wextra -I/opt/homebrew/opt/readline/include -L/opt/homebrew/opt/readline/lib -lreadline

all: myshell

security: CFLAGS += -g -fsanitize=address,undefined -fno-omit-frame-pointer
security: tsh

debug: CFLAGS += -g -DDEBUG
debug: tsh

tsh: tsh.c
	$(CC) $(CFLAGS) tsh.c -o tsh

clean:
	rm -f tsh