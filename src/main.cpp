// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011, 2012
// main module.
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
#include <libgen.h>

#include "clibase.h"
#include "const.h"
#include "utils.h"
#include "auth.h"
#include "coreinstance.h"
#include "session.h"
#include "servcli.h"
#include "servapp.h"




// GraphServ main module.
// from the compiler's pov, all code is contained in one compilation unit, which creates
// better opportunities for optimization and inlining.
// member functions which can't be defined inside their class declarations are defined here.


/////////////////////////////////////////// CoreInstance ///////////////////////////////////////////

// handle a line of text arriving from a core.
void CoreInstance::lineFromCore(string &line, class Graphserv &app)
{
    SessionContext *sc= app.findClient(lastClientID);
    // check state and forward status line or data set to client.
    if(expectingReply)
    {
        expectingReply= false;
        if(lineIndicatesDataset(line))
            expectingDataset= true;         // save flag to determine when a command is finished.
        if(sc)
        {
            if(logMask&(1<<LOG_INFO))
            {
                vector<string> words= Cli::splitString(line.c_str());
                if(words.size() && getStatusCode(words[0])!=CMD_SUCCESS)
                    flog(LOG_INFO, "core '%s', pid %d: status: %s", name.c_str(), pid, line.c_str());
            }
            sc->forwardStatusline(line);    // virtual fn does http-specific stuff
        }
    }
    else if(expectingDataset)
    {
        if(Cli::splitString(line.c_str()).size()==0)
            expectingDataset= false;        // save flag to determine when a command is finished.
        if(sc)
            sc->forwardDataset(line);       // virtual function does http-specific stuff, if any
    }
    else
    {
        // a core sent data we didn't ask for. this shouldn't happen.
        if(app.findClient(lastClientID))
            flog(LOG_ERROR, _("CoreInstance '%s', ID %u: lineFromCore(): not expecting anything from this core\n"), getName().c_str(), getID());
    }
}

// write out as much commands from queue to core process as possible.
void CoreInstance::flushCommandQ(class Graphserv &app)
{
    while( commandQ.size() && (!expectingReply) && (!expectingDataset) && commandQ.front().flushable() )
    {
        CommandQEntry &c= commandQ.front();
        write(c.command);
        for(deque<string>::iterator it= c.dataset.begin(); it!=c.dataset.end(); it++)
            write(*it);
        lastClientID= c.clientID;
        expectingReply= true;
        expectingDataset= false;
        commandQ.pop_front();
    }
}




/////////////////////////////////////////// SessionContext ///////////////////////////////////////////

// error handler called because of broken connection or similar. tell app to disconnect this client.
void SessionContext::writeFailed(int _errno)
{
    flog(LOG_ERROR, _("client %d: write failed, disconnecting.\n"), clientID);
    app.forceClientDisconnect(this);
}

// true if this session is waiting for a reply from its connected core instance.
bool SessionContext::isWaitingForCoreReply()
{
    CoreInstance *instance= app.findInstance(coreID);
    if(!instance) return false;
    return instance->hasDataForClient(clientID);
}

// forward statusline to http client, possibly mark client to be disconnected
void HTTPSessionContext::forwardStatusline(const string& line)
{
    // if we already ran at least one command, don't write the HTTP header again.
    if(++http.commandsExecuted > 1)
    {
        SessionContext::forwardStatusline(line);
        return;
    }
    vector<string> replyWords= Cli::splitString(line.c_str());
    if(replyWords.empty())
    {
        // this shouldn't happen.
        httpWriteErrorResponse(500, "Internal Server Error", _("Received empty status line from core. Please report."));
        app.shutdownClient(this);
    }
    else
    {
        bool hasDataset= lineIndicatesDataset(line);
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
                httpWriteErrorResponse(401, "Not Authorized", line, headerStatusLine);
                break;
            case CMD_VALUE:
                httpWriteResponseHeader(222, "Value", "text/plain", headerStatusLine);
                write(line);
                break;
            default:
                httpWriteErrorResponse(500, "Invalid GraphCore Status Line", line, headerStatusLine);
                break;
        }

        // if there's nothing left to forward, mark client to be disconnected.
        if(!hasDataset)
            flog(LOG_INFO, _("client %d: conversation finished.\n"), clientID),
            conversationFinished= true;
    }
}




/////////////////////////////////////////// ServCli ///////////////////////////////////////////

// parse and execute a server command line.
CommandStatus ServCli::execute(string command, class SessionContext &sc)
{
    vector<string> words= splitString(command.c_str(), " \t\n");
    if(words.empty()) return CMD_SUCCESS;
    ServCmd *cmd= (ServCmd*)findCommand(words[0]);
    if(!cmd)
    {
        sc.forwardStatusline(FAIL_STR + string(_(" no such server command.\n")));
        return CMD_FAILURE;
    }
    return execute(cmd, words, sc);
}

// execute a server command.
CommandStatus ServCli::execute(ServCmd *cmd, vector<string> &words, SessionContext &sc)
{
    if(cmd->getAccessLevel() > sc.accessLevel)
    {
        sc.forwardStatusline(DENIED_STR + format(_(" insufficient access level (command needs %s, you have %s)\n"),
                             gAccessLevelNames[cmd->getAccessLevel()], gAccessLevelNames[sc.accessLevel]));
        return CMD_FAILURE;
    }
    CommandStatus ret;
    switch(cmd->getReturnType())
    {
        case CliCommand::RT_OTHER:
            ret= ((ServCmd_RTOther*)cmd)->execute(words, app, sc);
            // ServCmd_RTOther::execute will forward everything to the client.
            break;
        case CliCommand::RT_NONE:
            ret= ((ServCmd_RTVoid*)cmd)->execute(words, app, sc);
            sc.forwardStatusline(cmd->getStatusMessage().c_str());
            break;
        default:
            flog(LOG_ERROR, _("ServCli::execute: invalid return type %d\n"), cmd->getReturnType());
            ret= CMD_ERROR;
            break;
    }
    return ret;
}



/////////////////////////////////////////// server commands ///////////////////////////////////////////

class ccQuit: public ServCmd_RTOther
{
    public:
        string getName() { return "quit"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("disconnect from the server."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            cliSuccess("bye.\n");
            sc.forwardStatusline(lastStatusMessage);
            app.shutdownClient(&sc);
            return CMD_SUCCESS;
        }

};

class ccCreateGraph: public ServCmd_RTVoid
{
    public:
        string getName() { return "create-graph"; }
        string getSynopsis() { return getName() + " GRAPHNAME"; }
        string getHelpText() { return _("create a named graphcore instance.\n"
                                        "# graph names may contain only alphabetic characters (a-z A-Z), digits (0-9), hyphens (-) and underscores (_).\n"
                                        "# graph names must start with an alphabetic character, a hyphen or an underscore."); }
        AccessLevel getAccessLevel() { return ACCESS_ADMIN; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=2)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            if(!app.isValidGraphName(words[1]))
            {
                cliFailure("invalid graph name.\n");
                return CMD_FAILURE;
            }
            // check whether named instance already exists, try spawning core instance, return.
            if(app.findNamedInstance(words[1])) { cliFailure(_("an instance with this name already exists.\n")); return CMD_FAILURE; }
            CoreInstance *core= app.createCoreInstance(words[1]);
            if(!core) { cliFailure(_("Graphserv::createCoreInstance() failed.\n")); return CMD_FAILURE; }
            if(!core->startCore()) { cliFailure("startCore(): %s\n", core->getLastError().c_str()); app.removeCoreInstance(core); return CMD_FAILURE; }
            cliSuccess(_("spawned pid %d.\n"), (int)core->getPid());
            return CMD_SUCCESS;
        }

};

class ccUseGraph: public ServCmd_RTVoid
{
    public:
        string getName() { return "use-graph"; }
        string getSynopsis() { return getName() + " GRAPHNAME"; }
        string getHelpText() { return _("connect to a named graphcore instance."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=2)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            
            CoreInstance *core= app.findNamedInstance(words[1]);
            if(!core) { cliFailure(_("no such instance.\n")); return CMD_FAILURE; }
            if(!app.reconnectSession(&sc, core))
            {
                cliFailure(_("could not reconnect session.\n"));
                return CMD_FAILURE;
            }
            cliSuccess(_("connected to pid %d.\n"), (int)core->getPid());
            return CMD_SUCCESS;
        }

};

class ccDropGraph: public ServCmd_RTVoid
{
    public:
        string getName() { return "drop-graph"; }
        string getSynopsis() { return getName() + " GRAPHNAME"; }
        string getHelpText() { return _("drop a named graphcore instance immediately (terminate the process)."); }
        AccessLevel getAccessLevel() { return ACCESS_ADMIN; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=2)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            CoreInstance *core= app.findNamedInstance(words[1]);
            if(!core) { cliNone(_("no such instance.\n")); return CMD_FAILURE; }
            if(!core->terminate())
            {
                cliFailure(_("couldn't kill the process. %s\n"), strerror(errno));
                return CMD_FAILURE;
            }
            flog(LOG_INFO, _("client %u killed core with ID %u, pid %d.\n"), sc.clientID, core->getID(), (int)core->getPid());
            cliSuccess(_("killed core with ID %u, pid %d.\n"), core->getID(), (int)core->getPid());
//  we shouldn't block here, waiting for the child is done in the select loop.
//            int status;
//            waitpid(core->getPid(), &status, 0);
//            app.removeCoreInstance(core);
            return CMD_SUCCESS;
        }

};

class ccListGraphs: public ServCmd_RTOther
{
    public:
        string getName() { return "list-graphs"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("list currently running graphcore instances."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                sc.forwardStatusline(lastStatusMessage);
                return CMD_FAILURE;
            }
            cliSuccess(_("running graphs:\n"));
            sc.forwardStatusline(lastStatusMessage);
            map<uint32_t,CoreInstance*>& cores= app.getCoreInstances();
            for(map<uint32_t,CoreInstance*>::iterator it= cores.begin(); it!=cores.end(); it++)
                if(it->second->isRunning())
                    sc.forwardDataset(it->second->getName() + "\n");
            sc.forwardDataset("\n");
            return CMD_SUCCESS;
        }

};

class ccSessionInfo: public ServCmd_RTOther
{
    public:
        string getName() { return "session-info"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("returns information on your current session."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                sc.forwardStatusline(lastStatusMessage);
                return CMD_FAILURE;
            }
            cliSuccess(_("session info:\n"));
            sc.forwardStatusline(lastStatusMessage);
            CoreInstance *ci= app.findInstance(sc.coreID);
            // prints minimal info. might print some session statistics (avg lines queued, etc).
            sc.forwardDataset(format("ConnectedGraph,%s\n", ci? ci->getName().c_str(): "None").c_str());
            sc.forwardDataset(format("AccessLevel,%s\n", gAccessLevelNames[sc.accessLevel]).c_str());
            sc.forwardDataset("\n");
            return CMD_SUCCESS;
        }

};

class ccServerStats: public ServCmd_RTOther
{
    public:
        string getName() { return "server-stats"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("returns information on the server."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                sc.forwardStatusline(lastStatusMessage);
                return CMD_FAILURE;
            }
            cliSuccess(_("server info:\n"));
            sc.forwardStatusline(lastStatusMessage);
            // this currently just outputs the minimal info: number of cores. should return more useful info.
            map<uint32_t,CoreInstance*>& cores= app.getCoreInstances();
            size_t runningCores= 0;
            for(map<uint32_t,CoreInstance*>::iterator it= cores.begin(); it!=cores.end(); it++)
                if(it->second->isRunning())
                    runningCores++;
            sc.forwardDataset(format("NCores,%zu\n", runningCores));
            sc.forwardDataset(format("TotalLinesFromClients,%u\n", app.linesFromClients));
            sc.forwardDataset("\n");
            return CMD_SUCCESS;
        }

};

class ccAuthorize: public ServCmd_RTVoid
{
    public:
        string getName() { return "authorize"; }
        string getSynopsis() { return getName() + " AUTHORITY CREDENTIALS"; }
        string getHelpText() { return _("authorize with the named authority using the given credentials."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=3)
            {
                syntaxError();
                return CMD_FAILURE;
            }
            // find the authority to use.
            // currently there's only one (password).
            Authority *auth= app.findAuthority(words[1]);
            if(!auth)
            {
                cliFailure(_("no such authority '%s'.\n"), words[1].c_str());
                return CMD_FAILURE;
            }
            AccessLevel newAccessLevel;
            if(!auth->authorize(words[2], newAccessLevel))
            {
                cliFailure(_("authorization failure.\n"));
                return CMD_FAILURE;
            }
            sc.accessLevel= newAccessLevel;
            cliSuccess(_("access level: %s\n"), gAccessLevelNames[sc.accessLevel]);
            return CMD_SUCCESS;
        }

};

class ccProtocolVersion: public ServCmd_RTVoid
{
    public:
        string getName() { return "protocol-version"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("the protocol-version is used to check for compatibility of the server and core binaries.\n"
                                        "# this command prints the protocol-version of the server."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                return CMD_FAILURE;
            }

            cliSuccess(stringify(PROTOCOL_VERSION) "\n");

            return CMD_SUCCESS;
        }

};

#ifdef DEBUG_COMMANDS
// the "i" command is really just for debugging.
class ccInfo: public ServCmd_RTOther
{
    public:
        string getName() { return "i"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("print info (debugging)"); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                sc.forwardStatusline(lastStatusMessage);
                return CMD_FAILURE;
            }

            sc.writef("Cores: %d\n", app.coreInstances.size());

            for(map<uint32_t,CoreInstance*>::iterator it= app.coreInstances.begin(); it!=app.coreInstances.end(); it++)
            {
                CoreInstance *ci= it->second;
                sc.writef("Core %d:\n", ci->getID());
                sc.writef("  running: %s\n", ci->isRunning()? "true": "false");
                sc.writef("  queue size: %u\n", ci->commandQ.size());
                sc.writef("  bytes in write buffer: %u\n", ci->getWritebufferSize());
                sc.writef("  expectingReply: %s\n", ci->expectingReply? "true": "false");
                sc.writef("  expectingDataset: %s\n", ci->expectingDataset? "true": "false");
                sc.writef("\n");
            }

            for(map<uint32_t,SessionContext*>::iterator it= app.sessionContexts.begin(); it!=app.sessionContexts.end(); it++)
            {
                SessionContext *ci= it->second;
                sc.writef("Session ID %d:\n", ci->clientID);
                sc.writef("  accessLevel: %s\n", gAccessLevelNames[ci->accessLevel]);
                sc.writef("  connectionType: %s\n", ci->connectionType==CONN_TCP? "TCP": "HTTP");
                sc.writef("  coreID: %u\n", ci->coreID);
                sc.writef("  shutdownTime: %.2f (%.2f)\n", ci->shutdownTime, (ci->shutdownTime? getTime()-ci->shutdownTime: -1));
                sc.writef("\n");
            }

            return CMD_SUCCESS;
        }

};
#endif

// the alternative help command. "help" would be for the server help, "corehelp" for the core.
// superseded by ccHelp below, which seems to work well.
class ccHelpAlt: public ServCmd_RTOther
{
    ServCli &cli;

    public:
        string getName() { return "help"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("get help on commands"); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        ccHelpAlt(ServCli &_cli): cli(_cli)
        { }

        CommandStatus execute(vector<string> words, Graphserv &app, SessionContext &sc)
        {
            cliSuccess(_("available commands:\n"));
            sc.forwardStatusline(lastStatusMessage);
            vector<CliCommand*> &commands= cli.getCommands();
            for(size_t i= 0; i<commands.size(); i++)
                sc.forwardDataset("# " + commands[i]->getSynopsis() + "\n");
            sc.forwardDataset(string("# ") + _("note: 'corehelp' prints help on core commands when connected to a core.\n"));
            sc.forwardDataset("\n");
            return CMD_SUCCESS;
        }

};

// the help command for both server and core
class ccHelp: public ServCmd_RTOther
{
    ServCli &cli;

    public:
        string getName() { return "help"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("get help on commands"); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        ccHelp(ServCli &_cli): cli(_cli)
        { }

        CommandStatus execute(vector<string> words, Graphserv &app, SessionContext &sc)
        {
            if(words.size()>2)
            {
                syntaxError();
                sc.forwardStatusline(lastStatusMessage);
                return CMD_FAILURE;
            }

            CoreInstance *ci= app.findInstance(sc.coreID);
            if(words.size()==1)
            {
                // show list of server commands
                cliSuccess(_("available server commands:\n"));
                sc.forwardStatusline(lastStatusMessage);
                vector<ServCmd*> &commands= (vector<ServCmd*> &)cli.getCommands();
                for(size_t i= 0; i<commands.size(); i++)
                    sc.forwardDataset("# " + commands[i]->getSynopsis() + "\n");
                if(ci)
                {
                    // if connected, show list of core commands too.
                    // sc.forwardDataset(string("# ") + _("the following are the core commands:\n"));
                    string line;
                    for(vector<string>::iterator it= words.begin(); it!=words.end(); it++)
                        line+= *it,
                        line+= " ";
                    line+= "\n";
                    app.sendCoreCommand(sc, line, false, &words);
                }
                else sc.forwardDataset("\n");
            }
            else
            {
                CliCommand *cmd= cli.findCommand(words[1]);
                if(cmd && cmd->getName()!="help")
                {
                    // show help on server command
                    cliSuccess("%s:\n", words[1].c_str());
                    sc.forwardStatusline(lastStatusMessage);
                    sc.forwardDataset("# " + cmd->getSynopsis() + "\n" +
                                      "# " + cmd->getHelpText() + "\n\n");
                }
                else if(app.findInstance(sc.coreID))
                {
                    // forward the command to the core
                    string line;
                    for(vector<string>::iterator it= words.begin(); it!=words.end(); it++)
                        line+= *it,
                        line+= " ";
                    line+= "\n";
                    app.sendCoreCommand(sc, line, false, &words);
                }
                else
                {
                    // no server command and not connected, forward error message.
                    cliFailure(_("no such server command and not connected to a core instance.\n"));
                    sc.forwardStatusline(lastStatusMessage);
                }
            }

            return CMD_SUCCESS;
        }

};

#if 0
// server-info command
class ccServerInfo: public ServCmd_RTOther
{
    public:
        string getName() { return "server-info"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("get information on the server."); }
        AccessLevel getAccessLevel() { return ACCESS_READ; }

        ccServerInfo()
        { }

        CommandStatus execute(vector<string> words, Graphserv &app, SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                sc.forwardStatusline(lastStatusMessage);
                return CMD_FAILURE;
            }

            cliSuccess(":\n");
            sc.forwardStatusline(lastStatusMessage);

            sc.forwardDataset("ProtocolVersion," stringify(PROTOCOL_VERSION));


            sc.forwardDataset("\n");

            return CMD_SUCCESS;
        }

};
#endif


// shutdown command is filtered by the server.
class ccShutdown: public ServCmd_RTVoid
{
    public:
        string getName() { return "shutdown"; }
        string getSynopsis() { return getName(); }
        string getHelpText() { return _("shut down the core instance you are connected to."); }
        AccessLevel getAccessLevel() { return ACCESS_ADMIN; }

        CommandStatus execute(vector<string> words, class Graphserv &app, class SessionContext &sc)
        {
            if(words.size()!=1)
            {
                syntaxError();
                return CMD_FAILURE;
            }

            CoreInstance *ci= app.findInstance(sc.coreID);
            if(!ci)
            {
                cliFailure(_("not connected to a core.\n"));
                return CMD_FAILURE;
            }

            flog(LOG_INFO, "sending shutdown command to core ID %u, pid %d, from client %u.\n",
                 ci->getID(), (int)ci->getPid(), sc.clientID);

            // send the command to the core, mark the core as no longer running.
            // the client will still receive the reply from the core.
            app.sendCoreCommand(sc, "shutdown\n", false);
            ci->processRunning= false;

            return CMD_SUCCESS;
        }

};



ServCli::ServCli(Graphserv &_app): app(_app)
{
    // register server commands.
    addCommand(new ccCreateGraph());
    addCommand(new ccUseGraph());
#ifdef DEBUG_COMMANDS
    addCommand(new ccInfo());
#endif
    addCommand(new ccAuthorize());
    addCommand(new ccHelp(*this));
    addCommand(new ccDropGraph());
    addCommand(new ccListGraphs());
    addCommand(new ccSessionInfo());
    addCommand(new ccServerStats());
    addCommand(new ccProtocolVersion());
    addCommand(new ccQuit());
    addCommand(new ccShutdown());
//    addCommand(new ccServerInfo());
}



// static member variables of main app class.
uint32_t Graphserv::coreIDCounter= 0;
uint32_t Graphserv::sessionIDCounter= 0;


uint32_t logMask= (1<<LOG_ERROR);



static void printHelp(const char *comm= "graphserv")
{
    printf(_("use: %s [options]\n"), comm);
    printf(_(
           "options:\n"
           "    -h              print this text\n"
           "    -t PORT         listen on PORT for tcp connections [" stringify(DEFAULT_TCP_PORT) "]. zero to disable.\n"
           "    -H PORT         listen on PORT for http connections [" stringify(DEFAULT_HTTP_PORT) "]. zero to disable.\n"
           "    -p FILENAME     set htpassword file name [" DEFAULT_HTPASSWD_FILENAME "]\n"
           "    -g FILENAME     set group file name [" DEFAULT_GROUP_FILENAME "]\n"
           "    -c FILENAME     set path of GraphCore binary [" DEFAULT_CORE_PATH "]\n"
           "    -l FLAGS        set logging flags.\n"
           "                        e: log error messages (default)\n"
           "                        i: log error and informational messages\n"
           "                        a: log authentication messages\n"
           "                        q: quiet mode, don't log anything\n"
           "                    flags can be combined.\n"
           "\n"));
}

// helper function for converting a string from the commandline to an unsigned int.
// will print help and exit when invalid input is found.
static unsigned cmdlnParseUint(char *str)
{
    if(!Cli::isValidUint(str))
    {
        printf(_("invalid argument -- '%s'\n"), str);
        printHelp();
        exit(1);
    }
    return atoi(str);
}

/////////////////////////////////////////// main ///////////////////////////////////////////

int main(int argc, char *argv[])
{
    // default values can be overridden on the command line.
    int tcpPort= DEFAULT_TCP_PORT;
    int httpPort= DEFAULT_HTTP_PORT;
    string htpwFilename= DEFAULT_HTPASSWD_FILENAME;
    string groupFilename= DEFAULT_GROUP_FILENAME;
    string corePath= DEFAULT_CORE_PATH;

    // parse the command line.
    char opt;
    while( (opt= getopt(argc, argv, "ht:H:p:g:c:l:"))!=-1 )
        switch(opt)
        {
            case '?':
                printHelp(argv[0]);
                exit(1);
            case 'h':
                printHelp(argv[0]);
                exit(0);
            case 't':
                tcpPort= cmdlnParseUint(optarg);
                break;
            case 'H':
                httpPort= cmdlnParseUint(optarg);
                break;
            case 'p':
                htpwFilename= optarg;
                break;
            case 'g':
                groupFilename= optarg;
                break;
            case 'c':
                corePath= optarg;
                break;
            case 'l':
                for(int i= 0; optarg[i]; i++) switch(optarg[i])
                {
                    case 'i':
                        logMask|= (1<<LOG_INFO);
                        // fall through
                    case 'e':
                        logMask|= (1<<LOG_ERROR);
                        break;
                    case 'a':
                        logMask|= (1<<LOG_AUTH);
                        break;
                    case 'q':
                        logMask= 0;
                        break;
                    default:
                        printf(_("unknown logging flag -- '%c'\n"), optarg[i]);
                        printHelp(argv[0]);
                        exit(1);
                }
                break;
        }

    if( !(tcpPort || httpPort) )
        puts(_("at least one of TCP or HTTP ports must be enabled.")),
        exit(1);

    bindtextdomain("graphserv", "./messages");
    textdomain("graphserv");

    // we don't want broken pipe signals to be delivered to us.
    // exiting core instances are handled in the select loop.
    signal(SIGPIPE, SIG_IGN);

    // instantiate app and kick off main loop.
    Graphserv s(tcpPort, httpPort, htpwFilename, groupFilename, corePath);
    if(!s.run()) return 1;  // exit with error.

    return 0;
}
