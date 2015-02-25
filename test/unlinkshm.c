/*
** destroy the shared memory area used by dsm; if you run this without
** knowing what you are doing, you will surely mess up dsm
**
** $Id: unlinkshm.c,v 1.2 2004/06/15 19:16:23 ckatz Exp $
*/


#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#ifdef __linux__
#  include <fcntl.h>
#  include <sys/stat.h>
#endif

#define _DSM_INTERNAL
#include "dsm.h"

int main(void) {

  int shm_descr;
  int s;

  shm_descr = shm_open(DSM_NOTIFY_TABLE_SHM_NAME, O_RDWR, S_IRWXU);
  if(shm_descr==-1) {
    if(errno==ENOENT) {
      printf("shm doesn't exist\n");
      return(0);
    }
    else {
      perror("shm_open");
      return(1);
    }
  }

  s = close(shm_descr);
  if(s!=0) {
    perror("close()");
    return(1);
  }

  s = shm_unlink(DSM_NOTIFY_TABLE_SHM_NAME);
  if(s!=0) {
    perror("shm_unlink");
    return(1);
  }

  printf("shm %s unlinked\n", DSM_NOTIFY_TABLE_SHM_NAME);
  return(0);
}
