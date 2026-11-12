# tclock — build with: make
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11

# FreeBSD uses native sysctl readers; generate a FreeBSD-enabled copy while
# keeping the Linux source path unchanged.
ifeq ($(shell uname -s),FreeBSD)
  TCLOCK_SRC := tclock-freebsd.c
  tclock-freebsd.c: tclock.c freebsd-build.awk freebsd-stats.inc
	awk -f freebsd-build.awk tclock.c > $@
else
  TCLOCK_SRC := tclock.c
endif

# prefer wide-character ncurses, fall back gracefully
ifeq ($(shell pkg-config --exists ncursesw && echo y),y)
  LDLIBS := $(shell pkg-config --libs ncursesw)
else ifeq ($(shell pkg-config --exists ncurses && echo y),y)
  LDLIBS := $(shell pkg-config --libs ncurses)
else
  LDLIBS := -lncursesw
endif

tclock: $(TCLOCK_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) tclock tclock-freebsd.c

.PHONY: clean
