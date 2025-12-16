
# Cross-compiler configuration
CROSS_PREFIX ?= /opt/cross-mint/bin/m68k-atari-mint-
CC := $(CROSS_PREFIX)gcc
AR := $(CROSS_PREFIX)ar
STRIP := $(CROSS_PREFIX)strip

# Project configuration
TARGET ?= main.prg
SRCS := main.c
OBJS := $(SRCS:.c=.o)

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
LDFLAGS ?=

.PHONY: all clean strip

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

strip: $(TARGET)
	$(STRIP) $<

clean:
	rm -f $(TARGET) $(OBJS)
