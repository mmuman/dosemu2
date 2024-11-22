#ifndef MPRIV_H
#define MPRIV_H

void *mmap_shm_hook(void *addr, size_t length, int prot, int flags,
                    int fd, off_t offset);

#endif
