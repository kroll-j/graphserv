// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011
// utilities and helper functions.

#ifndef UTILS_H
#define UTILS_H

enum Loglevel
{
    LOG_INFO,
    LOG_ERROR,
    LOG_AUTH,
    LOG_CRIT
};

extern uint32_t logMask;

void flog(Loglevel level, const char *fmt, ...)
{
    if( !(logMask & (1<<level)) && level!=LOG_CRIT ) return;

    char timeStr[200];
    time_t t;
    struct tm *tmp;

    t= time(0);
    tmp= localtime(&t);
    if(!tmp)
        strcpy(timeStr, "localtime failed");
    else if(strftime(timeStr, sizeof(timeStr), "%F %H:%M.%S", tmp)==0)
        strcpy(timeStr, "strftime returned 0");

    fprintf(stderr, "[%s] ", timeStr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define logerror(str)   \
    flog(LOG_ERROR, "%s: %s\n", str, strerror(errno))

// time measurement
inline double getTime()
{
    timeval tv;
    if(gettimeofday(&tv, 0)<0) logerror("gettimeofday");
    return tv.tv_sec + tv.tv_usec*0.000001;
}

// set an fd to non-blocking mode
inline bool setNonblocking(int fd, bool on= true)
{
	int opts= fcntl(fd, F_GETFL);
	if(opts<0)
    {
		logerror("fcntl(F_GETFL)");
		return false;
	}
	if(on) opts|= O_NONBLOCK;
	else opts&= (~O_NONBLOCK);
	if(fcntl(fd, F_SETFL, opts)<0)
	{
		logerror("fcntl(F_SETFL)");
		return false;
	}
	return true;
}

// set close-on-exec mode. used for fds which we don't want to hand down to child processes.
inline bool closeOnExec(int fd)
{
    int opts= fcntl(fd, F_GETFD);
    if(opts<0)
    {
        logerror("fcntl(F_GETFD)");
        return false;
    }
    opts|= FD_CLOEXEC;
    if(fcntl(fd, F_SETFD, opts)<0)
    {
        logerror("fcntl(F_SETFD)");
        return false;
    }
    return true;
}



// translate status-string to status-code
inline CommandStatus getStatusCode(const string& msg)
{
    for(unsigned i= 0; i<sizeof(statusMsgs)/sizeof(statusMsgs[0]); i++)
        if(statusMsgs[i]==msg)
            return (CommandStatus)i;
    flog(LOG_ERROR, "getStatusCode called with bad string %s. Please report this bug.\n", msg.c_str());
    return CMD_FAILURE;
}


// check whether a status line indicates that a data set will follow.
static bool statuslineIndicatesDataset(const string& line)
{
    size_t pos= line.find(':');
    if(pos==string::npos) return false;
    for(; pos<line.size(); pos++)
        if(!isspace(line[pos])) return false;
    return true;
}




// base class for handling buffered writes to a non-blocking fd.
class NonblockWriter
{
    public:
        NonblockWriter(): fd(-1) {}

        void setWriteFd(int _fd) { fd= _fd; setNonblocking(fd); }

        // try flushing the write buffer.
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

        // write or buffer a string.
        void write(const string s)
        {
            buffer.push_back(s);
            flush();
        }

        // write or buffer a printf-style string.
        void writef(const char *fmt, ...)
        {
            char c[2048];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(c, sizeof(c), fmt, ap);
            va_end(ap);
            write(c);
        }

        // calculate the size of the write buffer in bytes.
        size_t getWritebufferSize()
        {
            size_t ret= 0;
            for(deque<string>::iterator it= buffer.begin(); it!=buffer.end(); it++)
                ret+= it->length();
            return ret;
        }

        // error callback.
        virtual void writeFailed(int _errno)= 0;

    private:
        int fd;
        deque<string> buffer;

        // write a string without buffering. return number of bytes written.
        size_t writeString(const string& s)
        {
            ssize_t sz= ::write(fd, s.data(), s.size());
            if(sz<0)
            {
                if( (errno!=EAGAIN)&&(errno!=EWOULDBLOCK) )
                    logerror("write"),
                    writeFailed(errno);
                return 0;
            }
            return sz;
        }
};

#endif // UTILS_H
