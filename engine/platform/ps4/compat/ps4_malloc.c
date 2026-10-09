/*
ps4_malloc.c - route memory allocation of every PS4 image to the system allocator
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
so it never takes locks, and each module has a separate heap.

The linker redirects malloc family here with --wrap (see scripts/waifulib/ps4.py)
and everything goes to thread-safe libSceLibcInternal allocator shared by
the whole process, so memory can also be freed by any module.
*/

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

typedef uint32_t OrbisKernelModule;

typedef struct
{
	void    *address;
	uint32_t size;
	int32_t  prot;
} ps4_segment_info_t;

typedef struct
{
	size_t   size;
	char     name[256];
	ps4_segment_info_t segments[4];
	uint32_t segment_count;
	uint8_t  fingerprint[20];
} ps4_module_info_t;

int32_t sceKernelGetModuleList( OrbisKernelModule *array, size_t size, size_t *available );
int32_t sceKernelGetModuleInfo( OrbisKernelModule handle, ps4_module_info_t *info );
int32_t sceKernelDlsym( int32_t handle, const char *symbol, void **address );

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
} sys;

static void PS4_ResolveAllocator( void )
{
	OrbisKernelModule handles[256];
	size_t count = 0;

	if( sys.ready )
		return;

	if( sceKernelGetModuleList( handles, 256, &count ) < 0 )
		__builtin_trap( );

	for( size_t i = 0; i < count && i < 256; i++ )
	{
		ps4_module_info_t info;

		memset( &info, 0, sizeof( info ));
		info.size = sizeof( info );

		if( sceKernelGetModuleInfo( handles[i], &info ) < 0 )
			continue;

		if( strcmp( info.name, "libSceLibcInternal.sprx" ))
			continue;

		sceKernelDlsym( handles[i], "malloc", (void **)&sys.malloc );
		sceKernelDlsym( handles[i], "free", (void **)&sys.free );
		sceKernelDlsym( handles[i], "calloc", (void **)&sys.calloc );
		sceKernelDlsym( handles[i], "realloc", (void **)&sys.realloc );
		sceKernelDlsym( handles[i], "memalign", (void **)&sys.memalign );
		sceKernelDlsym( handles[i], "posix_memalign", (void **)&sys.posix_memalign );
		sceKernelDlsym( handles[i], "malloc_usable_size", (void **)&sys.malloc_usable_size );
		break;
	}

	// nothing works without memory
	if( !sys.malloc || !sys.free || !sys.calloc || !sys.realloc || !sys.memalign || !sys.posix_memalign )
		__builtin_trap( );

	sys.ready = 1;
}

#define ENSURE_ALLOCATOR() do { if( !sys.ready ) PS4_ResolveAllocator( ); } while( 0 )

void *__wrap_malloc( size_t size )
{
	ENSURE_ALLOCATOR( );
	return sys.malloc( size );
}

void __wrap_free( void *ptr )
{
	if( !ptr )
		return;

	ENSURE_ALLOCATOR( );
	sys.free( ptr );
}

void *__wrap_calloc( size_t num, size_t size )
{
	ENSURE_ALLOCATOR( );
	return sys.calloc( num, size );
}

void *__wrap_realloc( void *ptr, size_t size )
{
	ENSURE_ALLOCATOR( );
	return sys.realloc( ptr, size );
}

void *__wrap_memalign( size_t align, size_t size )
{
	ENSURE_ALLOCATOR( );
	return sys.memalign( align, size );
}

void *__wrap_aligned_alloc( size_t align, size_t size )
{
	ENSURE_ALLOCATOR( );
	return sys.memalign( align, size );
}

void *__wrap_valloc( size_t size )
{
	ENSURE_ALLOCATOR( );
	return sys.memalign( 16384, size );
}

int __wrap_posix_memalign( void **ptr, size_t align, size_t size )
{
	ENSURE_ALLOCATOR( );
	return sys.posix_memalign( ptr, align, size );
}

size_t __wrap_malloc_usable_size( void *ptr )
{
	ENSURE_ALLOCATOR( );
	return sys.malloc_usable_size ? sys.malloc_usable_size( ptr ) : 0;
}
