#ifndef _TIMER_H
#define _TIMER_H

#define MAX_TIMER 100

typedef int (* time_handler)(void*); 

typedef struct tag_timer
{
	struct list_head hook;
	int timer_id;
	int time_out;
	int time;
	time_handler handler;
	void *data;

}TIMER;

void init_timer_list();
int search_timer(int id);
int del_timer(int id);
int create_timer(int id, int time_out, time_handler handler, void *data);
void timer_loop();
int get_timer_fd(void);

#endif
