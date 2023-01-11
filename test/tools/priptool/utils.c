#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "utils.h"

static const char *names[PRIP_FILE_MAX] = {
	[PRIP_CONFIG] = "/proc/prip/prip_config",
	[PRIP_ALARM] = "/proc/prip/prip_alarm",
	[PRIP_TIMEOUT] = "/proc/prip/prip_cache_timeout",
	[PRIP_STATE] = "/proc/prip/prip_state",
	[PRIP_SET] = "/proc/sys/net/ipv4/prip_set",
};

void exec_config_cmd(char *cmd, char *str, int flag)
{
    FILE *fp = NULL;
	char buf[1024] = {0};

    fp = popen(cmd, "r");
    if (NULL == fp)
    {
        perror("popen");
        exit(-1);
    }

	printf("%s:\n",str);
	while(NULL != fgets(buf, sizeof(buf), fp))
	{
		printf("%s", buf);
		if(flag == 1)
			break;
	}

    pclose(fp);
}

void exec_cmd(char *cmd)
{
	FILE *fp = NULL;

    fp = popen(cmd, "r");
    if (NULL == fp)
    {
        perror("popen");
        exit(-1);
    }

	pclose(fp);
}

int check_ip(char *str)
{
	char ip[512];
	in_addr_t addr;
	char *identifer;

	if (!str)
	{
		return 0;
	}

	strncpy(ip, str, sizeof(ip));

	identifer = strrchr(ip, '/');
	if (identifer)
	{
		*identifer = '\0';
	}

	if (1 != inet_pton(AF_INET, ip, &addr))
	{
		return 0;
	}

	return 1;
}

int strrcmp(char *s1, char *s2, size_t len)
{
	int s2len = 0;

	if (!s1 || !s2)
	{
		return 0;
	}

	s2len  = strlen(s2);
	s1 = s1 + strlen(s1) - 1;
	s2 = s2 + s2len - 1;

	if (len > s2len)
	{
		return -1;
	}

	while(len--)
	{
		if (*s1-- != *s2--)
		{
			return -1;
		}
	}

	return 0;
}

