/*
ps4_malloc.c - make malloc of every PS4 image thread-safe
Copyright (C) 2026 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Every PS4 image (eboot.bin and each .prx) links its own static copy of
OpenOrbis musl. Its malloc doesn't know about threads created by libkernel,
so it never takes locks, and concurrent allocations corrupt the heap.

The linker redirects malloc family here with --wrap (see scripts/waifulib/ps4.py).

In eboot.bin calls are serialized with a recursive lock (musl's memalign and
friends call malloc and free internally) and exported as __ps4_heap_*.

Modules (built with PS4_MODULE) look these exports up and use the same heap,
so memory can be freed by any image. If lookup fails, module falls back to
its own locked heap.
*/

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>

void *__real_malloc( size_t size );
void __real_free( void *ptr );
void *__real_calloc( size_t num, size_t size );
void *__real_realloc( void *ptr, size_t size );
void *__real_memalign( size_t align, size_t size );
int __real_posix_memalign( void **ptr, size_t align, size_t size );
size_t __real_malloc_usable_size( void *ptr );

static volatile int ps4_heap_lock;
static volatile uintptr_t ps4_heap_owner;
static int ps4_heap_depth; // only touched by the owner

static inline uintptr_t PS4_ThreadId( void )
{
	return (uintptr_t)pthread_self( );
}

static void PS4_HeapLock( void )
{
	uintptr_t self = PS4_ThreadId( );

	if( ps4_heap_owner == self )
	{
		ps4_heap_depth++;
		return;
	}

	while( __atomic_exchange_n( &ps4_heap_lock, 1, __ATOMIC_ACQUIRE ))
	{
		while( __atomic_load_n( &ps4_heap_lock, __ATOMIC_RELAXED ))
			sched_yield( );
	}

	ps4_heap_owner = self;
	ps4_heap_depth = 1;
}

static void PS4_HeapUnlock( void )
{
	if( --ps4_heap_depth > 0 )
		return;

	ps4_heap_owner = 0;
	__atomic_store_n( &ps4_heap_lock, 0, __ATOMIC_RELEASE );
}

#define LOCKED( type, call ) \
	do { type ret; PS4_HeapLock( ); ret = call; PS4_HeapUnlock( ); return ret; } while( 0 )

#define EXPORT __attribute__(( visibility( "default" ), used ))

#if PS4_MODULE
#define HEAP_FUNC static
#else
#define HEAP_FUNC EXPORT
#endif

HEAP_FUNC void *__ps4_heap_malloc( size_t size ) { LOCKED( void *, __real_malloc( size )); }
HEAP_FUNC void __ps4_heap_free( void *ptr ) { if( !ptr ) return; PS4_HeapLock( ); __real_free( ptr ); PS4_HeapUnlock( ); }
HEAP_FUNC void *__ps4_heap_calloc( size_t num, size_t size ) { LOCKED( void *, __real_calloc( num, size )); }
HEAP_FUNC void *__ps4_heap_realloc( void *ptr, size_t size ) { LOCKED( void *, __real_realloc( ptr, size )); }
HEAP_FUNC void *__ps4_heap_memalign( size_t align, size_t size ) { LOCKED( void *, __real_memalign( align, size )); }
HEAP_FUNC int __ps4_heap_posix_memalign( void **ptr, size_t align, size_t size ) { LOCKED( int, __real_posix_memalign( ptr, align, size )); }
HEAP_FUNC size_t __ps4_heap_malloc_usable_size( void *ptr ) { LOCKED( size_t, __real_malloc_usable_size( ptr )); }

static struct
{
	void *( *malloc )( size_t size );
	void ( *free )( void *ptr );
	void *( *calloc )( size_t num, size_t size );
	void *( *realloc )( void *ptr, size_t size );
	void *( *memalign )( size_t align, size_t size );
	int ( *posix_memalign )( void **ptr, size_t align, size_t size );
	size_t ( *malloc_usable_size )( void *ptr );
	volatile int ready;
} heap;

#if PS4_MODULE
typedef struct
{
	size_t   size;
	char     name[256];
	struct { void *address; uint32_t size; int32_t prot; } segments[4];
	uint32_t segment_count;
	uint8_t  fingerprint[20];
} ps4_module_info_t;

int32_t sceKernelGetModuleList( uint32_t *array, size_t size, size_t *available );
int32_t sceKernelGetModuleInfo( uint32_t handle, ps4_module_info_t *info );
int32_t sceKernelDlsym( int32_t handle, const char *symbol, void **address );

static int PS4_FindEbootHeap( void )
{
	uint32_t handles[256];
	size_t count = 0;

	if( sceKernelGetModuleList( handles, 256, &count ) < 0 )
		return 0;

	for( size_t i = 0; i < count && i < 256; i++ )
	{
		ps4_module_info_t info;
		const char *a, *b;

		__builtin_memset( &info, 0, sizeof( info ));
		info.size = sizeof( info );

		if( sceKernelGetModuleInfo( handles[i], &info ) < 0 )
			continue;

		for( a = info.name, b = "eboot.bin"; *a && *a == *b; a++, b++ );
		if( *a || *b )
			continue;

		return sceKernelDlsym( handles[i], "__ps4_heap_malloc", (void **)&heap.malloc ) >= 0
			&& sceKernelDlsym( handles[i], "__ps4_heap_free", (void **)&heap.free ) >= 0
			&& sceKernelDlsym( handles[i], "__ps4_heap_calloc", (void **)&heap.calloc ) >= 0
			&& sceKernelDlsym( handles[i], "__ps4_heap_realloc", (void **)&heap.realloc ) >= 0
			&& sceKernelDlsym( handles[i], "__ps4_heap_memalign", (void **)&heap.memalign ) >= 0
			&& sceKernelDlsym( handles[i], "__ps4_heap_posix_memalign", (void **)&heap.posix_memalign ) >= 0
			&& sceKernelDlsym( handles[i], "__ps4_heap_malloc_usable_size", (void **)&heap.malloc_usable_size ) >= 0
			&& heap.malloc && heap.free && heap.calloc && heap.realloc && heap.memalign && heap.posix_memalign && heap.malloc_usable_size;
	}

	return 0;
}
#endif

static void PS4_InitHeap( void )
{
#if PS4_MODULE
	if( PS4_FindEbootHeap( ))
	{
		heap.ready = 1;
		return;
	}
#endif
	heap.malloc = __ps4_heap_malloc;
	heap.free = __ps4_heap_free;
	heap.calloc = __ps4_heap_calloc;
	heap.realloc = __ps4_heap_realloc;
	heap.memalign = __ps4_heap_memalign;
	heap.posix_memalign = __ps4_heap_posix_memalign;
	heap.malloc_usable_size = __ps4_heap_malloc_usable_size;
	heap.ready = 1;
}

// for diagnostics: 1 if this image uses heap of eboot.bin
EXPORT int __ps4_heap_shared( void )
{
	if( !heap.ready )
		PS4_InitHeap( );

	return heap.malloc != __ps4_heap_malloc;
}

#define HEAP() ( heap.ready ? (void)0 : PS4_InitHeap( ))

void *__wrap_malloc( size_t size ) { HEAP( ); return heap.malloc( size ); }
void __wrap_free( void *ptr ) { if( !ptr ) return; HEAP( ); heap.free( ptr ); }
void *__wrap_calloc( size_t num, size_t size ) { HEAP( ); return heap.calloc( num, size ); }
void *__wrap_realloc( void *ptr, size_t size ) { HEAP( ); return heap.realloc( ptr, size ); }
void *__wrap_memalign( size_t align, size_t size ) { HEAP( ); return heap.memalign( align, size ); }
void *__wrap_aligned_alloc( size_t align, size_t size ) { HEAP( ); return heap.memalign( align, size ); }
void *__wrap_valloc( size_t size ) { HEAP( ); return heap.memalign( 16384, size ); }
int __wrap_posix_memalign( void **ptr, size_t align, size_t size ) { HEAP( ); return heap.posix_memalign( ptr, align, size ); }
size_t __wrap_malloc_usable_size( void *ptr ) { HEAP( ); return heap.malloc_usable_size( ptr ); }
