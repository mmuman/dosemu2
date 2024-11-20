#ifndef DNATIVE_H
#define DNATIVE_H

int native_dpmi_setup(void);
void native_dpmi_done(void);
int native_dpmi_control(cpuctx_t *scp);
int native_dpmi_exit(cpuctx_t *scp);
void native_dpmi_enter(void);
void native_dpmi_leave(void);
int native_modify_ldt(int func, void *ptr, unsigned long bytecount);
int native_check_verr(unsigned short selector);
int native_debug_breakpoint(int op, cpuctx_t *scp, int err);

struct dnative_ops {
  int (*setup)(void);
  void (*done)(void);
  int (*control)(cpuctx_t *scp);
  int (*exit)(cpuctx_t *scp);
  void (*enter)(void);
  void (*leave)(void);
  int (*modify_ldt)(int func, void *ptr, unsigned long bytecount);
  int (*check_verr)(unsigned short selector);
  int (*debug_breakpoint)(int op, cpuctx_t *scp, int err);
};

int register_dnative_ops(const struct dnative_ops *ops);

#endif
