// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011
// server cli command base classes and cli handler class.

#ifndef SERVCLI_H
#define SERVCLI_H

// base class for server commands.
class ServCmd: public CliCommand
{
    public:
        virtual AccessLevel getAccessLevel() { return ACCESS_READ; }
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

// server cli class.
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



#endif // SERVCLI_H
