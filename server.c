#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

#define BACKLOG 3
#define CHUNKSIZE 2
#define NMMAX 30
#define THREAD_NUM 3  // it's < 100 so max two digits are needed
#define FS_NUM 2
#define TURNINGS 10
#define SPEED 10
#define MAP_SIZE TURNINGS*SPEED + 1
#define INITIAL_MONEY 100
#define COLLISION_MONEY 50
#define TAXI_SPACE 3
#define REWRITE_TIME 1

#define ERRSTRING "No such file or directory\n"

volatile sig_atomic_t work = 1;

typedef struct
{
	int id;
	int *idlethreads;
	int *socket;
	int *condition;
	pthread_cond_t *cond;
	pthread_mutex_t *mutex;
	sem_t *semaphore;
	int **map;
} thread_arg;

typedef struct
{
	int clientfd;
	int **map;
	int *cash;
} timer_arg;

typedef struct
{
	int clientfd;
	int *dir;  // -1 = left, 1 = right, 0 = don't turn on turnings
} client_thread_arg;

void siginthandler(int sig)
{
	work = 0;
}

void usage(char *name)
{
	fprintf(stderr, "USAGE: %s port\n",name);
	exit(EXIT_FAILURE);
}

void sethandler(void (*f)(int), int sigNo)
{
	struct sigaction act;
	memset(&act, 0x00, sizeof(struct sigaction));
	act.sa_handler = f;

	if (-1 == sigaction(sigNo, &act, NULL))
		ERR("sigaction");
}

ssize_t bulk_read(int fd, char *buf, size_t count)
{
	int c;
	size_t len = 0;

	do
	{
		c = TEMP_FAILURE_RETRY(read(fd, buf, count));
		if (c < 0)
			return c;
		if (c == 0)
			return len;
		buf += c;
		len += c;
		count -= c;
	}
	while (count > 0);

	return len;
}

ssize_t bulk_write(int fd, char *buf, size_t count)
{
	int c;
	size_t len = 0;

	do
	{
		c = TEMP_FAILURE_RETRY(write(fd, buf, count));
		if(c < 0)
			return c;
		buf += c;
		len += c;
		count -= c;
	}
	while (count > 0);

	return len;
}

void print_client(int clientfd, int **map, int cash)  // don't forget htons
{
	int i, j;
	char buf[3], c[30], *eol = "\n";
	
	// copy map to local memory before sending
	
	write(clientfd, eol, strlen(eol));
	sprintf(c, "cash: %d\n", cash);
	write(clientfd, c, strlen(c));
	
	for(i = 0; i < MAP_SIZE; ++i)
	{
		for(j = 0; j < MAP_SIZE; ++j)
		{
			sprintf(buf, "%d", map[i][j]);
			write(clientfd, buf, strlen(buf));
		}
		write(clientfd, eol, strlen(eol));
	}
	
}

// returns number of free places on map
int check_room(int **map, int *x, int *y)
{
	// check each turning
	int ti, tj, di, dj, ii, jj, free_places = 0, cfree;
	for(ti = 0; ti < TURNINGS; ++ti)
	{
		for(tj = 0; tj < TURNINGS; ++tj)
		{
			cfree = 1;
			for(di = -TAXI_SPACE+1; di < TAXI_SPACE; ++di)  // check turnings only
			{
				for(dj = -TAXI_SPACE+1; dj < TAXI_SPACE; ++dj)
				{
					ii = (ti + di)*10;
					jj = (tj + dj)*10;
					if(ii >= 0 && ii < MAP_SIZE && jj >= 0 && jj < MAP_SIZE)
					{
						if(map[ii][jj] > 0)
						{
							printf("taken [%d][%d]\n", ii, jj);
							cfree = 0;
							//break;
						}
					}
				}
			}
			if(cfree)  // take some free spot
			{
				*x = tj*10;
				*y = ti*10;
				++free_places;
				//break;
			}
		}
	}
	
	return free_places;
}

int make_socket(int domain, int type)
{
	int sock;
	sock = socket(domain, type, 0);
	if (sock < 0)
		ERR("socket");

	return sock;
}

void close_client_no_room(int clientfd)
{
	char *msg = "Sorry but there is no room for new player\n";
	write(clientfd, msg, strlen(msg));
	pthread_exit(NULL);
}

void put_player_on_map(int **map, int x, int y, int id, int *sign_under_taxi)
{
	*sign_under_taxi = map[x][y];
	map[x][y] = id;
}




// timer thread for rewriting map to client
void *map_timer_func(void *arg)
{
	int t, done = 0;
	timer_arg targ;
	
	memcpy(&targ, arg, sizeof(targ));
	
	while(!done)
	{
		if(!work)
			pthread_exit(NULL);
			
		for(t = REWRITE_TIME;t > 0;t = sleep(t));
		print_client(targ.clientfd, targ.map, *(targ.cash));
	}
	
	return NULL;
}

void set_map_timer(pthread_t *timer_p, int clientfd, int **map, int *cash, timer_arg *arg)
{
	arg->clientfd = clientfd;
	arg->map = map;
	arg->cash = cash;
	
	if (pthread_create(timer_p, NULL, map_timer_func, (void *)arg) != 0)
			ERR("pthread_create");
}

// result in nx, ny
void calculate_new_position(int x, int y, int *dx, int *dy, int *nx, int *ny)
{
	// provisional new position
	int cnx = x + *dx;
	int cny = y + *dy;
	int ndx = *dx;
	int ndy = *dy;
	
	// if going into map border - change direction
	if(cny < 0)  // left border
	{
		ndy = 0;
		ndx = -1 + 2*(rand()%2);  
		if(x == 0) ndx = 1;  // left top corner going left
		if(x == MAP_SIZE - 1) ndx = -1;  // left bottom corner going left
	}
	else if(cnx >= MAP_SIZE)  // bottom border
	{
		ndx = 0;
		ndy = -1 + 2*(rand()%2);
		if(y == 0) ndy = 1;  // left bottom corner going down
		if(y == MAP_SIZE - 1) ndy = -1;  // right bottom corner going down
	}
	else if(cny >= MAP_SIZE)  // top border
	{
		ndy = 0;
		ndx = -1 + 2*(rand()%2);
		if(x == 0) ndx = 1;  // right top corner going right
		if(x == MAP_SIZE - 1) ndx = -1;  // right bottom corner going right
	}
	else if(cnx < 0)  // right border
	{
		ndx = 0;
		ndy = -1 + 2*(rand()%2);
		if(y == 0) ndy = 1;  // left top corner going yp
		if(y == MAP_SIZE - 1) ndy = -1;  // right top corner going up
	}
	
	*nx = x + ndx;
	*ny = y + ndy;
	*dx = ndx;
	*dy = ndy;
}

void update_map(int *x, int *y, int *dx, int *dy, int *dir, int id, int **map, int *sign_under_taxi)
{
	int turning, ndx, ndy, cdir = *dir;
	//srand(time(NULL));	
		
	// check if taxi is on turning and calculate new (dx, dy)
	if(*x % SPEED == 0 && *y % SPEED == 0)
	{
		turning = 1;
		switch(cdir)  // using cdir to prevent *dir change during switch
		{
			case -1:				
					ndx = -1*(*dy);
					ndy = *dx;
					break;
			case 0:
					ndx = *dx;
					ndy = *dy;
					break;
			case 1:
					ndx = *dy;
					ndy = -1*(*dx);
					break;
			default:
					break;  // some error
		}
		*dir = 0;  // reset player direction
	}
	else
	{
		turning = 0;
		ndx = *dx;
		ndy = *dy;
	}
	
	int nx, ny;
	
	calculate_new_position(*x, *y, &ndx, &ndy, &nx, &ny);
	
	map[*x][*y] = *sign_under_taxi;
	map[nx][ny] = id;

	// update taxi position and direction
	*x = nx;
	*y = ny;
	*dx = ndx;
	*dy = ndy;
}

void turn_left(client_thread_arg *arg)
{
	*(arg->dir) = -1;
}

void turn_right(client_thread_arg *arg)
{
	*(arg->dir) = 1;
}

void prompt_user()
{
}




// thread for client input: 'l' and 'p'
void *client_thread_func(void *arg)
{
	int done = 0;
	client_thread_arg c_arg;
	
	memcpy(&c_arg, arg, sizeof(c_arg));
	
	ssize_t size;
	char buffer[CHUNKSIZE];
	
	while(!done)
	{
		if(!work)
			pthread_exit(NULL);
			
		if ((size = TEMP_FAILURE_RETRY(recv(c_arg.clientfd, buffer, CHUNKSIZE, MSG_WAITALL))) == -1)
			ERR("read");
		
		switch(buffer[0])
		{
			case 'l': 
				turn_left(&c_arg);
				break;
			case 'p':
				turn_right(&c_arg);
				break;
			default:
				prompt_user();
		}
	}
	
	return NULL;
}

void set_client_thread(pthread_t *p, int clientfd, client_thread_arg *c_arg, int *dir)
{
	c_arg->clientfd = clientfd;
	c_arg->dir = dir;
	
	if(pthread_create(p, NULL, client_thread_func, (void *)c_arg) != 0)
		ERR("pthread create");
}




// initial functions for first pthreads
void communicate(int clientfd, thread_arg *targ)
{
	int x, y, cash = INITIAL_MONEY;
	int dx = 0, dy = -1;  // current taxi direction (should be random)
	int dir = 0;  // initially taxi does not turn on turnings
	int sign_under_taxi;
	
	// check if there is a room for a new player
	if(check_room(targ->map, &x, &y) == 0)  // x, y should be assigned random
	{
		close_client_no_room(clientfd);
	}
	
	put_player_on_map(targ->map, x, y, targ->id, &sign_under_taxi);
	
	// create thread for map refreshing
	pthread_t timer_p;
	timer_arg t_arg;
	set_map_timer(&timer_p, clientfd, targ->map, &cash, &t_arg);
	
	// create thread for client input
	pthread_t client_p;
	client_thread_arg c_arg;
	set_client_thread(&client_p, clientfd, &c_arg, &dir); 
	
	// move taxi every second
	int t, done = 0;
	while(!done && work)
	{
		for(t = 1; t > 0; t = sleep(t));
		update_map(&x, &y, &dx, &dy, &dir, targ->id, targ->map, &sign_under_taxi);
	}
}

void cleanup(void *arg)
{
	pthread_mutex_unlock((pthread_mutex_t *)arg);
}

void *threadfunc(void *arg)
{
	int clientfd;
	thread_arg targ;

	memcpy(&targ, arg, sizeof(targ));

	while (1)
	{
		pthread_cleanup_push(cleanup, (void *) targ.mutex);
		if (pthread_mutex_lock(targ.mutex) != 0)
			ERR("pthread_mutex_lock");
		(*targ.idlethreads)++;
		while (!*targ.condition && work)
			if (pthread_cond_wait(targ.cond, targ.mutex) != 0)
				ERR("pthread_cond_wait");
		*targ.condition = 0;
		if (!work)
			pthread_exit(NULL);
		(*targ.idlethreads)--;
		clientfd = *targ.socket;
		pthread_cleanup_pop(1);
		communicate(clientfd, &targ);
	}

	return NULL;
}




void init(pthread_t *thread, thread_arg *targ, sem_t *semaphore, pthread_cond_t *cond, pthread_mutex_t *mutex, 
	int *idlethreads, int *socket, int *condition, int **map)
{
	int i;

	if (sem_init(semaphore, 0, FS_NUM) != 0)
		ERR("sem_init");

	for (i = 0; i < THREAD_NUM; i++)
	{
		targ[i].id = i + 1;
		targ[i].cond = cond;
		targ[i].mutex = mutex;
		targ[i].semaphore = semaphore;
		targ[i].idlethreads = idlethreads;
		targ[i].socket = socket;
		targ[i].condition = condition;
		targ[i].map = map;
		
		if (pthread_create(&thread[i], NULL, threadfunc, (void *)&targ[i]) != 0)
			ERR("pthread_create");
	}
}

int bind_tcp_socket(uint16_t port)
{
	struct sockaddr_in addr;
	int socketfd, t=1;

	socketfd = make_socket(PF_INET, SOCK_STREAM);
	memset(&addr, 0x00, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t)))
		ERR("setsockopt");
	if (bind(socketfd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		ERR("bind");
	if (listen(socketfd, BACKLOG) < 0)
		ERR("listen");

	return socketfd;
}

int add_new_client(int sfd, int **map)
{
	int nfd;
	
	if ((nfd = TEMP_FAILURE_RETRY(accept(sfd, NULL, NULL))) < 0)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return -1;
		ERR("accept");
	}
	
	return nfd;
}

void dowork(int socket, pthread_t *thread, thread_arg *targ, pthread_cond_t *cond, pthread_mutex_t *mutex, 
	int *idlethreads, int *cfd, sigset_t *oldmask, int *condition, int **map)
{
	int clientfd;
	fd_set base_rfds, rfds;
	FD_ZERO(&base_rfds);
	FD_SET(socket, &base_rfds);

	while (work)
	{
		rfds = base_rfds;
		if (pselect(socket + 1, &rfds, NULL, NULL, NULL, oldmask) > 0)
		{
			if ((clientfd = add_new_client(socket, map)) == -1)
				continue;
			if (pthread_mutex_lock(mutex) != 0)
				ERR("pthread_mutex_lock");
			if (*idlethreads == 0)
			{
				if (TEMP_FAILURE_RETRY(close(clientfd)) == -1)
					ERR("close");
				if (pthread_mutex_unlock(mutex) != 0)
					ERR("pthread_mutex_unlock");
			}
			else
			{
				*cfd = clientfd;
				if (pthread_mutex_unlock(mutex) != 0)
					ERR("pthread_mutex_unlock");
				*condition = 1;
				if (pthread_cond_signal(cond) != 0)
					ERR("pthread_cond_signal");
			}
		}
		else
		{
			if (EINTR == errno)
				continue;
			ERR("pselect");
		}
	}
}

void pcleanup(sem_t *semaphore, pthread_mutex_t *mutex, pthread_cond_t *cond)
{
	if (sem_destroy(semaphore) != 0)
		ERR("sem_destroy");
	if (pthread_mutex_destroy(mutex) != 0)
		ERR("pthread_mutex_destroy");
	if (pthread_cond_destroy(cond) != 0)
		ERR("pthread_cond_destroy");
}

void fill_map(int **map)
{
	int i, j;
	
	for(i = 0; i < MAP_SIZE; ++i)
	{
		for(j = 0; j < MAP_SIZE; ++j)
		{
			if(i % SPEED == 0 && j % SPEED == 0)
			{
				map[i][j] = 0;
			}
			else
			{
				map[i][j] = 0;
			}		
		}
	}
}

int main(int argc, char **argv)
{
	int i, condition = 0, socket, new_flags, cfd, idlethreads = 0;
	pthread_t thread[THREAD_NUM];
	thread_arg targ[THREAD_NUM];
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	sigset_t mask, oldmask;
	sem_t semaphore;

	// check parameters
	if (argc!=2)
		usage(argv[0]);

	sethandler(SIG_IGN, SIGPIPE);
	sethandler(siginthandler, SIGINT);
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, &oldmask);
	
	socket = bind_tcp_socket(atoi(argv[1]));
	new_flags = fcntl(socket, F_GETFL) | O_NONBLOCK;
	if (fcntl(socket, F_SETFL, new_flags) == -1)
		ERR("fcntl");
		
	// create and fill map
	int **map;
	map = (int**)malloc(sizeof(int*)*MAP_SIZE);
	for(i = 0; i < MAP_SIZE; ++i)
	{
		//map[i] = (int*)malloc(sizeof(int)*MAP_SIZE);
		map[i] = (int*)calloc(MAP_SIZE, sizeof(int));
	}
	
	fill_map(map);
	
	init(thread, targ, &semaphore, &cond, &mutex, &idlethreads, &cfd, &condition, map);
	
	dowork(socket, thread, targ, &cond, &mutex, &idlethreads, &cfd, &oldmask, &condition, map);
	
	if (pthread_cond_broadcast(&cond) != 0)
		ERR("pthread_cond_broadcast");
	for (i = 0; i < THREAD_NUM; i++)
		if (pthread_join(thread[i], NULL) != 0)
			ERR("pthread_join");
	pcleanup(&semaphore, &mutex, &cond);
	
	if (TEMP_FAILURE_RETRY(close(socket)) < 0)
		ERR("close");
		
	// free memory
	for(i = 0; i < MAP_SIZE; ++i)
	{
		free(map[i]);
	}	
	free(map);
	
	return EXIT_SUCCESS;
}
