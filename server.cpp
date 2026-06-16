#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <fcntl.h>

using namespace std;


void errorHandler(const char* msg)
{
    cout<<"errorHandlerHandler in: ";
    cout<<msg<<endl;
    abort();
}


void handleConn(int connfd)
{
    char buff[64]={};
    ssize_t n=read(connfd, buff, sizeof(buff)-1);

    if(n<0)
    {
        errorHandler("read errorHandler");
        return;
    }

    cout<<"client says: "<<buff<<endl;

    char retbuff[]="world!!";
    write(connfd, retbuff, strlen(retbuff));

    return;
}




int main()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0); //listening socket
    if(fd<0)
    {
        errorHandler("socket error");
    }


    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); //reuse the same address/port on restart

    //binding the socket

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5142);
    addr.sin_addr.s_addr = htonl(0LL);
    
    int ret=bind(fd, (const struct sockaddr*)&addr, sizeof(addr));
    if(ret)
    {
        errorHandler("bind error");
    }


    ret=listen(fd, SOMAXCONN);
    if(ret)
    {
        errorHandler("listen error");
    }

    //event loop
    while(true)
    {
        struct sockaddr_in client_info = {};
        socklen_t addrlen = sizeof(client_info);

        int connfd = accept(fd, (struct sockaddr* )&client_info, &addrlen);

        if(connfd<0) continue;

        handleConn(connfd);
        close(connfd);
    }


    return 0;

}
