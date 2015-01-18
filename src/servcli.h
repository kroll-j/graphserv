// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011, 2012
// server cli command base classes and cli handler class.
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
