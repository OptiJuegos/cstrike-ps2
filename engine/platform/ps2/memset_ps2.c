/* newlib's memset writes byte by byte unless the block starts 8-byte aligned, and structs on the
 * stack usually start 4-byte aligned. On de_aztec that cost 1.7 ms a frame in traces alone. */
#include <stddef.h>
#include <stdint.h>

__attribute__(( optimize( "no-tree-loop-distribute-patterns" )))
void *memset( void *dest, int c, size_t n )
{
	uint8_t *p = (uint8_t *)dest;
	const uint64_t v = (uint8_t)c * 0x0101010101010101ULL;

	while( n && ((uintptr_t)p & 7 ))
	{
		*p++ = (uint8_t)c;
		n--;
	}

	for( ; n >= 8; n -= 8, p += 8 )
		*(uint64_t *)p = v;

	while( n-- )
		*p++ = (uint8_t)c;

	return dest;
}
