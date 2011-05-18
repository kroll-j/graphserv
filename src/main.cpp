// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011

#include <libintl.h>
#include <iostream>
#include <vector>
#include <stdint.h>
#include <unistd.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdarg.h>
#include <algorithm>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "clibase.h"




class ServCli: public Cli
{
    public:
        ServCli()
        {
        }

        CommandStatus execute(char *command)
        {
        };
};

class ServCmd: public CliCommand
{
};


class CoreInstance
{
    public:
        CoreInstance()
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
                    return true;
                }
                else    // fgets() failed
                {
                    int status;
                    pid_t wpid= waitpid(pid, &status, 0);
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
            return false; // make compiler happy.
        }

        string getLastError() { return lastError; }

        void setLastError(string str) { lastError= str; }

    private:
        string lastError;

        int pipeToCore[2];      // writable from server
        int pipeFromCore[2];    // writable from core
        pid_t pid;
};


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

    CoreInstance test;
    if(!test.startCore())
        cout << FAIL_STR << " " << test.getLastError() << endl;

    return 0;
}
