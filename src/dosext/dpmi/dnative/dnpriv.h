#ifndef DNPRIV_H
#define DNPRIV_H

#define DPMI_TMP_SIG SIGUSR1

void dpmi_return(sigcontext_t *scp, int retcode);
void dpmi_switch_sa(int sig, siginfo_t * inf, void *uc);

#endif
