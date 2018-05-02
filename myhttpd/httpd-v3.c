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

/*
 * A httpd server
 * 2017-12-12
 * V2.0 -- A simple version for http server
 */

#define MAX_CLIENT_NUM 10
#define MAX_WORKER_NUM 3
#define HTTP_SERVER_HOST "172.0.1.3"
#define HTTP_SERVER_PORT 56430

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"

int setup_socket(int port);
int add_epoll_event(int efd, int fd, bool oneshot, bool et_mode);
void set_nonblock(int fd);
void *thread_worker(void *arg);
struct thread_pool_t * create_thread_pool(int nThread);
void distribute_fd_to_worker(int fd);
void send_header_info(int client, const char *filename);
void send_body_info(int client, const char *filename);
void not_found(int client);

int socket_read(int fd, char *ptr, int len);
int socket_write(int client, char * header, int length);

//Global
static int distribute_pipe[MAX_CLIENT_NUM][2];
static int listenfd = -1;

struct thread_t 
{
	pthread_t id;       //thread id
	int thread_index;   //thread index in pool
	int pipe_fd;        //read pipe fd
	int num_fd;	        //numbers of accept fd
};

struct thread_pool_t
{
	int max_thread_num; //max thread number
	struct thread_t thread[MAX_WORKER_NUM];
};

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
		//we want to know which port the kernal used
		rc = getsockname(fd, (struct sockaddr *)&serverAddr, &addrLen);
		assert(rc != -1);
		printf("httpd running on port %d\n", ntohs(serverAddr.sin_port));
	}

	rc = listen(fd, MAX_CLIENT_NUM);
	assert(rc != -1);
    
    listenfd = fd; //set global listen fd
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

int mod_epoll_event(int efd, int fd, int new_type)
{
    assert(efd != 0 && fd != 0);
    struct epoll_event event;
    event.data.fd = fd;
    //event.events = new_type | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    event.events = new_type | EPOLLET;
    int rc = epoll_ctl(efd, EPOLL_CTL_MOD, fd, &event );
    assert(rc != -1);
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
            return -3; //no more space
        }
    }

    return nread;
}


void send_header_info(int client, const char *filename)
{
    char *header = "HTTP/1.1 200 OK\r\nServer: jdbhttpd/0.1.0\r\nContent-Type: text/html\r\n\r\n";
    socket_write(client, header, strlen(header));
}

void not_found(int client)
{
    char buf[64] = {0};

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}


void send_body_info(int client, const char *filename)
{
    assert(filename != NULL);
    FILE *fp = fopen(filename, "r");
    if(fp == NULL)
    {
        printf("no find..\n");
        not_found(client);
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


void *thread_worker(void *arg)
{
	assert(arg != NULL);
	struct thread_t *thread = (struct thread_t *)arg;
	int nfds, i, rc, nread, fd = 0;
    char buf[64] = {0};

    struct epoll_event events[10];
    int efd = epoll_create(10);
    assert(efd != -1);
    add_epoll_event(efd, thread->pipe_fd, false, false);
    int conn_fd = -1;
    
	while(1)
    {   
        nfds = epoll_wait(efd, events, 10, -1);
        if((nfds < 0) && (errno != EINTR))
        {
            printf( "epoll failure\n" );
            break;
        }
      
        for(i=0; i<nfds; i++)
        {
            int ok_fd = events[i].data.fd;
            if((ok_fd == thread->pipe_fd) && (events[i].events & EPOLLIN))
            {
                nread = read(ok_fd, &conn_fd, sizeof(conn_fd));
                if(nread == 0)
                    continue;

                printf("Worker %d get connfd %d\n", thread->thread_index, conn_fd);

                //Now the worker get the connfd, it can receive and send data to client
                char buffer[1024];
                socket_read(conn_fd, buffer, sizeof(buffer));
                //printf("Read: %s\n", buffer);

                send_header_info(conn_fd, "./htdocs/index.html");
                send_body_info(conn_fd, "./htdocs/index.html");
                close(conn_fd);
            }
        }
	}
}

struct thread_pool_t *create_thread_pool(int nThread)
{
	struct thread_pool_t *pool = (struct thread_pool_t *)malloc(sizeof(struct thread_pool_t));
	assert(pool != NULL);

	pool->max_thread_num = nThread;
	int rc, i = 0;
	int pipe_fd[2];
	for(i=0; i<nThread; i++)
	{
		rc = pipe(pipe_fd); //pipe_fd[0]: read, pipe_fd[1]: write
		assert(rc != -1);
		memcpy(&distribute_pipe[i], pipe_fd, sizeof(pipe_fd));
		pool->thread[i].pipe_fd = distribute_pipe[i][0];
        pool->thread[i].thread_index = i;
		pthread_create(&pool->thread[i].id, NULL, (void *)&thread_worker, &pool->thread[i]);
	}

	return pool;
}

void distribute_fd_to_worker(int fd)
{
	static int cur_thread_index = -1;
	cur_thread_index = (cur_thread_index+1) % MAX_WORKER_NUM; //round robin schedule

	//According to cur_thread_index, distribute fd through PIPE
	int pipe_fd = distribute_pipe[cur_thread_index][1]; //write fd
	//printf("cur_thread_index %d, write fd %d, connfd %d\n", cur_thread_index, pipe_fd, fd);

	write(pipe_fd, &fd, sizeof(int)); //write fd to PIPE
}


int main(int argc, char *argv[])
{
	int port = 0;
	if(argc == 2)
		port = atoi(argv[1]);
	else
		port = HTTP_SERVER_PORT;
	
	int fd = setup_socket(port);
    //set fd to nonblock or it will block the accept operation!
	set_nonblock(fd);
    int efd = epoll_create(10);
    assert(efd != -1);
    add_epoll_event(efd, fd, false, true); //The listen fd should not use oneshot !

	struct epoll_event events[MAX_CLIENT_NUM];
	int i, j, connFd = 1;
	struct sockaddr_in clientAddr;
	socklen_t addrLen = sizeof(clientAddr);
	int client_fd[MAX_CLIENT_NUM] = {-1};

	create_thread_pool(MAX_WORKER_NUM);
    
	while(1)
	{
		int nfds = epoll_wait(efd, events, MAX_CLIENT_NUM, -1);
		if(nfds < 0)
			perror("epoll_wait");

		for(i=0; i<nfds; i++)
		{
		    int new_fd = events[i].data.fd;
			if(new_fd == fd)
			{
				while((connFd = accept(fd, (struct sockaddr *)&clientAddr, &addrLen)) > 0)
				{
					add_epoll_event(efd, connFd, true, true);
				}
                
				//How to deal the error here?
			}
            else if(events[i].events & EPOLLIN)
            {
                distribute_fd_to_worker(new_fd);
            }
            else if(events[i].events & EPOLLERR || events[i].events & EPOLLRDHUP)
            {
                //happen error, close the socket
                close(events[i].data.fd);
            }
		}
	}
	
	return 0;
}

