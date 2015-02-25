#include <stdarg.h>
#include <stdio.h>

#define _DSM_INTERNAL
#include "dsm.h"

/***********************/
/* diagnostic printf() */
/***********************/

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
