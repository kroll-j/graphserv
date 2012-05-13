// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011, 2012
// main application class.
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

#ifndef SERVAPP_H
#define SERVAPP_H



// main application class.
class Graphserv
{
    public:
        Graphserv(int _tcpPort, int _httpPort, const string& htpwFilename, const string& groupFilename, const string& _corePath):
            tcpPort(_tcpPort), httpPort(_httpPort), corePath(_corePath),
            cli(*this), linesFromClients(0)
        {
            initCoreCommandTable();

            Authority *auth= new PasswordAuth(htpwFilename, groupFilename);
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
            int listenSocket= (tcpPort? openListenSocket(tcpPort): 0);
            if(listenSocket<0)
            {
                flog(LOG_CRIT, _("couldn't create socket for TCP connections (port %d).\n"), tcpPort);
                return false;
            }
            int httpSocket= (httpPort? openListenSocket(httpPort): 0);
            if(httpSocket<0)
            {
                flog(LOG_CRIT, _("couldn't create socket for HTTP connections (port %d).\n"), httpPort);
                return false;
            }

            fd_set readfds, writefds;


            int maxfd;

            flog(LOG_INFO, "entering main loop. TCP port: %d, HTTP port: %d\n", tcpPort, httpPort);
            while(true)
            {
                double time= getTime();

                FD_ZERO(&readfds);
                FD_ZERO(&writefds);

                maxfd= 0;

                if(listenSocket) fd_add(readfds, listenSocket, maxfd);
                if(httpSocket) fd_add(readfds, httpSocket, maxfd);

                // deferred removal of clients
                for(set<uint32_t>::iterator i= clientsToRemove.begin(); i!=clientsToRemove.end(); i++)
                    removeSession(*i);
                clientsToRemove.clear();

                // init fd set for select: add client fds
                for( map<uint32_t,SessionContext*>::iterator i= sessionContexts.begin(); i!=sessionContexts.end(); i++ )
                {
                    SessionContext *sc= i->second;
                    double d= time-sc->stats.lastTime;
                    if(d>10.0)
                    {
                        sc->stats.normalize(time);
                        flog(LOG_INFO, "client %u: bytesSent %.2f, linesQueued %.2f, coreCommandsSent %.2f, servCommandsSent %.2f\n",
                             sc->clientID, sc->stats.bytesSent, sc->stats.linesQueued, sc->stats.coreCommandsSent, sc->stats.servCommandsSent);
                        // possibly do something like this later to prevent flooding.
                        /* if(sc->stats.bytesSent>1000) sc->chokeTime= time+0.5; */
                        sc->stats.reset();
                        sc->stats.lastTime= time;
                    }
                    if(sc->chokeTime<time)  // chokeTime could be used to slow down a spamming client.
                        fd_add(readfds, sc->sockfd, maxfd);
                    // only add write fd if there is something to write
                    if(!sc->writeBufferEmpty())
                        fd_add(writefds, sc->sockfd, maxfd);
                }

                // init fd set for select: add core fds
                for( map<uint32_t,CoreInstance*>::iterator i= coreInstances.begin(); i!=coreInstances.end(); i++ )
                {
                    CoreInstance *ci= i->second;
                    fd_add(readfds, ci->getReadFd(), maxfd);
                    ci->flushCommandQ(*this);
                    // only add write fd if there is something to write
                    if(!ci->writeBufferEmpty())
                        fd_add(writefds, ci->getWriteFd(), maxfd);
                }

                struct timeval timeout;
                timeout.tv_sec= 2;
                timeout.tv_usec= 0;
                int r= select(maxfd+1, &readfds, &writefds, 0, &timeout);
                if(r<0)
                {
                    logerror("select()");
                    switch(errno)
                    {
                        case EBADF:
                            // a file descriptor is bad, find out which and remove the client or core.
                            for( map<uint32_t,SessionContext*>::iterator i= sessionContexts.begin(); i!=sessionContexts.end(); i++ )
                                if( !i->second->writeBufferEmpty() && fcntl(i->second->sockfd, F_GETFL)==-1 )
                                    flog(LOG_ERROR, _("bad fd, removing client %d.\n"), i->second->clientID),
                                    forceClientDisconnect(i->second);
                            for( map<uint32_t,CoreInstance*>::iterator i= coreInstances.begin(); i!=coreInstances.end(); i++ )
                                if( fcntl(i->second->getReadFd(), F_GETFL)==-1 ||
                                    (!i->second->writeBufferEmpty() && fcntl(i->second->getWriteFd(), F_GETFL)==-1) )
                                    flog(LOG_ERROR, _("bad fd, removing core %d.\n"), i->second->getID()),
                                    removeCoreInstance(i->second);
                            continue;

                        default:
                            return false;
                    }
                }

                time= getTime();

                // check for incoming connections.

                if(listenSocket && FD_ISSET(listenSocket, &readfds))
                    if(!acceptConnection(listenSocket, CONN_TCP))
                        flog(LOG_ERROR, _("couldn't create connection.\n"));

                if(httpSocket && FD_ISSET(httpSocket, &readfds))
                    if(!acceptConnection(httpSocket, CONN_HTTP))
                        flog(LOG_ERROR, _("couldn't create connection.\n"));

                // loop through all the session contexts, handle incoming data, flush outgoing data if possible.
                for( map<uint32_t,SessionContext*>::iterator it= sessionContexts.begin(); it!=sessionContexts.end(); it++ )
                {
                    SessionContext &sc= *it->second;
                    int sockfd= sc.sockfd;
                    if(FD_ISSET(sockfd, &readfds))
                    {
                        const size_t BUFSIZE= 128;
                        char buf[BUFSIZE];
                        ssize_t sz= recv(sockfd, buf, sizeof(buf), 0);
                        if(sz==0)
                        {
                            flog(LOG_INFO, _("client %d: connection closed%s.\n"), sc.clientID, sc.shutdownTime? "": _(" by peer"));
                            clientsToRemove.insert(sc.clientID);
                        }
                        else if(sz<0)
                        {
                            flog(LOG_ERROR, _("i/o error, client %d: %s\n"), sc.clientID, strerror(errno));
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

                                    linesFromClients++;

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

                // loop through all the core instances, handle incoming data, flush outgoing data if possible.
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
                            flog(LOG_INFO, "core %s (ID %u, pid %d) has exited\n", ci->getName().c_str(), ci->getID(), (int)ci->getPid());
                            int status;
                            waitpid(ci->getPid(), &status, 0);  // un-zombify
                            coresToRemove.push_back(ci);
                        }
                        else if(sz<0)
                        {
                            flog(LOG_ERROR, "i/o error, core %s: %s\n", ci->getName().c_str(), strerror(errno));
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
                                    SessionContext *sc= findClient(ci->getLastClientID());
                                    bool clientWasWaiting= (sc && sc->isWaitingForCoreReply());
                                    ci->lineFromCore(ci->linebuf, *this);
                                    ci->linebuf.clear();
                                    // if this was the last line the client was waiting for, 
                                    // execute its queued commands now.
                                    if( clientWasWaiting && (!sc->isWaitingForCoreReply()) )
                                        while(!sc->lineQueue.empty())
                                        {
                                            string& line= sc->lineQueue.front();
                                            flog(LOG_INFO, "execing queued line from client: '%s", line.c_str());
                                            lineFromClient(line, *sc, time, true);
                                            sc->lineQueue.pop();
                                        }
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
                    {
                        if(!sc->shutdownTime)
                            shutdownClient(sc);
                    }
                }
            }

            return true;
        }

        // check for valid graph name.
        // [a-zA-Z_-][a-zA-Z0-9_-]*
        bool isValidGraphName(const string& name)
        {
            int sz= name.size();
            if(!sz) return false;
            char c= name[0];
            if( !isupper(c) && !islower(c) && c!='-' && c!='_' ) return false;
                for(size_t i= 0; i<name.size(); i++)
            {
                c= name[i];
                if( !isupper(c) && !islower(c) && !isdigit(c) && c!='-' && c!='_' )
                    return false;
            }
            return true;
        }

        // find a named instance.
        CoreInstance *findNamedInstance(string name, bool onlyRunning= true)
        {
            for(map<uint32_t,CoreInstance*>::iterator it= coreInstances.begin(); it!=coreInstances.end(); it++)
                if( it->second->getName()==name && (onlyRunning? it->second->isRunning(): true) )
                    return it->second;
            return 0;
        }

        // find an instance by ID (faster).
        CoreInstance *findInstance(uint32_t ID, bool onlyRunning= true)
        {
            map<uint32_t,CoreInstance*>::iterator it= coreInstances.find(ID);
            if( it!=coreInstances.end() && (onlyRunning? it->second->isRunning(): true) ) return it->second;
            return 0;
        }

        // creates a new instance, without starting it.
        CoreInstance *createCoreInstance(string name= "")
        {
            CoreInstance *inst= new CoreInstance(++coreIDCounter, corePath);
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
            flog(LOG_INFO, "shutting down session %d.\n", sc->clientID);
            if(shutdown(sc->sockfd, SHUT_RDWR)<0)
            {
                logerror("shutdown");
                forceClientDisconnect(sc);
            }
            sc->shutdownTime= getTime();
        }

        // mark client connection to be forcefully broken.
        void forceClientDisconnect(SessionContext *sc)
        {
            if(clientsToRemove.find(sc->clientID)!=clientsToRemove.end())
                return;
            clientsToRemove.insert(sc->clientID);
        }

        // send a command from this client to the core it is connected to.
        void sendCoreCommand(SessionContext& sc, string line, bool hasDataSet, vector<string> *cmdwords= 0)
        {
            const vector<string>& words= (cmdwords? *cmdwords: Cli::splitString(line.c_str()));
            sc.stats.coreCommandsSent++;
            CoreCommandInfo *cci= findCoreCommand(words[0]);
            if(cci)
            {
                AccessLevel al= cci->accessLevel;
                if( line.find(">")!=string::npos || line.find("<")!=string::npos )
                    al= ACCESS_ADMIN;   // i/o redirection requires admin level.
                if(sc.accessLevel>=al)
                {
                    // write command to instance
                    CoreInstance *ci= findInstance(sc.coreID);
                    if(ci) ci->queueCommand(line, sc.clientID, hasDataSet), sc.stats.linesQueued++;
                    else sc.writef(_("%s client has invalid core ID %u\n"), FAIL_STR, sc.coreID);
                }
                else
                {
                    if(hasDataSet)
                        // read data set, then print error message.
                        sc.invalidDatasetStatus= CMD_ACCESSDENIED,
                        sc.invalidDatasetMsg= format(_("%s insufficient access level (command needs %s, you have %s)\n"), DENIED_STR,
                                                     gAccessLevelNames[al], gAccessLevelNames[sc.accessLevel]);
                    else
                        // forward line as if it came from core so that the http code can do its stuff
                        sc.forwardStatusline(string(DENIED_STR " ") + format(_("insufficient access level (command needs %s, you have %s)\n"),
                                             gAccessLevelNames[al], gAccessLevelNames[sc.accessLevel]));
                }
            }
            else
            {
                if(hasDataSet)
                    // read data set, then print error message.
                    sc.invalidDatasetStatus= CMD_NOT_FOUND,
                    sc.invalidDatasetMsg= format(_("no such core command '%s'."), words[0].c_str());
                else
                    // print error message.
                    sc.commandNotFound(format(_("no such core command '%s'."), words[0].c_str()));
            }
        }

        // get the core instances
        map<uint32_t,CoreInstance*>& getCoreInstances()
        {
            return coreInstances;
        }
        
        // reconnect session to new core
        bool reconnectSession(SessionContext *sc, CoreInstance *core)
        {
            if(!sc || !core) return false;
            
            CoreInstance *oldCore= findInstance(sc->coreID);
            if(oldCore && oldCore->hasDataForClient(sc->clientID))
            {
                // this is not fatal, but commands arriving from a different core could confuse client code.
                // to avoid this, clients should always wait for cores to reply before switching instances.
                flog(LOG_ERROR, _("old core instance %s still has data for client %d. "
                    "client code should wait for core commands to finish before switching instances.\n"), 
                    oldCore->getName().c_str(), sc->clientID);
            }
            
            sc->coreID= core->getID();
            return true;
        }

    private:
        int tcpPort, httpPort;
        string corePath;

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

        uint32_t linesFromClients;

        // create a reusable socket listening on the given port.
        int openListenSocket(int port)
        {
            int listenSocket= socket(AF_INET, SOCK_STREAM, 0);
            if(listenSocket==-1) { logerror("socket()"); return false; }

            // Allow socket descriptor to be reuseable
            int on= 1;
            int rc= setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
            if (rc < 0)
            {
                logerror("setsockopt() failed");
                close(listenSocket);
                return -1;
            }

            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family= AF_INET;
            sa.sin_addr.s_addr= htonl(INADDR_ANY);
            sa.sin_port= htons(port);
            if(bind(listenSocket, (sockaddr*)&sa, sizeof(sa))<0)
            {
                logerror("bind()");
                close(listenSocket);
                return -1;
            }

            if(listen(listenSocket, LISTEN_BACKLOG)<0)
            {
                logerror("listen()");
                close(listenSocket);
                return -1;
            }

            return listenSocket;
        }

        // helper function to add a file descriptor to an fd_set.
        void fd_add(fd_set &set, int fd, int &maxfd)
        {
            FD_SET(fd, &set);
            if(fd>maxfd) maxfd= fd;
        }

        // find information about a core command
        CoreCommandInfo *findCoreCommand(const string &name)
        {
            map<string,CoreCommandInfo>::iterator it= coreCommandInfos.find(name);
            if(it!=coreCommandInfos.end()) return &it->second;
            return 0;
        }

        // initialize the core command info table.
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

        // accept connection on a socket and create session context.
        SessionContext *acceptConnection(int socket, ConnectionType type)
        {
            int newConnection= accept(socket, 0, 0);
            if(newConnection<0)
            {
                logerror("accept()");
                return 0;
            }
            else
            {
                // add new connection
                flog(LOG_INFO, "new connection, type %s, socket=%d\n", (type==CONN_TCP? "TCP": "HTTP"), newConnection);
                return createSession(newConnection, type);
            }
        }

        // create a SessionContext or HTTPSessionContext, depending on connection type
        SessionContext *createSession(int sock, ConnectionType connType= CONN_TCP)
        {
            uint32_t newID= ++sessionIDCounter;
            if(!closeOnExec(sock)) return 0;
            SessionContext *newSession;
            switch(connType)
            {
                case CONN_TCP:  newSession= new SessionContext(*this, newID, sock, connType); break;
                case CONN_HTTP: newSession= new HTTPSessionContext(*this, newID, sock); break;
                default:        flog(LOG_ERROR, "createSession: unknown connection type %d!\n", connType); return 0;
            }
            sessionContexts.insert( pair<uint32_t,SessionContext*>(newID, newSession) );
            return newSession;
        }

        // immediately remove a session.
        bool removeSession(uint32_t sessionID)
        {
            map<uint32_t,SessionContext*>::iterator it= sessionContexts.find(sessionID);
            if(it!=sessionContexts.end())
            {
                flog(LOG_INFO, "removing client %d\n", it->second->clientID);
                CoreInstance *ci;
                if( it->second->coreID && (ci= findInstance(it->second->coreID)) )
                {
                    CommandQEntry *cqe= ci->findLastClientCommand(it->second->clientID);
                    if(cqe && cqe->acceptsData && (!cqe->dataFinished))
                    {
                        flog(LOG_ERROR, _("terminating open data set of connected core '%s' (ID %u)\n"), ci->getName().c_str(), ci->getID());
                        cqe->appendToDataset("\n\n");
                    }
                }
                delete(it->second);
                sessionContexts.erase(it);
                return true;
            }
            return false;
        }

        // handle a line of text arriving from a client.
        void lineFromClient(string line, SessionContext &sc, double timestamp, bool fromServerQueue= false)
        {
            if(line.rfind('\n')!=line.size()-1) line.append("\n");

            sc.stats.linesSent++;
            sc.stats.bytesSent+= line.length();

            // handle case where client sends unknown/invalid command with terminating colon according to spec.
            // consume data set, then send error message.
            if(sc.invalidDatasetStatus)
            {
                if(Cli::splitString(line.c_str()).size()==0)
                {
                    if(sc.invalidDatasetStatus==CMD_NOT_FOUND)
                        sc.commandNotFound(sc.invalidDatasetMsg);
                    else
                        sc.forwardStatusline(sc.invalidDatasetMsg);
                    // reset status code so following commands can succeed
                    sc.invalidDatasetStatus= CMD_SUCCESS;
                    sc.invalidDatasetMsg.resize(0);
                }
                return;
            }

            // if(sc connected to core && running core command for this client accepts more data)
            //      append data to core's command queue entry
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
                        cqe->appendToDataset(line);
                        return;
                    }
                    ci->flushCommandQ(*this);
                }
                else
                {
                    // xxx notify client?
                    flog(LOG_INFO, "client %d has invalid coreID %d, zeroing.\n", sc.clientID, sc.coreID);
                    sc.coreID= 0;
                }
            }

            if(!fromServerQueue && sc.isWaitingForCoreReply())
            {
                flog(LOG_INFO, "queuing: '%s", line.c_str());
                sc.lineQueue.push(line);   // must finish pending core commands first, queue this line for later processing
                return;
            }
            
            vector<string> words= Cli::splitString(line.c_str(), " \t\n");
            if(words.empty()) return;

            // check line for terminating colon ':'
            bool hasDataSet= false;
            size_t sz= words.back().size();
            if(sz && words.back()[sz-1]==':')
            {
                hasDataSet= true;
                words.back().erase(sz-1);
            }
            
            // first check if there's a server command with this name
            ServCmd *cmd= (ServCmd*)cli.findCommand(words[0]);
            if(cmd)
            {
                sc.stats.servCommandsSent++;
                if(hasDataSet)  // currently, no server command takes a data set.
                    sc.invalidDatasetStatus= CMD_FAILURE,
                    sc.invalidDatasetMsg= format(_("%s %s accepts no data set.\n"), FAIL_STR, words[0].c_str());
                else if( line.find(">")!=string::npos || line.find("<")!=string::npos )
                    sc.forwardStatusline(string(FAIL_STR) + _(" input/output of server commands can't be redirected.\n"));
                else cli.execute(cmd, words, sc);
            }
            // if connected, forward the command to the core
            else if(findInstance(sc.coreID))
                sendCoreCommand(sc, line, hasDataSet, &words);
            // no server command and not connected => no such command.
            else
            {
                if(hasDataSet)  // invalid command with data set
                    sc.invalidDatasetStatus= CMD_NOT_FOUND,
                    sc.invalidDatasetMsg= format(_("no such server command '%s'."), words[0].c_str());
                else
                    sc.commandNotFound(format(_("no such server command '%s'."), words[0].c_str()));
            }
        }

        // handle a line of text arriving from a HTTP client
        void lineFromHTTPClient(string line, HTTPSessionContext &sc, double timestamp)
        {
            sc.http.request.push_back(line);
            if(line=="\n")  // end of request. CR is removed by buffering code
            {
                sc.http.requestString= sc.http.request[0];
                sc.http.request.clear();    // discard the rest of the header, as we don't currently use it.
                vector<string> words= Cli::splitString(sc.http.requestString.c_str());
                if(words.size()!=3)     // this does not look like an HTTP request. disconnect the client.
                {
                    flog(LOG_ERROR, _("bad HTTP request string, disconnecting.\n"));
                    sc.forwardStatusline(string(FAIL_STR) + _(" bad HTTP request string.\n"));
                    return;
                }
                transform(words[2].begin(), words[2].end(),words[2].begin(), ::toupper);
                if( (words[2]!="HTTP/1.0") && (words[2]!="HTTP/1.1") )  // accept HTTP/1.1 too, if only for debugging.
                {
                    flog(LOG_ERROR, _("unknown HTTP version, disconnecting.\n"));
                    sc.forwardStatusline(string(FAIL_STR) + _(" unknown HTTP version.\n"));
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
                            // "%%" -> %
                            if(i<urilen-1 && uri[i+1]=='%')
                            {
                                transformedURI+= '%';
                                i++;
                                break;
                            }
                            // translate hex string
                            unsigned hexChar;
                            if( urilen-i<3 || sscanf(uri+i+1, "%02X", &hexChar)!=1 || !isprint(hexChar) )
                            {
                                flog(LOG_ERROR, _("i=%d len=%d %s %02X bad hex in request URI, disconnecting\n"), i, urilen, uri+i+1, hexChar);
                                sc.forwardStatusline(string(FAIL_STR) + _(" bad hex in request URI.\n"));
                                return;
                            }
                            transformedURI+= (char)hexChar;
                            i+= 2;
                            break;
                        case '/':
                            // remove first forward slash.
                            if(i==0) break;
                            // fall through
                        default:
                            transformedURI+= uri[i];
                            break;
                    }
                }

                // split the string: /corename/command -> corename, command
                vector<string> uriwords= Cli::splitString(transformedURI.c_str(), "/");

                if(uriwords.size()>=2)
                {
                    string coreName= uriwords[0],
                           command= transformedURI.substr(coreName.size()+1, transformedURI.size()-coreName.size()-1);

//                    flog(LOG_INFO, "corename: '%s' command: '%s'\n", coreName.c_str(), command.c_str());

                    // immediately connect the client to the core named in the request string,
                    // then execute the requested command.
                    CoreInstance *ci= findNamedInstance(coreName);
                    if(!ci)
                    {
                        sc.forwardStatusline(string(FAIL_STR) + " " + _("No such instance.\n"));
                        return;
                    }

                    if(lineIndicatesDataset(command))
                    {
                        sc.forwardStatusline(string(FAIL_STR) + _(" data sets not allowed in HTTP GET requests.\n"));
                        return;
                    }

                    sc.coreID= ci->getID();
                    lineFromClient(command, sc, timestamp);
                }
                else
                {
                    if(Cli::splitString(transformedURI.c_str()).size())
                    {
                        if(lineIndicatesDataset(transformedURI))
                        {
                            sc.forwardStatusline(string(FAIL_STR) + _(" data sets not allowed in HTTP GET requests.\n"));
                            return;
                        }

                        // try to execute the request as one command.
                        lineFromClient(transformedURI, sc, timestamp);
                    }
                    else
                    {
                        // empty request string received. return information and disconnect.
                        flog(LOG_ERROR, _("empty HTTP request string, disconnecting.\n"));
                        sc.forwardStatusline(format(_("%s this is the GraphServ HTTP module listening on port %d. "
                                                      "protocol-version is %s. %d core instance(s) running, "
                                                      "%d client connection(s) active including yours.\n"),
                                                    SUCCESS_STR, httpPort, stringify(PROTOCOL_VERSION), coreInstances.size(), sessionContexts.size()));
                    }
                }
            }
        }

        friend class ccInfo;
        friend class ccServerStats;
};

#endif // SERVAPP_H
