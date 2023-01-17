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
	{ "dev", set_dev },
	{ "interval", set_interval },
	{ "rtt", set_rtt },
	{ "rtt_diff", set_rtt_diff },
	{ NULL, NULL }
};

struct pcap_handlers
{
	int sendfd1;
	int sendfd2;
	int nrecvers;
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

static void create_pcap_handlers(void)
{
	int i, ret;
	bpf_u_int32 mask = 0;
	struct monitior mon;
	strcpy(mon.recvinfo[0].name, master_dev);
	strcpy(mon.recvinfo[1].name, slave_dev);
	struct bpf_program fp;
	// char filter_exp[] = "icmp [icmp-echoreply] = 0";
	char errbuf[PCAP_ERRBUF_SIZE];

	g_pcap_h.sendfd1 = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (-1 == g_pcap_h.sendfd1)
	{
		fprintf(stderr, "Create Send Socket Failed.\n");
		exit(EXIT_FAILURE);
	}

	ret = setsockopt(g_pcap_h.sendfd1, SOL_SOCKET, SO_BINDTODEVICE, \
				(const void*)master_dev, strlen(master_dev));
	if (-1 == ret)
	{
		fprintf(stderr, "Bind socket to device %s failed.\n", master_dev);
		exit(EXIT_FAILURE);
	}

	g_pcap_h.sendfd2 = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (-1 == g_pcap_h.sendfd2)
	{
		fprintf(stderr, "Create Send Socket Failed.\n");
		exit(EXIT_FAILURE);
	}

	ret = setsockopt(g_pcap_h.sendfd2, SOL_SOCKET, SO_BINDTODEVICE, \
				(const void*)slave_dev, strlen(slave_dev));
	if (-1 == ret)
	{
		fprintf(stderr, "Bind socket to device %s failed.\n", slave_dev);
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < 2; i++)
	{
		g_pcap_h.recvers[i] = pcap_open_live(mon.recvinfo[i].name, 128, 0, 0, errbuf);
		if (!g_pcap_h.recvers[i])
		{
			fprintf(stderr, "%s\n", errbuf);
			exit(EXIT_FAILURE);
		}
	}

	g_pcap_h.nrecvers = 2;
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

	hash = icmp_hashfn(node->master_ip, node->id, node->seq);

	list_add_tail(&node->hook, &hash_table.hash_list[hash].head);

	pthread_mutex_init(&node->mutex, NULL);

	return node;
}

static struct monitior *get_node(uint32_t ip1, uint16_t id, uint16_t seq)
{
	uint32_t hash;
	struct monitior *node;
	struct list_head *list, *next;

	hash = icmp_hashfn(ip1, id, seq);

	list_for_each_safe(list, next, &hash_table.hash_list[hash].head)
	{
		node = list_entry(list, struct monitior, hook);
		if (!node)
			continue;

		if (node && node->master_ip == ip1  &&
			id == node->id && seq == node->seq)
		{
			return node;
		}
	}

	return NULL;
}

static long make_icmp_request(unsigned char *pkt, struct monitior *mon)
{
	long timest;
	int totlen;
    struct icmphdr *icmp;

    icmp = (struct icmphdr *)pkt;
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.id = mon->id;
    icmp->un.echo.sequence = mon->seq;
    icmp->checksum = 0;

    totlen = sizeof(struct icmphdr) + sizeof(timest) + sizeof(padding);
	timest = timestamp();
    memcpy(pkt + sizeof(struct icmphdr), &timest, sizeof(long));
	memcpy(pkt + sizeof(struct icmphdr) + sizeof(long), padding, sizeof(padding));

    icmp->checksum = cal_chksum((unsigned short *)icmp, totlen);

	return timest;
}

#if 0
static int cmp_double(const void *a, const void *b)
{
	return (*(double*)a > *(double*)b) ? 1 : -1;
}
#endif

static double sub_double(double a, double b)
{
	return (a > b) ? (a-b) : (b-a);
}

static int check_rtt(void *arg)
{
	double rtt;
	int n, m;
	double rtt_diff;
	double rtts[2];
	struct monitior *node;
	struct list_head *list, *next;

	for (int i = 0; i < hash_table.bucket_size; i++)
	{
		if (list_empty(&hash_table.hash_list[i].head))
			continue;

		list_for_each_safe(list, next, &hash_table.hash_list[i].head)
		{
			node = list_entry(list, struct monitior, hook);
			if (!node)
				continue;

			/* wait timeout. */
			if ((timestamp() - node->timesend) < (g_interval * 1000L))
				continue;

			/* calculate rtt. */
			pthread_mutex_lock(&node->mutex);
			for (n = 0; n < 2; n++)
			{
				if (node->recvinfo[n].timerecv == 0)
				{
					red_printf("%s no reply received", node->recvinfo[n].name);
					rtts[n] = 0;
					continue;
				}

				rtt = (node->recvinfo[n].timerecv - node->timesend) / (double)1000;
				if (rtt > g_rtt)
				{
					red_printf("%s rtt %.3fms", node->recvinfo[n].name, rtt);
				}

				rtts[n] = rtt;
			}

			/* calculate rtt diff. */
			for (n = 0; n < 2; n++)
			{
				if (rtts[n] == 0)
					continue;

				for (m = n + 1; m < 2; m++)
				{
					if (rtts[m] == 0)
						continue;

					rtt_diff = sub_double(rtts[n], rtts[m]);

					if (rtt_diff > g_rtt_diff)
					{
						red_printf("%s with %s rtt diff %.3fms",  \
											node->recvinfo[n].name, \
											node->recvinfo[m].name, \
											rtt_diff);
					}
				}
			}

#if 0
			/* get max rtt. */
			for (max = 0, n = 1; n < node->nslave; n++)
			{
				if (rtts[n] > rtts[max])
					max = n;
			}

			/* calculate rtt_diff. */
			for (n = 0; n < node->nslave && n < MAX_NSLAVE; n++)
			{
				if (n == max)
					continue;
				
				rtt_diff = rtts[max] - rtts[n];
				if (rtt_diff > g_rtt_diff)
				{
					red_printf("%s with %s rtt diff %.3fms", \
								node->slaves[max].name, \
								node->slaves[n].name, rtt_diff);
				}
			
#endif

			/* delete node before unlock. */
			list_del(&node->hook);

			pthread_mutex_unlock(&node->mutex);

			free(node);
		}
	}

	return 0;
}

static void pcap_callback(u_char *arg, const struct pcap_pkthdr *pkthdr, const u_char *packet)
{
	int pcap_i;
	size_t totlen;
	long timest, recvtime;
	struct iphdr *ip;
	struct icmphdr *icmp;
	struct monitior *node;

	pcap_i = *(int*)arg;
	totlen = sizeof(struct icmphdr) + sizeof(long) + sizeof(padding);

	if (pkthdr->caplen < sizeof(struct iphdr) + sizeof(struct ethhdr))
	{
		return;
	}

	ip = (struct iphdr *)(packet + sizeof(struct ethhdr));
	if ((pkthdr->caplen - (ip->ihl * 4) - sizeof(struct ethhdr)) != totlen)
	{
		return;
	}

	icmp = (struct icmphdr *)((u_char *)ip + ip->ihl * 4);
	if (icmp->type != ICMP_ECHOREPLY || icmp->code != 0)
	{
		return;
	}

	recvtime = pkthdr->ts.tv_sec * 1000000L + pkthdr->ts.tv_usec;

	node = get_node(ip->saddr, icmp->un.echo.id, icmp->un.echo.sequence);
	if (!node)
		return;

	pthread_mutex_lock(&node->mutex);
	memcpy(&timest, (unsigned char *)icmp + sizeof(struct icmphdr), sizeof(timest));
	if (timest == node->timesend && pcap_i < 2)
	{
		node->recvinfo[pcap_i].timerecv = recvtime;
	}
	pthread_mutex_unlock(&node->mutex);
}

static void* recv_icmp_reply(void *args)
{
	int pcap_i;
	pcap_t *pcap_h;

	pcap_i = *(int*)args;
	pcap_h = g_pcap_h.recvers[pcap_i]; 

	for (;;)
	{
		pcap_loop(pcap_h, 1, pcap_callback, (u_char *)&pcap_i);
	}

	return NULL;
}

static int send_icmp_request(void *arg)
{
	printf("send icmp request\n");
	size_t totlen;
	struct monitior *node;
	struct sockaddr_in addr;
	unsigned char buffer[1024];

	memset(&addr, 0, sizeof(addr));

	totlen = sizeof(struct icmphdr) + sizeof(long) + sizeof(padding);

	node = node_insert(inet_addr(master_ip), inet_addr(slave_ip));
	if (!node)
		return -1;
	
	printf("%s %s\n", master_ip, slave_ip);

	pthread_mutex_lock(&node->mutex);
	node->timesend = make_icmp_request(buffer, node);
	pthread_mutex_unlock(&node->mutex);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = node->master_ip;
	sendto(g_pcap_h.sendfd1, buffer, totlen, 0, (struct sockaddr*)&addr, sizeof(addr));
	perror("master:");

	addr.sin_addr.s_addr = node->slave_ip;
	sendto(g_pcap_h.sendfd2, buffer, totlen, 0, (struct sockaddr*)&addr, sizeof(addr));
	perror("slave:");

	g_curr_seq++;

	return 0;
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

static void set_dev(char *argv)
{
	char *argv1 = NULL;
	char *argv2 = NULL;
	argv1 = strtok(argv, ",");
	if (0 == if_nametoindex(argv1))
	{
		fprintf(stderr, "Device %s not exist.\n", argv1);
		exit(EXIT_FAILURE);
	}

	argv2 = strtok(NULL, ",");
	if (0 == if_nametoindex(argv2))
	{
		fprintf(stderr, "Device %s not exist.\n", argv2);
		exit(EXIT_FAILURE);
	}

	master_dev = argv1;
	slave_dev = argv2;

	printf("set dev over\n");
}

static void set_ip(char *argv)
{
	char *argv1 = NULL;
	char *argv2 = NULL;
	unsigned char buf[sizeof(struct in_addr)];

	argv1 = strtok(argv, ",");
	if (1 != inet_pton(AF_INET, argv1, buf))
	{
		fprintf(stderr, "Invalid ip address : %s\n", argv1);
		exit(EXIT_FAILURE);
	}

	argv2 = strtok(NULL, ",");
	if (1 != inet_pton(AF_INET, argv2, buf))
	{
		fprintf(stderr, "Invalid ip address : %s\n", argv2);
		exit(EXIT_FAILURE);
	}

	master_ip = argv1; 
	slave_ip = argv2;
}

static void set_interval(char *argv)
{
	g_interval = atoi(argv);
}

static void set_rtt(char *argv)
{
	g_rtt = atoi(argv);
}

static void set_rtt_diff(char *argv)
{
	g_rtt_diff = atoi(argv);	
}

static void check_arguments(void)
{
	if (g_rtt < 0 || g_rtt_diff < 0 || g_interval < 0)
	{
		fprintf(stderr, "\"rtt\" \"rtt_diff\" \"interval\" must great than 0\n");
		exit(EXIT_FAILURE);
	}

	if (g_rtt > g_interval * 1000L || g_rtt_diff > g_interval * 1000L)
	{
		fprintf(stderr, "\"rtt\" or \"rtt_diff\" must less than \"interval\"\n");
		exit(EXIT_FAILURE);
	}

	if (!master_ip && !slave_ip)
	{
		fprintf(stderr, "using option \"ip\" to set ip address.\n");
		exit(EXIT_FAILURE);
	}
}

static void parse_argument(int argc, char **argv)
{
	char *opt, *arg;
	const struct cmd *c;

	while ((opt = get_argument(argc, argv)))
	{
		c = match_cmd(opt);
		if (!c)
		{
			fprintf(stderr, "Invalid Option \"%s\".\n", opt);
			exit(EXIT_FAILURE);
		}

		if (!(arg = get_argument(argc, argv)))
		{
			fprintf(stderr, "Option \"%s\" need an argument.\n", opt);
			exit(EXIT_FAILURE);
		}

		c->func(arg);
	}
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

	printf("master_ip = %s, slave_ip = %s\n", master_ip, slave_ip);
	printf("master_dev = %s, slave_dev = %s\n", master_dev, slave_dev);
	
	/* create send socket and receive pcap handler. */
	create_pcap_handlers();

	/* create timer. */
	init_timer_list();
	create_timer(get_timer_fd(), g_interval, send_icmp_request, NULL);
	create_timer(get_timer_fd(), 1 , check_rtt, NULL);

	/* receive packet thread, one interface one thread. */
	for (i = 0; i < 2; i++)
	{
//		pthread_create(&thread_t, NULL, recv_icmp_reply, (void*)&recv_index[i]);
//		pthread_detach(thread_t);
	}

	/* start timer. */
	timer_loop();

	return 0;
}
