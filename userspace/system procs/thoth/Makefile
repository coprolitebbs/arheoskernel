
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11 -Wno-unused-parameter
LDFLAGS ?=

all: thoth

thoth: thoth.c platform_posix.c include/thoth.h include/thoth_platform.h
	$(CC) $(CFLAGS) -o $@ thoth.c platform_posix.c $(LDFLAGS)

clean:
	rm -f thoth

.PHONY: all clean
