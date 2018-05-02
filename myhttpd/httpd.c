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

/*
 * A httpd server
 * 2017-11-15
 */

#define MAX_CLIENT_NUM 10
#define MAX_WORKER_NUM 3
#define HTTP_SERVER_HOST "172.0.1.3"
#define HTTP_SERVER_PORT 56430

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"

int setup_socket(int port);
int add_epoll_event(int fd);
void set_nonblock(int fd);
void *thread_worker(void *arg);
struct thread_pool_t * create_thread_pool(int nThread);
void distribute_fd_to_worker(int fd);
void send_header_info(int client, const char *filename);
void send_body_info(int client, const char *filename);
void not_found(int client);

//Global
static int distribute_pipe[MAX_CLIENT_NUM][2];

struct thread_t 
{
	pthread_t id; //thread id
	int thread_index; //thread index in pool
	int read_fd; //read pipe fd
	int num_fd;	//numbers of accept fd
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
#if 1
	if(port == HTTP_SERVER_PORT)
		port = 0; //let kernel do the choice
#endif
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

	return fd;
}

int add_epoll_event(int fd)
{
	int efd = epoll_create(MAX_CLIENT_NUM);
	assert(efd != -1);

	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET; //ET mode
	ev.data.fd = fd;
	int rc = epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
	assert(rc != -1);

	return efd;
}

void send_header_info(int client, const char *filename)
{
    char buf[64] = {0};

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

void not_found(int client)
{
    printf("%s(): I am here...0\n", __FUNCTION__);
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
    printf("%s(): I am here...0\n", __FUNCTION__);
    assert(filename != NULL);
    FILE *fp = fopen(filename, "r");
    if(fp == NULL)
    {
        not_found(client);
        return;
    }

    char buf[64] = {0};
    fgets(buf, sizeof(buf), fp);
    while (!feof(fp))
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), fp);
    }
    fclose(fp);
}

void *thread_worker(void *arg)
{
	assert(arg != NULL);
	struct thread_t *thread = (struct thread_t *)arg;
	int nfds, i, rc, fd = 0;
    char buf[64] = {0};
    
    struct epoll_event ev, events[10];
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    int efd = epoll_create(10);
    assert(efd != -1);
    
	while(1)
	{
		char buf[10] = {0};
		read(thread->read_fd, buf, sizeof(buf));
		fd = atoi(buf);

		//we have get the connfd, now do what we want to do
		//for example, add connfd into our epoll event

        set_nonblock(fd);
        ev.data.fd = fd;
        rc = epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
        if(rc == -1)
        {
            perror("epoll_ctl");
            close(fd);
            continue;
        }

        //printf("Worker %lu add connfd %d into EPOLL is ok\n", thread->thread_index, fd);
        while((nfds = epoll_wait(efd, events, 10, 1)) > 0)
        {
            for(i=0; i<nfds; i++)
            {
                int ok_fd = events[i].data.fd;
                if(events[i].events & EPOLLIN)
                {
                    //printf("Worker %lu: connfd %d can read now\n", thread->id, ok_fd);
                    int nread = read(ok_fd, buf, sizeof(buf));
                    if(nread == 0)
                    {
                        //printf("peer client is closed.\n");
                        close(ok_fd); //client is closed
                    }
                }
                else if(events[i].events & EPOLLOUT)
                {
                    
                    printf("Worker %lu: connfd %d can write now\n", thread->id, ok_fd);
                    #if 0
                    const char *str = "Hello, you are client #%d";
                    sprintf(buf, str, ok_fd);
                    int nwrite = write(ok_fd, buf, strlen(buf));
                    if(nwrite == -1)
                        perror("write");
                    #endif

                    send_header_info(ok_fd, NULL);
                    send_body_info(ok_fd, "./htdocs/index.html2");
                }
            }
        }

        //printf("Worker %lu: After deal with thing, continue to wait for new connfd come in...\n", thread->id);
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
		pool->thread[i].read_fd = distribute_pipe[i][0];
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

	//why can not send int?
	char buf[10] = {0};
	sprintf(buf, "%d", fd);
	write(pipe_fd, buf, strlen(buf)); //write fd to PIPE
}

int main(int argc, char *argv[])
{
	int port = 0;
	if(argc == 2)
		port = atoi(argv[1]);
	else
		port = HTTP_SERVER_PORT;
	
	int fd = setup_socket(port);
	set_nonblock(fd); //set fd to nonblock or it will block the accept operation!
	int efd = add_epoll_event(fd);

	struct epoll_event events[MAX_CLIENT_NUM];
	int i, j, connFd;
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
			//new client is came in
			if(events[i].data.fd == fd)
			{
				while((connFd = accept(fd, (struct sockaddr *)&clientAddr, &addrLen)) > 0)
				{
				    //printf("New client fd %d came in...\n", connFd);
					for(j=0; j<MAX_CLIENT_NUM; j++)
					{
						if(client_fd[j] == -1)
						{
							client_fd[j] = connFd;
							break;
						}
					}
				}

				//How to deal the error here?
			}
		}

		//Distribute connFd to work thread by round_robin way, Send connFd value by PIPE way.
		for(j=0; j<MAX_CLIENT_NUM; j++)
		{
			if(client_fd[j] != -1)
			{
				distribute_fd_to_worker(client_fd[j]);
				client_fd[j] = -1; //After distribute, reset it
				break;
			}
		}
	}
	
	return 0;
}

