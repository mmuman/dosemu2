#ifndef DNATIVE_H
#define DNATIVE_H

int native_dpmi_setup(void);
void native_dpmi_done(void);
int native_dpmi_control(cpuctx_t *scp);
int native_dpmi_exit(cpuctx_t *scp);
int native_read_ldt(void *ptr, int bytecount);
int native_write_ldt(void *ptr, int bytecount);
int native_check_verr(unsigned short selector);
int native_debug_breakpoint(int op, cpuctx_t *scp, int err);

struct dnative_ops {
  int (*setup)(void);
  void (*done)(void);
  int (*control)(cpuctx_t *scp);
  int (*exit)(cpuctx_t *scp);
  int (*read_ldt)(void *ptr, int bytecount);
  int (*write_ldt)(void *ptr, int bytecount);
  int (*check_verr)(unsigned short selector);
  int (*debug_breakpoint)(int op, cpuctx_t *scp, int err);
};

int register_dnative_ops(const struct dnative_ops *ops);

extern const struct dnative_ops *dnops;

#endif
