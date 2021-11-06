#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <openssl/ssl.h>
#include "cJSON.h"

#define MAX_EVENT      1024
#define MAX_CONNECT    1024

#define MAXDATASIZE    2*1024*1024
#define KEEPALIVE            // 如果定义了，就是启动心跳包，不定义就不启动，下面3个参数就没有意义。
#define KEEPIDLE       60    // tcp完全没有数据传输的最长间隔为60s，操过60s就要发送询问数据包
#define KEEPINTVL      3     // 如果询问失败，间隔多久再次发出询问数据包
#define KEEPCNT        1     // 连续多少次失败断开连接

typedef enum {
    DISCONNECT,
    UNWATCH, // 已经连接，还没被epoll见提供
    TCPNOREADY, // tcp初始化尚未完成
    WAITAUTH, // 等待auth认证返回
    WAITCONNECT, // 等待connect代理返回
    REGISTER, // 注册完毕，开始双向对传
    TLSNOREADY, // tls认证失败，需要重新认证
} STATUS;

struct FDCLIENT {
    int fd;
    SSL *tls;
    STATUS status;
    int canwrite;
    char* data;
    unsigned int datasize;
    unsigned int fullsize;
    struct FDCLIENT* outclient;
};

#define defconf "{\n" \
                "  \"ssl\": false,\n" \
                "  \"bindport\": 1080,\n" \
                "  \"serveraddr\": \"proxyserver:443\",\n" \
                "  \"needauth\": false,\n" \
                "  \"path\": \"/\",\n" \
                "  \"key\": \"65f5bb36-8a0a-4be4-b0d0-18dee527b2d8\",\n" \
                "  \"connectmode\": false,\n" \
                "  \"targetaddr\": \"targetserver:443\"\n" \
                "}"
struct CONFIG {
    unsigned char ssl;
    unsigned short bindport;
    unsigned char serveraddrip[4];
    unsigned short serveraddrport;
    unsigned char needauth;
    unsigned char connectmode;
};
struct CONFIG c;
static char *auth;
static unsigned short authlen;
static char *connproxy;
static unsigned short connproxylen;

struct FDCLIENT *remainfdclienthead = NULL;
static SSL_CTX *ctx;
static int epollfd;

int initconfigdata();
int create_socketfd();
int writedata(struct FDCLIENT* fdclient);
int clientstatuschange(struct FDCLIENT* fdclient);
int readdata (struct FDCLIENT* fdclient);
int addclient(int acceptfd);
int removeclient(struct FDCLIENT* fdclient);
int modepoll (struct FDCLIENT *fdclient, uint32_t flags);

int main (int argc, char* argv[]) {
    printf("build at %s %s, in %s, at %d\n", __DATE__, __TIME__, __FILE__, __LINE__);
    if (initconfigdata()) {
        printf("init config data fail, in %s, at %d\n",  __FILE__, __LINE__);
        return -1;
    }
    if (c.ssl) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ctx = SSL_CTX_new(TLS_client_method());
        if(ctx == NULL) {
            printf("create SSL CTX fail, in %s, at %d\n",  __FILE__, __LINE__);
            return -2;
        }
    } else {
        ctx = NULL;
    }
    if ((epollfd = epoll_create(MAX_EVENT)) < 0) {
        printf("create epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
        return -3;
    }
    static int fd;
    fd = create_socketfd();
    if (fd < 0) {
        printf("create accept port fail, in %s, at %d\n",  __FILE__, __LINE__);
        return -4;
    }
    while (1) {
        static struct epoll_event evs[MAX_EVENT];
        int wait_count = epoll_wait(epollfd, evs, MAX_EVENT, -1);
        for (int i = 0 ; i < wait_count ; i++) {
            struct FDCLIENT* fdclient = (struct FDCLIENT*)evs[i].data.ptr;
            uint32_t events = evs[i].events;
            if (fdclient->status != DISCONNECT) {
                if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    removeclient(fdclient);
                } else if (events & EPOLLIN) {
                    if (fdclient->fd == fd) {
                        addclient(fd);
                    } else if (fdclient->status == TCPNOREADY || fdclient->status == TLSNOREADY) {
                        if (ctx) {
                            int r_code = SSL_connect(fdclient->tls);
                            int errcode = SSL_get_error(fdclient->tls, r_code);
                            if (r_code < 0) {
                                if (errcode == SSL_ERROR_WANT_WRITE) { // 资源暂时不可用，write没有ready.
                                    if (modepoll(fdclient, EPOLLOUT)) {
                                        printf("modify epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
                                        removeclient(fdclient);
                                    }
                                } else if (errcode != SSL_ERROR_WANT_READ) {
                                    perror("tls connect error");
                                    printf("errno:%d, errcode:%d, in %s, at %d\n", errno, errcode, __FILE__, __LINE__);
                                    removeclient(fdclient);
                                }
                            } else {
                                if (fdclient->status == TCPNOREADY) {
                                    clientstatuschange(fdclient);
                                } else {
                                    fdclient->status = REGISTER;
                                }
                                if (fdclient->datasize > 0) {
                                    if (modepoll(fdclient, EPOLLIN | EPOLLOUT)) {
                                        printf("modify epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
                                        removeclient(fdclient);
                                    }
                                }
                            }
                        } else {
                            clientstatuschange(fdclient);
                        }
                    } else {
                        readdata(fdclient);
                    }
                } else if (events & EPOLLOUT) {
                    if (fdclient->status == TCPNOREADY || fdclient->status == TLSNOREADY) {
                        if (ctx) {
                            int r_code = SSL_connect(fdclient->tls);
                            int errcode = SSL_get_error(fdclient->tls, r_code);
                            if (r_code < 0) {
                                if (errcode == SSL_ERROR_WANT_READ) { // 资源暂时不可用，read没有ready.
                                    if (modepoll(fdclient, EPOLLIN)) {
                                        printf("modify epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
                                        removeclient(fdclient);
                                    }
                                } else if (errcode != SSL_ERROR_WANT_WRITE) {
                                    perror("tls connect error");
                                    printf("errno:%d, errcode:%d, in %s, at %d\n", errno, errcode, __FILE__, __LINE__);
                                    removeclient(fdclient);
                                }
                            } else {
                                if (fdclient->status == TCPNOREADY) {
                                    clientstatuschange(fdclient);
                                } else {
                                    fdclient->status = REGISTER;
                                }
                                uint32_t flags = fdclient->datasize > 0 ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
                                if (modepoll(fdclient, flags)) {
                                    printf("modify epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
                                    removeclient(fdclient);
                                }
                            }
                        } else {
                            clientstatuschange(fdclient);
                        }
                    } else {
                        writedata(fdclient);
                    }
                } else {
                    printf("receive new event 0x%08x, in %s, at %d\n", events,  __FILE__, __LINE__);
                }
            }
        }
    }
    if (ctx != NULL) {
        SSL_CTX_free(ctx);
    }
    return 0;
}

int removeclient (struct FDCLIENT* fdclient) {
    if (fdclient->status == DISCONNECT) {
        return 0;
    }
    if (fdclient->status != UNWATCH) {
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fdclient->fd, NULL);
    }
    struct FDCLIENT *fdserver = fdclient->outclient;
    fdclient->outclient = remainfdclienthead;
    remainfdclienthead = fdclient;
    if (fdclient->tls) {
        SSL_shutdown(fdclient->tls);
        SSL_free(fdclient->tls);
    }
    close(fdclient->fd);
    fdclient->status = DISCONNECT;
    if (fdserver) {
        if (fdclient->status != UNWATCH) {
            epoll_ctl(epollfd, EPOLL_CTL_DEL, fdserver->fd, NULL);
        }
        fdserver->outclient = remainfdclienthead;
        remainfdclienthead = fdserver;
        if (fdserver->tls) {
            SSL_shutdown(fdserver->tls);
            SSL_free(fdserver->tls);
        }
        close(fdserver->fd);
        fdserver->status = DISCONNECT;
    }
    return 0;
}

int addtoepoll (struct FDCLIENT *fdclient, uint32_t flags) {
    struct epoll_event ev;
    ev.data.ptr = fdclient;
    ev.events = EPOLLERR | EPOLLHUP | EPOLLRDHUP | flags; // 水平触发，保证所有数据都能读到
    return epoll_ctl(epollfd, EPOLL_CTL_ADD, fdclient->fd, &ev);
}

int modepoll (struct FDCLIENT *fdclient, uint32_t flags) {
    struct epoll_event ev;
    ev.data.ptr = fdclient;
    ev.events = EPOLLERR | EPOLLHUP | EPOLLRDHUP | flags; // 水平触发，保证所有数据都能读到
    return epoll_ctl(epollfd, EPOLL_CTL_MOD, fdclient->fd, &ev);
}

int writedata (struct FDCLIENT* fdclient) {
    ssize_t len;
    if (fdclient->tls) {
        len = SSL_write(fdclient->tls, fdclient->data, fdclient->datasize);
        if (len < 0) {
            int errcode = SSL_get_error(fdclient->tls, len);
            if (errcode == SSL_ERROR_WANT_READ) {
                fdclient->status = TLSNOREADY;
                if (modepoll(fdclient, EPOLLIN)) {
                    perror("modify epoll error");
                    printf("fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                    removeclient(fdclient);
                    return -1;
                }
                return 0;
            }
        }
    } else {
        len = write(fdclient->fd, fdclient->data, fdclient->datasize);
    }
    if (len < 0) {
        if (errno != EAGAIN) {
            perror("write error");
            printf("fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -2;
        }
        return 0;
    }
    if (len < fdclient->datasize) {
        unsigned int datasize = fdclient->datasize - len;
        memcpy(fdclient->data, fdclient->data + len, datasize);
        fdclient->datasize = datasize;
    } else {
        if (modepoll(fdclient, EPOLLIN)) {
            perror("modify epoll error");
            printf("fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -3;
        }
        fdclient->datasize = 0;
        fdclient->canwrite = 1;
    }
    return 0;
}

int writenode (struct FDCLIENT* fdclient, const char* data, unsigned int size) {
    if (fdclient->canwrite) {
        ssize_t len;
        if (fdclient->tls) {
            len = SSL_write(fdclient->tls, data, size);
            if (len < 0) {
                int errcode = SSL_get_error(fdclient->tls, len);
                if (errcode == SSL_ERROR_WANT_READ) {
                    fdclient->status = TLSNOREADY;
                }
            }
        } else {
            len = write(fdclient->fd, data, size);
        }
        if (len < 0) {
            if (errno != EAGAIN) {
                perror("write error");
                printf("fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                removeclient(fdclient);
                return -2;
            }
            len = 0;
        }
        if (len < size) {
            unsigned int datasize = size - len;
            if (datasize > fdclient->fullsize) {
                if (fdclient->fullsize > 0) {
                    free(fdclient->data);
                }
                fdclient->data = (char*)malloc(datasize);
                if (fdclient->data == NULL) {
                    perror("malloc fail");
                    printf("size: %d, errno:%d, in %s, at %d\n", datasize, errno,  __FILE__, __LINE__);
                    removeclient(fdclient);
                    return -3;
                }
                fdclient->fullsize = datasize;
            }
            memcpy(fdclient->data, data + len, datasize);
            fdclient->datasize = datasize;
            if (fdclient->status != TLSNOREADY) {
                if (modepoll(fdclient, EPOLLIN | EPOLLOUT)) {
                    perror("modify epoll error");
                    printf("fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
                    removeclient(fdclient);
                    return -4;
                }
            }
            fdclient->canwrite = 0;
        }
    } else {
        unsigned int datasize = fdclient->datasize + size;
        if (datasize > fdclient->fullsize) {
            if (fdclient->fullsize > 0) {
                free(fdclient->data);
            }
            fdclient->data = (char*)malloc(datasize);
            if (fdclient->data == NULL) {
                perror("malloc fail");
                printf("size: %d, errno:%d, in %s, at %d\n", datasize, errno,  __FILE__, __LINE__);
                removeclient(fdclient);
                return -5;
            }
            fdclient->fullsize = datasize;
        }
        memcpy(fdclient->data + fdclient->datasize, data, size);
        fdclient->datasize = datasize;
    }
    return 0;
}

int readdata (struct FDCLIENT* fdclient) {
    static unsigned char readbuf[32*1024];
    ssize_t len;
    if (fdclient->tls) {
        len = SSL_read(fdclient->tls, readbuf, sizeof(readbuf)-1);
        if (len < 0) {
            int errcode = SSL_get_error(fdclient->tls, len);
            if (errcode == SSL_ERROR_WANT_WRITE) {
                fdclient->status = TLSNOREADY;
                if (modepoll(fdserver, EPOLLOUT)) {
                    printf("create epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
                    removeclient(fdserver);
                    return -1;
                }
                return 0;
            }
        }
    } else {
        len = read(fdclient->fd, readbuf, sizeof(readbuf)-1);
    }
    if (len < 0) {
        if (errno != EAGAIN) {
            perror("write error");
            printf("fd:%d, errno:%d, in %s, at %d\n", fdclient->fd, errno,  __FILE__, __LINE__);
            removeclient(fdclient);
            return -2;
        }
        return 0;
    }
    if (fdclient->status == WAITAUTH) {
        if (memcmp(readbuf, "HTTP/1.1 101 Switching Protocols\r\n", 34)) {
            printf("fd:%d, in %s, at %d\n", fdclient->fd,  __FILE__, __LINE__);
            readbuf[len] = '\0';
            printf("len:%d\n", len);
            printf("%s\n", readbuf);
            removeclient(fdclient);
            return -3;
        }
        clientstatuschange(fdclient);
    } else if (fdclient->status == WAITCONNECT) {
        if (memcmp(readbuf, "HTTP/1.1 200 Connection established\r\n", 37)) {
            printf("fd:%d, in %s, at %d\n", fdclient->fd,  __FILE__, __LINE__);
            readbuf[len] = '\0';
            printf("len:%d\n", len);
            printf("%s\n", readbuf);
            removeclient(fdclient);
            return -4;
        }
        clientstatuschange(fdclient);
    } else if (fdclient->status == REGISTER) {
        struct FDCLIENT* fdserver = fdclient->outclient;
        writenode(fdserver, readbuf, len);
    }
}

int clientstatuschange (struct FDCLIENT* fdclient) {
    switch (fdclient->status) {
        case TCPNOREADY:
                if (c.needauth) {
                    if (writenode(fdclient, auth, authlen)) {
                        printf("write node fail, in %s, at %d\n",  __FILE__, __LINE__);
                        removeclient(fdclient);
                        return -1;
                    }
                    fdclient->status = WAITAUTH;
                    return 0;
                }
                // 这里不要添加break，如果不需要auth认证，直接判断是否需要启动代理。
        case WAITAUTH:
                if (c.connectmode) {
                    if (writenode(fdclient, connproxy, connproxylen)) {
                        printf("write node fail, in %s, at %d\n",  __FILE__, __LINE__);
                        removeclient(fdclient);
                        return -2;
                    }
                    fdclient->status = WAITCONNECT;
                    return 0;
                }
                // 这里不要添加break，如果不需要connect代理，直接设置状态完成。
        case WAITCONNECT:
                fdclient->status = REGISTER;
                struct FDCLIENT *fdserver = fdclient->outclient;
                if (addtoepoll(fdserver, EPOLLIN)) {
                    perror("add epoll error");
                    printf("fd:%d, errno:%d, in %s, at %d\n", fdserver->fd, errno,  __FILE__, __LINE__);
                    removeclient(fdserver);
                    return -3;
                }
                fdserver->status = REGISTER;
    }
    return 0;
}

int setsocketoption (int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        printf("get flags fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        printf("set flags fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -2;
    }
    unsigned int socksval = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (unsigned char*)&socksval, sizeof(socksval))) { // 关闭Nagle协议
        printf("close Nagle protocol fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -3;
    }
#ifdef KEEPALIVE
    socksval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (unsigned char*)&socksval, sizeof(socksval))) { // 启动tcp心跳包
        printf("set socket keepalive fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -4;
    }
    socksval = KEEPIDLE;
    if (setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, (unsigned char*)&socksval, sizeof(socksval))) { // 设置tcp心跳包参数
        printf("set socket keepidle fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -5;
    }
    socksval = KEEPINTVL;
    if (setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, (unsigned char*)&socksval, sizeof(socksval))) { // 设置tcp心跳包参数
        printf("set socket keepintvl fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -6;
    }
    socksval = KEEPCNT;
    if (setsockopt(fd, SOL_TCP, TCP_KEEPCNT, (unsigned char*)&socksval, sizeof(socksval))) { // 设置tcp心跳包参数
        printf("set socket keepcnt fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        close(fd);
        return -7;
    }
#endif
    // 修改发送缓冲区大小
    socklen_t socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get send buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        close(fd);
        return -8;
    }
    socksval = MAXDATASIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (unsigned char*)&socksval, sizeof (socksval))) {
        printf("set send buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        close(fd);
        return -9;
    }
    socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get send buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        close(fd);
        return -10;
    }
    // 修改接收缓冲区大小
    socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get receive buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        close(fd);
        return -11;
    }
    socksval = MAXDATASIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&socksval, sizeof(socksval))) {
        printf("set receive buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        close(fd);
        return -12;
    }
    socksval_len = sizeof(socksval);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (unsigned char*)&socksval, &socksval_len)) {
        printf("get receive buffer fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        close(fd);
        return -13;
    }
    return 0;
}

int addclient (int acceptfd) {
    struct sockaddr_in sin;
    socklen_t in_addr_len = sizeof(struct sockaddr_in);
    int infd = accept(acceptfd, (struct sockaddr*)&sin, &in_addr_len);
    if (infd < 0) {
        printf("create socket fd is fail, in %s, at %d\n", __FILE__, __LINE__);
        return -1;
    }
    if (setsocketoption(infd)) {
        printf("set fd to nonblocking fail, in %s, at %d\n", __FILE__, __LINE__);
        close(infd);
        return -2;
    }
    struct FDCLIENT* fdclient;
    if (remainfdclienthead) { // 有存货，直接拿出来用
        fdclient = remainfdclienthead;
        remainfdclienthead = remainfdclienthead->outclient;
    } else { // 没有存货，malloc一个
        fdclient = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
        if (fdclient == NULL) {
            printf("malloc fail, in %s, at %d\n",  __FILE__, __LINE__);
            close(infd);
            return -3;
        }
        fdclient->data = NULL;
        fdclient->fullsize = 0;
    }
    fdclient->fd = infd;
    fdclient->tls = NULL;
    fdclient->status = UNWATCH;
    fdclient->canwrite = 1;
    fdclient->datasize = 0;
    int outfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (outfd < 0) {
        printf("create socket fd is fail, in %s, at %d\n", __FILE__, __LINE__);
        removeclient(fdclient);
        return -4;
    }
    if (setsocketoption(outfd)) {
        printf("set fd to nonblocking fail, in %s, at %d\n", __FILE__, __LINE__);
        removeclient(fdclient);
        close(outfd);
        return -5;
    }
    struct FDCLIENT* fdserver;
    if (remainfdclienthead) { // 有存货，直接拿出来用
        fdserver = remainfdclienthead;
        remainfdclienthead = remainfdclienthead->outclient;
    } else { // 没有存货，malloc一个
        fdserver = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
        if (fdserver == NULL) {
            printf("malloc fail, in %s, at %d\n",  __FILE__, __LINE__);
            removeclient(fdclient);
            close(outfd);
            return -6;
        }
        fdserver->data = NULL;
        fdserver->fullsize = 0;
    }
    fdserver->fd = outfd;
    fdserver->status = UNWATCH;
    fdserver->canwrite = 1;
    fdserver->datasize = 0;
    fdserver->tls = NULL;
    fdclient->outclient = fdserver;
    fdserver->outclient = fdclient;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(c.serveraddrport);
    memcpy(&sin.sin_addr, c.serveraddrip, 4);
    if (connect(outfd, (struct sockaddr*)&sin, sizeof(struct sockaddr)) < 0) {
        if (errno != EINPROGRESS) {
            perror("tcp connect error");
            printf("errno:%d, in %s, at %d\n", errno, __FILE__, __LINE__);
            removeclient(fdserver);
            return -7;
        }
    }
    if (ctx) {
        SSL *tls = SSL_new(ctx);
        SSL_set_fd(tls, outfd);
        fdserver->tls = tls;
    }
    if (addtoepoll(fdserver, EPOLLOUT)) {
        printf("create epoll fd fail, in %s, at %d\n",  __FILE__, __LINE__);
        removeclient(fdserver);
        return -8;
    }
    fdserver->status = TCPNOREADY;
}

int create_socketfd () {
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("run socket function is fail, fd:%d, in %s, at %d\n", fd, __FILE__, __LINE__);
        return -1;
    }
    memset(&sin, 0, sizeof(struct sockaddr_in));
    sin.sin_family = AF_INET; // ipv4
    sin.sin_addr.s_addr = INADDR_ANY; // 本机任意ip
    sin.sin_port = htons(c.bindport);
    if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        printf("bind port %d fail, fd:%d, in %s, at %d\n", c.bindport, fd, __FILE__, __LINE__);
        close(fd);
        return -2;
    }
    if (listen(fd, MAX_CONNECT) < 0) {
        printf("listen port %d fail, fd:%d, in %s, at %d\n", c.bindport, fd, __FILE__, __LINE__);
        close(fd);
        return -3;
    }
    struct FDCLIENT* fdserver;
    if (remainfdclienthead) { // 有存货，直接拿出来用
        fdserver = remainfdclienthead;
        remainfdclienthead = remainfdclienthead->outclient;
    } else { // 没有存货，malloc一个
        fdserver = (struct FDCLIENT*) malloc(sizeof(struct FDCLIENT));
        if (fdserver == NULL) {
            printf("malloc fail, in %s, at %d\n",  __FILE__, __LINE__);
            close(fd);
            return -5;
        }
        fdserver->data = NULL;
        fdserver->fullsize = 0;
    }
    fdserver->fd = fd;
    fdserver->tls = NULL;
    fdserver->status = UNWATCH;
    fdserver->canwrite = 1;
    fdserver->datasize = 0;
    fdserver->outclient = NULL;
    if (addtoepoll(fdserver, EPOLLIN)) {
        printf("serverfd add to epoll fail, fd:%d, in %s, at %d\n", fd,  __FILE__, __LINE__);
        close(fd);
        fdserver->status = DISCONNECT;
        fdserver->outclient = remainfdclienthead;
        remainfdclienthead = fdserver;
        return -6;
    }
    fdserver->status = REGISTER;
    return fd;
}

#define JSONSIZE   1024
#define HEADSIZE   1024
int initconfigdata () {
    char *json = (char*)malloc(JSONSIZE);
    if (json == NULL) {
        perror("malloc fail");
        printf("size:%d, in %s, at %d\n", JSONSIZE, __FILE__, __LINE__);
        return -1;
    }
    int fd = open("config.json", O_RDONLY );
    if (fd < 0) {
        fd = open("config.json", O_WRONLY|O_CREAT, 0777);
        if (fd < 0) {
            perror("config.json read fail.");
            printf("errno:%d, in %s, at %d\n", errno, __FILE__, __LINE__);
            free(json);
            return -2;
        }
        if (write(fd, defconf, sizeof(defconf)-1) < 0) {
            perror("init config.json fail.");
            printf("errno:%d, in %s, at %d\n", errno, __FILE__, __LINE__);
            close(fd);
            free(json);
            return -3;
        }
        memcpy(json, defconf, sizeof(defconf));
    } else {
        ssize_t len = read(fd, json, JSONSIZE);
        if (len < 0) {
            perror("config.json read fail.");
            printf("errno:%d, in %s, at %d\n", errno, __FILE__, __LINE__);
            close(fd);
            free(json);
            return -4;
        }
        json[len] = '\0';
    }
    close(fd);
    cJSON *obj = cJSON_Parse(json);
    free(json);
    cJSON *item = cJSON_GetObjectItem(obj, "ssl");
    c.ssl = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(obj, "bindport");
    c.bindport = cJSON_GetNumberValue(item);
    item = cJSON_GetObjectItem(obj, "serveraddr");
    char *serveraddr = cJSON_GetStringValue(item);
    short colon = -1;
    unsigned short serveraddrlen;
    for (serveraddrlen = 0 ; serveraddr[serveraddrlen] != '\0' ; serveraddrlen++) {
        if (serveraddr[serveraddrlen] == ':') {
            colon = serveraddrlen;
        }
    }
    if (colon == -1) {
        if (c.ssl) {
            c.serveraddrport = 443;
        } else {
            c.serveraddrport = 80;
        }
    } else {
        c.serveraddrport = 0;
        for (unsigned char i = colon+1 ; serveraddr[i] != '\0' ; i++) {
            c.serveraddrport = 10 * c.serveraddrport + serveraddr[i] - '0';
        }
        serveraddr[colon] = '\0';
    }
    struct hostent *ip = gethostbyname(serveraddr); // 域名dns解析
    if(ip == NULL) {
        printf("get ip by domain error, domain:%s, in %s, at %d\n", serveraddr,  __FILE__, __LINE__);
        cJSON_Delete(obj);
        return -5;
    }
    if (ip->h_addrtype == AF_INET) { // ipv4
        memcpy(&c.serveraddrip, ip->h_addr_list[0], 4);
    } else if (ip->h_addrtype == AF_INET6) { // ipv6
        printf("not support ipv6, in %s, at %d\n", __FILE__, __LINE__);
        cJSON_Delete(obj);
        return -6;
    }
    item = cJSON_GetObjectItem(obj, "needauth");
    c.needauth = cJSON_IsTrue(item);
    if (c.needauth) {
        item = cJSON_GetObjectItem(obj, "path");
        char *path = cJSON_GetStringValue(item);
        item = cJSON_GetObjectItem(obj, "key");
        char *key = cJSON_GetStringValue(item);
        char *head = (char*)malloc(HEADSIZE);
        if (head == NULL) {
            perror("malloc fail");
            printf("size: %d, in %s, at %d\n", HEADSIZE, __FILE__, __LINE__);
            cJSON_Delete(obj);
            return -7;
        }
        unsigned short len = sprintf(head, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: Upgrade\r\nPragma: no-cache\r\nCache-Control: no-cache\r\nUpgrade: websocket\r\nAuthorization: %s\r\n\r\n",
                    path, serveraddr, key);
        auth = (char*)malloc(len);
        if (auth == NULL) {
            perror("malloc fail");
            printf("size: %d, in %s, at %d\n", len, __FILE__, __LINE__);
            free(head);
            cJSON_Delete(obj);
            return -8;
        }
        memcpy(auth, head, len);
        free(head);
        authlen = len;
    }
    item = cJSON_GetObjectItem(obj, "connectmode");
    c.connectmode = cJSON_IsTrue(item);
    if (c.connectmode) {
        item = cJSON_GetObjectItem(obj, "targetaddr");
        char *targetaddr = cJSON_GetStringValue(item);
        char *head = (char*)malloc(HEADSIZE);
        if (head == NULL) {
            perror("malloc fail");
            printf("size: %d, in %s, at %d\n", HEADSIZE, __FILE__, __LINE__);
            cJSON_Delete(obj);
            return -9;
        }
        unsigned short len = sprintf(head, "CONNECT %s HTTP/1.1\r\nHost: %s\r\nProxy-Connection: keep-alive\r\n\r\n",
                    targetaddr, targetaddr);
        connproxy = (char*)malloc(len);
        if (connproxy == NULL) {
            perror("malloc fail");
            printf("size: %d, in %s, at %d\n", len, __FILE__, __LINE__);
            free(head);
            cJSON_Delete(obj);
            return -10;
        }
        memcpy(connproxy, head, len);
        free(head);
        connproxylen = len;
    }
    cJSON_Delete(obj);
    return 0;
}
