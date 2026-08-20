CFLAGS ?= -O2 -Wall -g

all: fred_bench checkfred

fred_bench: fred_bench.c
	$(CC) $(CFLAGS) -o $@ $< -lm

checkfred: checkfred.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f fred_bench checkfred

.PHONY: all clean
