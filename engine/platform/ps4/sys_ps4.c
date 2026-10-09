/*
sys_ps4.c - PlayStation 4 system utils
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

#include "platform/platform.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <pthread.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include "platform/ps4/dlfcn_ps4.h"

// application package mount point
#define PS4_APP_DIR      "/app0"

// default writable data directory
#define PS4_DATA_DIR     "/data/xash"

// optional command line, one or more arguments separated by whitespace
#define PS4_CMDLINE_FILE "xash3d.cmdline"

// everything printed by engine goes here, written without stdio buffering
// so it survives crashes
#define PS4_LOG_FILE     "stdout.txt"

#define PS4_MAX_ARGV 64

static char ps4_basedir[256];
static char *ps4_argv[PS4_MAX_ARGV];
static char ps4_logpath[300];

/*
==================
PS4_Log

Appends message to log file, opening it each time, so nothing is lost on crash
==================
*/
void PS4_Log( const char *fmt, ... )
{
	char buf[2048];
	va_list va;
	int fd, len;

	if( !ps4_logpath[0] )
		return;

	va_start( va, fmt );
	len = vsnprintf( buf, sizeof( buf ), fmt, va );
	va_end( va );

	if( len <= 0 )
		return;

	if( len >= (int)sizeof( buf ))
		len = sizeof( buf ) - 1;

	fd = open( ps4_logpath, O_WRONLY | O_CREAT | O_APPEND, 0666 );
	if( fd < 0 )
		return;

	if( write( fd, buf, len ) < 0 )
	{
		// nothing we can do
	}

	close( fd );
}

/*
=============================================================================

	DYNAMIC LIBRARY LOADING

PS4 modules (.prx) are loaded by the kernel, no dlopen in libc
Handles are kernel module ids, offset by 1 so 0 is never a valid handle

=============================================================================
*/
static char ps4_dlerror_buf[512];
static qboolean ps4_dlerror_set;

static void PS4_SetDlError( const char *fmt, ... )
{
	va_list va;

	va_start( va, fmt );
	vsnprintf( ps4_dlerror_buf, sizeof( ps4_dlerror_buf ), fmt, va );
	va_end( va );

	ps4_dlerror_set = true;
}

static qboolean PS4_FileExists( const char *path )
{
	struct stat st;

	return stat( path, &st ) == 0 && S_ISREG( st.st_mode );
}

static qboolean PS4_DirExists( const char *path )
{
	struct stat st;

	return stat( path, &st ) == 0 && S_ISDIR( st.st_mode );
}

void *PS4_dlopen( const char *name, int flags )
{
	char path[512];
	int32_t handle;

	(void)flags;

	if( !name )
	{
		PS4_SetDlError( "dlopen(NULL) is not supported" );
		return NULL;
	}

	if( name[0] == '/' )
	{
		Q_strncpy( path, name, sizeof( path ));
	}
	else
	{
		// engine modules (filesystem, menu, renderers) live in the package root
		const char *base = COM_FileWithoutPath( name );

		Q_snprintf( path, sizeof( path ), "%s/%s", PS4_APP_DIR, base );

		if( !PS4_FileExists( path ))
			Q_snprintf( path, sizeof( path ), "%s/%s", ps4_basedir, name );
	}

	if( !PS4_FileExists( path ))
	{
		PS4_SetDlError( "%s: file not found", path );
		PS4_Log( "dlopen: %s\n", ps4_dlerror_buf );
		return NULL;
	}

	handle = (int32_t)sceKernelLoadStartModule( path, 0, NULL, 0, NULL, NULL );

	if( handle < 0 )
	{
		PS4_SetDlError( "%s: sceKernelLoadStartModule failed: 0x%08x", path, (uint32_t)handle );
		PS4_Log( "dlopen: %s\n", ps4_dlerror_buf );
		return NULL;
	}

	PS4_Log( "loaded module %s: 0x%x\n", path, handle );

	return (void *)(intptr_t)( handle + 1 );
}

void *PS4_dlsym( void *handle, const char *symbol )
{
	void *addr = NULL;
	int32_t ret;

	if( !handle )
	{
		PS4_SetDlError( "dlsym(%s): invalid handle", symbol );
		return NULL;
	}

	ret = sceKernelDlsym( (int32_t)( (intptr_t)handle - 1 ), symbol, &addr );

	if( ret < 0 || !addr )
	{
		PS4_SetDlError( "dlsym(%s): symbol not found: 0x%08x", symbol, (uint32_t)ret );
		return NULL;
	}

	return addr;
}

int PS4_dlclose( void *handle )
{
	int32_t res = 0;

	if( !handle )
		return -1;

	return sceKernelStopUnloadModule( (OrbisKernelModule)( (intptr_t)handle - 1 ), 0, NULL, 0, NULL, &res ) < 0 ? -1 : 0;
}

char *PS4_dlerror( void )
{
	if( !ps4_dlerror_set )
		return NULL;

	ps4_dlerror_set = false;
	return ps4_dlerror_buf;
}

int PS4_dladdr( const void *addr, Dl_info *info )
{
	// no symbol information available, savegames use offsets instead
	(void)addr;
	(void)info;
	return 0;
}

/*
=============================================================================

	PATHS AND COMMAND LINE

=============================================================================
*/
static void PS4_MakeDir( const char *path )
{
	if( mkdir( path, 0777 ) < 0 && errno != EEXIST )
		PS4_Log( "mkdir %s failed: %s\n", path, strerror( errno ));
}

/*
==================
PS4_FindBaseDir

Prefer /data/xash, fall back to USB storage
==================
*/
static void PS4_FindBaseDir( void )
{
	const char *env = getenv( "XASH3D_BASEDIR" );
	char path[256];

	if( env && env[0] )
	{
		Q_strncpy( ps4_basedir, env, sizeof( ps4_basedir ));
		return;
	}

	// game data already in internal storage
	Q_snprintf( path, sizeof( path ), "%s/%s", PS4_DATA_DIR, XASH_GAMEDIR );
	if( PS4_DirExists( path ))
	{
		Q_strncpy( ps4_basedir, PS4_DATA_DIR, sizeof( ps4_basedir ));
		return;
	}

	for( int i = 0; i < 8; i++ )
	{
		Q_snprintf( path, sizeof( path ), "/mnt/usb%d/xash/%s", i, XASH_GAMEDIR );
		if( PS4_DirExists( path ))
		{
			Q_snprintf( ps4_basedir, sizeof( ps4_basedir ), "/mnt/usb%d/xash", i );
			return;
		}
	}

	// nothing found, use internal storage anyway so user sees where to put data
	Q_strncpy( ps4_basedir, PS4_DATA_DIR, sizeof( ps4_basedir ));
}

qboolean PS4_GetBasePath( char *buf, const size_t buflen )
{
	if( !ps4_basedir[0] )
		PS4_FindBaseDir();

	Q_strncpy( buf, ps4_basedir, buflen );
	return true;
}

/*
==================
PS4_CopyFileIfChanged

Copies a file shipped in the package to writable storage,
if destination is missing or has different size
==================
*/
static void PS4_CopyFileIfChanged( const char *src, const char *dst )
{
	static char buf[65536];
	struct stat sst, dst_st;
	int in, out;
	ssize_t len;

	if( stat( src, &sst ) < 0 )
		return;

	if( stat( dst, &dst_st ) == 0 && dst_st.st_size == sst.st_size )
		return;

	in = open( src, O_RDONLY );
	if( in < 0 )
		return;

	out = open( dst, O_WRONLY | O_CREAT | O_TRUNC, 0666 );
	if( out < 0 )
	{
		PS4_Log( "can't write %s: %s\n", dst, strerror( errno ));
		close( in );
		return;
	}

	while(( len = read( in, buf, sizeof( buf ))) > 0 )
	{
		if( write( out, buf, len ) != len )
		{
			PS4_Log( "write to %s failed: %s\n", dst, strerror( errno ));
			break;
		}
	}

	close( out );
	close( in );

	PS4_Log( "installed %s\n", dst );
}

static void PS4_SetupDataDir( void )
{
	char path[512];

	PS4_GetBasePath( path, sizeof( path ));
	PS4_MakeDir( path );

	// start new log on each launch
	Q_snprintf( ps4_logpath, sizeof( ps4_logpath ), "%s/%s", ps4_basedir, PS4_LOG_FILE );
	unlink( ps4_logpath );

	PS4_Log( "Xash3D FWGS PS4: base directory %s\n", ps4_basedir );

	// emulated by platform/ps4/compat/ps4_cwd.c, so relative paths work in engine image too
	if( chdir( ps4_basedir ) < 0 )
		PS4_Log( "chdir %s failed: %s\n", ps4_basedir, strerror( errno ));

	// engine resources are shipped in the package, but should be visible to the game
	Q_snprintf( path, sizeof( path ), "%s/%s", ps4_basedir, XASH_GAMEDIR );
	PS4_MakeDir( path );
	Q_strncat( path, "/extras.pk3", sizeof( path ));
	PS4_CopyFileIfChanged( PS4_APP_DIR "/" XASH_GAMEDIR "/extras.pk3", path );
}

/*
==================
PS4_GetArgv

There is no command line on PS4, read additional arguments from file
==================
*/
int PS4_GetArgv( int in_argc, char **in_argv, char ***out_argv )
{
	static char cmdline[2048];
	char path[512];
	int argc = 0;
	FILE *f;

	PS4_SetupDataDir();

	ps4_argv[argc++] = ( in_argc > 0 && in_argv[0] ) ? in_argv[0] : (char *)PS4_APP_DIR "/eboot.bin";

	Q_snprintf( path, sizeof( path ), "%s/%s", ps4_basedir, PS4_CMDLINE_FILE );
	f = fopen( path, "rb" );

	if( f )
	{
		size_t len = fread( cmdline, 1, sizeof( cmdline ) - 1, f );
		char *p = cmdline;

		fclose( f );
		cmdline[len] = 0;

		while( *p && argc < PS4_MAX_ARGV - 1 )
		{
			while( *p && isspace( *p ))
				*p++ = 0;

			if( !*p )
				break;

			ps4_argv[argc++] = p;

			while( *p && !isspace( *p ))
				p++;
		}
	}
	else
	{
		// write verbose log by default, so it's always possible to see what went wrong
		ps4_argv[argc++] = (char *)"-log";
		ps4_argv[argc++] = (char *)"-dev";
		ps4_argv[argc++] = (char *)"2";
	}

	ps4_argv[argc] = NULL;

	for( int i = 0; i < argc; i++ )
		PS4_Log( "argv[%d] = %s\n", i, ps4_argv[i] );

	*out_argv = ps4_argv;
	return argc;
}

/*
==================
PS4_RunOnBigStack

Default main thread stack is too small for the engine
==================
*/
#define PS4_MAIN_STACK_SIZE ( 16 * 1024 * 1024 )

typedef struct
{
	int ( *func )( void *arg );
	void *arg;
	int ret;
} ps4_thread_args_t;

static void *PS4_ThreadEntry( void *arg )
{
	ps4_thread_args_t *args = arg;

	args->ret = args->func( args->arg );
	return NULL;
}

int PS4_RunOnBigStack( int ( *func )( void *arg ), void *arg )
{
	ps4_thread_args_t args = { func, arg, 0 };
	pthread_attr_t attr;
	pthread_t thread;
	int ret;

	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, PS4_MAIN_STACK_SIZE );

	ret = pthread_create( &thread, &attr, PS4_ThreadEntry, &args );
	pthread_attr_destroy( &attr );

	if( ret != 0 )
	{
		PS4_Log( "pthread_create failed: %d, running on main thread\n", ret );
		return func( arg );
	}

	PS4_Log( "started engine thread\n" );
	pthread_join( thread, NULL );
	PS4_Log( "engine thread finished: %d\n", args.ret );

	return args.ret;
}

void PS4_Init( void )
{
	PS4_Log( "PS4_Init\n" );

	// SDL's PS4 backends expect user service to be initialized
	sceUserServiceInitialize( NULL );
}

void PS4_Shutdown( void )
{
	PS4_Log( "PS4_Shutdown\n" );
}
