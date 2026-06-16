#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <stdint.h>

using namespace std;

void errorHandler(const char* msg)
{
    cout<<"error in: ";
    cout<<msg<<endl;
    abort();
}

int main()
{
    int fd=socket(AF_INET, SOCK_STREAM, 0);
    if(fd<0)
    {
        errorHandler("socket");
    }

    struct sockaddr_in server_addr={};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5142);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int ret=connect(fd, (const struct sockaddr*)&server_addr, sizeof(server_addr));


    if(ret)
    {
        errorHandler("connect");
    }


    char msg[]="hello!! ";
    char recvbuff[64];
    write(fd, msg, sizeof(msg));


    ssize_t n = read(fd, recvbuff, sizeof(recvbuff)-1);

    if(n<0)
    {
        errorHandler("read");
    }


    cout<<"server says: "<<recvbuff<<endl;
    close(fd);

    return 0;

}