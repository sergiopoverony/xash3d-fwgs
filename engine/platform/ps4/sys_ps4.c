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
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include "platform/ps4/dlfcn_ps4.h"

// application package mount point
#define PS4_APP_DIR      "/app0"

// default writable data directory
#define PS4_DATA_DIR     "/data/xash"

// optional command line, one or more arguments separated by whitespace
#define PS4_CMDLINE_FILE "xash3d.cmdline"

// stdout and stderr are redirected here, as there is no console
#define PS4_STDOUT_FILE  "stdout.txt"

#define PS4_MAX_ARGV 64

static char ps4_basedir[256];
static char *ps4_argv[PS4_MAX_ARGV];

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
		return NULL;
	}

	handle = (int32_t)sceKernelLoadStartModule( path, 0, NULL, 0, NULL, NULL );

	if( handle < 0 )
	{
		PS4_SetDlError( "%s: sceKernelLoadStartModule failed: 0x%08x", path, (uint32_t)handle );
		return NULL;
	}

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
		fprintf( stderr, "mkdir %s failed: %s\n", path, strerror( errno ));
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
	struct stat sst, dst_st;
	char buf[65536];
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
		fprintf( stderr, "can't write %s: %s\n", dst, strerror( errno ));
		close( in );
		return;
	}

	while(( len = read( in, buf, sizeof( buf ))) > 0 )
	{
		if( write( out, buf, len ) != len )
		{
			fprintf( stderr, "write to %s failed: %s\n", dst, strerror( errno ));
			break;
		}
	}

	close( out );
	close( in );

	printf( "installed %s\n", dst );
}

static void PS4_SetupDataDir( void )
{
	char path[512];

	PS4_GetBasePath( path, sizeof( path ));
	PS4_MakeDir( path );

	// redirect stdio to file, so early errors can be read through FTP
	Q_snprintf( path, sizeof( path ), "%s/%s", ps4_basedir, PS4_STDOUT_FILE );
	if( freopen( path, "w", stdout ))
		setvbuf( stdout, NULL, _IOLBF, 0 );
	if( freopen( path, "a", stderr ))
		setvbuf( stderr, NULL, _IONBF, 0 );

	printf( "Xash3D FWGS PS4: base directory %s\n", ps4_basedir );

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
		printf( "argv[%d] = %s\n", i, ps4_argv[i] );

	*out_argv = ps4_argv;
	return argc;
}

void PS4_Init( void )
{
	// SDL's PS4 backends expect user service to be initialized
	sceUserServiceInitialize( NULL );
}

void PS4_Shutdown( void )
{
	fflush( stdout );
	fflush( stderr );
}
