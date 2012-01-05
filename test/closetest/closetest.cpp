// quick test hack to check for leaked file descriptors.
// running 'make test' on linux will spawn lots of graphserv connections using this program.
// lsof should show all fds being closed.

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>  /* for sockaddr_in and inet_addr() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */

void die(const char *str)
{
    perror(str);
    exit(1);
}

int main(int argc, char **argv)
{
    char *addrStr= (argc>1? argv[1]: (char*)"91.198.174.201");
    int sock= socket(AF_INET, SOCK_STREAM, 0);
    if(sock==-1) die("socket");
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family= AF_INET;
    addr.sin_addr.s_addr= inet_addr(addrStr);
    addr.sin_port= htons(6666);
    if( connect(sock, (const struct sockaddr*)&addr, sizeof(sockaddr_in)) != 0 ) die("connect");
    
    puts("connected.");
    const char *cmd= "use-graph test\n";
    if( write(sock, cmd, strlen(cmd)) != strlen(cmd) ) die("write");
    char buf[1024];
    int r= read(sock, buf, sizeof(buf));
    if(r==0) die("EOF");
    if(r<0) die("read");
    buf[r]= 0;
    printf("received: %s", buf);
//    sleep(3);
    puts("disconnecting.");
    close(sock);

    sleep(20);	// keep the process alive for a while.
    
    return 0;
}

