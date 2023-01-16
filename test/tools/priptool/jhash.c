#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>

#include "list.h"
#include "jhash.h"

uint32_t icmp_hashfn(uint32_t ip1, uint32_t ip2, uint32_t type, uint32_t code)
{
	return jhash_3words(ip1, ip2, type, code) & (HASHSZ - 1);
}
