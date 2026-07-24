#ifndef _DEVMGR_H
#define _DEVMGR_H
#include <sys/types.h> // 为了 pid_t

int devmgr_start(int *fd, pid_t *pid, const char *devpath);
int devmgr_open(int sockfd, const char *path);
void devmgr_finish(int sock, pid_t pid);

#endif
