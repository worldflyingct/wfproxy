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

#define MAX_EVENT             1024
#define MAX_CONNECT           51200
#define MAXDATASIZE           2*1024*1024

enum STATUS {
    FDCLOSE,                  // fd已经被close
    FDOK,                     // 可读可写
    FDSOCKSUNREGISTER,        // socks刚连接上来，未注册完成
    FDSOCKSREGISTER,          // socks已注册，等待需要连接的对端连接成功
    FDCLIENTUNREADY,          // 客户端等待连接成功
    FDSOCKSUNREADY            // socks等待对方ready
};

enum HOSTTYPE {
    IPv4,                     // ipv4地址
    IPv6,                     // ipv6地址
    DOMAIN                    // 域名地址
};

struct FDCLIENT {
    int fd;
    int targetfd;
    enum STATUS status;
    struct FDCLIENT* targetclient; // 正常使用时指向对端的client对象，remainclientlist中时指向下一个可用的clientlist
};
struct FDCLIENT *remainfdclienthead = NULL;
struct FDCLIENT *socksclient;
int epollfd;
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
    }
    socksclient->fd = fd;
    socksclient->targetfd = 0;
    socksclient->status = FDOK;
    socksclient->targetclient = NULL;
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
    if (fdclient->targetfd) {
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fdclient->targetfd, &ev)) {
            printf("remove fail, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno, __FILE__, __LINE__);
            perror("err");
        }
        close(fdclient->targetfd);
        fdclient->targetclient->status = FDCLOSE;
    }
    struct FDCLIENT* targetclient = fdclient->targetclient;
    if (targetclient) {
        targetclient->targetclient = remainfdclienthead;
        remainfdclienthead = targetclient;
    }
    fdclient->targetclient = remainfdclienthead;
    remainfdclienthead = fdclient;
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

int addsocksclient () {
    int socksfd = socksclient->fd;
    struct sockaddr_in sin;
    socklen_t in_addr_len = sizeof(struct sockaddr_in);
    int fd = accept(socksfd, (struct sockaddr*)&sin, &in_addr_len);
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
    }
    fdclient->fd = fd;
    fdclient->targetfd = 0;
    fdclient->status = FDSOCKSUNREGISTER;
    fdclient->targetclient = NULL;
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

int readdata (struct FDCLIENT *fdclient) {
    static unsigned char readbuf[MAXDATASIZE];
    if (fdclient->status == FDOK) {
        ssize_t len = read(fdclient->fd, readbuf, sizeof(readbuf));
        if (len < 0) {
            if (errno != 11) {
                printf("read error, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                perror("err");
                removeclient(fdclient);
            }
            return -1;
        }
        len = write(fdclient->targetfd, readbuf, len);
        if (len < 0) {
            if (errno != 11) {
                printf("write error, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                perror("err");
                removeclient(fdclient);
            }
            return -2;
        }
    } else if (fdclient->status == FDSOCKSUNREGISTER) {
        ssize_t len = read(fdclient->fd, readbuf, sizeof(readbuf));
        if (len < 0) {
            if (errno != 11) {
                printf("read error, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                perror("err");
                removeclient(fdclient);
            }
            return -3;
        }
        if (readbuf[0] == 0x05) { // 这里只处理socks5，不处理其他socks版本
            unsigned char data[] = {0x05, 0x00};
            len = write (fdclient->fd, data, sizeof(data)); // 直接返回成功，校验其他内容。
            if (len < 0) {
                if (errno != 11) {
                    printf("read error, errno:%d, in %s, at %d\n", errno,  __FILE__, __LINE__);
                    perror("err");
                    removeclient(fdclient);
                }
                return -4;
            }
            fdclient->status = FDSOCKSREGISTER; // 进入接收目的地址与端口的状态
        }
    } else if (fdclient->status == FDSOCKSREGISTER) {
        char host[256];
        ssize_t len = read(fdclient->fd, readbuf, sizeof(readbuf));
        if (len < 0) {
            if (errno != 11) {
                printf("read error, fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                perror("err");
                removeclient(fdclient);
            }
            return -5;
        }
        int targetfd;
        switch (readbuf[3]) {
            case 0x01: targetfd = connect_client(IPv4, readbuf + 4, (((unsigned short)readbuf[len-2]<<8) + (unsigned short)readbuf[len-1]), 4);break;
            case 0x03: targetfd = connect_client(DOMAIN, readbuf + 5, (((unsigned short)readbuf[len-2]<<8) + (unsigned short)readbuf[len-1]), readbuf[4]);break;
            case 0x04: targetfd = connect_client(IPv6, readbuf + 4, (((unsigned short)readbuf[len-2]<<8) + (unsigned short)readbuf[len-1]), 16);break;
        }
        if (targetfd < 0) {
            printf("connect client error, in %s, at %d\n",  __FILE__, __LINE__);
            removeclient(fdclient);
            return -6;
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
                return -7;
            }
        }
        targetclient->fd = targetfd;
        targetclient->targetfd = fdclient->fd;
        targetclient->status = FDCLIENTUNREADY;
        targetclient->targetclient = fdclient;
        fdclient->targetfd = targetfd;
        fdclient->targetclient = targetclient;
        if (addtoepoll(targetclient, EPOLLOUT)) {
            printf("add to epoll fail, fd:%d, in %s, at %d\n", targetfd,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -8;
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
    if (create_socks_socketfd ()) {
        printf("create socks fd fail, in %s, at %d\n",  __FILE__, __LINE__);
        return -2;
    }
    printf("init finish, in %s, at %d\n",  __FILE__, __LINE__);
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
            } else if (fdclient == socksclient) {
                addsocksclient();
            } else if (events & EPOLLIN) { // 数据可读
                readdata(fdclient);
            } else if (events & EPOLLOUT) { // 数据可写，或远端连接成功了。
                if (fdclient->status == FDCLIENTUNREADY) {
                    unsigned char data[] = {0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                    if (write(fdclient->targetfd, data, sizeof(data)) < 0) {
                        if (errno != 11) {
                            printf("read error, errno:%d, in %s, at %d\n", errno,  __FILE__, __LINE__);
                            perror("err");
                            removeclient(fdclient);
                        }
                        continue;
                    }
                    if (modepoll(fdclient, 0)) { // 取消监听可写事件
                        printf("modepoll fail, in %s, at %d\n",  __FILE__, __LINE__);
                        removeclient(fdclient);
                        continue;
                    }
                    fdclient->status = FDOK;
                    fdclient->targetclient->status = FDOK;
                }
            } else {
                printf("receive new event 0x%08x, in %s, at %d\n", events,  __FILE__, __LINE__);
                removeclient(fdclient);
            }
        }
    }
}
