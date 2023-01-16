#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/select.h>
#include <pthread.h>

#include "list.h"
#include "timer.h"

static struct list_head g_timer_list;
static pthread_mutex_t timer_mutex = PTHREAD_MUTEX_INITIALIZER;

int g_timer_fd[MAX_TIMER];

void init_timer_list()
{
	int i = 0;
	INIT_LIST_HEAD(&g_timer_list);

	for (i = 0; i < 100; i++)
	{
		g_timer_fd[i] = 0;
	}

	return;
}



int create_timer(int id, int time_out, time_handler handler, void *data)
{
	TIMER *pstTimer = NULL;

	if (id < 0 || id >= 100)	
	{
		printf("invalid timer id %d invalid \n", id);
		return -1;
	}

	if (search_timer(id))
	{
		printf("timer id %d exist\n", id);
		return -1;
	}

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

	return 0;
}

int del_timer(int id)
{
	struct list_head *pstlist = NULL;
	struct list_head *pstNextlist = NULL;
	TIMER *pstTimer = NULL;

	if (id < 0 || id >= 100)	
	{
		printf("timer id %d invalid \n", id);
		return -1;
	}

	if (!search_timer(id))
	{
		printf("timer id %d not exist\n", id);
		return -1;
	}

	if (list_empty(&g_timer_list))
	{
		goto unlock;
	}

	pthread_mutex_lock(&timer_mutex);

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
unlock:

	pthread_mutex_lock(&timer_mutex);

	return 0;
}

void timer_loop()
{
	struct timeval timeout;
	struct list_head *pstlist = NULL;
	struct list_head *pstNextlist = NULL;
	TIMER *pstTimer = NULL;

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

