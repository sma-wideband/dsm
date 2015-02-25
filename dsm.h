/*
** dsm.h
**
** Header file for SMA distributed shared memory (dsm).  This file
** contains both internal definitions and prototypes and those needed by
** users writing applications.  To get the internal information, #define
** the macro _DSM_INTERNAL before including this file.  Otherwise this
** file can be #included straightforwardly.
**
** Charlie Katz, 16 Mar 2001
**
** $Id: dsm.h,v 2.6 2013/03/01 21:01:39 ckatz Exp $
*/

#ifndef _DSM_H
#define _DSM_H



#ifdef _DSM_INTERNAL

#  include <pthread.h>
#  include <rpc/rpc.h>
#  include <semaphore.h>

/* which kind of semaphore do we use in the notification table? */
#  undef NOT_TAB_SEM_TYPE
#  define USE_POSIX_SEM 1
#  define USE_SYSV_SEM  2

#  if defined __linux__
#    define NOT_TAB_SEM_TYPE USE_SYSV_SEM
#  elif defined __Lynx__
#    define NOT_TAB_SEM_TYPE USE_POSIX_SEM
#  else
#    error No semaphore type specified
#  endif

#  if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
#    include <sys/sem.h>
#    include <sys/ipc.h>
#    ifdef _SEM_SEMUN_UNDEFINED
       /* I don't know why we have to define this ourselves, but we do */
       union semun {
         int val;                    /* value for SETVAL */
         struct semid_ds *buf;       /* buffer for IPC_STAT, IPC_SET */
         unsigned short int *array;  /* array for GETALL, SETALL */
         struct seminfo *__buf;      /* buffer for IPC_INFO */
       };
#    endif
#  endif
#endif

#include <time.h>

#define DSM_NAME_LENGTH 50

/* return codes */
#define DSM_SUCCESS         0 /* hooray                                   */
#define DSM_ERROR           1 /* boo hoo                                  */

#define DSM_UNAVAILABLE     2 /* can't connect to server                  */
#define DSM_NO_RESOURCE     3 /* can't open shared memory                 */
#define DSM_ALREADY_OPEN    4 /* don't run dsm_open() twice               */
#define DSM_NO_INIT         5 /* must run dsm_open() first                */
#define DSM_ALLOC_VERS      6 /* allocation version mismatch              */
#define DSM_NO_LOCAL_SVC    7 /* can't contact the local server           */
#define DSM_NO_REMOTE_SVC   8 /* can't contact the remote server          */
#define DSM_RPC_ERROR       9 /* something failed in the RPC system       */
#define DSM_LOCAL_SERVICE  10 /* this svc available to local machine only */
#define DSM_REMOTE_SERVICE 11 /* this svc available to remote host only   */
#define DSM_TARGET_INVALID 12 /* we're not sharing with requested host    */
#define DSM_NAME_INVALID   13 /* allocation name doesn't exist            */
#define DSM_TOO_MANY_PROC  14 /* too many procs waiting for notification  */
#define DSM_MON_LIST_EMPTY 15 /* nothing in monitor list                  */
#define DSM_IN_MON_LIST    16 /* host/alloc already in monitor list       */
#define DSM_NOT_IN_LIST    17 /* host/alloc not in monitor list           */
#define DSM_INTERRUPTED    18 /* read_wait() interrupted by signal        */
#define DSM_REQ_QUEUE_FULL 19 /* request queue is full; dropping request  */
#define DSM_STRUCT_INVALID 20 /* error with structure argument            */

#define DSM_INTERNAL_ERROR 99 /* oh no                                    */

#define DSM_HOST_INVALID DSM_TARGET_INVALID /* for backward compatibility */



/* structures */

/* callers use this to keep local versions of dsm structures */
struct dsm_local_structure_info {
  char   name[DSM_NAME_LENGTH];
  size_t size;
  int    n_elements;
  int    is_structure;
  struct {
    char    name[DSM_NAME_LENGTH];
    size_t  size;
    void   *datap;
  } *elements;
};
/* typedef for clients, who should never actually look inside by
   themselves; internally, we use the full structure declaration so we
   can remember that it's a structure */
typedef struct dsm_local_structure_info dsm_structure;

/* this is used for returning the allocation list to a client */
#define DSM_FULL_NAME_LENGTH (2*DSM_NAME_LENGTH+2)
struct dsm_allocation_list {
  /* the name of a host with which we share */
  char host_name[DSM_NAME_LENGTH];
  
  int n_entries;     /* number of entries in the array */
  char **alloc_list; /* array of strings with allocation names;
		        regular allocations just have a name;
			structure elements have a name like 
			allocname:elementname and are thus up to
			DSM_FULL_NAME_LENGTH bytes long */
};


/* prototypes */
int    dsm_open(void);
int    dsm_close(void);
int    dsm_read(char *, char *, void *, time_t *);
int    dsm_read_wait(char *, char *, void *);
int    dsm_write(char *, char *, void *);
int    dsm_write_notify(char *, char *, void *);
int    dsm_monitor(char *, char *, ...);
int    dsm_no_monitor(char *, char *);
int    dsm_clear_monitor(void);
int    dsm_get_allocation_list(int *, struct dsm_allocation_list **);
void   dsm_destroy_allocation_list(struct dsm_allocation_list **);
int    dsm_structure_init(dsm_structure *, char *);
void   dsm_structure_destroy(dsm_structure *);
int    dsm_structure_get_element(dsm_structure *, char *, void *);
int    dsm_structure_set_element(dsm_structure *, char *, void *);
void   dsm_error_message(int, char *);




/***********************************************/
/* stuff that only the internal dsm code needs */
/***********************************************/

#ifdef _DSM_INTERNAL

static char rcsid_hdr[] = "$Id: dsm.h,v 2.6 2013/03/01 21:01:39 ckatz Exp $";


/* this really shouldn't be openly available, but some of my test
   programs use it, so at least we can limit it to _DSM_INTERNAL
   programs
*/
int dsm_get_info(char *, char *, struct dsm_local_structure_info **);

/*
** macros
*/

/* this causes the inclusion of some workaround code in the threadpool
   management scheme */
#define BROKEN_THREADPOOL 1


#define DSM_EXCLUSIVE_NAME "excl_dsm"

#define DSM_DEFAULT_ALLOC_FILE "/global/dsm/dsm_allocation.bin"

#define DSM_ALLOC_FILE_ENV_NAME "DSM_ALLOCATION_FILE"

#define DSM_ACCESS_LOCK_SHM_NAME "/dsm_access_lock"

#define DSM_NOTIFY_TABLE_SHM_NAME "/dsm_notification_table"

#define DSM_NPROC_NOTIFY_MAX 20 /* maximum number of processes which can
				   be waiting for notification
				   simultaneously */

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
#  define DSM_SYSV_SEM_KEY ((key_t)0xDEDFAB3)
#endif


#define DSM_RPC_THREAD_POOL_COUNT 12 /* how many RPC client threads
				       should be in the pool */

#define DSM_REQ_QUEUE_LENGTH 64 /* length of queue which holds requests
				   ready to be serviced by the RPC client
				   threads */

#define DSM_CLIENT_CREATE_TIMEOUT \
      ((struct timeval){(time_t)3, (time_t)0}) /* sec, usec */
#define DSM_CLIENT_CALL_TIMEOUT   \
      ((struct timeval){(time_t)1, (time_t)0}) /* sec, usec */

/* how long we wait in dsm_read() to see whether a bad connection has
   been reestablished, giving us fresh data */
#define DSM_CLIENT_READ_RETRY_TIMEOUT   \
      ((struct timeval){(time_t)0, (time_t)400000}) /* sec, usec */

/* should the Nagle algorithm be disabled on sockets? */
#define DSM_NONAGLE 1

#define DSM_TRUE  1
#define DSM_FALSE 0

/* codes for keeping track of allocation version of other end */
#define DSM_VERSION_UNKNOWN  0
#define DSM_VERSION_MISMATCH 1

/* codes to tell decode_request() whether it's okay to accept a class
   name */
#define DSM_ALLOW_CLASS_AS_TARGET    DSM_TRUE
#define DSM_PROHIBIT_CLASS_AS_TARGET DSM_FALSE

/* other internal error codes */
#define DSM_GETARGS             1111
#define DSM_CONNECT_IN_PROGRESS 1112
#define DSM_BAD_VERSION         1113
#define DSM_NO_CONNECTION       1114
#define DSM_INTERNAL_CHANGE     1115 /* something changed in the server     */
#define DSM_RESUBMIT_REQ        1116 /* read_wait() should resubmit request */

/* mutex helpers */
/* give the address of the mutex and the name of the calling function */
#define MUTEX_LOCK(mutex_addr, function_name, host) {     \
  int ssssl;                                              \
  dprintf("%s(%s): locking mutex " #mutex_addr "\n",      \
          function_name, host);                           \
  ssssl = pthread_mutex_lock(mutex_addr);                 \
  if(ssssl != 0) {                                        \
    fprintf(stderr,                                       \
	    "%s(%s): pthread_mutex_lock() returned %d\n", \
	    function_name, host, ssssl);                  \
  }                                                       \
  else {                                                  \
    dprintf("%s(%s): locked mutex " #mutex_addr "\n",     \
            function_name, host);                         \
  }                                                       \
}

#define MUTEX_UNLOCK(mutex_addr, function_name, host) {     \
  int ssssu;                                                \
  dprintf("%s(%s): unlocking mutex " #mutex_addr "\n",      \
          function_name, host);                             \
  ssssu = pthread_mutex_unlock(mutex_addr);                 \
  if(ssssu != 0) {                                          \
    fprintf(stderr,                                         \
	    "%s(%s): pthread_mutex_unlock() returned %d\n", \
	    function_name, host, ssssu);                    \
  }                                                         \
  else {                                                    \
    dprintf("%s(%s): unlocked mutex " #mutex_addr "\n",     \
            function_name, host);                           \
  }                                                         \
}

/* compute the offset of an address from the base of the shared memory */
#define DSM_SHM_OFFSET(a) ((int)((void *)(a) - shared_memory))



/*
** structures
*/


/**************************************/
/* this is an actual allocation entry */
struct alloc_entry {
  struct alloc_list_head *owner; /* back pointer to alh that owns this alloc */

  char    name[DSM_NAME_LENGTH];
  size_t  size;          /* total size of data */
  int     n_elements;    /* how many elements make up this alloc */
  int     is_structure;  

  /* an allocation can contain one or more elements, each of which
     corresponds to an entry in the allocation file; this array will
     have length n_elements */
  struct alloc_element {
    char   name[DSM_NAME_LENGTH];
    size_t size;             /* total size of this element */
    int    n_sub_elements;   /* how many subelements make up this element */
    int    xdr_filter_index; /* xdr filter to use for each subelement */
    void  *datap;            /* the element's data */
  } *elements;
  
  time_t timestamp;      /* time of most recent update */

  pthread_mutex_t mutex; /* for protecting this datum */

  struct notification_table_reference *notification_list;
};

/**************************************/
/* this is a node for the allocation  */
/* for a single host.  The list       */
/* dangles from this, and is an array */
/* alloc_entry structures.            */
struct alloc_list_head {
  char machine_name[DSM_NAME_LENGTH];
  char machine_addr_string[DSM_NAME_LENGTH];
  u_long machine_addr; /* this will be kept in host byte order */

  u_long version;
  pthread_mutex_t version_mutex;

  /* RPC stuff */
  CLIENT *cl;  /* set to NULL if no connection */
  pthread_mutex_t client_call_mutex;
  pthread_mutex_t connect_mutex;
  int connect_in_progress;

  int remote_clock_offset; /* add this to a remote timestamp to find
			      what the local timestamp would be */
   
  int nallocs;                /* length of allocs array  */
  int nnames;                 /* total number of allocation names =
				 number of non-structure allocation +
				 total number of elements in structure
				 allocations */
  size_t largest;             /* size of largest alloc   */ 
  struct alloc_entry *allocs; /* array of length nallocs */
};


/* class list entry */
struct class_member {
  char machine_name[DSM_NAME_LENGTH];
  char machine_addr_string[DSM_NAME_LENGTH];
  u_long machine_addr; /* this will be kept in host byte order */
};

/* the top info of a class list */
struct class_entry {
  char class_name[DSM_NAME_LENGTH];
  int nmembers; /* number of machines in the class */

  struct class_member *machinelist;
};

/* track the aeps for a share */
struct class_share_alloc_entry {
  char allocname[DSM_NAME_LENGTH];
  struct alloc_entry **aepp;
};

/* share tracking */
struct class_share {
  struct class_entry *class;      /* gives class name and number of hosts */
  struct alloc_list_head **alhpp; /* alhps associated with class members  */

  int nallocs;                           /* no. of allocs in this share */
  struct class_share_alloc_entry *csaep; /* aeps for all the allocs     */
};


/* this holds decoded arguments from a client call */
struct decoded_request_args {
  struct class_share     *csp;  /* class to which request target resolved */
  struct class_share_alloc_entry *csaep; /* alloc in class share */

  struct alloc_list_head *alhp; /* host to which request target resolved */
  struct alloc_entry     *aep;
  void                   **datapp;
  int                     notify;
};


/* entries in the notification table in shared memory */
struct notification_table_entry {
  pid_t pid; /* we must track by pid and thread */
  int   tid;

#if NOT_TAB_SEM_TYPE==USE_SYSV_SEM
  int semid;
#elif NOT_TAB_SEM_TYPE==USE_POSIX_SEM
  sem_t sem;
#endif

  time_t when_set;

  char hostname[DSM_NAME_LENGTH];
  char allocname[DSM_NAME_LENGTH];
  size_t size;
};



/* the alloc table uses this to find things in the notification table */
struct notification_table_reference {
  int   index; /* index of the notification table entry */
  pid_t pid;   /* which process made the wait request   */
  int   tid;   /* thread within pid                     */

  struct alloc_entry *aep; /* sometime we need to know which alloc owns
			      us */

  struct notification_table_reference *next;
  struct notification_table_reference *prev;  /* for linked list of
						 pid/tid which are
						 waiting on this
						 allocation; we call
						 this the "list" */

  /* these are for the doubly-linked circular list of
     notification_table_reference structures which all belong to a
     specific pid/tid; we call this a "notification ring" */
  struct notification_table_reference *left;
  struct notification_table_reference *right;
};



/* 
** prototypes 
*/

/* why isn't this defined in the RPC header files? */
extern void xdr_free(xdrproc_t, char *);

int dprintf(char *, ...);
int compare_alloc_entries(const void *, const void *);
int compare_alloc_list_head_addr(const void *, const void *);
int compare_alloc_list_head_name(const void *, const void *);
int compare_class_share_entries(const void *, const void *);
int compare_class_share_allocnames(const void *, const void *);

struct class_share             *lookup_class_name(char *);
struct class_share_alloc_entry *lookup_class_share_alloc_entry(
						struct class_share *, char *);
struct alloc_list_head *lookup_machine_name(char *);
struct alloc_list_head *lookup_machine_addr(u_long);
struct alloc_entry     *lookup_allocation(struct alloc_list_head *, char *);

bool_t dsm_return_status_code(SVCXPRT *, int);

void dsm_robust_connect(struct alloc_list_head *);
int  dsm_robust_client_call(struct alloc_list_head *,
			   u_long,
			   xdrproc_t,
			   char *,
			   xdrproc_t,
			   char *);
int  decode_request(SVCXPRT *, struct decoded_request_args **, int);
int  duplicate_decoded_request_args(struct decoded_request_args **,
				    struct decoded_request_args *);
void free_decoded_request_args(struct decoded_request_args *);
void dsm_launch_connection_thread(struct alloc_list_head *);

int  dsm_determine_network_info(u_long **, int *, char *);
void dsm_determine_alloc_file_location(char *);
int  dsm_read_allocation_file(struct alloc_list_head **, u_long *,
			      struct class_entry **,     u_long *,
			      struct class_share **,     u_long *,
			      u_long *,
			      u_long [],
			      int,
			      u_long *,
			      char *);
int  dsm_initialize_notification_table(size_t);
int  dsm_access_lock(void);
void dsm_dispatcher(struct svc_req *,  SVCXPRT *);
void allocation_version_check(SVCXPRT *, struct alloc_list_head *);
void allocation_sync_reply(SVCXPRT *, struct alloc_list_head *);
void local_alloc_list_request(SVCXPRT *);
void local_query(SVCXPRT *, int);
void local_read_wait(SVCXPRT *);
void local_write(SVCXPRT *, struct alloc_list_head *);
void remote_write_getter(SVCXPRT *);
void dsm_notify_waiting_procs(char *, struct alloc_entry *);
int  destroy_notification_ring(struct notification_table_reference *);
void dsm_sync_allocations(struct alloc_list_head *, 
			  struct alloc_list_head *);

int dsm_get_allocation_list_unlocked(int *, struct dsm_allocation_list **);
void dsm_destroy_allocation_list_unlocked(struct dsm_allocation_list **);



/* create_notification_ring() is declared below because it needs one of
   the RPC structures */

/*************/
/* RPC stuff */
/*************/

/* general purpose data structure for dsm transactions */
struct dsm_transaction {
  char *targetname;
  char *allocname;
  
  u_int data_size;
  u_int n_elements;
  u_int is_structure;

  struct alloc_element *elements;

  u_long timestamp;
  int notify;
  int status; /* for transmitting error codes */
};


/* a read wait request needs lots of special information, so it gets its
   own data structure */
struct dsm_read_wait_req {
  u_int pid;
  u_int tid;

  u_int num_alloc;
  u_int string_size;

  char *hostnames;
  char *allocnames;
};


int create_notification_ring(struct dsm_read_wait_req *,
			     struct alloc_entry **,
			     int);

#define DSM_WRITE_REBUFFED DSM_ERROR


#define DSM      ((u_long)0x20006666)
#define DSM_VERS ((u_long)4)

/* RPC commands available */
#define DSM_ALLOC_VERSION_CHECK  ((u_long)1)
#define DSM_ALLOC_SYNC           ((u_long)2)
#define DSM_ALLOC_INFO_QUERY     ((u_long)3)
#define DSM_LOCAL_READ           ((u_long)4)
#define DSM_LOCAL_READ_WAIT      ((u_long)5)
#define DSM_LOCAL_WRITE          ((u_long)6)
#define DSM_LOCAL_WRITE_NOTIFY   ((u_long)7)
#define DSM_REMOTE_WRITE         ((u_long)8)
#define DSM_LOCAL_ALLOC_LIST_REQ ((u_long)9)


/* indices describing which xdr filter to use */
#define	XDR_VOID          0
#define	XDR_FILTER_OPAQUE 1
#define	XDR_FILTER_CHAR   2
#define XDR_FILTER_SHORT  3 
#define XDR_FILTER_LONG   4
#define XDR_FILTER_FLOAT  5
#define XDR_FILTER_DOUBLE 6
#define XDR_FILTER_STRING 7


bool_t xdr_dsm_transaction(XDR *, struct dsm_transaction *);
bool_t xdr_dsm_alloc_entry_array(XDR *, struct alloc_list_head *);
bool_t xdr_dsm_read_wait_req(XDR *, struct dsm_read_wait_req *);
bool_t xdr_dsm_allocation_list(XDR *, void *);

#endif  /* _DSM_INTERNAL defined */


#endif  /* _DSM_H defined */
