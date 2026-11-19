# Shikra. The documented build needs no make at all:
#     cc -std=c99 -O2 -Wall -Wextra src/*.c -o shikra
# (src/embed.c is generated but committed). This Makefile regenerates it and runs tests.

CC = cc
CFLAGS = -std=c99 -O2 -Wall -Wextra

EMBED_SRC = $(shell find lib -type f ! -name '.*' 2>/dev/null | LC_ALL=C sort) \
            src/rt.h $(sort $(wildcard src/rt_*.c)) src/prims.def

all: shikra

tools/mkembed: tools/mkembed.c
	$(CC) $(CFLAGS) tools/mkembed.c -o $@

src/embed.c: tools/mkembed $(EMBED_SRC)
	./tools/mkembed $(EMBED_SRC) > src/embed.c.tmp
	mv src/embed.c.tmp src/embed.c

shikra: src/embed.c $(wildcard src/*.c src/*.h) src/prims.def
	$(CC) $(CFLAGS) src/*.c -o $@

# Fails if the committed src/embed.c does not match its sources.
check-embed: tools/mkembed
	@./tools/mkembed $(EMBED_SRC) | cmp -s - src/embed.c || \
	  { echo "check-embed: src/embed.c is stale; run make and commit it"; exit 1; }
	@echo "check-embed: ok"

test: shikra check-embed
	sh tests/run.sh all

clean:
	rm -f shikra tools/mkembed src/embed.c.tmp
	rm -rf tests/tmp

.PHONY: all test check-embed clean
