CC ?= gcc
LUAJIT ?= /usr/local/luajit
CFLAGS = -Wall -O2 -g -I$(LUAJIT)/include/luajit-2.1 -I../include -I.
LDFLAGS = -L$(LUAJIT)/lib -lluajit-5.1 -L.. -lmydb -lvector_engine -lm -ldl -lpthread

SRCS = session.c lua_engine.c llm_client.c http_async.c gui_tick.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test cli

all: libaicoding_agent.a aicoding

libaicoding_agent.a: $(OBJS)
	ar rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

aicoding: cli.c libaicoding_agent.a
	$(CC) $(CFLAGS) $< gui_tick.o -o $@ -L. -laicoding_agent $(LDFLAGS) $(shell pkg-config --libs libcurl) -Wl,--export-dynamic

test: test/test_session libaicoding_agent.a
	./test/test_session

test/test_session: test/test_session.c libaicoding_agent.a
	$(CC) $(CFLAGS) $< -o $@ -L. -laicoding_agent $(LDFLAGS) $(shell pkg-config --libs libcurl)

clean:
	rm -f $(OBJS) libaicoding_agent.a test/test_session aicoding
