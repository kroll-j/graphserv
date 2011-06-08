GraphServ: Usage
================

The Graph Processor project aims to develop an infrastructure for rapidly analyzing and evaluating Wikipedia's category structure. The `GraphCore <https://github.com/jkroll20/graphserv/>`_ component maintains and processes large directed graphs in memory. `GraphServ <https://github.com/jkroll20/graphserv/>`_ handles access to running GraphCore instances.

This file documents GraphServ usage.

GraphServ Commands
------------------

Authorization
-------------

TCP Connections
---------------

HTTP Connections
----------------

GraphServ contains a rudimentary HTTP Server which implements a subset of `HTTP/1.0 <http://www.w3.org/Protocols/rfc1945/rfc1945>`_. The HTTP Server accepts GET requests. One command can be executed per request. The server will close the connection after replying to the request. 

As a convenience, an HTTP/1.1 version string will also be accepted in GET requests. The version string in the GET request does not change the behaviour of the server or the contents of the response.

In principle, an HTTP client can execute any core or server command. However, because the client is disconnected after executing the first command, an HTTP client can never execute a command which needs an access level above *read*. Also, HTTP clients cannot execute any command which takes a data set. These limitations could be removed in the future by implementing Keep-Alive connections (the default in HTTP/1.1), and/or POST.

The request must follow the form *GET Request-URI Version-String CRLF <header fields> CRLF*. Any header fields following the Request-Line are read and discarded.

The Request-URI can include `percent-encoded <http://en.wikipedia.org/wiki/Percent-encoding>`_ characters. Any '+' characters in the Request-URI will be translated to space (0x20).


Executing Server Commands
+++++++++++++++++++++++++

To execute a server command, simply include the command string in the Request-URI. Example: ::

	$ curl http://localhost:8090/help	# use curl to print help text of GraphServ on localhost listening on the default port.
	GET /help HTTP/1.0			# corresponding Request-Line.

Executing Core Commands
+++++++++++++++++++++++

To send a command to a core, include its name in the Request-URI. Separate core name and command by a forward slash. Example: ::
	
	$ curl http://localhost:8090/core0/list-predecessors+7	# print direct predecessors of node 7 in core0 on localhost.
	GET /core0/list-predecessors+7				# corresponding Request-Line.

HTTP Header and Status Code
+++++++++++++++++++++++++++




| 
| 
| `GraphServ, GraphCore (C) 2011 Wikimedia Deutschland, written by Johannes Kroll <jkroll at lavabit com>.`
| `Last update to this text: 2011/06/08`


