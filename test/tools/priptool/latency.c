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
