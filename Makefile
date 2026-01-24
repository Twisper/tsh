CC = gcc
CFLAGS = -Wall -Wextra
LIBS = -lreadline

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)

    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo "/opt/homebrew")
    
    CFLAGS += -I$(BREW_PREFIX)/opt/readline/include
    LDFLAGS += -L$(BREW_PREFIX)/opt/readline/lib
endif

all: tsh

security: CFLAGS += -g -fsanitize=address,undefined -fno-omit-frame-pointer
security: tsh

debug: CFLAGS += -g -DDEBUG
debug: tsh

tsh: tsh.c
	$(CC) $(CFLAGS) $(LDFLAGS) tsh.c -o tsh $(LIBS)

clean:
	rm -f tsh