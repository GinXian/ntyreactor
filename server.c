#include "reactor.h"
#include <fcntl.h>
#include <arpa/inet.h>

#define SERVER_PORT         9999

int send_cb(int, int, void*);
int accept_cb(int, int, void*);
int recv_cb(int, int, void*);


int main() {
    struct reactor r;
    reactor_init(&r);

    int listenfd = init_sock(SERVER_PORT);

    reactor_addlistener(&r, listenfd, accept_cb);

    reactor_run(&r);

    reactor_destroy(&r);

    close(listenfd);
    return 0;
}


int send_cb(int fd, int events, void *arg) {
    struct reactor *r = (struct reactor*)arg;
    struct event *ev = reactor_lookup(r, fd);

    int ret, len = 0;
    while(len < ev->length) {
        ret = send(fd, ev->buffer + len, ev->length - len, 0);
        if(ret == -1) {
            reactor_event_del(r->epfd, ev);
            close(fd);
            printf("send socket error: %s(errno = %d)\n", strerror(errno), errno);
            return -1;
        }
        len += ret;
    }

    reactor_event_set(ev, fd, recv_cb, r, EPOLLIN | EPOLLET);
    reactor_event_add(r->epfd, ev);
    return 0;
}

int recv_cb(int fd, int events, void *arg) {
    struct reactor *r = (struct reactor*)arg;
    struct event *ev = reactor_lookup(r, fd);

    int ret, len = 0;
    while(len < MAX_BUFFER_SIZE) {
        ret = recv(fd, ev->buffer + len, MAX_BUFFER_SIZE - len, 0);
        if(ret == 0) {
            reactor_event_del(r->epfd, ev);
            close(fd);
            return -1;

        } else if(ret == -1) {
            break;

        } else {
            len += ret;
            ev->buffer[len] = '\0';
            printf("msg recv from client: %s\n", ev->buffer);
        }
    }

    ev->length = len;

    reactor_event_set(ev, fd, send_cb, r, EPOLLOUT | EPOLLET);
    reactor_event_add(r->epfd, ev);
    return 0;
}

int accept_cb(int fd, int events, void *arg) {
    struct reactor *r = (struct reactor*)arg;
    struct sockaddr_in client_addr;
    int len = sizeof(client_addr);
    int clientfd;
    
    if((clientfd = accept(fd, (struct sockaddr*)&client_addr, &len)) == -1) {
        printf("accept socket error: %s(errno = %d)\n", strerror(errno), errno);
        return -1;
    }

    if(fcntl(clientfd, F_SETFL, O_NONBLOCK) == -1) {
        printf("fcntl error: %s(errno = %d)\n", strerror(errno), errno);
        return -1;
    }

    printf("<================ Client %s:%d Connected ================>\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    struct event *ev = reactor_lookup(r, clientfd);
    if(ev == NULL) {
        return -1;
    }
    reactor_event_set(ev, clientfd, recv_cb, r, EPOLLIN);
    reactor_event_add(r->epfd, ev);
    return 0;
}
