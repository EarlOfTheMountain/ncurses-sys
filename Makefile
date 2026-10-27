# tclock — build with: make
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11

# prefer wide-character ncurses, fall back gracefully
ifeq ($(shell pkg-config --exists ncursesw && echo y),y)
  LDLIBS := $(shell pkg-config --libs ncursesw)
else ifeq ($(shell pkg-config --exists ncurses && echo y),y)
  LDLIBS := $(shell pkg-config --libs ncurses)
else
  LDLIBS := -lncursesw
endif

tclock: tclock.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) tclock

.PHONY: clean
