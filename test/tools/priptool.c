#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "set.h"
#include "unset.h"
#include "show.h"
#include "list.h"
#include "utils.h"

#define IFNAME_MAX 16

/******************* Forward Declaration. ***************************/
static int usage(char *cmd);
static int do_help(int argc, char **argv);
static int do_cmd(const char *argv0, int argc, char **argv);
static int show_version(int argc, char **argv);


/****************** Global Variable. ********************************/
struct cmd
{
	const char *cmd;
	int (*func)(int argc, char **argv);
};

static const struct cmd cmds[] = 
{
	{ "set",	do_set},
	{ "unset", 	do_unset},
	{ "show",	do_show},
	{ "help",	do_help},
	{ "version",show_version},
	{ 0 }
};

int main(int argc, char **argv)
{
	int ret = -1;

	if(argc <= 1)
		return usage(argv[0]);

	ret = do_cmd(argv[1], argc-1, argv+1);
	if(-1 == ret)
	{
		fprintf(stderr, "no match valid command\n");
		return -1;
	}

	return 0;
}

static int do_cmd(const char *argv0, int argc, char **argv)
{
	const struct cmd *c;

	for(c = cmds; c->cmd; c++)
	{
		if(strcmp(argv0, c->cmd) == 0)
			return c->func(argc-1, argv+1);
	}

	return -1;
}

static int do_help(int argc, char **argv)
{
	return usage(*(--argv));
}

static int show_version(int argc, char **argv)
{
	printf("%s\n", VERSION);
	return 0;
}

static int usage(char *cmd)
{
	fprintf(stderr, "Usage %s [Options]\n"
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
			cmd);

	return 0;
}
