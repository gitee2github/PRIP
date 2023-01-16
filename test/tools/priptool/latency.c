#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include <net/if.h>
#include <linux/ip.h>
#include <linux/if.h>
#include <linux/icmp.h>
#include <linux/if_ether.h>

#include <pthread.h>
#include <pcap.h>

#include "list.h"
#include "jhash.h"
#include "timer.h"
#include "latency.h"
#include "utils.h"


static void set_ip(char *argv);
static void set_dev(char *argv);
static void set_interval(char *argv);
static void set_rtt(char *argv);
static void set_rtt_diff(char *argv);

static const struct cmd
{
	const char *cmd;
	void (*func)(char *argv);
} cmds[] = {
	{ "ip", set_ip },
	{ "dev", set_dev}
	{ "interval", set_interval },
	{ "rtt", set_rtt },
	{ "rtt_diff", set_rtt_diff },
	{ NULL, NULL }
};

struct pcap_handlers
{
	int sendfd1;
	int sendfd2;
	pcap_t *recvers[2];	
};

struct recv_info
{
	long timerecv;
	char name[IFNAMSIZ];
};

struct monitior
{
	pid_t id;
	uint16_t seq;
	uint32_t master_ip;
	uint32_t slave_ip;
	long timesend;
	struct recv_info recvinfo[2];
	pthread_mutex_t mutex;
	struct list_head hook;
};

struct hash_list
{
	struct list_head head;
};

struct hashtable
{
	int bucket_size;
	struct hash_list *hash_list;
};

/* global variable. */
static char *master_dev = NULL;
static char *slave_dev = NULL;
static char *master_ip = NULL;
static char *slave_ip = NULL;
static int g_interval = 1;
static long g_rtt = 1;
static long g_rtt_diff = 1;
static uint16_t g_curr_seq = 0;
static struct pcap_handlers g_pcap_h; 
static struct hashtable hash_table;
static char padding[] = "priptool";

static long timestamp(void)
{
	long timestamp;
	struct timeval tv;
	
	gettimeofday(&tv, NULL);

	timestamp = tv.tv_sec * 1000000L + tv.tv_usec; 

	return timestamp;
}

static unsigned short cal_chksum(unsigned short *addr, int len)
{
    int sum=0;
    int nleft=len;
    unsigned short *w=addr;
    unsigned short answer=0;

    while(nleft > 1)
    {
        sum+=*w++;
        nleft-=2;
    }

    if (nleft == 1)
    {
        *(unsigned char *)(&answer)=*(unsigned char *)w;
        sum+=answer;
    }

    sum =  (sum>>16) + (sum&0xffff);
    sum += (sum>>16);
    answer = ~sum;
    return answer;
}

static void red_printf(const char *__format, ...)
{
	char *start = "\033[1;31m";
	char *end = "\033[0m";
	char format[4096];
	va_list args;

	snprintf(format, sizeof(format), "%s%s%s\n", \
			 start, __format, end);

	va_start(args, __format);
	vprintf(format, args);
	va_end(args);
}



static void init_hashtable(void)
{
	int i;

	hash_table.hash_list = malloc(sizeof(struct hash_list) * HASHSZ);
	if (!hash_table.hash_list)
	{
		fprintf(stderr, "Alloc hash table failed : %s\n", \
				strerror(errno));
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < HASHSZ; i++)
	{
		INIT_LIST_HEAD(&hash_table.hash_list[i].head);
	}

	hash_table.bucket_size = HASHSZ;
}

static struct monitior *node_insert(uint32_t ipaddr1, uint32_t ipaddr2)
{
	uint32_t hash;
	struct monitior *node;

	node = malloc(sizeof(*node));
	if (!node)
		return NULL;

	memset(node, 0, sizeof(*node));
	node->master_ip = ipaddr1;
	node->slave_ip = ipaddr2;
	node->id  = getpid();
	node->seq = g_curr_seq;
	node->nslave = 2;

	hash = icmp_hashfn(ipaddr1, ipaddr2, node->id, node->seq);

	list_add_tail(&node->hook, &hash_table.hash_list[hash].head);

	pthread_mutex_init(&node->mutex, NULL);

	return node;
}

static struct monitior *get_node(uint32_t ip1, uint32_t ip2, uint16_t id, uint16_t seq)
{
	uint32_t hash;
	struct monitior *node;
	struct list_head *list, *next;

	hash = icmp_hashfn(ip1, ip2, id, seq);

	list_for_each_safe(list, next, &hash_table.hash_list[hash].head)
	{
		node = list_entry(list, struct monitior, hook);
		if (!node)
			continue;

		if (node && node->master_ip == ip1 && node->slave_ip == ip2 &&
			id == node->id && seq == node->seq)
		{
			return node;
		}
	}

	return NULL;
}


static const struct cmd *match_cmd(char *opt)
{
	const struct cmd *c;

	for (c = cmds; c->cmd; c++)
	{
		if (opt && strcmp(c->cmd, opt) == 0)
			return c;
	}

	return NULL;
}



int do_latency(int argc, char **argv)
{
	int i;
	pthread_t thread_t;
	int recv_index[2] = {0, 1};

	/* parse arguments. */
	parse_argument(argc, argv);

	/* check argument. */
	check_arguments();

	/* hash table. */
	init_hashtable();

	/* create send socket and receive pcap handler. */
	create_pcap_handlers();

	/* create timer. */
	init_timer_list();
	create_timer(get_timer_fd(), g_interval, send_icmp_request, NULL);
	create_timer(get_timer_fd(), 1 , check_rtt, NULL);

	/* receive packet thread, one interface one thread. */
	for (i = 0; i < 2; i++)
	{
		pthread_create(&thread_t, NULL, recv_icmp_reply, (void*)&recv_index[i]);
		pthread_detach(thread_t);
	}

	/* start timer. */
	timer_loop();

	return 0;
}
