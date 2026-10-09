/*
ps4_cwd.c - current working directory emulation for PS4
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
PS4 applications aren't allowed to call chdir() and the kernel rejects
relative paths, but the engine filesystem works relative to the game root.

This file is linked into every PS4 image (eboot.bin and each .prx module)
and the linker redirects file functions here with --wrap=<function>
(see scripts/waifulib/ps4.py). Relative paths are resolved against
emulated working directory, which is private to each image: modules don't
share libc state on PS4 anyway.
*/

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define PS4_PATH_MAX 1024

static char ps4_cwd[PS4_PATH_MAX];

int __real_open( const char *path, int flags, ... );
FILE *__real_fopen( const char *path, const char *mode );
int __real_stat( const char *path, struct stat *st );
int __real_lstat( const char *path, struct stat *st );
DIR *__real_opendir( const char *path );
int __real_mkdir( const char *path, mode_t mode );
int __real_rename( const char *oldpath, const char *newpath );
int __real_remove( const char *path );
int __real_unlink( const char *path );
int __real_rmdir( const char *path );
int __real_access( const char *path, int mode );
char *__real_getcwd( char *buf, size_t size );

// collapses "." and ".." components and duplicate slashes of an absolute path
static void PS4_NormalizePath( char *path )
{
	char *src = path, *dst = path;

	while( *src )
	{
		if( src[0] == '/' )
		{
			while( src[1] == '/' )
				src++;

			if( src[1] == '.' && ( src[2] == '/' || src[2] == 0 ))
			{
				src += 2;
				continue;
			}

			if( src[1] == '.' && src[2] == '.' && ( src[3] == '/' || src[3] == 0 ))
			{
				src += 3;

				// remove previous component
				while( dst > path && *--dst != '/' );
				continue;
			}
		}

		*dst++ = *src++;
	}

	*dst = 0;

	// remove trailing slash, but keep root
	if( dst > path + 1 && dst[-1] == '/' )
		dst[-1] = 0;

	if( !path[0] )
	{
		path[0] = '/';
		path[1] = 0;
	}
}

// returns path that kernel will accept: original if absolute or no cwd set
static const char *PS4_AbsPath( const char *path, char *buf, size_t size )
{
	if( !path || path[0] == '/' || !ps4_cwd[0] )
		return path;

	snprintf( buf, size, "%s/%s", ps4_cwd, path );
	PS4_NormalizePath( buf );

	return buf;
}

int __wrap_open( const char *path, int flags, ... )
{
	char buf[PS4_PATH_MAX];
	mode_t mode = 0;

	if( flags & O_CREAT )
	{
		va_list va;
		va_start( va, flags );
		mode = (mode_t)va_arg( va, int );
		va_end( va );
	}

	return __real_open( PS4_AbsPath( path, buf, sizeof( buf )), flags, mode );
}

FILE *__wrap_fopen( const char *path, const char *mode )
{
	char buf[PS4_PATH_MAX];
	return __real_fopen( PS4_AbsPath( path, buf, sizeof( buf )), mode );
}

int __wrap_stat( const char *path, struct stat *st )
{
	char buf[PS4_PATH_MAX];
	return __real_stat( PS4_AbsPath( path, buf, sizeof( buf )), st );
}

int __wrap_lstat( const char *path, struct stat *st )
{
	char buf[PS4_PATH_MAX];
	return __real_lstat( PS4_AbsPath( path, buf, sizeof( buf )), st );
}

DIR *__wrap_opendir( const char *path )
{
	char buf[PS4_PATH_MAX];
	return __real_opendir( PS4_AbsPath( path, buf, sizeof( buf )));
}

int __wrap_mkdir( const char *path, mode_t mode )
{
	char buf[PS4_PATH_MAX];
	return __real_mkdir( PS4_AbsPath( path, buf, sizeof( buf )), mode );
}

int __wrap_rename( const char *oldpath, const char *newpath )
{
	char buf1[PS4_PATH_MAX], buf2[PS4_PATH_MAX];
	return __real_rename( PS4_AbsPath( oldpath, buf1, sizeof( buf1 )), PS4_AbsPath( newpath, buf2, sizeof( buf2 )));
}

int __wrap_remove( const char *path )
{
	char buf[PS4_PATH_MAX];
	return __real_remove( PS4_AbsPath( path, buf, sizeof( buf )));
}

int __wrap_unlink( const char *path )
{
	char buf[PS4_PATH_MAX];
	return __real_unlink( PS4_AbsPath( path, buf, sizeof( buf )));
}

int __wrap_rmdir( const char *path )
{
	char buf[PS4_PATH_MAX];
	return __real_rmdir( PS4_AbsPath( path, buf, sizeof( buf )));
}

int __wrap_access( const char *path, int mode )
{
	char buf[PS4_PATH_MAX];
	return __real_access( PS4_AbsPath( path, buf, sizeof( buf )), mode );
}

int __wrap_chdir( const char *path )
{
	char buf[PS4_PATH_MAX];
	struct stat st;

	if( !path || !path[0] )
	{
		errno = ENOENT;
		return -1;
	}

	if( path[0] == '/' )
		snprintf( buf, sizeof( buf ), "%s", path );
	else if( ps4_cwd[0] )
		snprintf( buf, sizeof( buf ), "%s/%s", ps4_cwd, path );
	else
	{
		errno = ENOENT;
		return -1;
	}

	PS4_NormalizePath( buf );

	if( __real_stat( buf, &st ) < 0 )
		return -1;

	if( !S_ISDIR( st.st_mode ))
	{
		errno = ENOTDIR;
		return -1;
	}

	snprintf( ps4_cwd, sizeof( ps4_cwd ), "%s", buf );
	return 0;
}

char *__wrap_getcwd( char *buf, size_t size )
{
	size_t len;

	if( !ps4_cwd[0] )
		return __real_getcwd( buf, size );

	len = strlen( ps4_cwd ) + 1;

	if( !buf )
	{
		if( size < len )
			size = len;

		buf = malloc( size );
		if( !buf )
		{
			errno = ENOMEM;
			return NULL;
		}
	}
	else if( size < len )
	{
		errno = ERANGE;
		return NULL;
	}

	memcpy( buf, ps4_cwd, len );
	return buf;
}

char *__wrap_realpath( const char *path, char *resolved )
{
	char buf[PS4_PATH_MAX];
	struct stat st;

	if( !path )
	{
		errno = EINVAL;
		return NULL;
	}

	if( path[0] == '/' )
		snprintf( buf, sizeof( buf ), "%s", path );
	else if( ps4_cwd[0] )
		snprintf( buf, sizeof( buf ), "%s/%s", ps4_cwd, path );
	else
	{
		errno = ENOENT;
		return NULL;
	}

	// no symlinks on PS4 user storage, normalization is enough
	PS4_NormalizePath( buf );

	if( __real_stat( buf, &st ) < 0 )
		return NULL;

	if( !resolved )
		return strdup( buf );

	// POSIX requires PATH_MAX sized buffer here
	snprintf( resolved, 4096, "%s", buf );
	return resolved;
}
