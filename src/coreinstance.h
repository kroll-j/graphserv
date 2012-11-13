// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011, 2012
// CoreInstance class. handles graphcore command queueing and execution.
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

#ifndef COREINSTANCE_H
#define COREINSTANCE_H

// CoreInstance command queue entry.
struct CommandQEntry
{
	string command;         // command
	deque<string> dataset;  // data set, if any
	uint32_t clientID;      // who runs this command
	bool acceptsData;       // command accepts an input data set (colon)?
	bool dataFinished;      // data set was terminated with empty line?
    double sendBeginTime;   // when did the client begin to send this command

	CommandQEntry(): clientID(0), acceptsData(false), dataFinished(true)
	{ }

	bool flushable()
	{
	    return (!acceptsData) || dataFinished;
	}
    
    void appendToDataset(const string& line)
    {
        if(!acceptsData || dataFinished)
            return;
        dataset.push_back(line);
        if(Cli::splitString(line.c_str()).size()==0)
            dataFinished= true;
    }
};

class LineRecvQ
{
	public:
		// todo: figure out how to use move semantics
		deque<string> nextLines(const string& str)
		{
			deque<string> lineQueue;
			for(string::const_iterator it= str.begin(); it!=str.end(); it++)
			{
				readbuf+= *it;
				if(*it=='\n')
				{
					lineQueue.push_back(readbuf);
					readbuf.clear();
				}
			}
			return (lineQueue);
		}
		
		deque<string> nextLines(int fd)
		{
			const size_t BUFSIZE= 1024;
			char buf[BUFSIZE];
			ssize_t sz= read(fd, buf, sizeof(buf)-1);
			if(sz>0)
			{
				buf[sz]= 0;
				return (nextLines(buf));
			}
			return (deque<string>());
		}
	private:
		string readbuf;
};

// a CoreInstance object handles a GraphCore process.
class CoreInstance: public NonblockWriter
{
    public:
        string linebuf;     // data read from core gets buffered here.
		LineRecvQ stderrQ;	// data read from core stderr gets buffered here.

        CoreInstance(uint32_t _id, const string& _corePath):
            instanceID(_id), lastClientID(0), expectingReply(false), expectingDataset(false), corePath(_corePath),
            processRunning(false)
        {
            pipeToCore[0]= pipeToCore[1]= -1;
            pipeFromCore[0]= pipeFromCore[1]= -1;
            pipeFromCoreStderr[0]= pipeFromCoreStderr[1]= -1;
        }

        ~CoreInstance()
        {
            close(pipeToCore[1]);
            close(pipeFromCore[0]);
            close(pipeFromCoreStderr[0]);
        }

        void writeFailed(int _errno)
        {
            logerror(_("write failed"));
            // read will return 0, core will be removed.
        }

        // try to start with given binary path name (default parameter falls back to the path set in constructor).
        bool startCore(const char *path= 0)
        {
            if(pipe(pipeToCore)==-1 || 
			   pipe(pipeFromCore)==-1 ||
			   pipe(pipeFromCoreStderr)==-1)
            {
                setLastError(string("pipe(): ") + strerror(errno));
                return false;
            }

            if(path==0) path= corePath.c_str();

            flog(LOG_INFO, _("starting core: %s\n"), path);

            pid= fork();
            if(pid==-1)
            {
                setLastError(string("fork(): ") + strerror(errno));
                return false;
            }
            else if(pid==0)
            {
                // child process (core)

                if(dup2(pipeToCore[0], STDIN_FILENO)==-1 || 
				   dup2(pipeFromCore[1], STDOUT_FILENO)==-1 ||
				   dup2(pipeFromCoreStderr[1], STDERR_FILENO)==-1)
                    exit(101);  // setup failed

                close(pipeToCore[1]);
                close(pipeFromCore[0]);
                close(pipeFromCoreStderr[0]);

                setlinebuf(stdout);
                setlinebuf(stderr);

                // dirname and basename may modify their arguments, so duplicate the strings first.
                char dirnameBase[strlen(path)+1];
                char basenameBase[strlen(path)+1];
                strcpy(dirnameBase, path);
                strcpy(basenameBase, path);

                // change to the directory containing the binary
                if(chdir(dirname(dirnameBase))<0)
                    exit(103);  // couldn't chdir()

                char *binName= basename(basenameBase);
                if(execl(binName, binName, NULL)<0)
                    exit(102);  // couldn't exec()
            }
            else
            {
                // parent process (server)

                close(pipeToCore[0]);
                close(pipeFromCore[1]);
                close(pipeFromCoreStderr[1]);

                FILE *toCore= fdopen(pipeToCore[1], "w");
                FILE *fromCore= fdopen(pipeFromCore[0], "r");

                setlinebuf(toCore);

                char line[1024];

                // check that the protocol version strings match.
                fprintf(toCore, "protocol-version\n");
                if(fgets(line, 1024, fromCore))
                {
                    chomp(line);
                    // check that the protocol-version command succeeded.
                    if(strncmp(SUCCESS_STR, line, strlen(SUCCESS_STR))!=0)
                    {
                        setLastError(_("core replied: ") + string(line));
                        return false;
                    }
                    char *coreProtocolVersion= line + strlen(SUCCESS_STR);
                    while(isspace(*coreProtocolVersion) && *coreProtocolVersion) coreProtocolVersion++;
                    // check for matching version string.
                    if(strcmp(coreProtocolVersion, stringify(PROTOCOL_VERSION))!=0)
                    {
                        setLastError(string(_("protocol version mismatch (server: ")) +
                                     stringify(PROTOCOL_VERSION) + " core: " + coreProtocolVersion + ")");
                        return false;
                    }
                    setWriteFd(pipeToCore[1]);
                    processRunning= true;
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
                                     (estatus==101? _("setup failed."):
                                      estatus==102? string(_("couldn't exec '")) + path + "'.":
                                      estatus==103? string(_("couldn't change directory")):
                                      format(_("unknown error code %d"), estatus)) );
                    }
                    else
                       setLastError(_("child process terminated"));
                    return false;
                }
            }
            return false; // never reached. make compiler happy.
        }

        string getLastError() { return lastError; }

        void setLastError(string str) { lastError= str; }

        uint32_t getID() { return instanceID; }
        string getName() { return (name.length()? name: format("Core%02u", instanceID)); }
        void setName(string nm) { name= nm; }   // must *not* check for validity of name.
        pid_t getPid() { return pid; }
        int getReadFd() { return pipeFromCore[0]; }
        int getStderrReadFd() { return pipeFromCoreStderr[0]; }
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
            // this is inefficient because of the nature of deque<>, and currently not used.
            // commands from disconnected clients are removed on flush.
            for(commandQ_t::iterator it= commandQ.begin(); it!=commandQ.end(); it++)
            {
                if(it->clientID==clientID)
                {
                    commandQ.erase(it);
                    it= commandQ.begin();
                }
            }
        }

        // try writing out commands from queue to core process
        void flushCommandQ(class Graphserv &app);

        // queue a command for execution.
        void queueCommand(string &cmd, uint32_t clientID, bool hasDataSet)
        {
            CommandQEntry ce;
            ce.acceptsData= hasDataSet;
            ce.dataFinished= false;
            ce.clientID= clientID;
            ce.command= cmd;
            ce.sendBeginTime= getTime();
            commandQ.push_back(ce);
        }

        // return the client which executed the last command; i. e. the client which current output from
        // core will be sent to.
        uint32_t getLastClientID()
        {
            return lastClientID;
        }

        // true if this core is running a command for this client or has a command for this client in its queue.
        bool hasDataForClient(uint32_t clientID)
        {
//            flog(LOG_INFO, "hasDataForClient: isclientid %d, expectingReply %d, expectingDataset %d, findLastClientCommand(clientID) %d\n",
//                 lastClientID==clientID, expectingReply, expectingDataset, findLastClientCommand(clientID));
            return (lastClientID==clientID && (expectingReply||expectingDataset)) || findLastClientCommand(clientID);
        }

        // handle a line of text which was sent from the core process.
        void lineFromCore(string &line, class Graphserv &app);

        // whether the process is running. false means it has not started yet or was terminated.
        bool isRunning() { return processRunning; }

        // terminate process. main loop will be notified of termination.
        bool terminate()
        {
            if(kill(pid, SIGTERM)<0)
                return false;
            processRunning= false;
            return true;
        }

    private:
        uint32_t instanceID;
        string lastError;
        string name;

        pid_t pid;
        int pipeToCore[2];      	// writable from server (core's stdin)
        int pipeFromCore[2];    	// writable from core (core's stdout)
        int pipeFromCoreStderr[2];  // writable from core (core's stderr)

        typedef deque<CommandQEntry> commandQ_t;
        commandQ_t commandQ;

        uint32_t lastClientID;  // ID of client who executed the last command. ie: client who should receive output

        bool expectingReply;    // currently expecting a status reply from core (ok/failure/error)
        bool expectingDataset;  //          ''         a data set from core

        string corePath;

        bool processRunning;

        friend class ccInfo;
        friend class ccShutdown;
};




#endif // COREINSTANCE_H
