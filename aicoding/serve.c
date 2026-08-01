/* serve.c - TCP bridge for remote ACP sessions.
 *
 * `aicoding serve --port N` forks an embedded ACP engine (aicoding --acp)
 * and bridges TCP clients to it:
 *   - every JSON-RPC line from any client is forwarded to the engine stdin;
 *   - engine stdout lines are routed back to the client that owns the
 *     request id, or broadcast to all clients for events and engine-initiated
 *     requests (e.g. permission/request). Response routing is done by the
 *     JSON-RPC id field, so `agent-tui attach` works over the network.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_CONNS 64
#define MAX_LINE  65536

typedef struct {
    int        fd;
    char       buf[MAX_LINE];
    size_t     len;
    int        alive;
} conn_t;

typedef struct {
    long long  id;
    int        conn_idx;
} route_t;

static conn_t  conns[MAX_CONNS];
static route_t routes[MAX_CONNS * 8];
static int     n_routes = 0;

/* Per-connection id remapping: clients reuse the same small ids (1,2,3...),
 * so forwarded request ids are rewritten to (conn_idx+1)*1000000 + id and
 * engine responses are mapped back before being sent to the owner. */
#define ID_BASE  1000000LL
typedef struct {
    long long  remapped;
    int        conn_idx;
    long long  orig;
} remap_t;

static remap_t remaps[4096];
static int     n_remaps = 0;

static void remap_add(long long remapped, int conn_idx, long long orig) {
    if (n_remaps < (int)(sizeof(remaps) / sizeof(remaps[0]))) {
        remaps[n_remaps].remapped = remapped;
        remaps[n_remaps].conn_idx = conn_idx;
        remaps[n_remaps].orig = orig;
        n_remaps++;
        if (getenv("SERVE_DEBUG"))
            fprintf(stderr, "[serve] remap_add %lld -> conn%d orig%lld (n=%d)\n",
                    remapped, conn_idx, orig, n_remaps);
    }
}

static int remap_lookup(long long remapped) {
    for (int i = 0; i < n_remaps; i++) {
        if (remaps[i].remapped == remapped) return i;
    }
    return -1;
}

/* Rewrite the "id":N field in a client line to (base + N).
 * Returns the remapped id, or -1 if the line has no id field. */
static long long rewrite_client_id(const char* line, int base, char* out, size_t outsz) {
    const char* p = strstr(line, "\"id\"");
    if (!p) return -1;
    const char* colon = p + 4;
    while (*colon == ' ' || *colon == ':') colon++;
    long long orig = atoll(colon);
    if (orig < 0 || orig >= ID_BASE) return -1; /* engine-request responses: leave alone */
    long long remapped = (long long)base * ID_BASE + orig;
    char newid[32];
    snprintf(newid, sizeof(newid), "%lld", remapped);
    size_t idlen = 0;
    while (colon[idlen] >= '0' && colon[idlen] <= '9') idlen++;
    size_t n = (size_t)(colon - line);
    if (n + strlen(newid) + (strlen(line) - n - idlen) + 1 > outsz) return -1;
    memcpy(out, line, n);
    memcpy(out + n, newid, strlen(newid));
    strcpy(out + n + strlen(newid), colon + idlen);
    return remapped;
}

static int engine_out = -1; /* engine stdout (child -> us) */
static int engine_in  = -1; /* engine stdin  (us -> child) */
static pid_t engine_pid = 0;

static int listener = -1;

static void kill_engine(int sig) {
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        /* brief grace period, then SIGKILL */
        int tries = 0;
        while (tries++ < 20) {
            pid_t rc = waitpid(engine_pid, NULL, WNOHANG);
            if (rc == engine_pid || (rc < 0 && errno == ECHILD)) break;
            usleep(50 * 1000);
        }
        kill(engine_pid, SIGKILL);
        waitpid(engine_pid, NULL, 0);
        engine_pid = 0;
    }
    if (sig) _exit(0);
}

static void broadcast_line(const char* line) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].fd >= 0) {
            ssize_t w = write(conns[i].fd, line, strlen(line));
            (void)w;
        }
    }
}

static void send_to(int idx, const char* line) {
    if (idx >= 0 && idx < MAX_CONNS && conns[idx].fd >= 0) {
        ssize_t w = write(conns[idx].fd, line, strlen(line));
        (void)w;
    }
}

static void route_register(long long id, int conn_idx) {
    for (int i = 0; i < n_routes; i++) {
        if (routes[i].id == id) {
            routes[i].conn_idx = conn_idx;
            return;
        }
    }
    if (n_routes < (int)(sizeof(routes) / sizeof(routes[0]))) {
        routes[n_routes].id = id;
        routes[n_routes].conn_idx = conn_idx;
        n_routes++;
    }
}

static void route_forget(long long id) {
    for (int i = 0; i < n_routes; i++) {
        if (routes[i].id == id) {
            routes[i] = routes[n_routes - 1];
            n_routes--;
            return;
        }
    }
}

static int find_conn(int fd) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].fd == fd) return i;
    }
    return -1;
}

static int next_free_conn(void) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].fd < 0) return i;
    }
    return -1;
}

static long long line_id(const char* line) {
    const char* p = strstr(line, "\"id\"");
    if (!p) return -1;
    p += 4;
    while (*p == ' ' || *p == ':') p++;
    if (*p == '\"') {
        /* string id - parse digits */
        p++;
        return atoll(p);
    }
    return atoll(p);
}

static int is_response(const char* line) {
    return strstr(line, "\"result\"") || strstr(line, "\"error\"");
}

/* Handle a complete line from the engine (its stdout). */
static void handle_engine_line(const char* line) {
    long long id = line_id(line);
    if (id >= 0) {
        if (id >= ID_BASE && is_response(line)) {
            /* Response to a remapped client request: restore original id. */
            int ri = remap_lookup(id);
            if (getenv("SERVE_DEBUG"))
                fprintf(stderr, "[serve] engine resp id=%lld ri=%d n_remaps=%d\n",
                        id, ri, n_remaps);
            if (ri >= 0) {
                char out[2 * MAX_LINE];
                char newid[32];
                snprintf(newid, sizeof(newid), "%lld", remaps[ri].orig);
                const char* colon = strstr(line, "\"id\"") + 4;
                while (*colon == ' ' || *colon == ':') colon++;
                size_t idlen = 0;
                while (colon[idlen] >= '0' && colon[idlen] <= '9') idlen++;
                size_t n = (size_t)(colon - line);
                memcpy(out, line, n);
                memcpy(out + n, newid, strlen(newid));
                strcpy(out + n + strlen(newid), colon + idlen);
                send_to(remaps[ri].conn_idx, out);
                remaps[ri] = remaps[n_remaps - 1];
                n_remaps--;
                return;
            }
        }
        if (is_response(line)) {
            int idx = -1;
            for (int i = 0; i < n_routes; i++) {
                if (routes[i].id == id) { idx = routes[i].conn_idx; break; }
            }
            send_to(idx, line);
            route_forget(id);
            return;
        }
        /* Engine-initiated request (permission/request, question/request...):
         * broadcast, and route the matching response back to whichever
         * client responds. */
        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i].fd >= 0) route_register(id, i);
        }
        broadcast_line(line);
        return;
    }
    /* Plain event (session/update): broadcast. */
    broadcast_line(line);
}

/* Handle a complete line from a client. */
static void handle_client_line(int idx, const char* line) {
    long long id = line_id(line);
    if (id >= 0 && !is_response(line)) {
        if (id < ID_BASE) {
            char out[2 * MAX_LINE];
            long long remapped = rewrite_client_id(line, idx + 1, out, sizeof(out));
            if (remapped >= 0) {
                remap_add(remapped, idx, id);
                ssize_t w = write(engine_in, out, strlen(out));
                (void)w;
                return;
            }
        }
        route_register(id, idx);
    }
    ssize_t w = write(engine_in, line, strlen(line));
    (void)w;
}

static void accept_conn(void) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(listener, (struct sockaddr*)&addr, &alen);
    if (fd < 0) return;
    int idx = next_free_conn();
    if (idx < 0) {
        close(fd);
        return;
    }
    conns[idx].fd = fd;
    conns[idx].len = 0;
    conns[idx].alive = 1;
}

/* Read available bytes from fd into conn buffer; emit complete lines. */
static void pump(int idx, int fd, int is_engine, int emit_lines) {
    char tmp[8192];
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n <= 0) {
        if (is_engine) {
            /* engine exited: shutdown */
            for (int i = 0; i < MAX_CONNS; i++) {
                if (conns[i].fd >= 0) close(conns[i].fd);
            }
            exit(0);
        }
        conns[idx].alive = 0;
        conns[idx].fd = -1;
        return;
    }
    conn_t* c = is_engine ? NULL : &conns[idx];
    static char ebuf[MAX_LINE];
    static size_t elen = 0;
    char* buf = is_engine ? ebuf : c->buf;
    size_t* len = is_engine ? &elen : &c->len;

    for (ssize_t i = 0; i < n; i++) {
        if (*len >= MAX_LINE - 1) {
            *len = 0; /* line too long, drop */
            continue;
        }
        buf[(*len)++] = tmp[i];
        if (tmp[i] == '\n') {
            buf[*len] = '\0';
            if (emit_lines) {
                if (is_engine) handle_engine_line(buf);
                else handle_client_line(idx, buf);
            }
            *len = 0;
        }
    }
}

static void spawn_engine(int argc, char** argv) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) {
        fprintf(stderr, "[serve] pipe failed\n");
        exit(1);
    }
    engine_pid = fork();
    if (engine_pid < 0) {
        fprintf(stderr, "[serve] fork failed\n");
        exit(1);
    }
    if (engine_pid == 0) {
        /* child: engine side. stdin <- in_pipe[0], stdout -> out_pipe[1] */
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        char** eng_argv = calloc(argc + 2, sizeof(char*));
        if (!eng_argv) exit(1);
        int n = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "serve") == 0 || strcmp(argv[i], "--port") == 0) continue;
            if (i + 1 < argc && strcmp(argv[i], "--port") == 0) { i++; continue; }
            eng_argv[n++] = argv[i];
        }
        eng_argv[n++] = "--acp";
        eng_argv[n] = NULL;
        execv(argv[0], eng_argv);
        perror("[serve] exec");
        exit(1);
    }
    engine_in = in_pipe[1];
    engine_out = out_pipe[0];
    close(in_pipe[0]);
    close(out_pipe[1]);
    for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = -1;
}

int serve_main(int argc, char** argv) {
    int port = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
        }
    }
    if (port <= 0) {
        fprintf(stderr, "usage: aicoding serve --port <port> [--project DIR] [--model NAME]\n");
        return 1;
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, kill_engine);
    signal(SIGINT, kill_engine);
    signal(SIGHUP, kill_engine);

    spawn_engine(argc, argv);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fprintf(stderr, "[serve] socket failed\n");
        return 1;
    }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[serve] bind %d failed: %s\n", port, strerror(errno));
        return 1;
    }
    if (listen(listener, 8) < 0) {
        fprintf(stderr, "[serve] listen failed\n");
        return 1;
    }
    fprintf(stderr, "[serve] listening on 0.0.0.0:%d (engine pid %d)\n", port, engine_pid);

    atexit(kill_engine);

    struct pollfd fds[MAX_CONNS + 2];
    for (;;) {
        int nfds = 0;
        fds[nfds].fd = listener; fds[nfds].events = POLLIN; nfds++;
        fds[nfds].fd = engine_out; fds[nfds].events = POLLIN; nfds++;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i].fd >= 0) {
                fds[nfds].fd = conns[i].fd;
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }
        int rc = poll(fds, (nfds_t)nfds, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (getenv("SERVE_DEBUG")) {
            fprintf(stderr, "[serve] poll rc=%d revents=[%x %x]", rc,
                    fds[0].revents, fds[1].revents);
            for (int i = 2; i < nfds; i++) fprintf(stderr, " %d:%x", fds[i].fd, fds[i].revents);
            fprintf(stderr, "\n");
        }
        int pos = 2;
        if (fds[0].revents & POLLIN) accept_conn();
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) pump(-1, engine_out, 1, 1);
        for (int i = 2; i < nfds; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                int idx = find_conn(fds[i].fd);
                if (idx >= 0) pump(idx, conns[idx].fd, 0, 1);
            }
            pos++;
        }
    }
    return 0;
}
