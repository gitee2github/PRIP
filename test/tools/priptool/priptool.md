# 							priptool工具设计方案

## 1 main函数 priptool.c

1.1 实现priptool的用法描述Usage,具体如下：

```
            "[Options] : \n"
                    "\tset\t[dev]\t[devname xxx.xxx.xxx.xxx mask]\n"
                    "\tset\t[prip] [devname1 devname2 mask]\n"
                    "\tset\t[alarm] [value]\n"
                    "\tset\t[timeout] [value]\n"
                    "\tunset\t[prip]\n"
                    "\tshow\t[alarm]\n"
                    "\tshow\t[timeout]\n"
                    "\tshow\t[config]\n"
                    "\tshow\t[state]\n"
            "[Examples] :\n"
                    "\tset\t[dev]\t[eth1 192.168.122.80 24]\n"
                    "\tset\t[prip]\t[192.168.10.10 192.168.11.10 24]\n"
                    "\tset\t[alarm]\t[100]\n"
                    "\tshow\t[alarm]\n",

```

1.2  实现do_cmd函数，通过匹配对应的操作集（set unset show help version），调用相应的回调函数，操作集合如下：

```
static const struct cmd cmds[] =
{
    { "set",    do_set},
    { "unset",  do_unset},
    { "show",   do_show},
    { "help",   do_help},
    { "version",show_version},
    { 0 }
};

```



## 2 通用函数 utils.c utils.h

2.1 utils.h中定义各个prip相关文件的路径，声明通用函数，如下：

```
#define PATH_PRIP_CONFIG "/proc/prip/prip_config"
#define PATH_PRIP_ALARM "/proc/prip/prip_alarm"
#define PATH_PRIP_TIMEOUT "/proc/prip/prip_cache_timeout"
#define PATH_PRIP_STATE "/proc/prip/prip_state"
#define PATH_PRIP_SET "/proc/sys/net/ipv4/prip_set"

```

2.2 utils.c 中对通用函数进行实现，主要有如下函数：

```
void exec_config_cmd(char *cmd, char *str, int flag)
```

​	在程序中执行cmd指令对文件进行查看，并将查看结果打印输出，关键函数popen



```
void exec_cmd(char *cmd)
```

​	在程序中执行cmd指令，对文件进行修改或者设置网口ip地址



```
int check_ip(char *str)
```

​	检查ip地址的合法性



## 3 开启prip，设置prip参数 set.c set.h

主函数

```
int do_set(int argc, char **argv)
```

​	根据地一个参数，判断是设置ip地址还是设置prip

​	如果是设置prip，再根据第二个参数判断是设置prip的配置信息，或者告警阈值，或者超时时间



```
//设置网口ip地址和掩码
static int set_dev(char *devname, char *value, int mask)
```



```
//开启prip功能，并设置主从链路的ip地址对
static int set_config(char *ip1, char *ip2, int mask)
```



```
//	设置告警阈值的大小，默认为500，如果主链路或者从链路连续500次没有收到包，则进行报警
static int set_alarm(int value)
```



```
//设置超时时间，默认是3秒
static int set_timeout(int value)
```



## 4 关闭prip

主函数

```
int do_unset(int argc, char **argv)
```

通过exec_cmd函数将/proc/sys/net/ipv4/prip_set置为0，将/proc/prip/prip_config 置为空

```
static int unset_prip(void)
```



## 5 prip的显示功能

主函数

```
int do_show(int argc, char **argv)
```

根据第一个参数，确认出需要显示输出的配置项，包括：alarm timeout config state，并调用相应的函数实现

```
//输出告警阈值信息
static void show_alarm(void)
{
    char cmd[1024];
    char str[16];
    strcpy(str, "PRIP_ALARM"); 
    snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_ALARM);
    exec_config_cmd(cmd, str, 1);
    return;
}
```



```
//输出超时时间信息
static void show_timeout(void)
{
    char cmd[1024];
    char str[16];
    strcpy(str, "PRIP_TIMEOUT");
    snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_TIMEOUT);
    exec_config_cmd(cmd, str, 1);
    return;
}
```





```
//输出配置信息
static void show_config(void)
{
    char cmd[1024];
    char str[16];
    strcpy(str, "PRIP_CONFIG");
    snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_CONFIG);
    exec_config_cmd(cmd, str, 1);
    return;
}
```





```
//输出当前prip状态信息，包括PRIP_ON/OFF local_ip PRIP_MASTER_IP PRIP_SLAVE_IP PRIP_REFCNT refcnt peer_master_ip peer_slave_ip master_state slave_state master_send slave_send master_recv slave_recv
static void show_state(void)
{
    char cmd[1024];
    char str[16];
    strcpy(str, "PRIP_STATE");
    snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_STATE);
    exec_config_cmd(cmd, str, 0);
    return;
}
```



## 6 链路延迟告警功能

创建两个定时器，一个用于定期向主从链路发送icmp请求报文，记录发送报文的时间戳，另一个定时器定期检查每条链路收到icmp响应报文的延迟时间，并且比较他们之间的延迟差，如果超出入参指定阈值，则进行告警

创建两个线程，用于分别在两条链路上接收icmp响应报文，记录收到报文的时间戳。

源码文件： latency.c latency.h timer.c timer.h jhash.c jhash.h 



### 6.1 发送icmp请求报文

创建原始套接字，组织icmp请求报文，计算校验和，通过sendto发送给指定ip地址

-  创建socket文件描述符

  ```
  	g_pcap_h.sendfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  	if (-1 == g_pcap_h.sendfd)
  	{
  		fprintf(stderr, "Create Send Socket Failed.\n");
  		exit(EXIT_FAILURE);
  	}
  
  	ret = setsockopt(g_pcap_h.sendfd, SOL_SOCKET, SO_BINDTODEVICE, \
  				(const void*)bondname, strlen(bondname));
  ```

- 生成icmp请求报文

```
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
```



- 发送icmp请求报文

```
static int send_icmp_request(void *arg)
{
	size_t totlen;
	struct monitior *node;
	struct sockaddr_in addr;
	unsigned char buffer[1024];

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;

	totlen = sizeof(struct icmphdr) + sizeof(long) + sizeof(padding);

	node = node_insert(inet_addr(g_ipaddr));
	if (!node)
		return -1;

	addr.sin_addr.s_addr = node->ipaddr;

	pthread_mutex_lock(&node->mutex);
	node->timesend = make_icmp_request(buffer, node);
	pthread_mutex_unlock(&node->mutex);

	sendto(g_pcap_h.sendfd, buffer, totlen, 0, (struct sockaddr*)&addr, sizeof(addr));

	g_curr_seq++;

	return 0;
}
```

### 6.2 接收icmp响应报文

icmp响应报文的接收是通过libpcap中的方法进行接收的，直接调用该方法

```
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
```

解析icmp响应报文

```
ip = (struct iphdr *)(packet + sizeof(struct ethhdr));
```

```
icmp = (struct icmphdr *)((u_char *)ip + ip->ihl * 4);
```

通过icmp首部判断是否为回应报文

```
if (icmp->type != ICMP_ECHOREPLY || icmp->code != 0)
	{
		return;
	}
```

通过ip首部匹配到对应的节点

```
node = get_node(ip->saddr, icmp->un.echo.id, icmp->un.echo.sequence);
```

计算收包时间

```
recvtime = pkthdr->ts.tv_sec * 1000000L + pkthdr->ts.tv_usec;
```

### 

### 6.3 判断链路是否延迟

遍历哈希桶-哈希表，对每一个node进行延迟判断

```
for (int i = 0; i < hash_table.bucket_size; i++)
{
	list_for_each_safe(list, next, &hash_table.hash_list[i].head)
	{
		node = list_entry(list, struct monitior, hook);
		。。。
	}
}
```

如果节点收发报文延迟时间小于定时器timer的执行间隔interval，才进行延迟差计算

```
if ((timestamp() - node->timesend) < (g_interval * 1000L))
	continue;
```

分别计算主从链路的延迟rtt（收包时间-发包时间），如果延迟大于参数指定的值g_rtt，则进行告警

```
rtt = (node->slaves[n].timerecv - node->timesend) / (double)1000;
if (rtt > g_rtt)
{
					red_printf("%s rtt %.3fms", node->slaves[n].name, rtt);
}
```

比较主从链路之间的延迟差，如果查处参数指定值，进行告警

```
rtt_diff = sub_double(rtts[n], rtts[m]);
if (rtt_diff > g_rtt_diff)
{
	red_printf("%s with %s rtt diff %.3fms",  \
						node->slaves[n].name, \
						node->slaves[m].name, \
						rtt_diff);
}
```

## 7 定时器的设计

### 7.1 创建定时器

```
int create_timer(int id, int time_out, time_handler handler, void *data)
```



定时器结构体

```
typedef struct tag_timer
{
    struct list_head hook;
    int timer_id;
    int time_out;
    int time;
    time_handler handler;
    void *data;

}TIMER;
```

根据get_timer_fd获取一个空的定时器，设置该定时器的句柄handle，timer_id ，timer_out(定时器执行句柄的时间间隔)

如果time_out 小于0 ，则置为1,否则按照指定时间间隔进行设定

将组装好的定时器结构体加入定时器链表中

```
    pstTimer = malloc(sizeof(TIMER));
    if (NULL == pstTimer)
    {
        return -1;
    }

    memset(pstTimer, 0, sizeof(TIMER));
    pstTimer->timer_id = id;
    pstTimer->time = 0;
    pstTimer->handler = handler;

    pstTimer->time_out = time_out < 0 ? 1: time_out;

    pstTimer->data = data;

    pthread_mutex_lock(&timer_mutex);

    list_add(&pstTimer->hook, &g_timer_list);

    pthread_mutex_unlock(&timer_mutex);

```

### 7.2 删除定时器

```
int del_timer(int id)
```



遍历定时器链表，找到指定time_id的定时器

从链表中删除该定时器，更新“可用定时器”数组

释放定时器结构

```
    list_for_each_safe(pstlist, pstNextlist, &g_timer_list)
    {
        pstTimer = list_entry(pstlist, TIMER, hook);
        if (NULL != pstTimer)
        {
            if (pstTimer->timer_id == id)
            {
                list_del(&pstTimer->hook);
                put_timer_fd(pstTimer->timer_id);
                free(pstTimer);
                goto unlock;
            }
        }
    }
```

### 7.3 定期执行定时器的操作句柄

通过select指定循环遍历定时器链表的时间间隔

每个定时器在经过n次遍历后，会触发自己的定时时间，然后执行定时器中的操作句柄

```
void timer_loop()
{
‘’‘’‘’
    while(1)
    {
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000000;

        select(0, NULL, NULL, NULL, &timeout);
        pthread_mutex_lock(&timer_mutex);

        list_for_each_safe(pstlist, pstNextlist, &g_timer_list)
        {
            pstTimer = list_entry(pstlist, TIMER, hook);
            if (NULL != pstTimer)
            {
                pstTimer->time++;
                if (pstTimer->time >= pstTimer->time_out)
                {
                    pstTimer->time = 0;
                    pstTimer->handler(pstTimer->data);
                }
            }
        }

        pthread_mutex_unlock(&timer_mutex);
    }
}
```

### 7.4 获取一个可用的定时器

从定时器id数组中找到第一个为0的元素，设置为0，返回该元素的下标，即为获取到的定时器id

```
int get_timer_fd(void)
{
    int i = 0;
    int timerfd = -1;

    for (i = 0; i < MAX_TIMER; i++)
    {
        if (0 == g_timer_fd[i])
        {
            g_timer_fd[i] = 1;
            timerfd = i;
            break;
        }
    }

    return timerfd;
}

```

