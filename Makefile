# on solaris, we need -lsocket for socket functions. check for libsocket.
ifneq ($(shell nm -DP /lib/libsocket.so* 2>/dev/null | grep -v UNDEF | grep '^accept[[:space:]]*'),)
	SOCKETLIB=-lsocket -lnsl
else
	SOCKETLIB=
endif

CCFLAGS=-Wall -std=c++0x -Igraphcore/src
LDFLAGS=-lcrypt $(SOCKETLIB)

all: 		Release Debug

Release:	graphserv
Debug:		graphserv.dbg

graphcore/graphcore:	graphcore/src/*
		make -C graphcore STDERR_DEBUGGING=$(STDERR_DEBUGGING) Release

graphcore/graphcore.dbg:	graphcore/src/*
		make -C graphcore Debug

graphserv:	src/main.cpp src/*.h graphcore/src/*.h graphcore/graphcore
		g++ $(CCFLAGS) -DSESSIONCONTEXTLINEQUEUING=$(SESSIONCONTEXTLINEQUEUING) -O3 -march=native src/main.cpp $(LDFLAGS) -ographserv

graphserv.dbg:	src/main.cpp src/*.h graphcore/src/*.h graphcore/graphcore
		g++ $(CCFLAGS) -DSESSIONCONTEXTLINEQUEUING=$(SESSIONCONTEXTLINEQUEUING) -DDEBUG_COMMANDS -ggdb src/main.cpp $(LDFLAGS) -ographserv.dbg

# updatelang: update the language files
# running this will generate changes in the repository
updatelang:	#
		./update-lang.sh

# test:		Release Debug
# 		python test/talkback.py test/graphserv.tb ./graphserv

