/*
** read a value from dsm
**
** $Id: read.c,v 2.1 2006/02/09 14:47:01 ckatz Exp $
*/


#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

  int s, size, is_struct=DSM_FALSE,i,j;
  char *type,elname[DSM_NAME_LENGTH],name[DSM_NAME_LENGTH],host[DSM_NAME_LENGTH];
  time_t t;

  char *cp,*rdbuf;
  dsm_structure ds;

  elname[0] = '\0';

  if(   (argc!=4 && argc!=5)
     || (argc==5 && (argv[1][0]!='-' || argv[1][1]!='v'))) {
    fprintf(stderr,
	    "Usage: %s [-v] <hostname> <allocation name> <type>\n",argv[0]);
    fprintf(stderr,
	    "       (type= (f)loat, (d)ouble, (i)nt, (s)hort, (c)har) (x)structure\n");
    exit(1);
  }

  if( (s=dsm_open())!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_open()");
    exit(1);
  }


  if(argv[1][0]=='-' && argv[1][1]=='v') {
    verbose = DSM_TRUE;
    printf("verbose on\n");
    argv++;
  }

  strcpy(host,argv[1]);
  strcpy(name,argv[2]);
  type = argv[3];

  /* is it a structure element? */
  if( (cp = strchr(name, ':')) != NULL ) {
    /* yes! separate the structure name from the element name */
    *cp = '\0';
    cp++;
    strcpy(elname, cp);
    is_struct = DSM_TRUE;
  }

  if(is_struct==DSM_TRUE && type[0]=='x') {
    fprintf(stderr, "Invalid type for structure element\n");
    exit(1);
  }

  if(type[0]=='c') size = atoi(type+1);
  else             size = 8;
    
  cp = (char *)malloc(size);
  if(cp==NULL) {
    fprintf(stderr,"Error: can't malloc(%d)\n", size);
    exit(1);
  }
  rdbuf=cp;

  if(is_struct==DSM_TRUE || type[0]=='x') {
    s = dsm_structure_init(&ds, name);
    if(s!=DSM_SUCCESS) {
      dsm_error_message(s, "dsm_structure_init");
      exit(1);
    }
    rdbuf = (char *)&ds;
  }

  s=dsm_read(host, name, rdbuf, &t);
  if(s!=DSM_SUCCESS) {
    dsm_error_message(s, "dsm_read()");
    exit(1);
  }

  if(is_struct==DSM_TRUE) {
    s = dsm_structure_get_element(&ds, elname, cp);
    if(s!=DSM_SUCCESS) {
      dsm_error_message(s, "dsm_structure_get_element()");
      exit(1);
    }
  }    

  switch(type[0]) {
  case 'f':
    printf("float value for host %s, \"%s\" is %f ", host, name, *(float *)cp);
    break;
  case 'd':
    printf("double value for host %s, \"%s\" is %f ", host,name,*(double *)cp);
    break;
  case 'i':
    printf("int value for host %s, \"%s\" is 0x%x ", host, name,*(int *)cp);
    break;
  case 's':
    printf("short value for host %s, \"%s\" is 0x%x ", host,name,*(short *)cp);
    break;
  case 'c':
    printf("string value for host %s, \"%s\" is %s ", host, name, cp);
    break;
    
  case 'x':
    free(cp);
    cp = malloc(ds.size);
    if(cp==NULL) {
      perror("malloc(ds.size)");
      exit(1);
    }
    printf("%s:\n",ds.name);
    for(i=0; i<ds.n_elements; i++) {
      s = dsm_structure_get_element(&ds, ds.elements[i].name, cp);
      if(s!=DSM_SUCCESS) {
	dsm_error_message(s, "dsm_structure_get_element");
	exit(1);
      }
      printf("  %s: ", ds.elements[i].name);
      if(ds.elements[i].size > 8) printf(cp);
      else for(j=0; j<ds.elements[i].size; j++) printf("%02x ", cp[j]);
      printf("\n");
    }

    break;

  default:
    fprintf(stderr, "type %c unknown\n", type[0]);
    exit(1);
  }
  printf("(last update %d)\n",(int)t);
  
  return(0);
}
