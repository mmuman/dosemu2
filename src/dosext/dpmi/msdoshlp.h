#ifndef MSDOSHLP_H
#define MSDOSHLP_H

#ifdef DOSEMU
#include "sig.h"
#endif
#include "cpu.h"
#include "hlt.h"
#include "emudpmi.h"

enum MsdOpIds { MSDOS_FAULT, MSDOS_PAGEFAULT, API_CALL, API_WINOS2_CALL,
	MSDOS_LDT_CALL16, MSDOS_LDT_CALL32,
	MSDOS_RSP_CALL16, MSDOS_RSP_CALL32,
	MSDOS_EXT_CALL };

enum { MSDOS_NONE, MSDOS_RMINT, MSDOS_RM, MSDOS_PM, MSDOS_DONE };
enum { POSTEXT_NONE, POSTEXT_PUSH };

void msdos_pm_call(cpuctx_t *scp);

struct pmaddr_s get_pmcb_handler(void (*handler)(cpuctx_t *,
	const struct RealModeCallStructure *, int, void *),
	void *arg,
	void (*ret_handler)(cpuctx_t *,
	struct RealModeCallStructure *, int),
	int num);
struct pmaddr_s get_pm_handler(enum MsdOpIds id,
	void (*handler)(cpuctx_t *, void *), void *arg);
struct pmrm_ret {
    int ret;
    far_t faddr;
    int inum;
    DPMI_INTDESC prev;
};
struct pext_ret {
    int ret;
    unsigned arg;
};
struct pmaddr_s get_pmrm_handler_m(enum MsdOpIds id,
	struct pmrm_ret (*handler)(
	cpuctx_t *, struct RealModeCallStructure *,
	unsigned short, void *(*)(int), int),
	void *(*arg)(int),
	struct pext_ret (*ret_handler)(
	cpuctx_t *, const struct RealModeCallStructure *,
	unsigned short, int),
	unsigned short (*rm_seg)(cpuctx_t *, int, void *),
	void *rm_arg, int len, int r_offs[]);
far_t get_exec_helper(void);
far_t get_term_helper(void);

void msdoshlp_init(int (*is_32)(void), int len);

struct dos_helper_s {
    int tid;
    unsigned entry;
    unsigned short (*rm_seg)(cpuctx_t *, int, void *);
    void *rm_arg;
    int e_offs[256];
};
void doshlp_setup_retf(struct dos_helper_s *h,
	const char *name, void (*thr)(void *),
	unsigned short (*rm_seg)(cpuctx_t *, int, void *),
	void *rm_arg);
void doshlp_setup(struct dos_helper_s *h,
	const char *name, void (*thr)(void *),
	void (*post)(cpuctx_t *));

struct pmaddr_s doshlp_get_entry(unsigned entry);
struct pmaddr_s doshlp_get_entry16(unsigned entry);
struct pmaddr_s doshlp_get_entry32(unsigned entry);

void doshlp_quit_dpmi(cpuctx_t *scp);
struct pmaddr_s doshlp_get_abort_helper(void);
void doshlp_call_msdos(cpuctx_t *scp);
void doshlp_call_reinit(cpuctx_t *scp);
int doshlp_idle(void);

Bit16u hlt_register_handler_pm(emu_hlt_t handler);

struct msdos_ldt_ops {
    void (*reset)(void);
    int (*access)(dosaddr_t cr2);
    void (*write)(cpuctx_t *scp, uint32_t op, int len, dosaddr_t cr2);
    int (*pagefault)(cpuctx_t *scp);
    const char *(*describe_selector)(unsigned short sel);
};

void msdos_register_ops(const struct msdos_ldt_ops *ops);
void msdos_setup(void);
void msdos_reset(void);
int msdos_ldt_access(dosaddr_t cr2);
void msdos_ldt_write(cpuctx_t *scp, uint32_t op, int len,
    dosaddr_t cr2);
int msdos_ldt_pagefault(cpuctx_t *scp);
const char *msdos_describe_selector(unsigned short sel);

#endif
