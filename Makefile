CCFLAGS=-Wall -Igraphcore/src
LDFLAGS=-lcrypt

all: 		Release Debug

Release:	graphserv
Debug:		graphserv.dbg

graphcore/graphcore:	graphcore/src/*
		make -C graphcore Release

graphcore/graphcore.dbg:	graphcore/src/*
		make -C graphcore Debug

graphserv:	src/main.cpp graphcore/src/*.h graphcore/graphcore
		g++ $(CCFLAGS) -O3 -fexpensive-optimizations src/main.cpp $(LDFLAGS) -ographserv

graphserv.dbg:	src/main.cpp graphcore/src/*.h graphcore/graphcore
		g++ $(CCFLAGS) -DDEBUG_COMMANDS -ggdb src/main.cpp $(LDFLAGS) -ographserv.dbg

# updatelang: update the language files
# running this will generate changes in the repository
updatelang:	#
		./update-lang.sh

# test:		Release Debug
# 		python test/talkback.py test/graphserv.tb ./graphserv

