/*
Models, the map and their textures come and go with each level, and whatever
survives between them (HUD, cvars, strings, small blocks everywhere) splits the
heap until a big block no longer exists. These assets get their own region: nothing
else lives in it, so it returns to one free block after each level.

Payloads are 64-byte aligned like valloc: the header sits in the last 16 bytes
of the line before, and block sizes are multiples of 64.
*/
#include "arena_ps2.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

// cs_office peaks at 15,979 KB, de_inferno at 15,721 KB; an overflow falls back to malloc.
#define ARENA_BYTES ( 16u * 1024u * 1024u )
#define ARENA_LINE 64u
#define ARENA_MIN_SPLIT ( ARENA_LINE * 2u )

typedef struct block_s
{
	unsigned int size; // header included
	unsigned int prev; // size of the block before, 0 for the first
	unsigned int used;
	unsigned int pad;
} block_t;

#define BLOCK_NEXT( b ) ( (block_t *)( (char *)( b ) + ( b )->size ))
#define BLOCK_PREV( b ) ( (block_t *)( (char *)( b ) - ( b )->prev ))
#define BLOCK_OF( ptr ) ( (block_t *)( (char *)( ptr ) - sizeof( block_t )))

static char *arena_first, *arena_end;
static unsigned int arena_overflows, arena_overflow_bytes;

void PS2_Arena_Init( void )
{
	char *region = memalign( ARENA_LINE, ARENA_BYTES );
	block_t *first;

	if( !region )
	{
		printf( "[XASH-PS2] ARENA not available\n" );
		return;
	}

	arena_first = region + ARENA_LINE - sizeof( block_t );
	arena_end = region + ARENA_BYTES - sizeof( block_t );
	first = (block_t *)arena_first;
	first->size = (unsigned int)( arena_end - arena_first );
	first->prev = 0;
	first->used = 0;
}

int PS2_Arena_Contains( const void *ptr )
{
	return arena_first && (const char *)ptr >= arena_first && (const char *)ptr < arena_end;
}

static unsigned int Arena_BlockSize( size_t size )
{
	return (unsigned int)(( size + sizeof( block_t ) + ARENA_LINE - 1 ) & ~(size_t)( ARENA_LINE - 1 ));
}

// the block keeps "size" bytes, the rest becomes a free block
static void Arena_Split( block_t *b, unsigned int size )
{
	block_t *rest;

	if( b->size < size + ARENA_MIN_SPLIT )
		return;

	rest = (block_t *)( (char *)b + size );
	rest->size = b->size - size;
	rest->prev = size;
	rest->used = 0;
	b->size = size;

	if( (char *)BLOCK_NEXT( rest ) < arena_end )
		BLOCK_NEXT( rest )->prev = rest->size;
}

static void Arena_MergeNext( block_t *b )
{
	block_t *next = BLOCK_NEXT( b );

	if( (char *)next >= arena_end || next->used )
		return;

	b->size += next->size;

	if( (char *)BLOCK_NEXT( b ) < arena_end )
		BLOCK_NEXT( b )->prev = b->size;
}

void *PS2_Arena_Alloc( size_t size )
{
	unsigned int needed = Arena_BlockSize( size );
	block_t *b;

	if( !arena_first )
		return NULL;

	// lowest address first: long-lived assets stay packed at the bottom
	for( b = (block_t *)arena_first; (char *)b < arena_end; b = BLOCK_NEXT( b ))
	{
		if( b->used || b->size < needed )
			continue;

		Arena_Split( b, needed );
		b->used = 1;
		return (char *)b + sizeof( block_t );
	}

	arena_overflows++;
	arena_overflow_bytes += (unsigned int)size;
	return NULL;
}

static void Arena_Free( void *ptr )
{
	block_t *b = BLOCK_OF( ptr );

	b->used = 0;
	Arena_MergeNext( b );

	if( b->prev && !BLOCK_PREV( b )->used )
		Arena_MergeNext( BLOCK_PREV( b ));
}

void PS2_Free( void *ptr )
{
	if( PS2_Arena_Contains( ptr ))
		Arena_Free( ptr );
	else free( ptr );
}

void PS2_Arena_Shrink( void *ptr, size_t size )
{
	block_t *b = BLOCK_OF( ptr );
	unsigned int needed = Arena_BlockSize( size );

	if( needed > b->size )
		return;

	Arena_Split( b, needed );

	if( (char *)BLOCK_NEXT( b ) < arena_end && !BLOCK_NEXT( b )->used )
		Arena_MergeNext( BLOCK_NEXT( b ));
}

size_t PS2_Arena_LargestFree( void )
{
	block_t *b;
	size_t largest = 0;

	if( !arena_first )
		return 0;

	for( b = (block_t *)arena_first; (char *)b < arena_end; b = BLOCK_NEXT( b ))
		if( !b->used && b->size > largest )
			largest = b->size;

	return largest;
}

// Also checks the block chain: an overrun of an asset shows up here as "broken".
void PS2_Arena_Report( const char *when )
{
	block_t *b;
	unsigned int live = 0, live_blocks = 0, free_bytes = 0, largest = 0, prev = 0;
	int broken = 0;

	if( !arena_first )
		return;

	for( b = (block_t *)arena_first; (char *)b < arena_end; b = BLOCK_NEXT( b ))
	{
		if( b->prev != prev || !b->size || b->size % ARENA_LINE )
		{
			broken = 1;
			break;
		}
		prev = b->size;

		if( b->used )
		{
			live += b->size;
			live_blocks++;
		}
		else
		{
			free_bytes += b->size;
			if( b->size > largest ) largest = b->size;
		}
	}

	printf( "[XASH-PS2] ARENA %s live_kb=%u live_blocks=%u free_kb=%u largest_kb=%u overflows=%u overflow_kb=%u%s\n",
		when, live / 1024, live_blocks, free_bytes / 1024, largest / 1024, arena_overflows,
		arena_overflow_bytes / 1024, broken ? " BROKEN" : "" );
}

/*
Largest block malloc can return, by bisection (nothing stays allocated). The
volatile keeps GCC from removing the pair and assuming that malloc succeeded.
*/
size_t PS2_Heap_LargestBlock( size_t resolution )
{
	size_t low = 0, high = 32u * 1024u * 1024u;

	while( high - low > resolution )
	{
		size_t mid = ( low + high ) / 2;
		void *volatile probe = malloc( mid );

		if( probe )
		{
			free( probe );
			low = mid;
		}
		else high = mid;
	}

	return low;
}
