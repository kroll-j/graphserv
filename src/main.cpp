// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011

#include <libintl.h>
#include <iostream>
#include <vector>
#include <queue>
#include <map>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <cstring>
#include <cstdlib>
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

#include "clibase.h"


#define DEFAULT_PORT    6666
#define LISTEN_BACKLOG  100


enum AccessLevel
{
    ACCESS_READ= 0,
    ACCESS_WRITE,
    ACCESS_ADMIN
};
static const char *gAccessLevelNames[]=
    { "ACCESS_READ", "ACCESS_WRITE", "ACCESS_ADMIN" };

enum ConnectionType
{
    CONN_TCP= 0,
    CONN_HTTP
};



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


static bool setNonblocking(int sock)
{
	int opts= fcntl(sock, F_GETFL);
	if(opts<0)
    {
		perror("fcntl(F_GETFL)");
		return false;
	}
	opts|= O_NONBLOCK;
	if(fcntl(sock,F_SETFL,opts)<0)
	{
		perror("fcntl(F_SETFL)");
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
                if(!writeString(buffer.front())) return false;
                buffer.pop();
            }
            return true;
        }

        bool writeBufferEmpty()
        { return buffer.empty(); }

        void write(const string s)
        {
            flush();
            if( (!buffer.empty()) || (!writeString(s)) ) buffer.push(s);
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

        virtual void writeFailed(int _errno)= 0;

    private:
        int fd;
        queue<string> buffer;

        bool writeString(const string s)
        {
            cout << "writing " << s << ::flush;
            ssize_t sz= ::write(fd, s.data(), s.size());
            if((unsigned)sz!=s.size())
            {
                perror("write");

                if( (errno!=EAGAIN)&&(errno!=EWOULDBLOCK) )
                    writeFailed(errno);

                return false;
            }
            return true;
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
            exit(1); //xxxxx todo: do some sensible error handling (like marking the instance to be removed)
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

        // find command for this client in queue
        CommandQEntry *findClientCommand(uint32_t clientID)
        {
            for(commandQ_t::iterator it= commandQ.begin(); it!=commandQ.end(); it++)
                if(it->clientID==clientID)
                    return & (*it);
            return 0;
        }

        // try writing out commands from queue to core process
        void flushCommandQ()
        {
            while( (!expectingReply) && (!expectingDataset) && commandQ.front().flushable() )
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
};

struct SessionContext: public NonblockWriter
{
    uint32_t clientID;
    AccessLevel accessLevel;
    ConnectionType connectionType;
    uint32_t coreID;    // non-zero if connected to a core instance
    int sockfd;
    string linebuf;

    SessionContext(uint32_t cID, int sock, ConnectionType connType= CONN_TCP):
        clientID(cID), accessLevel(ACCESS_ADMIN/*XXX*/), connectionType(connType), coreID(0), sockfd(sock)
    {
        setWriteFd(sockfd);
    }

    ~SessionContext()
    {
        close(sockfd);
    }

    void writeFailed(int _errno)
    {
        perror("write failed"); // xxxxxxxx todo: do some sensible error handling (like marking the session to be removed)
    }
};


class Graphserv
{
    public:
        Graphserv(): cli(*this)
        {
            initCoreCommandTable();
        }

        bool run()
        {
            int listensock= socket(AF_INET, SOCK_STREAM, 0);
            if(listensock==-1) { perror("socket()"); return false; }

            // Allow socket descriptor to be reuseable
            int on= 1;
            int rc= setsockopt(listensock, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
            if (rc < 0)
            {
                perror("setsockopt() failed");
                close(listensock);
                return false;
            }

            if(!setNonblocking(listensock)) { close(listensock); return false; }

            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family= AF_INET;
            sa.sin_addr.s_addr= htonl(INADDR_ANY);
            sa.sin_port= htons(DEFAULT_PORT);
            if(bind(listensock, (sockaddr*)&sa, sizeof(sa))<0)
            {
                perror("bind()");
                close(listensock);
                return false;
            }

            if(listen(listensock, LISTEN_BACKLOG)<0)
            {
                perror("listen()");
                close(listensock);
                return false;
            }


            fd_set readfds, writefds;

            int maxfd;

            puts("main loop");
            while(true)
            {
                FD_ZERO(&readfds);

                maxfd= 0;

                FD_SET(listensock, &readfds);
                maxfd= listensock;

                for( map<uint32_t,SessionContext*>::iterator i= sessionContexts.begin(); i!=sessionContexts.end(); i++ )
                {
                    SessionContext *sc= i->second;
                    fd_add(readfds, sc->sockfd, maxfd);
                    if(!sc->writeBufferEmpty())
                        fd_add(writefds, sc->sockfd, maxfd);
                }

                for( map<uint32_t,CoreInstance*>::iterator i= coreInstances.begin(); i!=coreInstances.end(); i++ )
                {
                    CoreInstance *ci= i->second;
                    fd_add(readfds, ci->getReadFd(), maxfd);
                    if(!ci->writeBufferEmpty())
                        fd_add(writefds, ci->getWriteFd(), maxfd);
                }

                int r= select(maxfd+1, &readfds, 0, 0, 0);
                if(r<0) { perror("select()"); return false; }

                if(FD_ISSET(listensock, &readfds))
                {
                    int newConnection= accept(listensock, 0, 0);
                    if(newConnection<0)
                    {
                        perror("accept()");
                        if(errno!=EWOULDBLOCK)
                            return false;
                    }
                    else
                    {
                        // add new connection
                        printf("new connection, socket=%d\n", newConnection);
                        createSession(newConnection, CONN_TCP);
                    }
                }

                vector<uint32_t> clientsToRemove;

                for( map<uint32_t,SessionContext*>::iterator i= sessionContexts.begin(); i!=sessionContexts.end(); i++ )
                {
                    SessionContext &sc= *i->second;
                    int sockfd= sc.sockfd;
                    if(FD_ISSET(sockfd, &readfds))
                    {
                        const size_t BUFSIZE= 1024;
                        char buf[BUFSIZE];
                        ssize_t sz= recv(sockfd, buf, sizeof(buf), 0);
                        if(sz==0)
                        {
                            printf("client %d has closed the connection\n", sc.clientID);
                            clientsToRemove.push_back(sc.clientID);
                        }
                        else if(sz<0)
                        {
                            fprintf(stderr, "i/o error, client %d: %s\n", sc.clientID, strerror(errno));
                            clientsToRemove.push_back(sc.clientID);
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
                                    lineFromClient(string(sc.linebuf), sc);
                                    sc.linebuf.clear();
                                }
                            }
                        }
                    }
                    if(FD_ISSET(sockfd, &writefds))
                        sc.flush();
                }

                // remove outside of loop to avoid invalidating iterators
                for(size_t i= 0; i<clientsToRemove.size(); i++)
                    removeSession(clientsToRemove[i]);

                vector<CoreInstance*> coresToRemove;

                for( map<uint32_t,CoreInstance*>::iterator i= coreInstances.begin(); i!=coreInstances.end(); i++ )
                {
                    CoreInstance *ci= i->second;
                    if(FD_ISSET(ci->getReadFd(), &readfds))
                    {
                        printf("core %s has something to say\n", ci->getName().c_str());
                        const size_t BUFSIZE= 1024;
                        char buf[BUFSIZE];
                        ssize_t sz= read(ci->getReadFd(), buf, sizeof(buf));
                        if(sz==0)
                        {
                            printf("core %s has exited?\n", ci->getName().c_str());
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
                                    printf("%s> %s", ci->getName().c_str(), ci->linebuf.c_str());
                                    fflush(stdout);
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
            map<uint32_t,SessionContext*>::iterator it= sessionContexts.find(ID);
            if(it!=sessionContexts.end()) return it->second;
            return 0;
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

        ServCli cli;

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

        SessionContext *createSession(int sock, ConnectionType connType= CONN_TCP)
        {
            uint32_t newID= ++sessionIDCounter;
            SessionContext *newSession= new SessionContext(newID, sock, connType);
            sessionContexts.insert( pair<uint32_t,SessionContext*>(newID, newSession) );
            return newSession;
        }

        bool removeSession(uint32_t sessionID)
        {
            map<uint32_t,SessionContext*>::iterator it= sessionContexts.find(sessionID);
            if(it!=sessionContexts.end())
            {
                delete(it->second);
                sessionContexts.erase(it);
                return true;
            }
            return false;
        }

        void lineFromClient(string line, SessionContext &sc)
        {
//            printf("%d> %s", sc.clientID, sc.linebuf.c_str());
//            string tmp= format("%d < %s", sc.clientID, sc.linebuf.c_str());
//            write(sc.sockfd, tmp.c_str(), tmp.size());
//            cli.execute(line, sc);

            // if(sc connected && running command for this client accepts more data)
            //      append data to command queue entry
            if(sc.coreID)
            {
                CoreInstance *ci= findInstance(sc.coreID);
                if(ci)
                {
                    CommandQEntry *cqe= ci->findClientCommand(sc.clientID);
                    if(cqe && cqe->acceptsData && (!cqe->dataFinished))
                    {
                        cqe->dataSet.push_back(line);
                        if(Cli::splitString(line.c_str()).size()==0)
                        {
                            cqe->dataFinished= true;
                            ci->flushCommandQ();
                        }
                        return;
                    }
                }
                else
                {
                    fprintf(stderr, "BUG: client %d has invalid coreID %d\n", sc.clientID, sc.coreID);
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
                if(hasDataSet)  // currently, no server command takes a data set.
                    sc.writef(_("%s %s accepts no data set.\n"), FAIL_STR, words[0].c_str());
                else cli.execute(cmd, words, sc);
            }
            else if(sc.coreID)
            {
                CoreCommandInfo *cci= findCoreCommand(words[0]);
                if(cci)
                {
                    if(sc.accessLevel>=cci->accessLevel)
                    {
                        // write command to instance (todo: something like CoreInstance::execute() does queueing etc)
                        CoreInstance *ci= findInstance(sc.coreID);
                        if(ci)
                        {
//                            ci->write(line);
                            ci->queueCommand(line, sc.clientID, hasDataSet);
                        }
                        else sc.writef(_("%s no such command.\n"), FAIL_STR);
                    }
                    else
                    {
                        sc.writef(FAIL_STR);
                        sc.writef(_(" insufficient access level (command needs %s, you have %s)\n"),
                                  gAccessLevelNames[cci->accessLevel], gAccessLevelNames[sc.accessLevel]);
                    }
                }
                else
                    sc.writef(_("%s no such command.\n"), FAIL_STR);
            }
            else
                sc.writef(_("%s no such command.\n"), FAIL_STR);

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
};

uint32_t Graphserv::coreIDCounter= 0;
uint32_t Graphserv::sessionIDCounter= 0;



void CoreInstance::lineFromCore(string &line, class Graphserv &app)
{
    SessionContext *sc= app.findClient(lastClientID);
    if(!sc) { fprintf(stderr, "CoreInstance '%s', ID %u: limeFromCore(): invalid lastClientID %u\n", getName().c_str(), getID(), lastClientID); return; }
    if(expectingReply)
    {
        expectingReply= false;
        if(line.find(":")!=string::npos)
            expectingDataset= true;
        sc->write(line);
    }
    else if(expectingDataset)
    {
        if(Cli::splitString(line.c_str()).size()==0)
            expectingDataset= false;
        sc->write(line);
    }
    else
        fprintf(stderr, "CoreInstance '%s', ID %u: limeFromCore(): not expecting anything from client %u\n", getName().c_str(), getID(), lastClientID); return;
}



CommandStatus ServCli::execute(string command, class SessionContext &sc)
{
    vector<string> words= splitString(command.c_str());
    if(words.empty()) return CMD_SUCCESS;
    ServCmd *cmd= (ServCmd*)findCommand(words[0]);
    if(!cmd)
    {
        sc.writef(FAIL_STR);
        sc.writef(_(" no such command.\n"));
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


ServCli::ServCli(Graphserv &_app): app(_app)
{
    addCommand(new ccCreateGraph());
    addCommand(new ccUseGraph());
}



void sigchld_handler(int signum)
{
//    puts("sigchld received");
}


int main()
{
    bindtextdomain("graphserv", "./messages");
    textdomain("graphserv");

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
