#ifndef __J_HASH_H__
#define __J_HASH_H__

#include <stdint.h>

#define HASHSZ 1024
#define JHASH_INITVAL 0xdeadbeef

uint32_t icmp_hashfn(uint32_t ip1, uint32_t type, uint32_t code);

static inline uint32_t rol32(uint32_t word, unsigned int shift)
{
	return (word << shift) | (word >> ((-shift) & 31));
}


#define __jhash_final(a, b, c)\
{							  \
	c ^= b; c -= rol32(b, 14);\
	a ^= c; a -= rol32(c, 11);\
	b ^= a; b -= rol32(a, 25);\
	c ^= b; c -= rol32(b, 16);\
	a ^= c; a -= rol32(c, 4);\
	b ^= a; b -= rol32(a, 14);\
	c ^= b; c -= rol32(b, 24);\
}							  \

static inline uint32_t __jhash_nwords(uint32_t a, uint32_t b, uint32_t c, uint32_t initval)
{
	a += initval;
	b += initval;
	c += initval;

	__jhash_final(a, b, c);

	return c;
}

static inline uint32_t jhash_1word(uint32_t a, uint32_t initval)
{
	return __jhash_nwords(a, 0, 0, initval + JHASH_INITVAL + (1 << 2));
}

static inline uint32_t jhash_2words(uint32_t a, uint32_t b, uint32_t initval)
{
	return __jhash_nwords(a, b, 0, initval + JHASH_INITVAL + (1 << 2));
}

static inline uint32_t jhash_3words(uint32_t a, uint32_t b, uint32_t c, uint32_t initval)
{
	return __jhash_nwords(a, b, c, initval + JHASH_INITVAL + (1 << 2));
}

#endif
