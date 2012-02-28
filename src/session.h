// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011, 2012
// session contexts.
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

#ifndef SESSION_H
#define SESSION_H

// session context with information about and methods for handling a client connection.
// this base class handles TCP connections.
struct SessionContext: public NonblockWriter
{
    uint32_t clientID;
    AccessLevel accessLevel;
    ConnectionType connectionType;
    uint32_t coreID;    // non-zero if connected to a core instance
    int sockfd;
    string linebuf;             // text which is read from this client is buffered here.
    queue<string> lineQueue;    // lines which arrive from this client while the session is waiting for core reply are buffered here.
    class Graphserv &app;
    double chokeTime;
    // this is set when a client sends an invalid command with a data set.
    // the data set must be read and discarded.
    CommandStatus invalidDatasetStatus;
    string invalidDatasetMsg;   // the status line to send after invalid data set has been read
    double shutdownTime;        // time when shutdown was called on the socket, or 0 if the connection is running.

    // some statistics about this connection. currently mostly used for debugging.
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
        clientID(cID), accessLevel(ACCESS_READ), connectionType(connType), coreID(0), sockfd(sock), app(_app),
        chokeTime(0), invalidDatasetStatus(CMD_SUCCESS), shutdownTime(0)
    {
        setWriteFd(sockfd);
    }

    virtual ~SessionContext()
    {
        setNonblocking(sockfd, false);  // force output to be drained on close.
        flog(LOG_INFO, "closing session context socket %d\n", sockfd);
        close(sockfd);
    }
    
    // true if this session is waiting for a reply from its connected core instance.
    bool isWaitingForCoreReply();

    // write-error callback
    void writeFailed(int _errno);

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


// session context with information about and methods for handling a client connection.
// this class handles HTTP connections.
struct HTTPSessionContext: public SessionContext
{
    bool conversationFinished;  // client will be disconnected when this is false and there's no buffered data left.

    struct HttpClientState
    {
        vector<string> request;
        string requestString;
        unsigned commandsExecuted;
        HttpClientState(): commandsExecuted(0) { }
    } http;

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
//        write(title + "\n" + description + "\n");
        write(description);
        if(description.rfind('\n')!=description.size()-1) write("\n");
    }

    void httpWriteErrorResponse(int code, const string &title, const string &description, const string &optionalField= "")
    {
        httpWriteResponseHeader(code, title, "text/plain", optionalField);
        httpWriteErrorBody(title, description);
    }

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



#endif // SESSION_H
