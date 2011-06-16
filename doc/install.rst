GraphServ: Installation
=======================

The Graph Processor project aims to develop an infrastructure for rapidly analyzing and evaluating Wikipedia's category structure. The `GraphCore <https://github.com/jkroll20/graphserv/>`_ component maintains and processes large directed graphs in memory. `GraphServ <https://github.com/jkroll20/graphserv/>`_ handles access to running GraphCore instances.

This file documents getting, building and running GraphServ.


Getting the Code
----------------

Prerequisites:
	- `git <http://git-scm.com/>`_

To clone a read-only copy of the GraphServ repository, use the following command: ::

	$ git clone --recursive git://github.com/jkroll20/graphserv.git

The `--recursive` switch will automatically clone the required GraphCore repository as a submodule.

Optionally, you can check out the commit which this documentation refers to: ::

	$ cd graphserv
	$ git checkout <todo:tested-commit-or-tag>
	$ git submodule update



Building
--------

Prerequisites:
	- GNU Toolchain (make, g++, libc)
	- GNU `Readline <http://cnswww.cns.cwru.edu/php/chet/readline/rltop.html>`_.

The build process does not involve the use of any autofrobnication scripts. To compile the code, simply run: :: 

	$ make

This will build debug and release binaries of GraphServ and GraphCore. 

The code should build and run on 32-Bit Linux and 64-Bit Solaris systems. Care was taken to ensure compatibility to other Unix-ish systems. If the code does not build or run on your platform, please drop me a line.


Running GraphServ
-----------------

By default, GraphServ will look for the GraphCore binary in the subdirectory `graphcore`. Default values for TCP ports and authentication files are also set at compile time. You may want to override these defaults using one of the command line options. ::

	use: graphserv [options]
	options:
	    -h              print this text
	    -t PORT         listen on PORT for tcp connections [6666]
	    -H PORT         listen on PORT for http connections [8090]
	    -p FILENAME     set htpassword file name [gspasswd.conf]
	    -g FILENAME     set group file name [gsgroups.conf]
	    -c FILENAME     set path of GraphCore binary [./graphcore/graphcore]
	    -l FLAGS        set logging flags. 
	                    	e: log error messages (default)
        	            	i: log error and informational messages
        	            	a: log authentication messages
        	            	q: quiet mode, don't log anything
			    flags can be combined.

Before spawning a GraphCore instance, the child process will chdir() to the directory where graphcore resides. Currently, any redirected output will be written to that directory. Server log messages are written to stderr.


| 
| 
| `GraphServ, GraphCore (C) 2011 Wikimedia Deutschland, written by Johannes Kroll <jkroll at lavabit com>.`
| `Last update to this text: 2011/06/08`


