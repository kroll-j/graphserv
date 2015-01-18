// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011, 2012
// defines and constants used in the server.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

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


// log levels for flog(). LOG_CRIT is always printed, other levels can be individually enabled on the command line.
enum Loglevel
{
    LOG_INFO,
    LOG_ERROR,
    LOG_AUTH,
    LOG_CRIT
};


#endif // CONST_H
