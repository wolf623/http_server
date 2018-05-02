#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <stdbool.h>

#define MAX_CLIENT_NUM 10
#define MAX_WORKER_NUM 3
#define HTTP_SERVER_HOST "172.0.1.3"
#define HTTP_SERVER_PORT 7865

static g_listenfd = -1;

void set_nonblock(int fd);
int setup_socket(int port);
int add_epoll_event(int epollfd, int fd, bool oneshot, bool et_mode);

void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int setup_socket(int port)
{
	if(port == HTTP_SERVER_PORT)
		port = 0; //let kernel do the choice

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd != 0);

	struct sockaddr_in serverAddr;
	socklen_t addrLen = sizeof(serverAddr);
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(port);
	serverAddr.sin_addr.s_addr = inet_addr(HTTP_SERVER_HOST);
	int rc = bind(fd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
	assert(rc != -1);

	if(port == 0)
	{
		rc = getsockname(fd, (struct sockaddr *)&serverAddr, &addrLen);
		assert(rc != -1);
		printf("httpd running on port %d\n", ntohs(serverAddr.sin_port));
	}

	rc = listen(fd, MAX_CLIENT_NUM);
	assert(rc != -1);
    
    g_listenfd = fd; //set global listen fd
	return fd;
}

int add_epoll_event(int epollfd, int fd, bool oneshot, bool et_mode)
{
    int efd = epollfd;
    if(efd == 0)
    {
	    efd = epoll_create(MAX_CLIENT_NUM);
	    assert(efd != -1);
    }
    
	struct epoll_event ev;
    if(oneshot == true)
        ev.events = EPOLLIN | EPOLLONESHOT;
    else
	    ev.events = EPOLLIN;

    if(et_mode == true)
        ev.events |= EPOLLET;
   
	ev.data.fd = fd;
	int rc = epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
	assert(rc != -1);

	return efd;
}

int socket_read(int fd, char *ptr, int len)
{
    assert(len > 0 && fd > 0);
    assert(ptr != NULL);
    int nread = 0, n = 0;
    
    while(1) 
    {
        nread = read(fd, ptr+n, len-1);
        if(nread < 0) 
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK) 
            {
                return nread; //have read one
            } 
            else if(errno == EINTR) 
            {
                continue; //interrupt by signal, continue
            } 
            else if(errno == ECONNRESET) 
            {
                return -1; //client send RST
            } 
            else 
            {
                return -1; //faild
            }
        } 
        else if(nread == 0)
        {
            return -2; //client is closed
        }
        else if(nread < len-1) 
        {
            return nread; //no more data, read done
        } 
        else
        {
            /*
             * Here, if nread == len-1, maybe have add done,
             * For simple, we just return here,
             * A better way is to MOD EPOLLIN into epoll events again
             */
            printf("Error: no more space...\n");
            return -3; //no more space
        }
    }

    return nread;
}

int socket_write(int client, char *header, int length)
{
    int len = strlen(header);
    while(len > 0)
    {
        int nwrite = write(client, header, len);
        if(nwrite < 0)
        {
            if(errno == EINTR)
            {
                nwrite = 0;
            }
            else if(errno == EAGAIN)
            {
                nwrite = 0;
                usleep(1);
                continue;
            }
            else
            {
                return -1; //send failed!
            }   
        }
        else if(nwrite == 0)
        {
            close(client);
            printf("Peer client is closed!\n");
            return;
        }

        header = header + nwrite;
        len = len - nwrite;
    }

    return 0;
}


void send_header_info(int client, const char *filename)
{
    char *header = "HTTP/1.1 200 OK\r\nServer: jdbhttpd/0.1.0\r\nContent-Type: text/html\r\n\r\n";
    socket_write(client, header, strlen(header));
}

void send_body_info(int client, const char *filename)
{
    assert(filename != NULL);
    FILE *fp = fopen(filename, "r");
    if(fp == NULL)
    {
        printf("no find..\n");
        return;
    }

    char buf[512] = {0};
    fgets(buf, sizeof(buf), fp);
    while (!feof(fp))
    {
        socket_write(client, buf, strlen(buf));
        fgets(buf, sizeof(buf), fp);
    }
    fclose(fp);
}

void *worker_thread(void *arg)
{
    int fd = *(int *)arg;

    //Must read the http head firstly, then send respone, but why???
    //And pls take care there has enough space to receive data
    //Here, we get all information, but in fact, we only need to read and discard http head information
    char buffer[1024];
    socket_read(fd, buffer, sizeof(buffer));
    printf("Read: %s\n", buffer);
    
    send_header_info(fd, NULL);
    send_body_info(fd, "./htdocs/index.html");
    printf("ByeBye...\n");
    close(fd);
    return;
}

int main(int argc, char *argv[])
{
    int port = 0;
    if(argc == 2)
        port = atoi(argv[1]);
    else
        port = HTTP_SERVER_PORT;
    
    int fd = setup_socket(port);
    int connfd = -1;
    pthread_t newthreadid;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    while(1)
    {
        connfd = accept(fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if(connfd == -1)
            perror("accept\n");

        if(pthread_create(&newthreadid, NULL, (void *)&worker_thread, &connfd) != 0)
            perror("pthread_create\n");
    }
    
    return 0;
}


