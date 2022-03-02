#ifndef _H_REACTOR_
#define _H_REACTOR_

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>

#define MAX_BLOCK_EVENTS        (1 << 10)
#define MAX_BUFFER_SIZE         (1 << 11)
#define MAX_EPOLL_EVENTS        (1 << 10)

typedef int (*TCALLBACK)(int, int, void*);

struct event {
    int fd;
    int events;
    void *arg;
    int (*callback)(int fd, int events, void *arg);

    int status;     // Whether the event exists in epoll
    char buffer[MAX_BUFFER_SIZE];
    int length;
    long last_active;
};

struct event_block {
    struct event_block *next;
    struct event *events;
};

struct reactor {
    int epfd;
    int blkcnt;
    struct event_block *block;
};

void reactor_event_set(struct event *ev, int fd, TCALLBACK callback, void *arg, int events);

int  reactor_event_add(int epfd, struct event *ev);

int  reactor_event_del(int epfd, struct event *ev);

int  init_sock(short port);

int  reactor_alloc(struct reactor *r);

struct event *reactor_lookup(struct reactor *r, int sockfd);

int  reactor_init(struct reactor *r);

int  reactor_destroy(struct reactor *r);

int  reactor_run(struct reactor *r);

int  reactor_addlistener(struct reactor *r, int listenfd, TCALLBACK acceptor);

#endif      /* ENDIF _H_REACTOR */


