#ifndef UFFD_H
#define UFFD_H

int uffd_open(int sock);
int uffd_attach(void);
int uffd_reattach(int sock, void *addr, size_t len);
void uffd_init(int sock);
int uffd_wp(int idx, void *addr, size_t len, int prot);

#endif
