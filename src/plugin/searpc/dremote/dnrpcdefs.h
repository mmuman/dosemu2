#ifndef DNRPCDEFS_H
#define DNRPCDEFS_H

int dnrpc_srv_init(const char *svc_name, int fd);
int dnrpc_exiting(void);

extern void *rpc_shared_page;

struct full_state {
    cpuctx_t cpu;
    emu_fpstate fpu;
};

static inline void send_state(cpuctx_t *scp)
{
    struct full_state *f = rpc_shared_page;
    f->cpu = *scp;
    f->fpu = vm86_fpu_state;
}

static inline void recv_state(cpuctx_t *scp)
{
    struct full_state *f = rpc_shared_page;
    *scp = f->cpu;
    vm86_fpu_state = f->fpu;
}

#endif
