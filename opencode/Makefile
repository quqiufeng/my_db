CC ?= gcc
LUAJIT ?= /usr/local/luajit
CFLAGS = -Wall -O2 -g -I$(LUAJIT)/include/luajit-2.1 -I../include -I.
LDFLAGS = -L$(LUAJIT)/lib -lluajit-5.1 -L.. -lmydb -lm -ldl -lpthread

SRCS = session.c lua_engine.c llm_client.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean test cli

all: libopencode_agent.a opencode_cli

libopencode_agent.a: $(OBJS)
	ar rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

opencode_cli: cli.c libopencode_agent.a
	$(CC) $(CFLAGS) $< -o $@ -L. -lopencode_agent $(LDFLAGS) $(shell pkg-config --libs libcurl)

test: test/test_session libopencode_agent.a
	./test/test_session

test/test_session: test/test_session.c libopencode_agent.a
	$(CC) $(CFLAGS) $< -o $@ -L. -lopencode_agent $(LDFLAGS) $(shell pkg-config --libs libcurl)

clean:
	rm -f $(OBJS) libopencode_agent.a test/test_session opencode_cli
