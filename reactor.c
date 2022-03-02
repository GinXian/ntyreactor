#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define BUFFER_LENGTH       (1 << 12)
#define MAX_EPOLL_EVENTS    (1 << 10)
#define SERVER_PORT         8888
#define PORT_COUNT          100


typedef int TCALLBACK(int, int, void*);


struct ntyevent {
    int fd;
    int events;
    void *arg;
    TCALLBACK *callback;

    int status;                        // whether already under epoll monitor
    char buffer[BUFFER_LENGTH];        // read/write buffer
    int length;                        // the length of the occupied portion of the buffer
    long last_active;                  // time-out mechanism
};

struct eventblock {
    struct eventblock *next;
    struct ntyevent *events;
};

struct ntyreactor {
    struct eventblock *evblk;       // a list of blocks of events
    struct eventblock *tail;
    int epfd;                       // epoll file descriptor
    int blkcnt;                     // the number of blocks
};


int recv_cb(int fd, int events, void *arg);
int send_cb(int fd, int events, void *arg);
int accept_cb(int fd, int events, void *arg);

struct ntyevent *ntyreactor_idx(struct ntyreactor *r, int fd);


void nty_event_set(struct ntyevent *ev, int fd, TCALLBACK *cb, void *arg) {
    ev->fd = fd;
    ev->callback = cb;
    ev->arg = arg;
    ev->last_active = time(NULL);
    return;
}

int nty_event_add(int epfd, int events, struct ntyevent *ev) {
    struct epoll_event ep_ev = {0, {0}};
    ep_ev.data.ptr = ev;
    ep_ev.events = ev->events = events;

    int op;
    if(ev->status) {
        // the event is already under epoll monitor
        // only need to modify it
        op = EPOLL_CTL_MOD;
    } else {
        // the event is fresh in town
        // need to add it to epoll monitor list
        op = EPOLL_CTL_ADD;
    }

    if(epoll_ctl(epfd, op, ev->fd, &ep_ev) == -1) {
        printf("event add failed [fd=%d], events[%d]", ev->fd, events);
        return -1;
    }
    return 0;
}

int nty_event_del(int epfd, struct ntyevent *ev) {
    struct epoll_event ep_ev = {0, {0}};

    if(ev->status != 1) {
        // the event doesn't exist yet
        return -1;
    }

    ep_ev.data.ptr = ev;
    ev->status = 0;
    epoll_ctl(epfd, EPOLL_CTL_DEL, ev->fd, &ep_ev);
    return 0;
}

int recv_cb(int fd, int events, void *arg) {
    struct ntyreactor *reactor = (struct ntyreactor*)arg;
    struct ntyevent *ev = ntyreactor_idx(reactor, fd);

    int len = recv(fd, ev->buffer, BUFFER_LENGTH, 0);

    if(len > 0) {
        // Get ready to write next time
        ev->buffer[len] = '\0';
        ev->length = len;
        printf("Client[%d]:%s\n", fd, ev->buffer);
        nty_event_set(ev, fd, send_cb, reactor);
        nty_event_add(reactor->epfd, events, ev);

    } else if(len == 0) {
        // Close the connection
        nty_event_del(reactor->epfd, ev);
        close(ev->fd);

    } else if(len == -1) {
        // nothing to read, or other errors
        nty_event_del(reactor->epfd, ev);
        close(ev->fd);
        printf("recv[fd=%d] error[%d]:%s\n", ev->fd, errno, strerror(errno));
    }
    return len;
}

int send_cb(int fd, int events, void *arg) {
    struct ntyreactor *reactor = (struct ntyreactor*)arg;
    struct ntyevent *ev = ntyreactor_idx(reactor, fd);

    int len = send(fd, ev->buffer, ev->length, 0);

    if(len > 0) {
        // successfully send 
        printf("send[fd=%d], [%d]:%s\n", fd, len, ev->buffer);
        nty_event_set(ev, fd, recv_cb, reactor);
        nty_event_add(reactor->epfd, events, ev);

    } else {
        // close the connection
        nty_event_del(reactor->epfd, ev);
        close(fd);
        printf("send[%d] error[%d]:%s\n", fd, errno, strerror(errno));
    }

    return len;
}

int accept_cb(int fd, int events, void *arg) {
    struct ntyreactor *reactor = (struct ntyreactor*)arg;

    int clientfd;
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    socklen_t len = sizeof(client_addr);

    if((clientfd = accept(fd, (struct sockaddr*)&client_addr, &len)) == -1) {
        // error occurred when accepting connection
        printf("accept error[%d]: %s\n", errno, strerror(errno));
        return -1;
    }

    // Set attributes of the new connection
    if(fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        printf("%s: fcntl nonblocking failed\n", strerror(errno));
        return -1;
    }

    struct ntyevent *client_ev = ntyreactor_idx(reactor, fd);

    nty_event_set(client_ev, clientfd, recv_cb, reactor);
    nty_event_add(reactor->epfd, events, client_ev);

    printf("new connect [%s:%d], pos[%d]\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), clientfd);

    return 0;
}

int init_sock(short port) {
    int serverfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    server_addr.sin_family = AF_INET;

    if(bind(serverfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        printf("bind socket error[%d]:%s\n", errno, strerror(errno));
        return -1;
    }

    if(listen(serverfd, 20) == -1) {
        printf("listen socket error[%d]: %s\n", errno, strerror(errno));
        return -1;
    }
    return serverfd;
}

int ntyreactor_alloc(struct ntyreactor *reactor) {
    if(NULL == reactor) {
        return -1;
    }
    if(NULL == reactor->evblk || NULL == reactor->tail) {
        return -1;
    }

    // struct eventblock *target = reactor->tail;
    struct ntyevent *evs = (struct ntyevent*)malloc(sizeof(struct ntyevent) * MAX_EPOLL_EVENTS);

    if(NULL == evs) {
        printf("ntyevents alloc failed.\n");
        return -2;
    }

    // Sanitize the newly allocated area
    memset(evs, 0, sizeof(struct ntyevent) * MAX_EPOLL_EVENTS);

    struct eventblock *blk = (struct eventblock*)malloc(sizeof(struct eventblock));

    if(NULL == blk) {
        printf("eventblock alloc failed.\n");
        return -2;
    }

    memset(blk, 0, sizeof(struct eventblock));

    blk->events = evs;
    blk->next = NULL;
    reactor->tail->next = blk;
    reactor->blkcnt++;
    return 0;   
}

struct ntyevent *ntyreactor_idx(struct ntyreactor *reactor, int fd) {
    int blk_idx = fd / MAX_EPOLL_EVENTS;       // start from 0
    int evs_idx = fd % MAX_EPOLL_EVENTS;

    while(reactor->blkcnt <= blk_idx) {
        ntyreactor_alloc(reactor);
    }

    struct eventblock *ptr = reactor->evblk;
    for(int i = 0; i < blk_idx; ++i) {
        ptr = ptr->next;
    }

    return &ptr->events[evs_idx];
}

int ntyreactor_init(struct ntyreactor *reactor) {
    if(NULL == reactor) {
        return -1;
    }

    memset(reactor, 0, sizeof(struct ntyreactor));

    reactor->epfd = epoll_create(1);
    if(reactor->epfd <= 0) {
        printf("create epoll error in %s. error: %s\n", __func__, strerror(errno));
        return -2;
    }

    struct ntyevent *evs = (struct ntyevent*)malloc(sizeof(struct ntyevent) * MAX_EPOLL_EVENTS);
    if(NULL == evs) {
        printf("ntyevent alloc failed.\n");
        return -2;
    }

    memset(evs, 0, sizeof(struct ntyevent) * MAX_EPOLL_EVENTS);

    struct eventblock *blk = (struct eventblock*)malloc(sizeof(struct eventblock));

    if(NULL == blk) {
        printf("eventblock alloc failed.\n");
        return -2;
    }

    memset(blk, 0, sizeof(struct eventblock));

    blk->events = evs;
    blk->next = NULL;
    reactor->blkcnt = 1;
    reactor->evblk = reactor->tail = blk;

    return 0;
}

int ntyreactor_destroy(struct ntyreactor *reactor) {
    close(reactor->epfd);

    struct eventblock *blk = reactor->evblk;
    struct eventblock *blk_next = NULL;

    while(blk != NULL) {
        blk_next = blk->next;
        free(blk->events);
        free(blk);
        blk = blk_next;
    }
    return 0;
}

int ntyreactor_addlistener(struct ntyreactor *reactor, int fd, TCALLBACK *acceptor) {
    if(NULL == reactor) {
        return -1;
    }
    if(NULL == reactor->evblk) {
        return -1;
    }

    struct ntyevent *event = ntyreactor_idx(reactor, fd);
    nty_event_set(event, fd, acceptor, reactor);
    nty_event_add(reactor->epfd, EPOLLIN, event);

    return 0;
}

int ntyreactor_run(struct ntyreactor *reactor) {
    if(NULL == reactor) {
        return -1;
    }
    if(NULL == reactor->evblk) {
        return -1;
    }
    if(reactor->epfd < 0) {
        return -1;
    }

    int epfd = reactor->epfd;
    struct epoll_event events[MAX_EPOLL_EVENTS + 1];

    while(1) {
        int nready = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, 1000);

        if(nready < 0) {
            printf("epoll_wait error, exit!\n");
            continue;
        }

        for(int i = 0; i < nready; ++i) {
            struct ntyevent *ev = (struct ntyevent*)events[i].data.ptr;

            if((events[i].events & EPOLLIN) && (ev->events & EPOLLIN)) {
                ev->callback(ev->fd, EPOLLOUT, ev->arg);
            }
            if((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)) {
                ev->callback(ev->fd, EPOLLIN, ev->arg);
            }
        }
    }
}


int main(int argc, char **argv) {
    unsigned short port = SERVER_PORT;

    if(argc == 2) {
        port = atoi(argv[1]);
    }

    struct ntyreactor reactor;
    ntyreactor_init(&reactor);

    int listenfds[PORT_COUNT] = {0};
    for(int i = 0; i < PORT_COUNT; ++i) {
        listenfds[i] = init_sock(port + i);
        ntyreactor_addlistener(&reactor, listenfds[i], accept_cb);
    }

    ntyreactor_run(&reactor);

    ntyreactor_destroy(&reactor);

    for(int i = 0; i < PORT_COUNT; ++i) {
        close(listenfds[i]);
    }

    return 0;
}