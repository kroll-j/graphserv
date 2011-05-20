// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011

#include <libintl.h>
#include <iostream>
#include <vector>
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

enum ConnectionType
{
    CONN_TCP= 0,
    CONN_HTTP
};



class ServCmd: public CliCommand
{
};


class ServCli: public Cli
{
    public:
        ServCli(class Graphserv &_app);

        void addCommand(ServCmd *cmd)
        { commands.push_back(cmd); }

        CommandStatus execute(string command, class SessionContext &sc);

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





class CoreInstance
{
    public:
        CoreInstance(uint32_t _id): instanceID(_id)
        {
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

                toCore= fdopen(pipeToCore[1], "w");
                fromCore= fdopen(pipeFromCore[0], "r");

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

    private:
        uint32_t instanceID;
        string lastError;
        string name;

        pid_t pid;
        int pipeToCore[2];      // writable from server
        int pipeFromCore[2];    // writable from core
        FILE *toCore;
        FILE *fromCore;
};

struct SessionContext
{
    uint32_t clientID;
    AccessLevel accessLevel;
    ConnectionType connectionType;
    uint32_t coreID;    // non-zero if connected to a core instance
    int sockfd;
    string linebuf;
    FILE *sockFile;

    SessionContext(uint32_t cID, int sock, ConnectionType connType= CONN_TCP):
        clientID(cID), accessLevel(ACCESS_ADMIN/*XXX*/), connectionType(connType), coreID(0), sockfd(sock)
    {
        if(!(sockFile= fdopen(sockfd, "w")))
           perror("fdopen"), abort();
        setlinebuf(sockFile);
    }

    ~SessionContext()
    {
        fclose(sockFile);
    }
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

class Graphserv
{
    public:
        Graphserv(): cli(*this)
        {
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


            fd_set readfds, exceptfds;

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
                    int sockfd= i->second->sockfd;
                    FD_SET(sockfd, &readfds);
                    if(sockfd>maxfd) maxfd= sockfd;
                }

                int r= select(maxfd+1, &readfds, 0, 0, 0);
                if(r<0) { perror("select()"); return false; }

                if(FD_ISSET(listensock, &readfds))
                {
                    int newConnection= accept(listensock, 0, 0);
                    if(newConnection<0)
                    {
//                        if(errno!=EWOULDBLOCK)
                        {
                            perror("accept()");
                            return false;
                        }
                    }
                    // add new connection
                    printf("new connection, socket=%d\n", newConnection);
                    SessionContext *newSession= createSession(newConnection, CONN_TCP);
                }

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
                            removeSession(sc.clientID);
                        }
                        else if(sz<0)
                        {
                            fprintf(stderr, "i/o error, client %d: %s\n", sc.clientID, strerror(errno));
                            removeSession(sc.clientID);
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
                }
            }

            return true;
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

    private:
        static uint32_t coreIDCounter;
        static uint32_t sessionIDCounter;

        map<uint32_t,CoreInstance*> coreInstances;
        map<uint32_t,SessionContext*> sessionContexts;

        ServCli cli;

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
            }
        }

        void lineFromClient(string line, SessionContext &sc)
        {
//            printf("%d> %s", sc.clientID, sc.linebuf.c_str());
//            string tmp= format("%d < %s", sc.clientID, sc.linebuf.c_str());
//            write(sc.sockfd, tmp.c_str(), tmp.size());
            cli.execute(line, sc);
        }
};

uint32_t Graphserv::coreIDCounter= 0;
uint32_t Graphserv::sessionIDCounter= 0;



CommandStatus ServCli::execute(string command, class SessionContext &sc)
{
    vector<string> words= splitString(command.c_str());
    if(words.empty()) return CMD_SUCCESS;
    ServCmd *cmd= (ServCmd*)findCommand(words[0]);
    if(!cmd)
    {
        fprintf(sc.sockFile, FAIL_STR);
        fprintf(sc.sockFile, _(" no such command.\n"));
        fflush(sc.sockFile);
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
            fprintf(sc.sockFile, cmd->getStatusMessage().c_str());
            fflush(sc.sockFile);
            break;
    }
    return ret;
}


class ccCreateGraph: public ServCmd_RTVoid
{
    public:
        string getName() { return "create-graph"; }
        string getSynopsis() { return getName() + " graphname"; }
        string getHelpText() { return _("create a named graphcore instance."); }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=2)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            CoreInstance *core= app.createCoreInstance(words[1]);
            if(!core) { cliFailure(_("Graphserv::createCoreInstance() failed.\n")); return CMD_FAILURE; }
            if(!core->startCore()) { cliFailure("startCore(): %s\n", core->getLastError().c_str()); app.removeCoreInstance(core); return CMD_FAILURE; }
            cliSuccess(_("spawned pid %d.\n"), core->getPid());
            return CMD_SUCCESS;
        }

};

ServCli::ServCli(Graphserv &_app): app(_app)
{
    addCommand(new ccCreateGraph());
}



void sigchld_handler(int signum)
{
//    puts("sigchld received");
}


int main()
{
    bindtextdomain("graphserv", "./messages");
    textdomain("graphserv");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler= sigchld_handler;
    sigaction(SIGCHLD, &sa, 0);

//    CoreInstance test(0);
//    if(!test.startCore())
//        cout << FAIL_STR << " " << test.getLastError() << endl;

    Graphserv s;
    s.run();

    return 0;
}
