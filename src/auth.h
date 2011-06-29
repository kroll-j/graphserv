// Graph Processor server component.
// (c) Wikimedia Deutschland, written by Johannes Kroll in 2011
// authority base class, password authority.

#ifndef AUTH_H
#define AUTH_H


enum AccessLevel
{
    ACCESS_READ= 0,
    ACCESS_WRITE,
    ACCESS_ADMIN
};
// these must match the access levels.
static const char *gAccessLevelNames[]=
    { "read", "write", "admin" };


// abstract base class for authorities
class Authority
{
    public:
        Authority() { }
        virtual ~Authority() { }

        virtual string getName()= 0;
        // try to authorize using given credentials. write maximum access level to 'level' on success.
        virtual bool authorize(const string& credentials, AccessLevel &level)= 0;
};


// the password authority reads from a htpasswd file and corresponding group file.
// authorize() takes a string in the form "user:password".
class PasswordAuth: public Authority
{
    string htpasswdFilename;
    string groupFilename;

    struct userInfo
    {
        string hash;                // the crypt()ed password
        AccessLevel accessLevel;    // maximum access level
    };
    map<string,userInfo> users;
    time_t lastCacheRefresh;

    vector<string> splitLine(string line, char sep= ':')
    {
        string s;
        vector<string> ret;
        if(line.empty()) return ret;
        while(isspace(line[line.size()-1])) line.resize(line.size()-1);
        line+= sep;
        for(size_t i= 0; i<line.size(); i++)
        {
            if(line[i]==sep)
            { ret.push_back(s); s.clear(); }
            else s+= line[i];
        }
        return ret;
    }

    bool readCredentialFiles()
    {
        char line[1024];
        FILE *f= fopen(htpasswdFilename.c_str(), "r");
        if(!f)
        {
            flog(LOG_CRIT, _("couldn't open %s: %s\n"), htpasswdFilename.c_str(), strerror(errno));
            exit(1);
        }
        map<string,userInfo> newUsers;
        // for each line in htpassword file
        while(fgets(line, 1024, f))
        {
            // read line in the form user:hash and stuff it into the cache
            vector<string> fields= splitLine(line);
            if(fields.size()!=2 || fields[0].empty() || fields[1].size()!=13)
            {
                flog(LOG_ERROR, _("PasswordAuth: invalid line in htpasswd file\n"));
                return false;
            }
            userInfo ui= { fields[1], ACCESS_READ };
            newUsers[fields[0]]= ui;
        }

        f= fopen(groupFilename.c_str(), "r");
        if(!f)
        {
            flog(LOG_CRIT, _("couldn't open %s: %s\n"), groupFilename.c_str(), strerror(errno));
            exit(1);
        }
        // for each line in group file
        while(fgets(line, 1024, f))
        {
            // read line of the form accesslevel:::user1,user2,userX
            vector<string> fields= splitLine(line);
            if(fields.size()!=4 || fields[0].empty())
            {
                flog(LOG_ERROR, _("PasswordAuth: invalid line in group file\n"));
                return false;
            }
            AccessLevel level;
            if(fields[0]==gAccessLevelNames[ACCESS_READ]) level= ACCESS_READ;
            else if(fields[0]==gAccessLevelNames[ACCESS_WRITE]) level= ACCESS_WRITE;
            else if(fields[0]==gAccessLevelNames[ACCESS_ADMIN]) level= ACCESS_ADMIN;
            else
            {
                flog(LOG_ERROR, _("PasswordAuth: invalid access level '%s' in group file\n"), fields[0].c_str());
                return false;
            }

            // go through the specified users and elevate their access levels
            // so that each user gets the maximum specified:
            // a user in both "admin" and "write" groups gets admin access.
            vector<string> usernames= splitLine(fields[3], ',');
            for(vector<string>::iterator it= usernames.begin(); it!=usernames.end(); it++)
            {
                map<string,userInfo>::iterator user= newUsers.find(*it);
                if(user!=newUsers.end() && level>user->second.accessLevel)
                    user->second.accessLevel= level;
            }
        }

        // save cache only after we're successfully finished
        users.clear();
        users= newUsers;
        return true;
    }

    void refreshFileCache()
    {
        time_t curtime= time(0);
        struct stat st;
        bool needRefresh= false;
        // check if any of the credential files have changed since last refresh.
        if(stat(htpasswdFilename.c_str(), &st)<0)
        { logerror(_("couldn't stat passwdfile")); return; }
        if(st.st_mtime>=lastCacheRefresh) needRefresh= true;
        else if(stat(groupFilename.c_str(), &st)<0)
        { logerror(_("couldn't stat groupfile")); return; }
        if(st.st_mtime>=lastCacheRefresh || needRefresh)
        {
            // something has changed, or we didn't read the files yet.
            // refresh the cache.
            lastCacheRefresh= curtime;
            readCredentialFiles();
        }
    }

    public:
        PasswordAuth(const string& _htpasswdFilename, const string& _groupFilename):
            htpasswdFilename(_htpasswdFilename), groupFilename(_groupFilename), lastCacheRefresh(0)
        {
            readCredentialFiles();
            lastCacheRefresh= time(0);
        }

        string getName() { return "password"; }

        bool authorize(const string& credentials, AccessLevel &level)
        {
            // load valid user/password combinations and group info into cache, if necessary.
            refreshFileCache();

            vector<string> cred= splitLine(credentials);
            if(cred.size()!=2 || cred[0].empty()||cred[1].empty())
            { flog(LOG_AUTH, _("PasswordAuth: invalid credentials.\n")); return false; }

            map<string,userInfo>::iterator it= users.find(cred[0]);
            if(it==users.end())
            { flog(LOG_AUTH, _("PasswordAuth: invalid user.\n")); return false; }

            // crypt() the password and compare to stored hash.
            char *crypted= crypt(cred[1].c_str(), it->second.hash.c_str());
            if(crypted != it->second.hash)
            {
                flog(LOG_AUTH, _("PasswordAuth: failure, user %s\n"), it->first.c_str());
                return false;
            }

            flog(LOG_AUTH, _("PasswordAuth: success, user %s, level %s\n"), it->first.c_str(), gAccessLevelNames[it->second.accessLevel]);

            level= it->second.accessLevel;

            return true;
        }
};


#endif // AUTH_H
