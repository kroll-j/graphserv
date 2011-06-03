// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011

#include <libintl.h>
#include <iostream>
#include <vector>
#include <queue>
#include <map>
#include <set>
#include <fcntl.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <stdarg.h>
#include <algorithm>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clibase.h"


// defaults for tcp & http listen ports
#define DEFAULT_TCP_PORT    6666
#define DEFAULT_HTTP_PORT   8090
// listen backlog: how large may the queue of incoming connections grow
#define LISTEN_BACKLOG      100

// default filenames for htpasswd and group file
#define DEFAULT_HTPASSWD_FILENAME   "gspasswd.conf"
#define DEFAULT_GROUP_FILENAME      "gsgroups.conf"


// the command status codes, including those used in the core.
enum CommandStatus
{
    CORECMDSTATUSCODES,
    // server only status codes:
    CMD_ACCESSDENIED,       // insufficient access level for command
    CMD_NOT_FOUND,          // "command not found" results in a different HTTP status code, therefore it needs its own code.
};
// NOTE: these entries must match the status codes above.
static const string statusMsgs[]=
{ CORECMDSTATUSSTRINGS, DENIED_STR, FAIL_STR };


enum AccessLevel
{
    ACCESS_READ= 0,
    ACCESS_WRITE,
    ACCESS_ADMIN
};
// these must match the access levels.
static const char *gAccessLevelNames[]=
    { "read", "write", "admin" };

enum ConnectionType
{
    CONN_TCP= 0,
    CONN_HTTP
};


__attribute__((unused))
static const string& getStatusString(CommandStatus status)
{
    return statusMsgs[status];
}

static CommandStatus getStatusCode(const string& msg)
{
    for(unsigned i= 0; i<sizeof(statusMsgs)/sizeof(statusMsgs[0]); i++)
        if(statusMsgs[i]==msg)
            return (CommandStatus)i;
    fprintf(stderr, "getStatusCode called with bad string %s. Please report this bug.\n", msg.c_str());
    return CMD_FAILURE;
}



// abstract base class for authorities
class Authority
{
    public:
        Authority() { }
        virtual ~Authority() { }

        virtual string getName()= 0;
        // authorize a user using given credentials: set its access level to the appropriate value on success. return value indicates success.
        virtual bool authorize(class SessionContext& sc, const string& credentials)= 0;
};

// the password authority reads from a htpasswd file and corresponding group file.
// authorize() takes a string in the form "user:password".
class PasswordAuth: public Authority
{
    string htpasswdFilename;
    string groupFilename;

    struct userInfo
    {
        string hash;                // the crypt()ed password
        AccessLevel accessLevel;    // maximum access level
    };
    map<string,userInfo> users;
    time_t lastCacheRefresh;

    vector<string> splitLine(string line, char sep= ':');
    void refreshFileCache();
    bool readCredentialFiles();

    public:
        PasswordAuth(const string& _htpasswdFilename, const string& _groupFilename):
            htpasswdFilename(_htpasswdFilename), groupFilename(_groupFilename), lastCacheRefresh(0)
        { }

        string getName() { return "password"; }
        bool authorize(class SessionContext& sc, const string& credentials);
};


// time measurement
double getTime()
{
    timeval tv;
    if(gettimeofday(&tv, 0)<0) perror("gettimeofday");
    return tv.tv_sec + tv.tv_usec*0.000001;
}



class ServCmd: public CliCommand
{
    public:
        virtual AccessLevel getAccessLevel() { return ACCESS_READ; }
};


class ServCli: public Cli
{
    public:
        ServCli(class Graphserv &_app);

        void addCommand(ServCmd *cmd)
        { commands.push_back(cmd); }

        CommandStatus execute(string command, class SessionContext &sc);
        CommandStatus execute(class ServCmd *cmd, vector<string> &words, class SessionContext &sc);

    private:
        class Graphserv &app;
};


// cli commands which do not return any data.
class ServCmd_RTVoid: public ServCmd
{
    public:
        ReturnType getReturnType() { return RT_NONE; }
        virtual CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)= 0;
};

// cli commands which return some other data set. execute() must write the result to the client.
class ServCmd_RTOther: public ServCmd
{
    public:
        ReturnType getReturnType() { return RT_OTHER; }
        virtual CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)= 0;
};


static bool setNonblocking(int fd, bool on= true)
{
	int opts= fcntl(fd, F_GETFL);
	if(opts<0)
    {
		perror("fcntl(F_GETFL)");
		return false;
	}
	if(on) opts|= O_NONBLOCK;
	else opts&= (~O_NONBLOCK);
	if(fcntl(fd, F_SETFL, opts)<0)
	{
		perror("fcntl(F_SETFL)");
		return false;
	}
	return true;
}

static bool closeOnExec(int fd)
{
    int opts= fcntl(fd, F_GETFD);
    if(opts<0)
    {
        perror("fcntl(F_GETFD)");
        return false;
    }
    opts|= FD_CLOEXEC;
    if(fcntl(fd, F_SETFD, opts)<0)
    {
        perror("fcntl(F_SETFD)");
        return false;
    }
    return true;
}

class NonblockWriter
{
    public:
        NonblockWriter(): fd(-1) {}

        void setWriteFd(int _fd) { fd= _fd; setNonblocking(fd); }

        bool flush()
        {
            while(!buffer.empty())
            {
                size_t sz= writeString(buffer.front());
                if(sz==buffer.front().size())
                    buffer.pop_front();
                else
                {
                    buffer.front().erase(0, sz);
                    return false;
                }
            }
            return true;
        }

        bool writeBufferEmpty()
        { return buffer.empty(); }

        void write(const string s)
        {
//            cout << "writing " << s << endl << std::flush;
            buffer.push_back(s);
            flush();
        }

        void writef(const char *fmt, ...)
        {
            char c[2048];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(c, sizeof(c), fmt, ap);
            va_end(ap);
            write(c);
        }

        size_t getWritebufferSize()
        {
            size_t ret= 0;
            for(deque<string>::iterator it= buffer.begin(); it!=buffer.end(); it++)
                ret+= it->length();
            return ret;
        }

        virtual void writeFailed(int _errno)= 0;

    private:
        int fd;
        deque<string> buffer;

        size_t writeString(const string& s)
        {
//            cout << "writing " << s << ::flush;
//            ssize_t charsLeft= s.size();
//            while(charsLeft)
//            {
//                ssize_t sz= ::write(fd, s.data(), s.size());
//                if(sz<0)
//                {
//                    if( (errno!=EAGAIN)&&(errno!=EWOULDBLOCK) )
//                        perror("write"),
//                        writeFailed(errno);
//                    return false;
//                }
//            }
//            return true;

            ssize_t sz= ::write(fd, s.data(), s.size());
            if(sz<0)
            {
                if( (errno!=EAGAIN)&&(errno!=EWOULDBLOCK) )
                    perror("write"),
                    writeFailed(errno);
                return 0;
            }
            return sz;
        }
};


struct CommandQEntry
{
	string command;         // command
	deque<string> dataSet;  // data set, if any
	uint32_t clientID;      // who runs this command
	bool acceptsData;       // command accepts an input data set (colon)?
	bool dataFinished;      // data set was terminated with empty line?

	CommandQEntry(): clientID(0), acceptsData(false), dataFinished(true)
	{ }

	bool flushable()
	{
	    return (!acceptsData) || dataFinished;
	}
};

class CoreInstance: public NonblockWriter
{
    public:
        string linebuf;     // data read from core gets buffered here.

        CoreInstance(uint32_t _id): instanceID(_id), lastClientID(0), expectingReply(false), expectingDataset(false)
        {
        }

        void writeFailed(int _errno)
        {
            perror("write failed");
//            exit(1); //xxxxx todo: do some sensible error handling (like marking the instance to be removed)
        }

        bool startCore(const char *command= "./graphcore/graphcore")
        {
            if(pipe(pipeToCore)==-1 || pipe(pipeFromCore)==-1)
            {
                setLastError(string("pipe(): ") + strerror(errno));
                return false;
            }

            pid= fork();
            if(pid==-1)
            {
                setLastError(string("fork(): ") + strerror(errno));
                return false;
            }
            else if(pid==0)
            {
                // child process (core)

                if(dup2(pipeToCore[0], STDIN_FILENO)==-1 || dup2(pipeFromCore[1], STDOUT_FILENO)==-1)
                    exit(101);  // setup failed

                close(pipeToCore[1]);
                close(pipeFromCore[0]);

                setlinebuf(stdout);

                if(execl(command, command, NULL)==-1)
                    exit(102);  // couldn't exec()
            }
            else
            {
                // parent process (server)

                close(pipeToCore[0]);
                close(pipeFromCore[1]);

                FILE *toCore= fdopen(pipeToCore[1], "w");
                FILE *fromCore= fdopen(pipeFromCore[0], "r");

                setlinebuf(toCore);

                char line[1024];

                fprintf(toCore, "protocol-version\n");
                if(fgets(line, 1024, fromCore))
                {
                    chomp(line);
                    if(strncmp(SUCCESS_STR, line, strlen(SUCCESS_STR))!=0)
                    {
                        setLastError(_("core replied: ") + string(line));
                        return false;
                    }
                    char *coreProtocolVersion= line + strlen(SUCCESS_STR);
                    while(isspace(*coreProtocolVersion) && *coreProtocolVersion) coreProtocolVersion++;
                    if(strcmp(coreProtocolVersion, stringify(PROTOCOL_VERSION))!=0)
                    {
                        setLastError(string(_("protocol version mismatch (server: ")) +
                                     stringify(PROTOCOL_VERSION) + " core: " + coreProtocolVersion + ")");
                        return false;
                    }
                    setWriteFd(pipeToCore[1]);
//                    closeOnExec(pipeToCore[1]);
//                    closeOnExec(pipeFromCore[0]);
                    return true;
                }
                else    // fgets() failed
                {
                    int status;
                    waitpid(pid, &status, 0);
                    if(WIFSIGNALED(status))
                        setLastError(_("child process terminated by signal"));
                    else if(WIFEXITED(status))
                    {
                        int estatus= WEXITSTATUS(status);
                        setLastError(string(_("child process exited: ")) +
                                     (estatus==101? "setup failed.":
                                      estatus==102? string("couldn't exec() '") + command + "'.":
                                      format("unknown error code %d", estatus)) );
                    }
                    else
                       setLastError("child process terminated");
                    return false;
                }
            }
            return false; // never reached. make compiler happy.
        }

        string getLastError() { return lastError; }

        void setLastError(string str) { lastError= str; }

        uint32_t getID() { return instanceID; }
        string getName() { return (name.length()? name: format("Core%02u", instanceID)); }
        void setName(string nm) { name= nm; }
        pid_t getPid() { return pid; }
        int getReadFd() { return pipeFromCore[0]; }
        int getWriteFd() { return pipeToCore[1]; }

        // find *last* command for this client in queue
        CommandQEntry *findLastClientCommand(uint32_t clientID)
        {
            for(commandQ_t::reverse_iterator it= commandQ.rbegin(); it!=commandQ.rend(); it++)
                if(it->clientID==clientID)
                    return & (*it);
            return 0;
        }

        void removeClientCommands(uint32_t clientID)
        {
            // xxx as it is, this is inefficient, and currently not used. remove?
            for(commandQ_t::iterator it= commandQ.begin(); it!=commandQ.end(); it++)
            {
                if(it->clientID==clientID)
                {
//                    printf("removing command: '%s", it->command.c_str());
                    commandQ.erase(it);
                    it= commandQ.begin();
                }
            }
        }

        // try writing out commands from queue to core process
        void flushCommandQ(class Graphserv &app);

        void queueCommand(string &cmd, uint32_t clientID, bool hasDataSet)
        {
            CommandQEntry ce;
            ce.acceptsData= hasDataSet;
            ce.dataFinished= false;
            ce.clientID= clientID;
            ce.command= cmd;
            commandQ.push_back(ce);
        }

        uint32_t getLastClientID()
        {
            return lastClientID;
        }

        // true if this core is running a command for this client or has a command for this client in its queue.
        bool hasDataForClient(uint32_t clientID)
        {
            return (lastClientID==clientID && (expectingReply||expectingDataset)) || findLastClientCommand(clientID);
        }

        void lineFromCore(string &line, class Graphserv &app);

    private:
        uint32_t instanceID;
        string lastError;
        string name;

        pid_t pid;
        int pipeToCore[2];      // writable from server
        int pipeFromCore[2];    // writable from core

        typedef deque<CommandQEntry> commandQ_t;
        deque<CommandQEntry> commandQ;

        uint32_t lastClientID;  // ID of client who executed the last command. ie: client who should receive output

        bool expectingReply;    // currently expecting a status reply from core (ok/failure/error)
        bool expectingDataset;  //          ''         a data set from core

        friend class ccInfo;
};

struct SessionContext: public NonblockWriter
{
    uint32_t clientID;
    AccessLevel accessLevel;
    ConnectionType connectionType;
    uint32_t coreID;    // non-zero if connected to a core instance
    int sockfd;
    string linebuf;
    class Graphserv &app;
    double chokeTime;

    struct HttpClientState
    {
        vector<string> request;
        string requestString;
    } http;

    struct Stats
    {
        double lastTime;
        union { struct {
        double linesSent, coreCommandsSent, servCommandsSent,
               bytesSent, dataRecordsSent, linesQueued;
        }; double values[6]; };
        Stats()
        { reset(); }
        void reset(double t= getTime())
        {
            lastTime= t;
            memset(values, 0, sizeof(values));
        }
        void normalize(double t= getTime())
        {
            double idt= 1.0/(t-lastTime);
            for(unsigned i= 0; i<sizeof(values)/sizeof(values[0]); i++)
                values[i]*= idt;
        }
    };
    Stats stats;


    SessionContext(class Graphserv &_app, uint32_t cID, int sock, ConnectionType connType= CONN_TCP):
        clientID(cID), accessLevel(ACCESS_ADMIN/*XXX change after pw auth*/), connectionType(connType), coreID(0), sockfd(sock), app(_app), chokeTime(0)
    {
        setWriteFd(sockfd);
    }

    virtual ~SessionContext()
    {
        setNonblocking(sockfd, false);  // force output to be drained on close.
        close(sockfd);
    }

    void writeFailed(int _errno);

// xxx remove?
//    string makeStatusLine(CommandStatus status, const string& message)
//    {
//        string ret= getStatusString(status) + "  " + message;
//        while(ret[ret.size()-1]=='\n') ret.resize(ret.size()-1);   // make sure we don't break the protocol by adding extraneous newlines
//        return ret;
//    }
//
//    // write out the status line. overridden by superclasses (such as the HTTP session context)
//    virtual void writeStatusline(CommandStatus status, const string& message)
//    {
//        // the default for TCP connections: just write out the line.
//        write(makeStatusLine(status, message + '\n'));
//    }

    // forward a status line from a core to the client.
    virtual void forwardStatusline(const string& line)
    {
        // default for tcp: just write out the line to the client.
        write(line);
    }

    virtual void forwardDataset(const string& line)
    {
        // default for tcp: just write out the line to the client.
        write(line);
    }

    // send string to client indicating that a command was not found.
    // there has to be a special case for this in the http handling code
    // to change the http status-code, therefore this is virtual.
    // 'text' must not be terminated by a newline.
    virtual void commandNotFound(const string& text)
    {
        writef("%s %s\n", FAIL_STR, text.c_str());
    }
};


struct HTTPSessionContext: public SessionContext
{
    bool conversationFinished;  // client will be disconnected when this is false and there's no buffered data left.

    HTTPSessionContext(class Graphserv &_app, uint32_t cID, int sock):
        SessionContext(app, cID, sock, CONN_HTTP),
        conversationFinished(false)
    {
    }

    void httpWriteResponseHeader(int code, const string &title, const string &contentType, const string &optionalField= "")
    {
        writef("HTTP/1.0 %d %s\r\n", code, title.c_str());
        writef("Content-Type: %s\r\n", contentType.c_str());
        if(optionalField.length())
        {
            string field= optionalField;
            while(isspace(field[field.size()-1]))
                field.resize(field.size()-1);
            write(field);
            write("\r\n");  // make sure we have consistent newlines in the header.
        }
        writef("\r\n");
    }

    void httpWriteErrorBody(const string& title, const string& description)
    {
//        writef("<http><head><title>%s</title></head><body><h1>%s</h1><p>%s</p></body></html>\n", title.c_str(), title.c_str(), description.c_str());
        write(title + "\n" + description + "\n");
    }

    void httpWriteErrorResponse(int code, const string &title, const string &description, const string &optionalField= "")
    {
        httpWriteResponseHeader(code, title, "text/plain", optionalField);
        httpWriteErrorBody(title, description);
    }

//    void writeStatusline(CommandStatus status, const string& message)
//    {
//        // xxx remove?
//    }
//

    // forward statusline to http client, possibly mark client to be disconnected
    void forwardStatusline(const string& line);

    // forward data set to http client, possibly mark client to be disconnected.
    void forwardDataset(const string& line)
    {
        write(line);
        if(Cli::splitString(line.c_str()).empty())
            conversationFinished= true; // empty line marks end of data set, we're ready to disconnect.
    }

    virtual void commandNotFound(const string& text)
    {
        // special case: send http status code 501 instead of 400.
        httpWriteErrorResponse(501, "Not Implemented", string(FAIL_STR) + " " + text);
        conversationFinished= true;
    }
};


class Graphserv
{
    public:
        Graphserv(): cli(*this)
        {
            initCoreCommandTable();

            Authority *auth= new PasswordAuth(DEFAULT_HTPASSWD_FILENAME, DEFAULT_GROUP_FILENAME);
            authorities.insert(pair<string,Authority*> (auth->getName(), auth));
        }

        ~Graphserv()
        {
            for(map<string,Authority*>::iterator it= authorities.begin(); it!=authorities.end(); it++)
                delete it->second;
            authorities.clear();
        }

        Authority *findAuthority(const string& name)
        {
            map<string,Authority*>::iterator it= authorities.find(name);
            if(it!=authorities.end()) return it->second;
            return 0;
        }

        bool run()
        {
            int listenSocket= openListenSocket(DEFAULT_TCP_PORT);
            int httpSocket= openListenSocket(DEFAULT_HTTP_PORT);
            fd_set readfds, writefds;

            int maxfd;

            puts("main loop");
            while(true)
            {
                double time= getTime();

                FD_ZERO(&readfds);
                FD_ZERO(&writefds);

                maxfd= 0;

                fd_add(readfds, listenSocket, maxfd);
                fd_add(readfds, httpSocket, maxfd);

                // deferred removal of clients
                for(set<uint32_t>::iterator i= clientsToRemove.begin(); i!=clientsToRemove.end(); i++)
                    removeSession(*i);
                clientsToRemove.clear();

                for( map<uint32_t,SessionContext*>::iterator i= sessionContexts.begin(); i!=sessionContexts.end(); i++ )
                {
                    SessionContext *sc= i->second;
                    double d= time-sc->stats.lastTime;
                    if(d>10.0)
                    {
                        sc->stats.normalize(time);
                        fprintf(stderr, "client %u: bytesSent %.2f, linesQueued %.2f, coreCommandsSent %.2f, servCommandsSent %.2f\n",
                                sc->clientID, sc->stats.bytesSent, sc->stats.linesQueued, sc->stats.coreCommandsSent, sc->stats.servCommandsSent);
//                        if(sc->stats.bytesSent>1000) sc->chokeTime= time+0.5;
                        sc->stats.reset();
                        sc->stats.lastTime= time;
                    }
                    if(sc->chokeTime<time) fd_add(readfds, sc->sockfd, maxfd);
                    if(!sc->writeBufferEmpty())
                        fd_add(writefds, sc->sockfd, maxfd);
                }

                for( map<uint32_t,CoreInstance*>::iterator i= coreInstances.begin(); i!=coreInstances.end(); i++ )
                {
                    CoreInstance *ci= i->second;
                    fd_add(readfds, ci->getReadFd(), maxfd);
                    ci->flushCommandQ(*this);
                    if(!ci->writeBufferEmpty())
                        fd_add(writefds, ci->getWriteFd(), maxfd);
                }

                struct timeval timeout;
                timeout.tv_sec= 2;
                timeout.tv_usec= 0;
                int r= select(maxfd+1, &readfds, &writefds, 0, &timeout);
                if(r<0)
                {
                    perror("select()");
                    switch(errno)
                    {
                        case EBADF:
                            // xxx one of the file descriptors was unexpectedly closed. find out which.
                            continue;

                        default:
                            return false;
                    }
                }

                time= getTime();

                if(FD_ISSET(listenSocket, &readfds))
                    if(!acceptConnection(listenSocket, CONN_TCP))
                        fprintf(stderr, "couldn't create connection.\n");

                if(FD_ISSET(httpSocket, &readfds))
                    if(!acceptConnection(httpSocket, CONN_HTTP))
                        fprintf(stderr, "couldn't create connection.\n");

                for( map<uint32_t,SessionContext*>::iterator i= sessionContexts.begin(); i!=sessionContexts.end(); i++ )
                {
                    SessionContext &sc= *i->second;
                    int sockfd= sc.sockfd;
                    if(FD_ISSET(sockfd, &readfds))
                    {
                        const size_t BUFSIZE= 128;
                        char buf[BUFSIZE];
                        ssize_t sz= recv(sockfd, buf, sizeof(buf), 0);
                        if(sz==0)
                        {
                            printf("client %d has closed the connection\n", sc.clientID);
                            clientsToRemove.insert(sc.clientID);
                        }
                        else if(sz<0)
                        {
                            fprintf(stderr, "i/o error, client %d: %s\n", sc.clientID, strerror(errno));
                            clientsToRemove.insert(sc.clientID);
                        }
                        else
                        {
                            for(ssize_t i= 0; i<sz; i++)
                            {
                                char c= buf[i];
                                if(c=='\r') continue;   // someone is feeding us DOS newlines?
                                sc.linebuf+= c;
                                if(c=='\n')
                                {
                                    if(clientsToRemove.find(sc.clientID)!=clientsToRemove.end())
                                        break;

                                    if(sc.connectionType==CONN_HTTP)
                                        lineFromHTTPClient(sc.linebuf, *(HTTPSessionContext*)&sc, time);
                                    else
                                        lineFromClient(string(sc.linebuf), sc, time);
                                    sc.linebuf.clear();
                                }
                            }
                        }
                    }
                    if(FD_ISSET(sockfd, &writefds))
                        sc.flush();
                }

                vector<CoreInstance*> coresToRemove;

                for( map<uint32_t,CoreInstance*>::iterator i= coreInstances.begin(); i!=coreInstances.end(); i++ )
                {
                    CoreInstance *ci= i->second;
                    if(FD_ISSET(ci->getReadFd(), &readfds))
                    {
                        const size_t BUFSIZE= 1024;
                        char buf[BUFSIZE];
                        ssize_t sz= read(ci->getReadFd(), buf, sizeof(buf));
                        if(sz==0)
                        {
                            printf("core %s has exited?\n", ci->getName().c_str());
                            int status;
                            waitpid(ci->getPid(), &status, 0);  // un-zombify
                            coresToRemove.push_back(ci);
                        }
                        else if(sz<0)
                        {
                            fprintf(stderr, "i/o error, core %s: %s\n", ci->getName().c_str(), strerror(errno));
                            coresToRemove.push_back(ci);
                        }
                        else
                        {
                            for(ssize_t i= 0; i<sz; i++)
                            {
                                char c= buf[i];
                                if(c=='\r') continue;
                                ci->linebuf+= c;
                                if(c=='\n')
                                {
                                    ci->lineFromCore(ci->linebuf, *this);
                                    ci->linebuf.clear();
                                }
                            }
                        }
                    }
                    if(FD_ISSET(ci->getWriteFd(), &writefds))
                        ci->flush();
                }
                // remove outside of loop to avoid invalidating iterators
                for(size_t i= 0; i<coresToRemove.size(); i++)
                    removeCoreInstance(coresToRemove[i]);

                // go through any HTTP session contexts immediately after i/o.
                for( map<uint32_t,SessionContext*>::iterator i= sessionContexts.begin(); i!=sessionContexts.end(); i++ )
                {
                    SessionContext *sc= i->second;
                    CoreInstance *ci;
                    // HTTP clients are disconnected once we don't have any more output for them.
                    if( sc->connectionType==CONN_HTTP &&
                        ((HTTPSessionContext*)sc)->conversationFinished &&
                        sc->writeBufferEmpty() &&
                        ((ci= findInstance(sc->coreID))==NULL || ci->hasDataForClient(sc->clientID)==false) )
                        shutdownClient(sc);
                }
            }

            return true;
        }

        // find a named instance.
        CoreInstance *findNamedInstance(string name)
        {
            for(map<uint32_t,CoreInstance*>::iterator it= coreInstances.begin(); it!=coreInstances.end(); it++)
                if(it->second->getName()==name) return it->second;
            return 0;
        }

        // find an instance by ID (faster).
        CoreInstance *findInstance(uint32_t ID)
        {
            map<uint32_t,CoreInstance*>::iterator it= coreInstances.find(ID);
            if(it!=coreInstances.end()) return it->second;
            return 0;
        }

        // creates a new instance, without starting it.
        CoreInstance *createCoreInstance(string name= "")
        {
            CoreInstance *inst= new CoreInstance(++coreIDCounter);
            inst->setName(name);
            coreInstances.insert( pair<uint32_t,CoreInstance*>(coreIDCounter, inst) );
            return inst;
        }

        // removes a core instance from the list and deletes it
        void removeCoreInstance(CoreInstance *core)
        {
            map<uint32_t,CoreInstance*>::iterator it= coreInstances.find(core->getID());
            if(it!=coreInstances.end()) coreInstances.erase(it);
            delete core;
        }

        // find a session context (client).
        SessionContext *findClient(uint32_t ID)
        {
            set<uint32_t>::iterator i= clientsToRemove.find(ID);
            if(i!=clientsToRemove.end()) return 0;
            map<uint32_t,SessionContext*>::iterator it= sessionContexts.find(ID);
            if(it!=sessionContexts.end()) return it->second;
            return 0;
        }

        // shut down the client socket. disconnect will happen in select loop when read returns zero.
        void shutdownClient(SessionContext *sc)
        {
            if(shutdown(sc->sockfd, SHUT_RDWR)<0)
            {
                perror("shutdown");
                forceClientDisconnect(sc);
            }
        }

        // mark client connection to be forcefully broken.
        void forceClientDisconnect(SessionContext *sc)
        {
            if(clientsToRemove.find(sc->clientID)!=clientsToRemove.end())
                return;
            clientsToRemove.insert(sc->clientID);
        }

    private:
        struct CoreCommandInfo
        {
            AccessLevel accessLevel;
            string coreImpDetail;
        };
        map<string,CoreCommandInfo> coreCommandInfos;

        static uint32_t coreIDCounter;
        static uint32_t sessionIDCounter;

        map<uint32_t,CoreInstance*> coreInstances;
        map<uint32_t,SessionContext*> sessionContexts;

        set<uint32_t> clientsToRemove;

        ServCli cli;

        map<string,Authority*> authorities;

        // create a reusable socket listening on the given port.
        int openListenSocket(int port)
        {
            int listenSocket= socket(AF_INET, SOCK_STREAM, 0);
            if(listenSocket==-1) { perror("socket()"); return false; }

            // Allow socket descriptor to be reuseable
            int on= 1;
            int rc= setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
            if (rc < 0)
            {
                perror("setsockopt() failed");
                close(listenSocket);
                return -1;
            }

//            if(!setNonblocking(listenSocket)) { close(listenSocket); return -1; }

            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family= AF_INET;
            sa.sin_addr.s_addr= htonl(INADDR_ANY);
            sa.sin_port= htons(port);
            if(bind(listenSocket, (sockaddr*)&sa, sizeof(sa))<0)
            {
                perror("bind()");
                close(listenSocket);
                return -1;
            }

            if(listen(listenSocket, LISTEN_BACKLOG)<0)
            {
                perror("listen()");
                close(listenSocket);
                return -1;
            }

            return listenSocket;
        }

        void fd_add(fd_set &set, int fd, int &maxfd)
        {
            FD_SET(fd, &set);
            if(fd>maxfd) maxfd= fd;
        }

        CoreCommandInfo *findCoreCommand(const string &name)
        {
            map<string,CoreCommandInfo>::iterator it= coreCommandInfos.find(name);
            if(it!=coreCommandInfos.end()) return &it->second;
            return 0;
        }

        void initCoreCommandTable()
        {
#define CORECOMMANDS_BEGIN
#define CORECOMMANDS_END
#define CORECOMMAND(name, level, imp...) ({ \
    CoreCommandInfo cci= { level, #imp };         \
    coreCommandInfos.insert( pair<string,CoreCommandInfo> (name, cci) ); })
#include "corecommands.h"
#undef CORECOMMANDS_BEGIN
#undef CORECOMMANDS_END
#undef CORECOMMAND
        }

        SessionContext *acceptConnection(int socket, ConnectionType type)
        {
            int newConnection= accept(socket, 0, 0);
            if(newConnection<0)
            {
                perror("accept()");
                return 0;
//                if(errno!=EWOULDBLOCK && errno!=EAGAIN)
//                    return false;
            }
            else
            {
                // add new connection
                printf("new connection, type %s, socket=%d\n", (type==CONN_TCP? "TCP": "HTTP"), newConnection);
                return createSession(newConnection, type);
            }
        }

        SessionContext *createSession(int sock, ConnectionType connType= CONN_TCP)
        {
            uint32_t newID= ++sessionIDCounter;
            if(!closeOnExec(sock)) return 0;
            SessionContext *newSession;
            switch(connType)
            {
                case CONN_TCP:  newSession= new SessionContext(*this, newID, sock, connType); break;
                case CONN_HTTP: newSession= new HTTPSessionContext(*this, newID, sock); break;
            }
            sessionContexts.insert( pair<uint32_t,SessionContext*>(newID, newSession) );
            return newSession;
        }

        bool removeSession(uint32_t sessionID)
        {
            map<uint32_t,SessionContext*>::iterator it= sessionContexts.find(sessionID);
            if(it!=sessionContexts.end())
            {
//                // remove all commands queued for this client
//                for(map<uint32_t,CoreInstance*>::iterator i= coreInstances.begin(); i!=coreInstances.end(); i++)
//                    i->second->removeClientCommands(sessionID);
                delete(it->second);
                sessionContexts.erase(it);
                return true;
            }
            return false;
        }

        // handle a line of text arriving from a client.
        void lineFromClient(string line, SessionContext &sc, double timestamp)
        {
            sc.stats.linesSent++;
            sc.stats.bytesSent+= line.length();

            // xxx handle case where client sends unknown/invalid command with terminating colon (slurp data set anyway)

            // if(sc connected && running command for this client accepts more data)
            //      append data to command queue entry
            if(sc.coreID)
            {
                CoreInstance *ci= findInstance(sc.coreID);
                if(ci)
                {
                    CommandQEntry *cqe= ci->findLastClientCommand(sc.clientID);
                    if(cqe && cqe->acceptsData && (!cqe->dataFinished))
                    {
                        sc.stats.dataRecordsSent++;
                        sc.stats.linesQueued++;

                        cqe->dataSet.push_back(line);
                        if(Cli::splitString(line.c_str()).size()==0)
                            cqe->dataFinished= true;
                        return;
                    }
                    ci->flushCommandQ(*this);
                }
                else
                {
                    fprintf(stderr, "BUG? client %d has invalid coreID %d\n", sc.clientID, sc.coreID);
                    // XXX handle error. something like "ERROR! core exited unexpectedly" (if that ever happens)
                    return;
                }
            }

            vector<string> words= Cli::splitString(line.c_str());
            if(words.empty()) return;

            // check line for terminating colon ':'
            bool hasDataSet= false;
            size_t sz= words.back().size();
            if(sz && words.back()[sz-1]==':')
            {
                hasDataSet= true;
                words.back().erase(sz-1);
            }

            ServCmd *cmd= (ServCmd*)cli.findCommand(words[0]);
            if(cmd)
            {
                sc.stats.servCommandsSent++;
                if(hasDataSet)  // currently, no server command takes a data set.
                    sc.writef(_("%s %s accepts no data set.\n"), FAIL_STR, words[0].c_str());
                else cli.execute(cmd, words, sc);
            }
            else if(sc.coreID)
            {
                sc.stats.coreCommandsSent++;
                CoreCommandInfo *cci= findCoreCommand(words[0]);
                if(cci)
                {
                    if(sc.accessLevel>=cci->accessLevel)
                    {
                        // write command to instance
                        CoreInstance *ci= findInstance(sc.coreID);
                        sc.stats.linesQueued++;
                        if(ci) ci->queueCommand(line, sc.clientID, hasDataSet);
                        else sc.writef(_("%s client has invalid core ID %u\n"), FAIL_STR, sc.clientID);
                    }
                    else
                    {
//                        sc.writef(FAIL_STR);
//                        sc.writef(_(" insufficient access level (command needs %s, you have %s)\n"),
//                                  gAccessLevelNames[cci->accessLevel], gAccessLevelNames[sc.accessLevel]);
                        // forward line as if it came from core so that the http code can do its stuff
                        sc.forwardStatusline(string(FAIL_STR " ") + format(_(" insufficient access level (command needs %s access, you have %s access)\n"),
                                             gAccessLevelNames[cci->accessLevel], gAccessLevelNames[sc.accessLevel]));
                    }
                }
                else
//                    sc.writef(_("%s no such core command '%s'.\n"), FAIL_STR, words[0].c_str());
                    sc.commandNotFound(format(_("no such core command '%s'."), words[0].c_str()));
            }
            else
//                sc.writef(_("%s no such server command.\n"), FAIL_STR);
                sc.commandNotFound(format(_("no such server command '%s'."), words[0].c_str()));

//            else if(sc connected)
//            {
//                if(core-command found)
//                {
//                    if(has sufficient access level)
//                        write command to instance
//                    else
//                        write error("insufficient access level");
//                }
//                else
//                    write error("no such command");
//
//            }
//            else
//                write error("no such command");
        }


        void lineFromHTTPClient(string line, HTTPSessionContext &sc, double timestamp)
        {
            // xxx handle case where someone tries to GET something like "add-arcs:"
            sc.http.request.push_back(line);    // xxx not needed if we don't parse the header, remove?
            if(line=="\n")  // end of request. CR is removed by buffering code
            {
                sc.http.requestString= sc.http.request[0];
                sc.http.request.clear();    // discard the rest of the header, as we don't currently use it.
                vector<string> words= Cli::splitString(sc.http.requestString.c_str());
                if(words.size()!=3)     // this does not look like an HTTP request. disconnect the client.
                {
                    fprintf(stderr, _("bad HTTP request string, disconnecting.\n"));
                    shutdownClient(&sc);
                    return;
                }
                transform(words[2].begin(), words[2].end(),words[2].begin(), ::toupper);
                if( (words[2]!="HTTP/1.0") && (words[2]!="HTTP/1.1") )  // accept HTTP/1.1 too, if only for debugging.
                {
                    fprintf(stderr, _("unknown HTTP version, disconnecting.\n"));
                    shutdownClient(&sc);
                    return;
                }

                const char *uri= words[1].c_str();
                int urilen= words[1].size();
                string transformedURI;
                transformedURI.reserve(urilen);
                for(int i= 0; i<urilen; i++)
                {
                    switch(uri[i])
                    {
                        case '+':
                            transformedURI+= ' ';
                            break;
                        case '%':
                            unsigned hexChar;
                            if( urilen-i<3 || sscanf(uri+i+1, "%02X", &hexChar)!=1 || !isprint(hexChar) )
                            {
                                fprintf(stderr, _("i=%d len=%d %s %02X bad hex in request URI, disconnecting\n"), i, urilen, uri+i+1, hexChar);
                                shutdownClient(&sc);
                                return;
                            }
                            transformedURI+= (char)hexChar;
                            i+= 2;
                            break;
                        case '/':
                            if(i==0) break;
                            // else fall through
                        default:
                            transformedURI+= uri[i];
                            break;
                    }
                }

                vector<string> uriwords= Cli::splitString(transformedURI.c_str(), "/");

                if(uriwords.size()!=2)
                {
                    sc.forwardStatusline(string(FAIL_STR) + " " + _("Expecting a request string like: 'GET /corename/command+to+run HTTP/1.0' (see docs)."));
                    // client will be disconnected after response has been flushed.
                    return;
                }

                // immediately connect the client to the core named in the request string.
                CoreInstance *ci= findNamedInstance(uriwords[0]);
                if(!ci)
                {
                    sc.forwardStatusline(string(FAIL_STR) + " " + _("No such instance."));
                    return;
                }

                sc.coreID= ci->getID();

                lineFromClient(uriwords[1] + '\n', sc, timestamp);
            }
        }

        friend class ccInfo;
};

uint32_t Graphserv::coreIDCounter= 0;
uint32_t Graphserv::sessionIDCounter= 0;


vector<string> PasswordAuth::splitLine(string line, char sep)
{
    string s;
    vector<string> ret;
    if(line.empty()) return ret;
    while(isspace(line[line.size()-1])) line.resize(line.size()-1);
    line+= sep;
    for(size_t i= 0; i<line.size(); i++)
    {
        if(line[i]==sep)
        { ret.push_back(s); s.clear(); }
        else s+= line[i];
    }
    return ret;
}

bool PasswordAuth::readCredentialFiles()
{
    char line[1024];
    FILE *f= fopen(htpasswdFilename.c_str(), "r");
    if(!f) { perror(("couldn't open " + groupFilename).c_str()); return false; }
    map<string,userInfo> newUsers;
    while(fgets(line, 1024, f))
    {
        vector<string> fields= splitLine(line);
        if(fields.size()!=2 || fields[0].empty() || fields[1].size()!=13)
        {
            fprintf(stderr, "invalid line in htpasswd file\n");
            return false;
        }
        userInfo ui= { fields[1], ACCESS_READ };
        newUsers[fields[0]]= ui;
    }

    f= fopen(groupFilename.c_str(), "r");
    if(!f) { perror(("couldn't open " + groupFilename).c_str()); return false; }
    while(fgets(line, 1024, f))
    {
        vector<string> fields= splitLine(line);
        if(fields.size()!=4 || fields[0].empty())
        {
            fprintf(stderr, "invalid line in group file\n");
            return false;
        }
        AccessLevel level;
        if(fields[0]==gAccessLevelNames[ACCESS_READ]) level= ACCESS_READ;
        else if(fields[0]==gAccessLevelNames[ACCESS_WRITE]) level= ACCESS_WRITE;
        else if(fields[0]==gAccessLevelNames[ACCESS_ADMIN]) level= ACCESS_ADMIN;
        else
        {
            fprintf(stderr, "invalid access level '%s' in group file\n", fields[0].c_str());
            return false;
        }

        // go through the specified users and elevate their access levels
        vector<string> usernames= splitLine(fields[3], ',');
        for(vector<string>::iterator it= usernames.begin(); it!=usernames.end(); it++)
        {
            map<string,userInfo>::iterator user= newUsers.find(*it);
            if(user!=newUsers.end() && level>user->second.accessLevel)
            {
//                printf("%s -> %s\n", user->first.c_str(), fields[0].c_str());
                user->second.accessLevel= level;
            }
        }
    }

    users.clear();
    users= newUsers;
    return true;
}

void PasswordAuth::refreshFileCache()
{
    time_t curtime= time(0);
    struct stat st;
    bool needRefresh= false;
    // check if any of the credential files have changed since last refresh.
    if(stat(htpasswdFilename.c_str(), &st)<0)
    { perror("couldn't stat passwdfile"); return; }
    if(st.st_mtime>=lastCacheRefresh) needRefresh= true;
    else if(stat(groupFilename.c_str(), &st)<0)
    { perror("couldn't stat groupfile"); return; }
    if(st.st_mtime>=lastCacheRefresh || needRefresh)
    {
        lastCacheRefresh= curtime;
        readCredentialFiles();
    }
}

bool PasswordAuth::authorize(class SessionContext& sc, const string& credentials)
{
    refreshFileCache();

    vector<string> cred= splitLine(credentials);
    if(cred.size()!=2 || cred[0].empty()||cred[1].empty())
    { fprintf(stderr, "invalid credentials.\n"); return false; }

    map<string,userInfo>::iterator it= users.find(cred[0]);
    if(it==users.end()) return false;

    char *crypted= crypt(cred[1].c_str(), it->second.hash.c_str());
    if(crypted != it->second.hash)
    {
        fprintf(stderr, "PasswordAuth: failure, user %s\n", it->first.c_str());
        return false;
    }

    fprintf(stderr, "PasswordAuth: success, user %s, level %s\n", it->first.c_str(), gAccessLevelNames[it->second.accessLevel]);

    sc.accessLevel= it->second.accessLevel;

    return true;
}



void CoreInstance::lineFromCore(string &line, class Graphserv &app)
{
//    printf("lineFromCore %s", line.c_str());
    SessionContext *sc= app.findClient(lastClientID);
    if(expectingReply)
    {
        expectingReply= false;
        if(line.find(":")!=string::npos)
            expectingDataset= true;
        if(sc)
            sc->forwardStatusline(line);    // virtual fn does http-specific stuff
    }
    else if(expectingDataset)
    {
        if(Cli::splitString(line.c_str()).size()==0)
            expectingDataset= false;
        if(sc)
            sc->forwardDataset(line);  // virtual function which also does http-specific stuff, if any
//            sc->write(line);
    }
    else
    {
        if(app.findClient(lastClientID))
            fprintf(stderr, "CoreInstance '%s', ID %u: lineFromCore(): not expecting anything from this core\n", getName().c_str(), getID());
    }
}

// try writing out commands from queue to core process
void CoreInstance::flushCommandQ(class Graphserv &app)
{
    while( commandQ.size() && (!expectingReply) && (!expectingDataset) && commandQ.front().flushable() )
    {
        CommandQEntry &c= commandQ.front();
        write(c.command);
        for(deque<string>::iterator it= c.dataSet.begin(); it!=c.dataSet.end(); it++)
            write(*it);
        lastClientID= c.clientID;
        expectingReply= true;
        expectingDataset= false;
        commandQ.pop_front();
    }
}


// error handler called because of broken connection or similar. tell app to disconnect this client.
void SessionContext::writeFailed(int _errno)
{
    app.forceClientDisconnect(this); // probably ripped out the cable or something. no use for shutdown.
}

static bool statuslineIndicatesDataset(const string& line)
{
    size_t pos= line.find(':');
    if(pos==string::npos) return false;
    for(; pos<line.size(); pos++)
        if(!isspace(line[pos])) return false;
    return true;
}

// forward statusline to http client, possibly mark client to be disconnected
void HTTPSessionContext::forwardStatusline(const string& line)
{
    printf("HTTPSessionContext::forwardStatusline(%s)\n", line.c_str());
    fflush(stdout);
    vector<string> replyWords= Cli::splitString(line.c_str());
    if(replyWords.empty())
    {
        httpWriteErrorResponse(500, "Internal Server Error", "Received empty status line from core. Please report.");
        app.shutdownClient(this);
    }
    else
    {
        bool hasDataset= statuslineIndicatesDataset(line);
        string headerStatusLine= "X-GraphProcessor: " + line;

        switch(getStatusCode(replyWords[0]))
        {
            case CMD_SUCCESS:
                httpWriteResponseHeader(200, "OK", "text/plain", headerStatusLine);
                write(line);
                break;
            case CMD_FAILURE:
                httpWriteErrorResponse(400, "Bad Request", line, headerStatusLine);
                break;
            case CMD_ERROR:
                httpWriteErrorResponse(500, "Internal Server Error", line, headerStatusLine);
                break;
            case CMD_NONE:
                httpWriteErrorResponse(404, "Not Found", line, headerStatusLine);
                break;
            case CMD_ACCESSDENIED:
            case CMD_NOT_FOUND:
                // XXX handle these where?
                break;
        }

        if(!hasDataset)
            conversationFinished= true;

        printf("conversationFinished: %s\n", conversationFinished? "true": "false");
        fflush(stdout);
    }
}



CommandStatus ServCli::execute(string command, class SessionContext &sc)
{
    vector<string> words= splitString(command.c_str());
    if(words.empty()) return CMD_SUCCESS;
    ServCmd *cmd= (ServCmd*)findCommand(words[0]);
    if(!cmd)
    {
        sc.writef(FAIL_STR);
        sc.writef(_(" no such server command.\n"));
        return CMD_FAILURE;
    }
    return execute(cmd, words, sc);
}

CommandStatus ServCli::execute(ServCmd *cmd, vector<string> &words, SessionContext &sc)
{
    if(cmd->getAccessLevel() > sc.accessLevel)
    {
        sc.writef(FAIL_STR);
        sc.writef(_(" insufficient access level (command needs %s, you have %s)\n"),
                  gAccessLevelNames[cmd->getAccessLevel()], gAccessLevelNames[sc.accessLevel]);
        return CMD_FAILURE;
    }
    CommandStatus ret;
    switch(cmd->getReturnType())
    {
        case CliCommand::RT_OTHER:
            ret= ((ServCmd_RTOther*)cmd)->execute(words, app, sc);
            break;
        case CliCommand::RT_NONE:
            ret= ((ServCmd_RTVoid*)cmd)->execute(words, app, sc);
            sc.writef(cmd->getStatusMessage().c_str());
            break;
        default:
            ret= CMD_ERROR;
            break;
    }
    return ret;
}



/////////////////////////////////////////// server commands ///////////////////////////////////////////

class ccCreateGraph: public ServCmd_RTVoid
{
    public:
        string getName() { return "create-graph"; }
        string getSynopsis() { return getName() + " graphname"; }
        string getHelpText() { return _("create a named graphcore instance. [ADMIN]"); }
        AccessLevel getAccessLevel() { return ACCESS_ADMIN; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=2)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            if(app.findNamedInstance(words[1])) { cliFailure(_("an instance with this name already exists.\n")); return CMD_FAILURE; }
            CoreInstance *core= app.createCoreInstance(words[1]);
            if(!core) { cliFailure(_("Graphserv::createCoreInstance() failed.\n")); return CMD_FAILURE; }
            if(!core->startCore()) { cliFailure("startCore(): %s\n", core->getLastError().c_str()); app.removeCoreInstance(core); return CMD_FAILURE; }
            cliSuccess(_("spawned pid %d.\n"), core->getPid());
            return CMD_SUCCESS;
        }

};


class ccUseGraph: public ServCmd_RTVoid
{
    public:
        string getName() { return "use-graph"; }
        string getSynopsis() { return getName() + " graphname"; }
        string getHelpText() { return _("use a named graphcore instance."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=2)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            if(sc.coreID) { cliFailure(_("already connected. switching instances is not currently supported.\n")); return CMD_FAILURE; }
            CoreInstance *core= app.findNamedInstance(words[1]);
            if(!core) { cliFailure(_("no such instance.\n")); return CMD_FAILURE; }
            sc.coreID= core->getID();
            cliSuccess(_("connected to pid %d.\n"), core->getPid());
            return CMD_SUCCESS;
        }

};

class ccAuthorize: public ServCmd_RTVoid
{
    public:
        string getName() { return "authorize"; }
        string getSynopsis() { return getName() + " authority credentials"; }
        string getHelpText() { return _("authorize authorize with the given authority using the given credentials."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=3)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            Authority *auth= app.findAuthority(words[1]);
            if(!auth)
            {
                cliFailure(_("no such authority '%s'.\n"), words[1].c_str());
                return CMD_FAILURE;
            }
            if(!auth->authorize(sc, words[2]))
            {
                cliFailure(_("authorization failure.\n"));
                return CMD_FAILURE;
            }
            cliSuccess(_("access level: %s\n"), gAccessLevelNames[sc.accessLevel]);
            return CMD_SUCCESS;
        }

};

class ccInfo: public ServCmd_RTOther
{
    public:
        string getName() { return "i"; }
        string getSynopsis() { return getName() + " graphname"; }
        string getHelpText() { return _("print info (debugging)"); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                return CMD_FAILURE;
            }

            sc.writef("Cores: %d\n", app.coreInstances.size());

            for(map<uint32_t,CoreInstance*>::iterator it= app.coreInstances.begin(); it!=app.coreInstances.end(); it++)
            {
                CoreInstance *ci= it->second;
                sc.writef("Core %d:\n", ci->getID());
                sc.writef("  queue size: %u\n", ci->commandQ.size());
                sc.writef("  bytes in write buffer: %u\n", ci->getWritebufferSize());
            }

            return CMD_SUCCESS;
        }

};


ServCli::ServCli(Graphserv &_app): app(_app)
{
    addCommand(new ccCreateGraph());
    addCommand(new ccUseGraph());
    addCommand(new ccInfo());
    addCommand(new ccAuthorize());
}



void sigchld_handler(int signum)
{
//    puts("sigchld received");
}


int main()
{
    bindtextdomain("graphserv", "./messages");
    textdomain("graphserv");

    signal(SIGPIPE, SIG_IGN);

//    struct sigaction sa;
//    memset(&sa, 0, sizeof(sa));
//    sa.sa_handler= sigchld_handler;
//    sigaction(SIGCHLD, &sa, 0);

//    CoreInstance test(0);
//    if(!test.startCore())
//        cout << FAIL_STR << " " << test.getLastError() << endl;

    Graphserv s;
    s.run();

    return 0;
}
