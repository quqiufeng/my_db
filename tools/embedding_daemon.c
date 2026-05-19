#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include "onnx_embedder.h"

#define SOCKET_PATH "/tmp/embedding_daemon.sock"
#define BUFFER_SIZE 4096
#define MAX_MODELS 2

typedef struct {
    char name[32];
    onnx_embedder_t* embedder;
} model_entry_t;

static model_entry_t models[MAX_MODELS];
static int num_models = 0;
static int server_running = 1;

static void signal_handler(int sig) {
    server_running = 0;
}

static int init_models() {
    // MPNet
    printf("Loading MPNet model...\n");
    models[0].embedder = onnx_embedder_init(
        "models/all-mpnet-base-v2/model.onnx",
        "models/all-mpnet-base-v2/vocab.txt",
        128, 768
    );
    if (!models[0].embedder) {
        fprintf(stderr, "Failed to load MPNet: %s\n", onnx_embedder_error());
        return -1;
    }
    strcpy(models[0].name, "mpnet");
    num_models++;
    printf("  MPNet loaded\n");
    
    // Jina
    printf("Loading Jina model...\n");
    models[1].embedder = onnx_embedder_init(
        "models/jina-embeddings-v2-base-code/model.onnx",
        "models/jina-embeddings-v2-base-code/vocab.json",
        512, 768
    );
    if (!models[1].embedder) {
        fprintf(stderr, "Failed to load Jina: %s\n", onnx_embedder_error());
        return -1;
    }
    strcpy(models[1].name, "jina");
    num_models++;
    printf("  Jina loaded\n");
    
    return 0;
}

static onnx_embedder_t* find_model(const char* name) {
    for (int i = 0; i < num_models; i++) {
        if (strcmp(models[i].name, name) == 0) {
            return models[i].embedder;
        }
    }
    return NULL;
}

static void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    int n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) return;
    buffer[n] = '\0';
    
    // Parse request: "model_name|query_text"
    char* sep = strchr(buffer, '|');
    if (!sep) {
        write(client_fd, "ERROR|invalid_format", 20);
        return;
    }
    
    *sep = '\0';
    const char* model_name = buffer;
    const char* query = sep + 1;
    
    onnx_embedder_t* embedder = find_model(model_name);
    if (!embedder) {
        write(client_fd, "ERROR|model_not_found", 21);
        return;
    }
    
    float vector[768];
    if (onnx_embedder_encode(embedder, query, vector) != 0) {
        write(client_fd, "ERROR|encode_failed", 19);
        return;
    }
    
    // Send vector as binary (768 * 4 bytes)
    write(client_fd, "OK|", 3);
    write(client_fd, vector, 768 * sizeof(float));
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("Embedding Daemon v1.0\n");
    printf("=======================\n");
    
    if (init_models() != 0) {
        fprintf(stderr, "Failed to initialize models\n");
        return 1;
    }
    
    // Create socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    // Remove old socket file
    unlink(SOCKET_PATH);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }
    
    printf("Listening on %s\n", SOCKET_PATH);
    printf("Ready for queries\n\n");
    
    while (server_running) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        
        handle_client(client_fd);
        close(client_fd);
    }
    
    printf("\nShutting down...\n");
    close(server_fd);
    unlink(SOCKET_PATH);
    
    for (int i = 0; i < num_models; i++) {
        onnx_embedder_free(models[i].embedder);
    }
    
    return 0;
}
