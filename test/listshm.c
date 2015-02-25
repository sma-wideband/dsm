/*
** list the contents of the shared memory structure used by dsm
**
** $Id: listshm.c,v 2.3 2006/11/15 17:57:40 ckatz Exp $
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

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
#  include <sys/sem.h>
#endif

int main(void) {

  int i,s;
  void *shared_memory;
  int shm_descr;
  int shm_size;
  struct notification_table_entry *notification_table;
  char   *data_buffers;
  int    *dsm_nproc_notify_max_p;
  size_t *max_buffer_size_p, max_buffer_size;
  int sem;
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  union semun dummy_semarg;
#endif

  /*********/
  /* Begin */
  /*********/

  printf("opening shm %s\n", DSM_NOTIFY_TABLE_SHM_NAME);
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


  printf("setting shm size (%d) so we can reach max_buffer_size\n",
	 sizeof(*dsm_nproc_notify_max_p)+sizeof(*max_buffer_size_p));

  printf("mapping shm so we can reach max_buffer_size\n");
  shared_memory = mmap(0,
		       sizeof(*dsm_nproc_notify_max_p)
		       + sizeof(*max_buffer_size_p),
		       PROT_READ | PROT_WRITE,
		       MAP_SHARED, shm_descr, (off_t)0);
  if(shared_memory == MAP_FAILED) {
    close(shm_descr);
    return(1);
  }
  
  /* now we can get the value of max_buffer_size */
  max_buffer_size = *( (size_t *)(shared_memory 
				  + sizeof(*dsm_nproc_notify_max_p)));
  printf("max_buffer_size = %d\n", max_buffer_size);
  
  /* now we gotta close up the shm so we can reopen it at the correct
     size */
  munmap(shared_memory, 
	 sizeof(*dsm_nproc_notify_max_p) + sizeof(*max_buffer_size_p));
  close(shm_descr);
    

  
  /* reopen the shm */
  printf("reopening shared memory\n");
  shm_descr = shm_open(DSM_NOTIFY_TABLE_SHM_NAME, O_RDWR, S_IRWXU);
  if(shm_descr == -1) {
    perror("shm_open");
    return(1);
  }
    
  shm_size =
    sizeof(*dsm_nproc_notify_max_p)
    + sizeof(*max_buffer_size_p)
    + DSM_NPROC_NOTIFY_MAX * sizeof(struct notification_table_entry)
    + DSM_NPROC_NOTIFY_MAX * max_buffer_size;
  

  /* set shm size */
  printf("setting shared memory size to %d\n", shm_size);
  s = ftruncate(shm_descr, (off_t)shm_size);
  if(s!=0) {
    s = close(shm_descr);
    return(1);
  }

  /* map it */
  shared_memory = mmap(0, shm_size, PROT_READ | PROT_WRITE,
		       MAP_SHARED, shm_descr, (off_t)0);
  if(shared_memory == MAP_FAILED) {
    close(shm_descr);
    return(1);
  }
    
  /* set up the pointers into the shared memory */
  dsm_nproc_notify_max_p = (int *)shared_memory;
  
  max_buffer_size_p = (size_t *)(shared_memory
				 +sizeof(*dsm_nproc_notify_max_p));
  
  notification_table =
    (struct notification_table_entry *)(shared_memory 
					+ sizeof(int)
					+ sizeof(size_t));
  
  data_buffers = (char *)notification_table
    + *dsm_nproc_notify_max_p * sizeof(*notification_table);


  /* now print out the tables */
  printf("Shared memory segment %s contents:\n",DSM_NOTIFY_TABLE_SHM_NAME);
  printf("\n");
  printf("*dsm_nproc_notify_max_p = %d\n",*dsm_nproc_notify_max_p);
  printf("*max_buffer_size_p      = %d\n", *max_buffer_size_p);
  printf("\n");
  printf("Allocation table:\n");

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  printf("#  pid   tid        semid  s  nwt time       sz  host                 alloc\n");
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
  printf("#  pid tid s  time       sz  host                 alloc\n");
#endif

  for(i=0; i<*dsm_nproc_notify_max_p; i++) {
#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
    s = semctl(notification_table[i].semid, i, GETVAL, &dummy_semarg);
    if(s==-1) sem=99;
    else      sem=s;

    s = semctl(notification_table[i].semid, i, GETNCNT, &dummy_semarg);

    printf("%-2d %-5d %-10d %-6d %-2d %-3d %-10d %-3d %-20s %-20s\n",
	   i,
	   (int)notification_table[i].pid,
	   (int)notification_table[i].tid,
	   notification_table[i].semid,
	   sem,
	   s,
	   notification_table[i].when_set,
	   notification_table[i].size,
	   notification_table[i].hostname,
	   notification_table[i].allocname);
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
    s = sem_getvalue(&notification_table[i].sem, &sem);
    if(s!=0) sem=99;

    printf("%-2d %-3d %-3d %-2d %-10d %-3d %-20s %-20s\n",
	   i,
	   (int)notification_table[i].pid,
	   (int)notification_table[i].tid,
	   sem,
	   notification_table[i].when_set,
	   notification_table[i].size,
	   notification_table[i].hostname,
	   notification_table[i].allocname);
#endif
  }



  munmap(shared_memory, shm_size);

  s = close(shm_descr);
  if(s!=0) {
    perror("close()");
    return(1);
  }

  return(0);
}
