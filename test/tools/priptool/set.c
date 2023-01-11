#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "set.h"
#include "utils.h"

static int set_dev(char *devname, char *value, int mask);
static int set_config(char *devname, char *devname2, int mask);
static int set_alarm(int value);
static int set_timeout(int value);

static int set_dev(char *devname, char *value, int mask)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "ifconfig %s %s/%d", devname, value, mask);
	printf("%s\n", cmd);
	exec_cmd(cmd);

	return 0;
}


static int set_config(char *ip1, char *ip2, int mask)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "echo \"%s %s %d\" > %s", ip1, ip2, mask, PATH_PRIP_CONFIG);
	exec_cmd(cmd);

	snprintf(cmd, sizeof(cmd), "echo 1 > %s", PATH_PRIP_SET);
	exec_cmd(cmd);

	return 0;
}

static int set_alarm(int value)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "echo %d > %s", value, PATH_PRIP_ALARM);
	exec_cmd(cmd);

	return 0;
}

static int set_timeout(int value)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "echo %d > %s", value, PATH_PRIP_TIMEOUT);
	exec_cmd(cmd);

	return 0;
}

int do_set(int argc, char **argv)
{
	if(argc != 2 && argc != 4)
		return -1;

	if(strcmp(argv[0], "dev") == 0)
	{
		if(argc != 4)
			return -1;
			
		set_dev(argv[1], argv[2], atoi(argv[3]));
		return 0;
	}

	if(strcmp(argv[0], "prip") == 0) 
	{
		if(argc != 4)
			return -1;

		set_config(argv[1], argv[2], atoi(argv[3]));
		return 0;
	}

	if(strcmp(argv[0], "alarm") == 0)
	{
		if(argc != 2)
			return -1;
		
		set_alarm(atoi(argv[1]));
		return 0;
	}

	if(strcmp(argv[0], "timeout") == 0)
	{
		if(argc != 2)
			return -1;
		
		set_timeout(atoi(argv[1]));
		return 0;
	}

	return 0;
}
