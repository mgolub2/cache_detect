CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion
LDFLAGS ?=

# Link librt when building on Linux (needed for clock_gettime on some systems)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDFLAGS += -lrt
endif

TARGET := cache_detect
SRCS := cache_detect.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean run run-gptoss

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) gptoss


