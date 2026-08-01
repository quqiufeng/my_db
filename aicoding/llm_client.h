#ifndef OPCODE_LLM_H
#define OPCODE_LLM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Supported API protocols */
enum {
    LLM_PROTOCOL_OPENAI = 0,
    LLM_PROTOCOL_ANTHROPIC = 1
};

/* Configuration for an LLM endpoint */
typedef struct {
    int    protocol;      /* LLM_PROTOCOL_OPENAI or LLM_PROTOCOL_ANTHROPIC */
    char*  base_url;      /* e.g. "https://api.kimi.com/coding/v1" */
    char*  api_key;
    char*  model;         /* e.g. "gpt-4o-mini" or "claude-3-5-sonnet" */
    char*  user_agent;    /* optional client identity spoofing */
    char*  extra_header;  /* optional "Name: Value" header */
    double temperature;
    int    max_tokens;
} llm_config_t;

typedef struct llm_client llm_client_t;

/* Lifecycle */
llm_client_t* llm_client_create(const llm_config_t* config);
void          llm_client_free(llm_client_t* c);

/* Build an llm_config_t from environment variables. Strings are strdup'd;
 * free them with llm_config_free_fields. */
void llm_config_from_env(llm_config_t* cfg);
void llm_config_free_fields(llm_config_t* cfg);
int           llm_client_protocol(const llm_client_t* c); /* returns LLM_PROTOCOL_* */
const char*   llm_client_model(const llm_client_t* c);

/* Synchronous completion. Returns malloc'd response JSON or NULL. */
char* llm_complete(llm_client_t* c,
                   const char* system_prompt,
                   const char* user_prompt,
                   const char* tools_json);   /* optional, JSON array or NULL */

/* Synchronous completion with full messages array (for tool-call loops).
 * messages_json: JSON array of {role, content} messages.
 * Returns malloc'd response JSON or NULL. */
char* llm_complete_messages(llm_client_t* c,
                            const char* system_prompt,
                            const char* messages_json,
                            const char* tools_json);

/* Send a pre-built request body JSON. Useful when caller wants full control. */
char* llm_complete_raw(llm_client_t* c, const char* request_body_json);

/* Streaming callback: receives each SSE delta chunk. Return 0 to continue, non-zero to abort. */
typedef int (*llm_stream_cb_t)(const char* delta_text, void* userdata);

/* Streaming completion. Returns 0 on success, negative on error. */
int llm_complete_stream(llm_client_t* c,
                        const char* system_prompt,
                        const char* user_prompt,
                        const char* tools_json,
                        llm_stream_cb_t cb,
                        void* userdata);

#ifdef __cplusplus
}
#endif

#endif /* OPCODE_LLM_H */
