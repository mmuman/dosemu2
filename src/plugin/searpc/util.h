#ifndef SRPC_UTIL_H
#define SRPC_UTIL_H

SearpcClient *clnt_init(int *sock_rx, int (*init_cb)(int, int, void *),
        void *init_arg, void (*run_cb)(void), void (*ex_cb)(void *));

#endif
