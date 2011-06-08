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

GraphServ contains a rudimentary HTTP Server which implements a subset of HTTP/1.0. The HTTP Server accepts GET requests. One command can be executed per request. The Server will close the connection after replying to the request. 

As a convenience, an HTTP/1.1 version string will also be accepted in GET requests. The version string in the GET request does not change the behaviour of the server or the contents of the Response.

In principle, an HTTP client can execute any core or server command. However, because the client is disconnected after executing the first command, an HTTP client can never execute a command which needs an access level above `read`. Also, HTTP clients cannot execute any command which takes a data set. These limitations could be removed in the future by implementing Keep-Alive connections (the default in HTTP/1.1), and/or POST.

Executing Server Commands
+++++++++++++++++++++++++

Executing Core Commands
+++++++++++++++++++++++

HTTP Header and Status Code
+++++++++++++++++++++++++++




| 
| 
| `GraphServ, GraphCore (C) 2011 Wikimedia Deutschland, written by Johannes Kroll <jkroll at lavabit com>.`
| `Last update to this text: 2011/06/08`


