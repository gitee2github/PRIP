#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "show.h"
#include "utils.h"

static void show_alarm(void);
static void show_timeout(void);
static void show_config(void);
static void show_state(void);



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
