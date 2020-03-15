#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
// 包入网络相关的头部
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
// 包入自定义头部
#include "wfhttpproxy.h"

#define MAX_EVENT             1024
#define MAX_CONNECT           51200
#define MAXDATASIZE           2*1024*1024

enum STATUS {
    FDCLOSE,                  // fd已经被close
    FDOK,                     // 可读可写
    FDSOCKSUNREGISTER,        // socks刚连接上来，未注册完成
    FDSOCKSREGISTER,          // socks已注册，等待需要连接的对端连接成功
    FDCLIENTUNREADY,          // socks客户端等待连接成功
    FDSOCKSUNREADY,           // socks等待对方ready
    FDHTTPUNREADY,            // http等待对方ready
    FDHTTPUNREGISTER,         // http刚连接上来，未注册完成
    FDHTTPCLIENTUNREADY,      // http一般模式客户端未连接成功
    FDCONNECTIONUNREADY,      // http直连模式客户端未连接成功
    FDHTTPCLIENTREADY         // http一般模式客户端连接成功
};

enum HOSTTYPE {
    IPv4,                     // ipv4地址
    IPv6,                     // ipv6地址
    DOMAIN                    // 域名地址
};

struct FDCLIENT {
    int fd;
    enum STATUS status;
    struct FDCLIENT* targetclient; // 正常使用时指向对端的client对象，remainclientlist中时指向下一个可用的clientlist,
    int canwrite;
    unsigned char *data;
    int usesize;
    int fullsize;
};
struct FDCLIENT *remainfdclienthead = NULL;
struct FDCLIENT *socksclient;
struct FDCLIENT *httpclient;
int epollfd;
int httpport = 1080;
int socksport = 1081;

int setnonblocking (int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        printf("get flags fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        printf("set flags fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -2;
    }
    return 0;
}

int addtoepoll (struct FDCLIENT *fdclient, int flags) {
    struct epoll_event ev;
    ev.data.ptr = fdclient;
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP | flags; // 水平触发，保证所有数据都能读到
    return epoll_ctl(epollfd, EPOLL_CTL_ADD, fdclient->fd, &ev);
}

int modepoll (struct FDCLIENT *fdclient, int flags) {
    struct epoll_event ev;
    ev.data.ptr = fdclient;
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP | flags; // 水平触发，保证所有数据都能读到
    return epoll_ctl(epollfd, EPOLL_CTL_MOD, fdclient->fd, &ev);
}

int addclientdata (struct FDCLIENT *fdclient, unsigned int size) {
    int totalsize = fdclient->usesize + size;
    if (fdclient->fullsize < totalsize) {
        if (fdclient->data) {
            free(fdclient->data);
        }
        fdclient->data = (unsigned char*)malloc(totalsize);
        if (fdclient->data == NULL) {
            printf("malloc new fdclient data fail, fd:%d, in %s, at %d\n", fdclient->fd,  __FILE__, __LINE__);
            return -1;
        }
        fdclient->fullsize = totalsize;
    }
    return 0;
}

int create_http_socketfd () {
    if (httpport == 0) {
        printf("http proxy is disabled, in %s, at %d\n", __FILE__, __LINE__);
        return 0;
    }
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("run socket function is fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -1;
    }
    memset(&sin, 0, sizeof(struct sockaddr_in));
    sin.sin_family = AF_INET; // ipv4
    sin.sin_addr.s_addr = INADDR_ANY; // 本机任意ip
    sin.sin_port = htons(httpport);
    if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        printf("bind port %d fail, fd:%d, in %s, at %d\n", httpport, fd, __FILE__, __LINE__);
        close(fd);
        return -2;
    }
    if (listen(fd, MAX_CONNECT) < 0) {
        printf("listen port %d fail, fd:%d, in %s, at %d\n", httpport, fd, __FILE__, __LINE__);
        close(fd);
        return -3;
    }
    if (remainfdclienthead) {
        httpclient = remainfdclienthead;
        remainfdclienthead = remainfdclienthead->targetclient;
    } else {
        httpclient = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
        if (httpclient == NULL) {
            printf("malloc fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
            close(fd);
            return -4;
        }
        httpclient->data = NULL;
        httpclient->fullsize = 0;
    }
    httpclient->fd = fd;
    httpclient->status = FDOK;
    httpclient->targetclient = NULL;
    httpclient->usesize = 0;
    httpclient->canwrite = 1;
    if (addtoepoll(httpclient, 0)) {
        printf("socks server fd add to epoll fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        httpclient->targetclient = remainfdclienthead;
        remainfdclienthead = httpclient;
        return -5;
    }
    return 0;
}

int create_socks_socketfd () {
    if (socksport == 0) {
        printf("socket proxy is disabled, in %s, at %d\n", __FILE__, __LINE__);
        return 0;
    }
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("run socket function is fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -1;
    }
    memset(&sin, 0, sizeof(struct sockaddr_in));
    sin.sin_family = AF_INET; // ipv4
    sin.sin_addr.s_addr = INADDR_ANY; // 本机任意ip
    sin.sin_port = htons(socksport);
    if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        printf("bind port %d fail, fd:%d, in %s, at %d\n", socksport, fd, __FILE__, __LINE__);
        close(fd);
        return -2;
    }
    if (listen(fd, MAX_CONNECT) < 0) {
        printf("listen port %d fail, fd:%d, in %s, at %d\n", socksport, fd, __FILE__, __LINE__);
        close(fd);
        return -3;
    }
    if (remainfdclienthead) {
        socksclient = remainfdclienthead;
        remainfdclienthead = remainfdclienthead->targetclient;
    } else {
        socksclient = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
        if (socksclient == NULL) {
            printf("malloc fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
            close(fd);
            return -4;
        }
        socksclient->data = NULL;
        socksclient->fullsize = 0;
    }
    socksclient->fd = fd;
    socksclient->status = FDOK;
    socksclient->targetclient = NULL;
    socksclient->usesize = 0;
    socksclient->canwrite = 1;
    if (addtoepoll(socksclient, 0)) {
        printf("socks server fd add to epoll fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        socksclient->targetclient = remainfdclienthead;
        remainfdclienthead = socksclient;
        return -5;
    }
    return 0;
}

int removeclient (struct FDCLIENT *fdclient) {
    if (fdclient->status == FDCLOSE) {
        return -1;
    }
    struct epoll_event ev;
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fdclient->fd, &ev)) {
        printf("remove fail, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno, __FILE__, __LINE__);
        perror("err");
    }
    close(fdclient->fd);
    fdclient->status = FDCLOSE;
    struct FDCLIENT* targetclient = fdclient->targetclient;
    fdclient->targetclient = remainfdclienthead;
    remainfdclienthead = fdclient;
    if (targetclient) {
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, targetclient->fd, &ev)) {
            printf("remove fail, fd:%d, errno:%d, in %s, at %d\n", targetclient->fd, errno, __FILE__, __LINE__);
            perror("err");
        }
        close(targetclient->fd);
        targetclient->status = FDCLOSE;
        targetclient->targetclient = remainfdclienthead;
        remainfdclienthead = targetclient;
    }
}

int change_socket_opt (int fd) { // 修改发送缓冲区大小
    if (setnonblocking(fd) < 0) { // 设置为非阻塞IO
        printf("set nonblocking fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -1;
    }
    unsigned int socksval = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (unsigned char*)&socksval, sizeof(socksval))) { // 关闭Nagle协议
        printf("close Nagle protocol fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -2;
    }
#ifdef KEEPALIVE
    socksval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (unsigned char*)&socksval, sizeof(socksval))) { // 启动tcp心跳包
        printf("set socket keepalive fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -3;
    }
    socksval = KEEPIDLE;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (unsigned char*)&socksval, sizeof(socksval))) { // 设置tcp心跳包参数
        printf("set socket keepidle fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -4;
    }
    socksval = KEEPINTVL;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (unsigned char*)&socksval, sizeof(socksval))) { // 设置tcp心跳包参数
        printf("set socket keepintvl fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -5;
    }
    socksval = KEEPCNT;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, (unsigned char*)&socksval, sizeof(socksval))) { // 设置tcp心跳包参数
        printf("set socket keepcnt fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -6;
    }
#endif
    socklen_t socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get send buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        return -7;
    }
    // printf("old send buffer is %d, socksval_len:%d, fd:%d, in %s, at %d\n", socksval, socksval_len, fd,  __FILE__, __LINE__);
    socksval = MAXDATASIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (unsigned char*)&socksval, sizeof (socksval))) {
        printf("set send buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        return -8;
    }
    socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get send buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        return -9;
    }
    // printf("new send buffer is %d, socksval_len:%d, fd:%d, in %s, at %d\n", socksval, socksval_len, fd,  __FILE__, __LINE__);
    // 修改接收缓冲区大小
    socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get receive buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        return -10;
    }
    // printf("old receive buffer is %d, socksval_len:%d, fd:%d, in %s, at %d\n", socksval, socksval_len, fd,  __FILE__, __LINE__);
    socksval = MAXDATASIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&socksval, sizeof(socksval))) {
        printf("set receive buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        return -11;
    }
    socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get receive buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        return -12;
    }
    // printf("new receive buffer is %d, socksval_len:%d, fd:%d, in %s, at %d\n", socksval, socksval_len, fd,  __FILE__, __LINE__);
    return 0;
}

int addclient (struct FDCLIENT *listenclient, enum STATUS status) {
    int listenfd = listenclient->fd;
    struct sockaddr_in sin;
    socklen_t in_addr_len = sizeof(struct sockaddr_in);
    int fd = accept(listenfd, (struct sockaddr*)&sin, &in_addr_len);
    if (fd < 0) {
        printf("accept a new fd fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        return -1;
    }
    if (change_socket_opt (fd)) {
        printf("change socket buffer fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -2;
    }
    struct FDCLIENT *fdclient;
    if (remainfdclienthead) { // 有存货，直接拿出来用
        fdclient = remainfdclienthead;
        remainfdclienthead = remainfdclienthead->targetclient;
    } else { // 没有存货，malloc一个
        fdclient = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
        if (fdclient == NULL) {
            printf("malloc new fdclient object fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
            close(fd);
            return -3;
        }
        fdclient->data = NULL;
        fdclient->fullsize = 0;
    }
    fdclient->fd = fd;
    fdclient->status = status;
    fdclient->targetclient = NULL;
    fdclient->usesize = 0;
    fdclient->canwrite = 1;
    if (addtoepoll(fdclient, 0)) {
        printf("add to epoll fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        removeclient(fdclient);
        return -4;
    }
    return 0;
}

int connect_client (enum HOSTTYPE type, unsigned char *host, unsigned short port, int host_len) {
    int targetfd;
    if (type == DOMAIN) { // 域名
        unsigned char domain[256];
        memcpy(domain, host, host_len);
        domain[host_len] = '\0';
        struct hostent *ip = gethostbyname(domain); // 域名dns解析
        if(ip == NULL) {
            printf("get ip by domain error, domain:%s, in %s, at %d\n", domain,  __FILE__, __LINE__);
            return -1;
        }
        printf("target domain:%s, port:%d, in %s, at %d\n", domain, port, __FILE__, __LINE__);
        unsigned char *addr = ip->h_addr_list[0];
        if (ip->h_addrtype == AF_INET) { // ipv4
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(struct sockaddr_in));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(port);
            memcpy(&sin.sin_addr.s_addr, addr, 4);
            if ((targetfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                printf("create target fd fail, in %s, at %d\n", __FILE__, __LINE__);
                perror("err");
                close(targetfd);
                return -2;
            }
            if (change_socket_opt (targetfd)) {
                printf("change socket buffer fail, fd:%d, in %s, at %d\n", targetfd, __FILE__, __LINE__);
                close(targetfd);
                return -3;
            }
            connect(targetfd, (struct sockaddr*)&sin, sizeof(sin));
        } else if (ip->h_addrtype == AF_INET6) { // ipv6
            struct sockaddr_in6 sin6;
            memset(&sin6, 0, sizeof(struct sockaddr_in6));
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = htons(port);
            memcpy(sin6.sin6_addr.s6_addr, addr, 16);
            if ((targetfd = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
                printf("create target fd fail, in %s, at %d\n", __FILE__, __LINE__);
                perror("err");
                close(targetfd);
                return -4;
            }
            if (change_socket_opt (targetfd)) {
                printf("change socket buffer fail, fd:%d, in %s, at %d\n", targetfd, __FILE__, __LINE__);
                close(targetfd);
                return -5;
            }
            connect(targetfd, (struct sockaddr*)&sin6, sizeof(sin6));
        }
    } else if (type == IPv4) { // ipv4
        printf("target ipv4:%d.%d.%d.%d, in %s, at %d\n", host[0], host[1], host[2], host[3], __FILE__, __LINE__);
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(struct sockaddr_in));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        memcpy(&sin.sin_addr.s_addr, host, 4);
        if ((targetfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            printf("create target fd fail, in %s, at %d\n", __FILE__, __LINE__);
            perror("err");
            close(targetfd);
            return -6;
        }
        if (change_socket_opt (targetfd)) {
            printf("change socket buffer fail, fd:%d, in %s, at %d\n", targetfd, __FILE__, __LINE__);
            close(targetfd);
            return -7;
        }
    } else if (type == IPv6) { // ipv6
        printf("target ipv6:%x%x:%x%x:%x%x:%x%x:%x%x:%x%x:%x%x:%x%x, in %s, at %d\n",
            host[0], host[1], host[2], host[3], host[4], host[5], host[6], host[7],
            host[8], host[9], host[10], host[11], host[12], host[13], host[14], host[15],
            __FILE__, __LINE__);
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(struct sockaddr_in6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);
        memcpy(sin6.sin6_addr.s6_addr, host, 16);
        if ((targetfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            printf("create target fd fail, in %s, at %d\n", __FILE__, __LINE__);
            perror("err");
            close(targetfd);
            return -8;
        }
        if (change_socket_opt (targetfd)) {
            printf("change socket buffer fail, fd:%d, in %s, at %d\n", targetfd, __FILE__, __LINE__);
            close(targetfd);
            return -9;
        }
        connect(targetfd, (struct sockaddr*)&sin6, sizeof(struct sockaddr_in6));
    }
    return targetfd;
}

int writenode (struct FDCLIENT *fdclient) {
    ssize_t len = write(fdclient->fd, fdclient->data, fdclient->usesize);
    if (len < fdclient->usesize) {
        if (len < 0) {
            if (errno != 11) {
                printf("write error, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                perror("err");
                removeclient(fdclient);
            }
            return -1;
        }
        int remainsize = fdclient->usesize - len;
        for (int i = 0 ; i < remainsize ; i++) {
            fdclient->data[i] = fdclient->data[i+len];
        }
        fdclient->usesize = remainsize;
        if (fdclient->canwrite) {
            fdclient->canwrite = 0;
            if (modepoll(fdclient, EPOLLOUT)) {
                printf("change epoll status fail, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                perror("err");
                removeclient(fdclient);
            }
        }
    } else {
        fdclient->usesize = 0;
        if (!fdclient->canwrite) {
            fdclient->canwrite = 1;
            if (modepoll(fdclient, 0)) {
                printf("change epoll status fail, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                perror("err");
                removeclient(fdclient);
            }
        }
    }
    return 0;
}

int readdata (struct FDCLIENT *fdclient) {
    static unsigned char readbuf[MAXDATASIZE];
    ssize_t len = read(fdclient->fd, readbuf, sizeof(readbuf));
    if (len < 0) {
        if (errno != 11) {
            printf("read error, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
            perror("err");
            removeclient(fdclient);
        }
        return -1;
    }
    if (fdclient->status == FDOK) {
        struct FDCLIENT *targetclient = fdclient->targetclient;
        if (addclientdata(targetclient, len)) {
            removeclient(targetclient);
            return -2;
        }
        memcpy(targetclient->data + targetclient->usesize, readbuf, len);
        targetclient->usesize += len;
        if (targetclient->canwrite) {
            writenode(targetclient);
        }
    } else if (fdclient->status == FDHTTPUNREGISTER) {
        static unsigned char newheader[MAXDATASIZE];
        static unsigned char host[256];
        unsigned short port = 0;
        unsigned int host_len = 0;
        int isconnect = 0;
        unsigned int oldheader_len = 0;
        unsigned int newheader_len = parsehttpproxyheader(readbuf, newheader, host, &host_len, &port, &isconnect, &oldheader_len);
        if (newheader_len == 0 || oldheader_len == 0 || host_len == 0 || port == 0) {
            readbuf[len-1] = '\0';
            printf("parse http proxy header error, oldheader:%s, in %s, at %d\n", readbuf,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -3;
        }
        int targetfd = connect_client(DOMAIN, host, port, host_len);
        if (targetfd < 0) {
            printf("connect client error, in %s, at %d\n",  __FILE__, __LINE__);
            removeclient(fdclient);
            return -4;
        }
        struct FDCLIENT *targetclient;
        if (remainfdclienthead) { // 有存货，直接拿出来用
            targetclient = remainfdclienthead;
            remainfdclienthead = remainfdclienthead->targetclient;
        } else { // 没有存货，malloc一个
            targetclient = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
            if (targetclient == NULL) {
                printf("malloc new fdclient object fail, fd:%d, in %s, at %d\n", targetfd,  __FILE__, __LINE__);
                close(targetfd);
                removeclient(fdclient);
                return -5;
            }
            targetclient->data = NULL;
            targetclient->fullsize = 0;
        }
        targetclient->fd = targetfd;
        targetclient->targetclient = fdclient;
        targetclient->usesize = 0;
        fdclient->targetclient = targetclient;
        if (isconnect == 1) {
            targetclient->status = FDCONNECTIONUNREADY;
        } else {
            targetclient->status = FDHTTPCLIENTUNREADY;
            unsigned int usesize = newheader_len + len - oldheader_len;
            if (addclientdata(targetclient, usesize)) {
                return -6;
            }
            memcpy(targetclient->data + targetclient->usesize, newheader, newheader_len);
            memcpy(targetclient->data + targetclient->usesize + newheader_len, readbuf + oldheader_len, len - oldheader_len);
            targetclient->usesize += usesize;
        }
        targetclient->canwrite = 0;
        if (addtoepoll(targetclient, EPOLLOUT)) {
            printf("add to epoll fail, fd:%d, in %s, at %d\n", targetfd,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -7;
        }
        fdclient->status = FDHTTPUNREADY; // 成功获取到目的地址，等待连接目的端成功
    } else if (fdclient->status == FDHTTPCLIENTREADY) {
        static unsigned char newheader[MAXDATASIZE];
        static unsigned char host[256];
        unsigned short port = 0;
        unsigned int host_len = 0;
        int isconnect = 0;
        unsigned int oldheader_len = 0;
        unsigned int newheader_len = parsehttpproxyheader(readbuf, newheader, host, &host_len, &port, &isconnect, &oldheader_len);
        if (newheader_len == 0 || oldheader_len == 0 || host_len == 0 || port == 0) {
            readbuf[len-1] = '\0';
            printf("parse http proxy header error, oldheader:%s, in %s, at %d\n", readbuf,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -8;
        }
        struct FDCLIENT *targetclient = fdclient->targetclient;
        unsigned int usesize = newheader_len + len - oldheader_len;
        if (addclientdata(targetclient, usesize)) {
            return -9;
        }
        memcpy(targetclient->data + targetclient->usesize, newheader, newheader_len);
        memcpy(targetclient->data + targetclient->usesize + newheader_len, readbuf + oldheader_len, len - oldheader_len);
        targetclient->usesize += usesize;
        if (targetclient->canwrite) {
            writenode(targetclient);
        }
    } else if (fdclient->status == FDSOCKSUNREGISTER) {
        if (readbuf[0] == 0x05) { // 这里只处理socks5，不处理其他socks版本
            unsigned char data[] = {0x05, 0x00};
            if (addclientdata(fdclient, sizeof(data))) {
                return -10;
            }
            memcpy(fdclient->data + fdclient->usesize, data, sizeof(data));
            fdclient->usesize += sizeof(data);
            if (fdclient->canwrite) {
                writenode(fdclient);
            }
            fdclient->status = FDSOCKSREGISTER; // 进入接收目的地址与端口的状态
        }
    } else if (fdclient->status == FDSOCKSREGISTER) {
        static char host[256];
        int targetfd;
        switch (readbuf[3]) {
            case 0x01: targetfd = connect_client(IPv4, readbuf + 4, (((unsigned short)readbuf[len-2]<<8) + (unsigned short)readbuf[len-1]), 4);break;
            case 0x03: targetfd = connect_client(DOMAIN, readbuf + 5, (((unsigned short)readbuf[len-2]<<8) + (unsigned short)readbuf[len-1]), readbuf[4]);break;
            case 0x04: targetfd = connect_client(IPv6, readbuf + 4, (((unsigned short)readbuf[len-2]<<8) + (unsigned short)readbuf[len-1]), 16);break;
            default:
                printf("socks5 domain type is unknown, in %s, at %d\n", readbuf[3],  __FILE__, __LINE__);
                removeclient(fdclient);
                return -11;
        }
        if (targetfd < 0) {
            printf("connect client error, in %s, at %d\n",  __FILE__, __LINE__);
            removeclient(fdclient);
            return -12;
        }
        struct FDCLIENT *targetclient;
        if (remainfdclienthead) { // 有存货，直接拿出来用
            targetclient = remainfdclienthead;
            remainfdclienthead = remainfdclienthead->targetclient;
        } else { // 没有存货，malloc一个
            targetclient = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
            if (targetclient == NULL) {
                printf("malloc new fdclient object fail, fd:%d, in %s, at %d\n", targetfd,  __FILE__, __LINE__);
                close(targetfd);
                removeclient(fdclient);
                return -13;
            }
            targetclient->data = NULL;
            targetclient->fullsize = 0;
        }
        targetclient->fd = targetfd;
        targetclient->status = FDCLIENTUNREADY;
        targetclient->targetclient = fdclient;
        targetclient->usesize = 0;
        fdclient->targetclient = targetclient;
        targetclient->canwrite = 0;
        if (addtoepoll(targetclient, EPOLLOUT)) {
            printf("add to epoll fail, fd:%d, in %s, at %d\n", targetfd,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -14;
        }
        fdclient->status = FDSOCKSUNREADY; // 成功获取到目的地址，等待连接目的端成功
    }
    return 0;
}

int main (int argc, char *argv[]) {
    epollfd = epoll_create(MAX_EVENT);
    if (epollfd < 0) {
        printf("create epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
        perror("err");
        return -1;
    }
    printf("create epoll fd success, in %s, at %d\n",  __FILE__, __LINE__);
    if (create_http_socketfd ()) {
        printf("create http fd fail, in %s, at %d\n",  __FILE__, __LINE__);
        return -2;
    }
    printf("listen http port success, in %s, at %d\n",  __FILE__, __LINE__);
    if (create_socks_socketfd ()) {
        printf("create socks fd fail, in %s, at %d\n",  __FILE__, __LINE__);
        return -3;
    }
    printf("listen socks5 port success, in %s, at %d\n",  __FILE__, __LINE__);
    while (1) {
        static struct epoll_event evs[MAX_EVENT];
        static int wait_count;
        wait_count = epoll_wait(epollfd, evs, MAX_EVENT, -1);
        for (int i = 0 ; i < wait_count ; i++) {
            struct FDCLIENT *fdclient = evs[i].data.ptr;
            uint32_t events = evs[i].events;
            if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) { // 检测到数据异常
                removeclient(fdclient);
                continue;
            } else if (fdclient == httpclient) {
                addclient(fdclient, FDHTTPUNREGISTER);
            } else if (fdclient == socksclient) {
                addclient(fdclient, FDSOCKSUNREGISTER);
            } else if (events & EPOLLIN) { // 数据可读
                readdata(fdclient);
            } else if (events & EPOLLOUT) { // 数据可写，或远端连接成功了。
                if (fdclient->status == FDCLIENTUNREADY) {
                    fdclient->status = FDOK;
                    struct FDCLIENT* targetclient = fdclient->targetclient;
                    fdclient->canwrite = 1;
                    if (modepoll(fdclient, 0)) {
                        printf("change epoll status fail, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                        perror("err");
                        removeclient(fdclient);
                        continue;
                    }
                    targetclient->status = FDOK;
                    unsigned char data[] = {0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                    if (addclientdata(targetclient, sizeof(data))) {
                        continue;
                    }
                    memcpy(targetclient->data + targetclient->usesize, data, sizeof(data));
                    targetclient->usesize += sizeof(data);
                    if (targetclient->canwrite) {
                        writenode(targetclient);
                    }
                } else if (fdclient->status == FDHTTPCLIENTUNREADY) {
                    fdclient->status = FDOK;
                    struct FDCLIENT* targetclient = fdclient->targetclient;
                    targetclient->status = FDHTTPCLIENTREADY;
                    writenode(fdclient);
                } else if (fdclient->status == FDCONNECTIONUNREADY) {
                    fdclient->status = FDOK;
                    struct FDCLIENT* targetclient = fdclient->targetclient;
                    fdclient->canwrite = 1;
                    if (modepoll(fdclient, 0)) {
                        printf("change epoll status fail, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                        perror("err");
                        removeclient(fdclient);
                        continue;
                    }
                    targetclient->status = FDOK;
                    unsigned char data[] = "HTTP/1.1 200 Connection established\r\n\r\n";
                    if (addclientdata(targetclient, sizeof(data)-1)) {
                        continue;
                    }
                    memcpy(targetclient->data + targetclient->usesize, data, sizeof(data)-1);
                    targetclient->usesize += sizeof(data)-1;
                    if (targetclient->canwrite) {
                        writenode(targetclient);
                    }
                }
            } else {
                printf("receive new event 0x%08x, in %s, at %d\n", events,  __FILE__, __LINE__);
                removeclient(fdclient);
            }
        }
    }
}
