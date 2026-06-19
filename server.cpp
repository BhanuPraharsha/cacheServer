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
#include <vector>
#include <poll.h>
#include <map>
#include "hashtable.h"

#define MAX_MSG_SIZE  (32 << 20)

using namespace std;


/*
message protocol:
number_of_arguments + 4 bytes message length + 'len' bytes message +...number_of_arguments times
*/

enum{
    RES_OK = 0, 
    RES_ERR = 1, //error
    RES_NF =  2, //key not found
};

struct response 
{
    uint32_t status=RES_OK;
    vector<uint8_t> data;
};

/*
response format :
status code + data
*/


#define container_of(ptr, type, member) \
    ((type* )((char*)ptr - offsetof(type, member))) // to get back the pointer to the parent struct, from the intrusive structure 'member'

 
struct{ //global k-v storage
    hash_map db;
} g_map;

struct entry{
    struct hash_node node;
    string key;
    string val;
};

bool equal_func(hash_node* a, hash_node* b)
{
    struct entry* e1=container_of(a, entry, node);
    struct entry* e2=container_of(b, entry, node);

    return e1->key == e2->key;
}


void errorHandler(const char* msg)
{
    cout<<"error in: ";
    cout<<msg<<endl;
    abort();
}

void msg_error(const char* msg)
{
    cerr<<msg<<endl;
    return;
}

bool read_4bytes(uint8_t **curr, uint8_t *end, uint32_t &out)
{
    if(*curr+4 > end) return false;

    memcpy(&out, curr, 4);
    *curr += 4;
    return true;
}

bool read_string(uint8_t **curr, uint8_t *end, size_t n, string out)
{
    if(*curr + n > end) return false;

    out.assign(*curr, *curr+n);
    *curr +=n;
    return true;
}


void consume_buffer(vector<uint8_t>& buff, size_t n) 
{
    buff.erase(buff.begin(), buff.begin()+n);
    return;
}



void append_buffer(vector<uint8_t>& buff, uint8_t* from, size_t n) 
{
    buff.insert(buff.end(), from, from+n);
    return;
}

uint64_t str_hash(uint8_t* str, size_t len)
{
    //FNV string hash function
     uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++)
    {
        h = (h + str[i]) * 0x01000193;
    }
    return h;
}


int parse_request(uint8_t *in, size_t n, vector<string>& out) //parsing the execution commands from the message stream
{
    uint32_t nargs;
    uint8_t *end=in+n;

    if(!read_4bytes(&in, end, nargs)) return -1;

    while(out.size()!=nargs)
    {
        uint32_t len;
        if(!read_4bytes(&in, end, len)) return -1;

        string cmd;
        if(!read_string(&in, end, len, cmd)) return -1;

        out.push_back(cmd);
    }


    if(in!=end) return -1; //trailing garbage

    return 0;
}


void GET(vector<string>& cmd, response& resp) 
{
    entry key;
    key.key=cmd[1];
    key.node.hash=str_hash((uint8_t*)(key.key.data()), key.key.size());

    hash_node* target=hm_lookup(&g_map.db, &key.node, equal_func);

    if(target)
    {
        string& val=container_of(target, entry, node)->val;
        resp.data.assign(val.begin(), val.end());
    }
    else{
        resp.status=RES_NF;
    }

    return;
}



void SET(vector<string>& cmd, response& resp) 
{
    entry key;
    key.key=cmd[1];
    key.node.hash=str_hash((uint8_t*)(key.key.data()), key.key.size());
    
    hash_node* target=hm_lookup(&g_map.db, &key.node, equal_func);

    if(target)
    {
        container_of(target, entry, node)->val = cmd[2];
    }
    else{
        entry *curr =new entry();
        curr->node.hash = key.node.hash;
        curr->key=cmd[1];
        curr->val=cmd[2];
        hm_insert(&g_map.db, &curr->node);
    }

    return;
}


void DEL(vector<string>& cmd, response& resp) 
{
    entry key;
    key.key=cmd[1];
    key.node.hash=str_hash((uint8_t*)(key.key.data()), key.key.size());

    hash_node* todelete=hm_delete(&g_map.db, &key.node, equal_func);
    if(todelete!=NULL) delete container_of(todelete, entry, node);

    return;
}



void resolve_request(vector<string>& cmd, response &out) //execution of the commands
{
    if(cmd.size()==2 && cmd[0]=="GET")
    {
        return GET(cmd, out);
    }
    else if(cmd.size()==3 && cmd[0]=="SET") 
    {
        return SET(cmd, out);
    }
    else if(cmd.size()==2 && cmd[0]=="DEL")
    {
        return DEL(cmd, out);
    }
    else{
        out.status=RES_ERR;
    }
    
    return;
}


void make_response(response &resp, vector<uint8_t> &out) // serialise the response
{
    uint32_t len=4 + resp.data.size();
    append_buffer(out, (uint8_t*)&len, 4);
    append_buffer(out, (uint8_t*)&resp.status, 4);
    append_buffer(out, resp.data.data(), resp.data.size());

    return;
}



void setFdNonBlocking(int fd)
{
    int flags=fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;

    fcntl(fd, F_SETFL, flags);

    return;
}

struct conn{
    int fd=-1;
    bool want_to_read=false;
    bool want_to_write=false;
    bool want_to_close=false;
    vector<uint8_t> incomingBuff;
    vector<uint8_t> outgoingBuff;
};


bool try_once(conn* curr) 
{
    if(curr->incomingBuff.size()<4) return false;

    uint32_t len=0;
    memcpy(&len, curr->incomingBuff.data(), 4);


    if(len>MAX_MSG_SIZE) 
    {
        curr->want_to_close=true;
        errorHandler("msg size too large");
        return false;
    }

    if(len+4 > curr->incomingBuff.size()) return false;
    uint8_t* request=&curr->incomingBuff[4];

    vector<string> cmd;
    if(parse_request(request, len, cmd)!=0)
    {
        curr->want_to_close=true;
        msg_error("bad request");
        return false;
    }


    response resp;
    resolve_request(cmd, resp);
    make_response(resp, curr->outgoingBuff);
    consume_buffer(curr->incomingBuff, 4+len);

    return true;
}


conn* handle_accept(int fd) 
{
    struct sockaddr_in client_info ={};
    socklen_t addrlen=sizeof(client_info);
    int connfd=accept(fd, (struct sockaddr*)&client_info, &addrlen);

    if(connfd<0)
    {
        errorHandler("accept()");
        return NULL;
    }

    uint32_t ip=client_info.sin_addr.s_addr;
    cerr<<"client connected from: "<<ip<<": "<<ntohs(client_info.sin_port)<<endl;

    setFdNonBlocking(connfd);
    conn* curr=new conn();
    curr->fd=connfd;
    curr->want_to_read=true;

    return curr;
}

void handle_write(conn* curr) 
{
    ssize_t ret = write(curr->fd, &curr->outgoingBuff[0], curr->outgoingBuff.size());

    if(ret<0)
    {
        curr->want_to_close=true;
        errorHandler("write()");
    }

    consume_buffer(curr->outgoingBuff, (size_t)ret);

    if(curr->outgoingBuff.size()==0)
    {
        curr->want_to_read=true;
        curr->want_to_write=false;
    }
    return;
}

void handle_read(conn* curr) 
{
    uint8_t buff[1<<16];
    ssize_t ret=read(curr->fd, buff, sizeof(buff));

    if(ret<0)
    {
        curr->want_to_close=true;
        errorHandler("read()");
    }

    if(ret==0)
    {
        // handle EOF, which occurs when client disconnects abruptly
        curr->want_to_close=true;
        return;
    }

    append_buffer(curr->incomingBuff, buff, ret);

    

    //parsing requests with pipelining
    while(try_once(curr)) {};

    if(curr->outgoingBuff.size())
    {
        // response ready
        curr->want_to_write=true;
        curr->want_to_read=false;
        return handle_write(curr);
    }

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

    setFdNonBlocking(fd); //non blocking listening socket

    ret=listen(fd, SOMAXCONN);
    if(ret) errorHandler("listen()");


    vector<conn*> fd2conn_mp;
    vector<struct pollfd> poll_args;



    //event loop
    while(true)
    {
        poll_args.clear();
        struct pollfd pfd={fd, POLLIN, 0};
        poll_args.push_back(pfd);
        for(conn *curr : fd2conn_mp)
        {
            if(!curr) continue;
            struct pollfd pfd={curr->fd, POLLERR, 0};
            if(curr->want_to_read)
            {
                pfd.events |= POLLIN;
            }
            if(curr->want_to_write)
            {
                pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        int ret=poll(poll_args.data(), poll_args.size(), -1); //indefinite waiting mode
        if(ret<0)
        {
            errorHandler("poll()");
        }

        // listening socket setup
        if(poll_args[0].revents)
        {
            if(conn* curr=handle_accept(fd))
            {
                if(fd2conn_mp.size() <= curr->fd)
                {
                    fd2conn_mp.resize(fd2conn_mp.size() + 100);
                }
                fd2conn_mp[curr->fd]=curr;
            }
        }

        // connection sockets handling

        for(int j=1;j<poll_args.size();j++)
        {
            uint32_t is_ready=poll_args[j].revents;
            if(!is_ready) continue;

            conn* curr=fd2conn_mp[poll_args[j].fd];

            if(is_ready & POLLIN)
            {
                handle_read(curr);
            }
            if(is_ready & POLLOUT)
            {
                handle_write(curr);
            }

            if(is_ready & POLLERR || curr->want_to_close)
            {
                close(curr->fd);
                fd2conn_mp[curr->fd]=NULL;
                delete curr;
            }   
        }
    }


    return 0;

}
