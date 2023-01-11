#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "show.h"
#include "utils.h"

static void show_alarm(void);
static void show_timeout(void);
static void show_config(void);
static void show_state(void);

static void show_alarm(void)
{
	char cmd[1024];
	char str[16];
	strcpy(str, "PRIP_ALARM");
	snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_ALARM);
	exec_config_cmd(cmd, str, 1);
	return;
}

static void show_timeout(void)
{
	char cmd[1024];
	char str[16];
	strcpy(str, "PRIP_TIMEOUT");
	snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_TIMEOUT);
	exec_config_cmd(cmd, str, 1);
	return;
}

static void show_config(void)
{
	char cmd[1024];
	char str[16];
	strcpy(str, "PRIP_CONFIG");
	snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_CONFIG);
	exec_config_cmd(cmd, str, 1);
	return;
}

int do_show(int argc, char **argv)
{
	if(argc < 1)
	{
		printf("error arg for show function\n");
		return -1;
	}

	if(strcmp(argv[0], "alarm") == 0)
	{
		show_alarm();
		return 0;	
	}

	if(strcmp(argv[0], "timeout") == 0)
	{
		show_timeout();
		return 0;	
	}

	if(strcmp(argv[0], "config") == 0)
	{
		show_config();
		return 0;	
	}

	if(strcmp(argv[0], "state") == 0)
	{
		show_state();
		return 0;	
	}

	printf("error arg for show function\n");
	return -1;
}
