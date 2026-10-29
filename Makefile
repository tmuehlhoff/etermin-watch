CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  = -lcurl -lcjson

etermin_watch: etermin_watch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f etermin_watch
