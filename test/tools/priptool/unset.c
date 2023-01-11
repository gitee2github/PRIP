#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unset.h"
#include "utils.h"

static int unset_prip(void)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "echo \"\" > %s", PATH_PRIP_CONFIG);
	exec_cmd(cmd);

	snprintf(cmd, sizeof(cmd), "echo 0 > %s", PATH_PRIP_SET);
	exec_cmd(cmd);

	return 0;
}


int do_unset(int argc, char **argv)
{
	unset_prip();
	return 0;
}
