// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011
// defines and constants used in the server.

#ifndef CONST_H
#define CONST_H


// default values for tcp & http listen ports
#define DEFAULT_TCP_PORT    6666
#define DEFAULT_HTTP_PORT   8090

// listen backlog: how large may the queue of incoming connections grow
#define LISTEN_BACKLOG      100

// default filenames for htpasswd file, group file, and core binary
#define DEFAULT_HTPASSWD_FILENAME   "gspasswd.conf"
#define DEFAULT_GROUP_FILENAME      "gsgroups.conf"
#define DEFAULT_CORE_PATH           "./graphcore/graphcore"


// the command status codes, including those used in the core.
enum CommandStatus
{
    CORECMDSTATUSCODES,
    // server-only status codes:
    CMD_ACCESSDENIED,       // insufficient access level for command
    CMD_NOT_FOUND,          // "command not found" results in a different HTTP status code, therefore it needs its own code.
};
// NOTE: these entries must match the status codes above.
static const string statusMsgs[]=
{ CORECMDSTATUSSTRINGS, DENIED_STR, FAIL_STR };


// the connection type of session contexts.
enum ConnectionType
{
    CONN_TCP= 0,
    CONN_HTTP
};




#endif // CONST_H
