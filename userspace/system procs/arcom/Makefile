CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -Iinclude
TARGET   = arcom

SRC = fm_core.c fm_ui.c posix/fm_posix.c posix/main_posix.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
