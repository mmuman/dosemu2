#ifndef SRPC_UTIL_H
#define SRPC_UTIL_H

typedef int (*init_cb_t)(const char *, int, void *);
SearpcClient *clnt_init(int *sock_rx, init_cb_t init_cb,
        void *init_arg, int (*svc_ex)(void),
        void (*ex_cb)(void *), const char *svc_name);

#define BAD_RPC(msg) do { \
    bad_rpc(__FUNCTION__, msg); \
    return -1; \
} while (0)
#define CHECK_RPC(st) do { \
    if (st) { \
        BAD_RPC(st->message); \
        g_error_free(st); \
    } \
} while (0)

#endif
