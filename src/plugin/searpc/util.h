#ifndef SRPC_UTIL_H
#define SRPC_UTIL_H

SearpcClient *clnt_init(int *sock_rx, int (*init_cb)(int, int, void *),
        void *init_arg, void (*run_cb)(void), void (*ex_cb)(void *));

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
