CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2 $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)

FUSE_SRCS = xv6fuse.c xv6fs.c
FSCK_SRCS = fsck.c    xv6fs.c

.PHONY: all clean

all: xv6fuse xv6fsck

xv6fuse: $(FUSE_SRCS) xv6fs.h
	$(CC) $(CFLAGS) -o $@ $(FUSE_SRCS) $(LDFLAGS)

xv6fsck: $(FSCK_SRCS) xv6fs.h
	$(CC) $(CFLAGS) -o $@ $(FSCK_SRCS)

clean:
	rm -f xv6fuse xv6fsck *.o
