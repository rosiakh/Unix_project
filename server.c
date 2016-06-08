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
#define THREAD_NUM 20  // it's < 100 so max two digits are needed
#define TURNINGS 5
#define SPEED 4
#define MAP_SIZE TURNINGS*SPEED + 1

#define INITIAL_MONEY 100
#define COLLISION_MONEY 30
#define ORDER_MONEY 20

#define TAXI_SPACE 3
#define REWRITE_TIME 1
#define MAX_ORDERS 5
#define INITIAL_ORDER_ID 100  // order ids have are > 99 while taxi ids < 99

#define COLLISION_TIME 3
#define ORDER_TIME 3

#define MIN_NEW_ORDER_TIME 5
#define MAX_NEW_ORDER_TIME 10

#define ERRSTRING "No such file or directory\n"

volatile sig_atomic_t work = 1;

typedef struct
{
	int order_id;  // to distinguish orders with the same source & destination
	int x_from;
	int y_from;
	int x_to;
	int y_to;
	int taxi_id;  // meaningful only if taken and active are true - thread id of taxi thread (not pthread_t!)
	int taken;  // true if some taxi has already taken this order
	int active;  // true if this order is active (taken/available); false means this data is meaningless
} taxi_order;

typedef struct
{
	int id;
	int *idlethreads;
	int *socket;
	int *condition;
	pthread_cond_t *cond;
	pthread_mutex_t *mutex;
	int **map;
	taxi_order *orders;
	sem_t *orders_sems;
} thread_arg;

typedef struct
{
	int clientfd;
	int **map;
	int *cash;
	int taxi_id;
	taxi_order *orders;
	int *dx;
	int *dy;
} timer_arg;

typedef struct
{
	int clientfd;
	int *dir;  // -1 = left, 1 = right, 0 = don't turn on turnings
} client_thread_arg;

typedef struct
{
	taxi_order *orders_tab;
} orders_thread_arg;


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

// returns order_id of taken order or -1 if none
int get_order(int taxi_id, taxi_order *orders)
{
	int i;
	for(i = 0; i < MAX_ORDERS; ++i)
	{
		if(orders[i].active)
		{
			if(orders[i].taxi_id == taxi_id) return orders[i].order_id;
		}
	}
	return -1;
}

void print_client(int clientfd, timer_arg *targ)  // don't forget htons
{
	int i, j;
	char buf[3], c[30], *eol = "\n";
	int cash = *(targ->cash);
	int **ext_map = targ->map;
	taxi_order *orders = targ->orders;
	int order = get_order(targ->taxi_id, orders);
	int *dx = targ->dx, *dy = targ->dy;
	
	// copy map to local memory before sending
	int **map;
	map = (int**)malloc(sizeof(int*)*MAP_SIZE);
	for(i = 0; i < MAP_SIZE; ++i)
	{
		map[i] = (int*)malloc(sizeof(int)*MAP_SIZE);
	}
	
	for(i = 0; i < MAP_SIZE; ++i)
	{
		for(j = 0; j < MAP_SIZE; ++j)
		{
			map[i][j] = ext_map[i][j];
		}
	}
	
	int c1 = INITIAL_ORDER_ID + 'A', c2 = INITIAL_ORDER_ID + 'a';
	for(i = 0; i < MAX_ORDERS; ++i)
	{
		if(orders[i].active)
		{
			//map[orders[i].x_from][orders[i].y_from] = orders[i].order_id*10;
			//map[orders[i].x_to][orders[i].y_to] = orders[i].order_id*10 + 1;
			if(order > -1)
			{
				if(orders[i].order_id == order)
				{
					map[orders[i].x_to][orders[i].y_to] = c2 + i;
				}
			}
			else if(!orders[i].taken)
			{
				map[orders[i].x_from][orders[i].y_from] = c1 + i;
			}
		}
	}
	
	write(clientfd, eol, strlen(eol));
	for(i = 0; i < MAP_SIZE; ++i)
	{
		for(j = 0; j < MAP_SIZE; ++j)
		{
			if(map[i][j] >= INITIAL_ORDER_ID)
			{
				sprintf(buf, "%c", map[i][j] - INITIAL_ORDER_ID);
			}
			else if(map[i][j] == 0)
			{
				if(i % SPEED == 0 && j % SPEED == 0)
				{
					sprintf(buf, "%c", 'x');
				}
				else if(i % SPEED == 0)
				{
					sprintf(buf, "%c", '-');				
				}
				else if(j % SPEED == 0)
				{
					sprintf(buf, "%c", '|');
				}
				else
				{
					sprintf(buf, "%c", ' ');
				}
			}
			else if(map[i][j] == targ->taxi_id)
			{
				sprintf(buf, "%c", 'P');
			}
			else
			{
				sprintf(buf, "%d", map[i][j]);
			}
			if(TEMP_FAILURE_RETRY(write(clientfd, buf, strlen(buf))) == -1)
				ERR("write");
		}
		if(TEMP_FAILURE_RETRY(write(clientfd, eol, strlen(eol))) == -1)
			ERR("write");
	}
	
	// cash
	sprintf(c, "cash: %d\n", cash);
	if(TEMP_FAILURE_RETRY(write(clientfd, c, strlen(c))) == -1)
		ERR("write");
	
	// orders
	//sprintf(c, "orders: %d\n", order);
	//write(clientfd, c, strlen(c));
	
	// direction
	char *dir;
	if(*dx == -1) dir = "up";
	if(*dx == 1) dir = "down";
	if(*dy == -1) dir = "left";
	if(*dy == 1) dir = "right";
	
	sprintf(c, "direction: %s\n", dir);
	if(TEMP_FAILURE_RETRY(write(clientfd, c, strlen(c))) == -1)
		ERR("write");
	
	// free memory
	for(i = 0; i < MAP_SIZE; ++i)
	{
		free(map[i]);
	}
	free(map);
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
					ii = (ti + di)*SPEED;
					jj = (tj + dj)*SPEED;
					if(ii >= 0 && ii < MAP_SIZE && jj >= 0 && jj < MAP_SIZE)
					{
						if(map[ii][jj] > 0 && map[ii][jj] < INITIAL_ORDER_ID)
						{
							//printf("taken [%d][%d]\n", ii, jj);
							cfree = 0;
							//break;
						}
					}
				}
			}
			if(cfree)  // take some free spot
			{
				*x = tj*SPEED;
				*y = ti*SPEED;
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
	if(TEMP_FAILURE_RETRY(write(clientfd, msg, strlen(msg))) == -1)
		ERR("write");
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
	// signals
	//sigset_t mask, oldmask;

	//sethandler(SIG_IGN, SIGPIPE);
	//sethandler(timer_siginthandler, SIGINT);
	//sigemptyset(&mask);
	//sigaddset(&mask, SIGINT);
	
	int t, done = 0;
	timer_arg targ;
	
	memcpy(&targ, arg, sizeof(targ));
	
	while(!done)
	{
		if(!work)
			pthread_exit(NULL);
			
		for(t = REWRITE_TIME;t > 0;t = sleep(t));
		print_client(targ.clientfd, &targ);
	}
	
	return NULL;
}

void set_map_timer(pthread_t *timer_p, int taxi_id, int clientfd, int **map, int *cash, taxi_order *orders, timer_arg *arg,
	int *dx, int *dy)
{
	arg->clientfd = clientfd;
	arg->map = map;
	arg->cash = cash;
	arg->taxi_id = taxi_id;
	arg->orders = orders;
	arg->dx = dx;
	arg->dy = dy;
	
	if (pthread_create(timer_p, NULL, map_timer_func, (void *)arg) != 0)
			ERR("pthread_create");
}

// (x,y) - current position
// (*dx,*dy) - vector of planned translation
// (nx,ny) - result taking into account map borders
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

void turn_left(client_thread_arg *arg)
{
	*(arg->dir) = -1;
}

void turn_right(client_thread_arg *arg)
{
	*(arg->dir) = 1;
}




// thread for client input: 'l' and 'p'
void *client_thread_func(void *arg)
{
	// signals
	//sigset_t mask, oldmask;

	///sethandler(SIG_IGN, SIGPIPE);
	//sethandler(client_siginthandler, SIGINT);
	//sigemptyset(&mask);
	//sigaddset(&mask, SIGINT);
	
	
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
			case 'L':
				turn_left(&c_arg);
				break;
			case 'p':
			case 'P':
				turn_right(&c_arg);
				break;
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


// (nx, ny) - new position that taxi wants to move on
void check_order(int nx, int ny, thread_arg *targ, int *cash)
{
	int i, t;
	taxi_order *orders = targ->orders;
	
	for(i = 0; i < MAX_ORDERS; ++i)
	{
		if(orders[i].active)
		{
			if (TEMP_FAILURE_RETRY(sem_wait(&((targ->orders_sems)[i]))) == -1)
				ERR("sem_wait");
				
			if(orders[i].taken == 0 && orders[i].x_from == nx && orders[i].y_from == ny)  // take available order
			{
				if(get_order(targ->id, orders) < 0)
				{
					orders[i].taken = 1;  // checking and taking order should be atomic operation
					orders[i].taxi_id = targ->id;
					
					if (sem_post(&((targ->orders_sems)[i])) == -1)
						ERR("sem_post");
					
					for(t = ORDER_TIME; t > 0; t = sleep(t));				
				}
				else
				{
					if (sem_post(&((targ->orders_sems)[i])) == -1)
						ERR("sem_post");
				}
			}
			else if(orders[i].taxi_id == targ->id && orders[i].x_to == nx && orders[i].y_to == ny)  // drop client & finish order
			{
				if (sem_post(&((targ->orders_sems)[i])) == -1)
					ERR("sem_post");
				
				orders[i].active = 0;
				(*cash) += ORDER_MONEY;				
				for(t = ORDER_TIME; t > 0; t = sleep(t));
			}
			else
			{
				if (sem_post(&((targ->orders_sems)[i])) == -1)
					ERR("sem_post");
			}					
		}
	}	
}

// nx, ny - position that is going to be taken by taxi
int check_collision(thread_arg *targ, int *x, int *y, int *dx, int *dy, int *ndx, int *ndy, int *nx, int *ny, int *sign_under_taxi, int *cash)
{
	int m = (targ->map)[*nx][*ny], t, col = 0;
	if(m > 0 && m < INITIAL_ORDER_ID)  // must be taxi id so collision
	{
		for(t = COLLISION_TIME; t > 0; t = sleep(t));
		
		(*cash) -= COLLISION_MONEY;
		(*ndx) *= -1;
		(*ndy) *= -1;
		*nx = *x;
		*ny = *y;
		col = 1;
	}
	
	return col;
}

// *x + *ndx = *ny
void move_taxi(thread_arg *targ, int *x, int *y, int *dx, int *dy, int *ndx, int *ndy, int *nx, int *ny, int *sign_under_taxi, int *cash)
{
	if(!check_collision(targ, x, y, dx, dy, ndx, ndy, nx, ny, sign_under_taxi, cash))
	{
		(targ->map)[*x][*y] = *sign_under_taxi;
		(targ->map)[*nx][*ny] = targ->id;
	}
	
	// update taxi position and direction
	*x = *nx;
	*y = *ny;
	*dx = *ndx;
	*dy = *ndy;
}

// actual moving & collisions and orders checking
void update_map(int *x, int *y, int *dx, int *dy, int *dir, thread_arg *targ, int *sign_under_taxi, int *cash)
{
	int ndx, ndy, cdir = *dir;
		
	// check if taxi is on turning and calculate new (dx, dy)
	if(*x % SPEED == 0 && *y % SPEED == 0)
	{
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
			//default:  // some error
		}
		*dir = 0;  // reset player direction
	}
	else
	{
		ndx = *dx;
		ndy = *dy;
	}
	
	int nx, ny;
	
	// check map borders
	calculate_new_position(*x, *y, &ndx, &ndy, &nx, &ny);
	
	// function for moving taxi
	move_taxi(targ, x, y, dx, dy, &ndx, &ndy, &nx, &ny, sign_under_taxi, cash);
		
	// check order after moving
	check_order(*x, *y, targ, cash);
}

void clear_on_disconnect(thread_arg *targ, int x, int y, int sign_under_taxi, pthread_t timer_p, pthread_t client_p)
{
	int i;
	taxi_order *orders = targ->orders;
	for(i = 0; i < MAX_ORDERS; ++i)
	{
		if(orders[i].active)
		{
			if(orders[i].taxi_id == targ->id)
			{
				orders[i].active = 0;
			}
		}
	}
	
	(targ->map)[x][y] = sign_under_taxi;
	//kill(timer_p, SIGINT);
	//kill(client_p, SIGINT);
	
	//puts("before join disconnect");
	//if (pthread_join(timer_p, NULL) != 0)
		//ERR("pthread_join");
	//if (pthread_join(client_p, NULL) != 0)
		//ERR("pthread_join");
	//puts("after join disconnect");
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
	set_map_timer(&timer_p, targ->id, clientfd, targ->map, &cash, targ->orders, &t_arg, &dx, &dy);
	
	// create thread for client input
	pthread_t client_p;
	client_thread_arg c_arg;
	set_client_thread(&client_p, clientfd, &c_arg, &dir); 
	
	// move taxi every second
	int t;
	while(cash > 0 && work)
	{
		for(t = 1; t > 0; t = sleep(t));
		update_map(&x, &y, &dx, &dy, &dir, targ, &sign_under_taxi, &cash);
	}
	
	// clear orders & map on closing communication
	clear_on_disconnect(targ, x, y, sign_under_taxi, timer_p, client_p);
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




void init(pthread_t *thread, thread_arg *targ, pthread_cond_t *cond, pthread_mutex_t *mutex, 
	int *idlethreads, int *socket, int *condition, int **map, taxi_order *orders, sem_t *orders_sems)
{	
	int i;
	for (i = 0; i < THREAD_NUM; i++)
	{
		targ[i].id = i + 1;
		targ[i].cond = cond;
		targ[i].mutex = mutex;
		targ[i].idlethreads = idlethreads;
		targ[i].socket = socket;
		targ[i].condition = condition;
		targ[i].map = map;
		targ[i].orders = orders;
		targ[i].orders_sems = orders_sems;
		
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

void pcleanup(sem_t *orders_sems, pthread_mutex_t *mutex, pthread_cond_t *cond)
{
	int i;
	for(i = 0; i < MAX_ORDERS; ++i)
	{
		if (sem_destroy(&orders_sems[i]) != 0)
			ERR("sem_destroy");
	}
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

void genereate_new_order(taxi_order *orders, int *next_order_id)
{
	int i;
	for(i = 0; i < MAX_ORDERS; ++i)
	{
		if(!orders[i].active)
		{
			orders[i].order_id = *next_order_id;
			orders[i].x_from = SPEED*(rand()%(TURNINGS + 1));
			orders[i].y_from = SPEED*(rand()%(TURNINGS + 1));
			orders[i].x_to = SPEED*(rand()%(TURNINGS + 1));
			orders[i].y_to = SPEED*(rand()%(TURNINGS + 1));
			orders[i].taxi_id = -1;
			orders[i].taken = 0;
			orders[i].active = 1;
			
			(*next_order_id)++;
			//puts("generated order");
			break;
		}
	}
}

void *orders_thread_func(void *arg)
{
	orders_thread_arg oarg;

	memcpy(&oarg, arg, sizeof(oarg));
	taxi_order *ords = oarg.orders_tab;

	int t, done = 0, next_order_id = INITIAL_ORDER_ID;
	
	while(!done)
	{
		if(!work)
			pthread_exit(NULL);
		
		int tm = MIN_NEW_ORDER_TIME + (rand()%(MAX_NEW_ORDER_TIME + 1));
		for(t = tm;t > 0;t = sleep(t));
		
		genereate_new_order(ords, &next_order_id);
	}
	
	return NULL;
}

int main(int argc, char **argv)
{
	int i, condition = 0, socket, new_flags, cfd, idlethreads = 0;
	pthread_t thread[THREAD_NUM];
	thread_arg targ[THREAD_NUM];
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	sigset_t mask, oldmask;
	
	// taxi orders preparation & orders thread creation
	taxi_order orders[MAX_ORDERS];
	sem_t orders_sems[MAX_ORDERS];
	
	for(i = 0; i < MAX_ORDERS; ++i)
	{
		orders[i].active = 0;
		if (sem_init(&orders_sems[i], 0, 1) != 0)
			ERR("sem_init");
	}
	
	pthread_t orders_thread;
	orders_thread_arg oarg;
	oarg.orders_tab = orders;
	if(pthread_create(&orders_thread, NULL, orders_thread_func, (void *)&oarg) != 0)
		ERR("pthread_create");
	
	if (argc!=2)
		usage(argv[0]);

	sethandler(SIG_IGN, SIGPIPE);
	sethandler(siginthandler, SIGINT);
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, &oldmask);
	//pthread_sigmask(SIG_BLOCK, &mask, &oldmask);
	
	socket = bind_tcp_socket(atoi(argv[1]));
	new_flags = fcntl(socket, F_GETFL) | O_NONBLOCK;
	if (fcntl(socket, F_SETFL, new_flags) == -1)
		ERR("fcntl");
		
	// create and fill map
	int **map;
	map = (int**)malloc(sizeof(int*)*MAP_SIZE);
	for(i = 0; i < MAP_SIZE; ++i)
	{
		map[i] = (int*)calloc(MAP_SIZE, sizeof(int));
	}
	
	init(thread, targ, &cond, &mutex, &idlethreads, &cfd, &condition, map, orders, orders_sems);
	
	dowork(socket, thread, targ, &cond, &mutex, &idlethreads, &cfd, &oldmask, &condition, map);
	
	if (pthread_cond_broadcast(&cond) != 0)
		ERR("pthread_cond_broadcast");
		
	//puts("before main join");	
	for (i = 0; i < THREAD_NUM; i++)
		if (pthread_join(thread[i], NULL) != 0)
			ERR("pthread_join");
	//puts("after main join");		
	
	pcleanup(orders_sems, &mutex, &cond);
	
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
