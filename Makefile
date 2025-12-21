
# Cross-compiler configuration
CROSS_PREFIX ?= m68k-atari-mint-
CC := $(CROSS_PREFIX)gcc
AR := $(CROSS_PREFIX)ar
STRIP := $(CROSS_PREFIX)strip

# Project configuration
TARGET ?= suptool
OBJS := suptool.o

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -fomit-frame-pointer
LDFLAGS ?=

.PHONY: all clean strip

all: $(TARGET).tos

$(TARGET).tos: $(TARGET)
	cp -a $< $@

$(TARGET): $(OBJS)

strip: $(TARGET).tos
	$(STRIP) -s $<

clean:
	rm -f $(TARGET).tos $(TARGET) $(OBJS)
