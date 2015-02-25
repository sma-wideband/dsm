/*
** write a value to dsm
**
** $Id: write.c,v 2.1 2006/02/09 14:48:08 ckatz Exp $
*/


#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define _DSM_INTERNAL
#include "dsm.h"

int verbose=DSM_FALSE;
int dprintf(char *format, ...) {
  extern int verbose;
  va_list ap;
  int s=0;

  if(verbose) {
    va_start(ap, format);
    s = vfprintf(stderr, format, ap);
    va_end(ap);
    fflush(stderr);
  }

  return(s);
}

int main(int argc, char *argv[]) {

  int s, size, notify, is_struct=0;
  char element[DSM_NAME_LENGTH],name[DSM_NAME_LENGTH],host[DSM_NAME_LENGTH];
  char *p, *type, *val;
  dsm_structure ds;

  if(argc!=6 && argc!=7) {
    fprintf(stderr,
	    "Usage: %s [-v] <hostname> <allocation name> <type> "
	    "<value> <notify>\n",
	    argv[0]);
    fprintf(stderr,
	    "       (type= (f)loat, (d)ouble, (i)nt, (s)hort, (c)har)\n");
    fprintf(stderr, "notify=1  no notify=0\n");
    exit(1);
  }


  if(argc==6) {
    strcpy(host,argv[1]);
    strcpy(name,argv[2]);
    type   = argv[3];
    val    = argv[4];
    notify = atoi(argv[5]);
  }
  else if(argc==7 && argv[1][0]!='-' && argv[1][1]!='v') {
    fprintf(stderr,
	    "Usage: %s [-v] <hostname> <allocation name> <type> "
	    "<value> <notify>\n",
	    argv[0]);
    fprintf(stderr,
	    "       (type= (f)loat, (d)ouble, (i)nt, (s)hort, (c)har)\n");
    fprintf(stderr, "notify=1  no notify=0\n");
    exit(1);
  }
  else {
    strcpy(host,argv[2]);
    strcpy(name,argv[3]);
    type = argv[4];
    val = argv[5];
    notify = atoi(argv[6]);
    verbose = DSM_TRUE;
  }

  if( (s=dsm_open())!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_open()");
    exit(1);
  }

  /* is it a structure? */
  if((p = strchr(name, ':')) !=NULL) {
    /* yes! separate the structure name from the element name */
    *p = '\0';
    p++;
    strcpy(element, p);
    is_struct = DSM_TRUE;

    s = dsm_structure_init(&ds, name);
    if(s!=DSM_SUCCESS) {
      dsm_error_message(s, "dsm_structure_init");
      exit(1);
    }

    s = dsm_read(host, name, &ds, NULL);
    if(s!=DSM_SUCCESS) {
      dsm_error_message(s, "dsm_read");
      exit(1);
    }
  }

  if(type[0]=='c') size = atoi(type+1);
  else             size = 8;
    
  p = (char *)malloc(size);
  if(p==NULL) {
    fprintf(stderr,"Error: can't malloc(%d)\n", size);
    exit(1);
  }


  switch(type[0]) {
  case 'f':
    *((float *)p) = (float)atof(val);
    break;
  case 'd':
    *((double *)p) = atof(val);
    break;
  case 'i':
    *((unsigned int *)p) = (int)strtoul(val,NULL,0);
    break;
  case 's':
    *((short *)p) = (short)strtol(val,NULL,0);
    break;
  case 'c':
    strcpy(p,val);
    break;
  default:
    fprintf(stderr, "type %c unknown\n", type[0]);
    exit(1);
  }

  if(is_struct) {
    s = dsm_structure_set_element(&ds, element, p);
    if(s!=DSM_SUCCESS) {
      dsm_error_message(s, "dsm_structure_set_element");
      exit(1);
    }
    p = &ds;
  }


  if(notify==1) {
    s=dsm_write_notify(host, name, p);
    if(s!=DSM_SUCCESS) {
      dsm_error_message(s, "dsm_write_notify()");
      exit(1);
    }
  }
  else {
    s=dsm_write(host, name, p);
    if(s!=DSM_SUCCESS) {
      dsm_error_message(s, "dsm_write()");
      exit(1);
    }
  }


  return(0);
}
