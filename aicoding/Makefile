CC ?= gcc
LUAJIT ?= /usr/local/luajit
CFLAGS = -Wall -O2 -g -I$(LUAJIT)/include/luajit-2.1 -I../include -I.
LDFLAGS = -L$(LUAJIT)/lib -lluajit-5.1 -L.. -lmydb -lvector_engine -lm -ldl -lpthread

SRCS = session.c lua_engine.c llm_client.c http_async.c serve.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test cli regression install uninstall

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: libaicoding_agent.a aicoding

libaicoding_agent.a: $(OBJS)
	ar rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

aicoding: cli.c libaicoding_agent.a
	$(CC) $(CFLAGS) $< -o $@ -L. -laicoding_agent $(LDFLAGS) $(shell pkg-config --libs libcurl) -Wl,--export-dynamic

test: all test/test_session
	LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:./ ./test/test_session
	LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:./ ./aicoding --test

regression: all
	./tests/run_regression.sh .

test/test_session: test/test_session.c libaicoding_agent.a
	$(CC) $(CFLAGS) $< -o $@ -L. -laicoding_agent $(LDFLAGS) $(shell pkg-config --libs libcurl)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 aicoding $(DESTDIR)$(BINDIR)/aicoding.bin
	install -m 755 aicoding.sh $(DESTDIR)$(BINDIR)/aicoding

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/aicoding $(DESTDIR)$(BINDIR)/aicoding.bin

clean:
	rm -f $(OBJS) libaicoding_agent.a test/test_session aicoding
