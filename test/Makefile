# Test suite for dsm
#
# $Id: Makefile,v 2.6 2012/05/21 15:32:24 ckatz Exp $
#

DSMDIR = $(GLOBAL)/dsm
DSMINC = $(GLOBALINC)/dsm.h
DSMLIB = $(COMMONLIB)/libdsm.a


LYNXOSCC     = gcc
LYNXOSCFLAGS = -g -Wall -mthreads -I$(GLOBALINC)
LYNXOSLIBS   = $(DSMLIB) -lrpc -lnetinet

LINUXCC     = gcc
LINUXCFLAGS = -g -Wall -I$(GLOBALINC) \
              -D_POSIX_SOURCE -D_BSD_SOURCE -D_SVID_SOURCE
LINUXLIBS   = $(DSMLIB) -lpthread -lrt


###################################################################
# pick the definitions based on the OS
ifeq ($(HOSTTYPE),i386-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  BINDIR   = x86linux-bin
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),i486-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  BINDIR   = x86linux-bin
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),i686-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  BINDIR   = x86linux-bin
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),x86_64-linux)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS) -m32
  LIBS     = $(LINUXLIBS)
  BINDIR   = x86_64-linux-bin
  OSFOUND  = 1
endif

# PowerPC Linux
ifeq ($(HOSTTYPE),powerpc)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  BINDIR   = ppclinux-bin
  OSFOUND  = 1
endif

ifeq ($(HOSTTYPE),lynxos-powerpc)
  HOSTDESC = $(HOSTTYPE)
  CC       = $(LYNXOSCC)
  CFLAGS   = $(LYNXOSCFLAGS)
  LIBS     = $(LYNXOSLIBS)
  BINDIR   = lynxos-bin
  OSFOUND  = 1
endif

# assume lynxos cross-development environment
ifeq ($(HOSTTYPE),sun4)
  HOSTDESC = "lynxos-powerpc (cross-compiled)"
  CC       = $(LYNXOSCC)
  CFLAGS   = $(LYNXOSCFLAGS)
  LIBS     = $(LYNXOSLIBS)
  BINDIR   = lynxos-bin
  OSFOUND  = 1
endif

# roach
UNAMER = $(shell uname -r)
ifeq ($(findstring borph, $(UNAMER)),borph)
  HOSTDESC = "roach"
  CC       = $(LINUXCC)
  CFLAGS   = $(LINUXCFLAGS)
  LIBS     = $(LINUXLIBS)
  BINDIR   = roach
  OSFOUND  = 1
endif

all: report bindir test


################################################
# say what we're doing
report:
ifndef OSFOUND
	@echo HOSTTYPE "$(HOSTTYPE)" unknown
	$(warning HOSTTYPE "$(HOSTTYPE)" unknown)
	$(error HOSTTYPE "$(HOSTTYPE)" unknown)
else
	@echo Building dsm test suite for $(HOSTDESC)
endif


###############################################
# keep the binaries for different platforms
# in different places
bindir:
	test -d $(BINDIR) || mkdir $(BINDIR)


#############################################
# the test suite programs

test: $(BINDIR)/read           \
      $(BINDIR)/read_wait      \
      $(BINDIR)/write          \
      $(BINDIR)/pound          \
      $(BINDIR)/unlinkshm      \
      $(BINDIR)/listshm        \
      $(BINDIR)/poundrec       \
      $(BINDIR)/poundsend      \
      $(BINDIR)/latency        \
      $(BINDIR)/get_alloc_list

$(BINDIR)/read: read.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ read.c $(LIBS)

$(BINDIR)/read_wait: read_wait.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ read_wait.c $(LIBS)

$(BINDIR)/write: write.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ write.c $(LIBS)

$(BINDIR)/pound: pound.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ pound.c $(LIBS)

$(BINDIR)/poundrec: poundrec.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ poundrec.c $(LIBS)

$(BINDIR)/poundsend: poundsend.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ poundsend.c $(LIBS)

$(BINDIR)/unlinkshm: unlinkshm.c $(DSMINC)
	$(CC) $(CFLAGS) -o $@ unlinkshm.c $(LIBS)

$(BINDIR)/listshm: listshm.c $(DSMINC)
	$(CC) $(CFLAGS) -o $@ listshm.c $(LIBS)

$(BINDIR)/latency: latency.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ latency.c $(LIBS)

$(BINDIR)/get_alloc_list: get_alloc_list.c $(DSMINC) $(DSMLIB)
	$(CC) $(CFLAGS) -o $@ get_alloc_list.c $(LIBS)


##############
# cleanup 
clean:
	/bin/rm -f *.o *~ core*                    \
			  $(BINDIR)/read           \
			  $(BINDIR)/write          \
			  $(BINDIR)/pound          \
			  $(BINDIR)/read_wait      \
			  $(BINDIR)/listshm        \
		          $(BINDIR)/unlinkshm      \
	   		  $(BINDIR)/poundrec       \
			  $(BINDIR)/poundsend      \
			  $(BINDIR)/get_alloc_list \
			  $(BINDIR)/latency
