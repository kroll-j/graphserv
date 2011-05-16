CCFLAGS=-Wall -Igraphcore/src
LDFLAGS=-lreadline

all: 		Release Debug

Release:	graphserv
Debug:		graphserv.dbg

graphserv:	src/main.cpp graphcore/src/*.h
		g++ $(CCFLAGS) -O3 -fexpensive-optimizations src/main.cpp $(LDFLAGS) -ographserv

graphserv.dbg:	src/main.cpp graphcore/src/*.h
		g++ $(CCFLAGS) -DDEBUG_COMMANDS -ggdb src/main.cpp $(LDFLAGS) -ographserv.dbg

# updatelang: update the language files
# running this will generate changes in the repository
updatelang:	#
		./update-lang.sh

# test:		Release Debug
# 		python test/talkback.py test/graphserv.tb ./graphserv
