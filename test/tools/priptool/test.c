#include <stdio.h>
#include "list.h"
#include "timer.h"

static void print(void)
{
	printf("i am timer handler\n");
	return;
}

int main() 
{
	int ret;
	int time_out = 1;
	init_timer_list();
	ret = create_timer(time_out, print, NULL);
	printf("%d\n", ret);

	timer_loop();
}
