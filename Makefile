# on solaris, we need -lsocket for socket functions. check for libsocket.
ifneq ($(shell nm -DP /lib/libsocket.so* 2>/dev/null | grep -v UNDEF | grep '^accept[[:space:]]*'),)
	SOCKETLIB=-lsocket
else
	SOCKETLIB=
endif

CCFLAGS=$(CFLAGS) -Wall -Wstrict-overflow=3 -std=c++0x -Igraphcore/src -DSYSTEMPAGESIZE=$(shell getconf PAGESIZE)
LDFLAGS=-lcrypt $(SOCKETLIB)

all: 		Release Debug

Release:	graphserv
Debug:		graphserv.dbg

graphcore/graphcore:	graphcore/src/*
		+make -C graphcore STDERR_DEBUGGING=$(STDERR_DEBUGGING) USE_MMAP_POOL=$(USE_MMAP_POOL) Release

graphcore/graphcore.dbg:	graphcore/src/*
		+make -C graphcore STDERR_DEBUGGING=$(STDERR_DEBUGGING) USE_MMAP_POOL=$(USE_MMAP_POOL) Debug

graphserv:	src/main.cpp src/*.h graphcore/src/*.h graphcore/graphcore
		g++ $(CCFLAGS) -O3 -march=native src/main.cpp $(LDFLAGS) -ographserv

graphserv.dbg:	src/main.cpp src/*.h graphcore/src/*.h graphcore/graphcore.dbg
		g++ $(CCFLAGS) -DDEBUG_COMMANDS -ggdb src/main.cpp $(LDFLAGS) -ographserv.dbg

# updatelang: update the language files
# running this will generate changes in the repository
updatelang:	#
		./update-lang.sh

clean:		#
		-rm graphserv graphserv.dbg graphcore/graphcore graphcore/graphcore.dbg

# test:		Release Debug
# 		python test/talkback.py test/graphserv.tb ./graphserv

.PHONY:		updatelang clean
