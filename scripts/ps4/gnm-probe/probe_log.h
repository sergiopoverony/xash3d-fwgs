/*
probe_log.h - redirects standard output of GNM probe to a log file
Copyright (C) 2026 sergiopoverony

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// Force-included into every probe source (including opengnm and example helpers),
// so all their diagnostics end up in /data/xash/gnmprobe.txt instead of nowhere:
// PS4 doesn't let us redirect stdout.
#ifndef PROBE_LOG_H
#define PROBE_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int probe_printf( const char *fmt, ... ) __attribute__(( format( printf, 1, 2 )));
int probe_vprintf( const char *fmt, va_list ap );
int probe_fprintf( FILE *f, const char *fmt, ... ) __attribute__(( format( printf, 2, 3 )));
int probe_puts( const char *s );
int probe_putc( int c, FILE *f );
_Noreturn void probe_exit( int code );

#ifndef PROBE_LOG_IMPL
#undef putc
#undef putchar
#define printf  probe_printf
#define vprintf probe_vprintf
#define fprintf probe_fprintf
#define puts    probe_puts
#define putc    probe_putc
#define putchar( c ) probe_putc( c, stdout )
#define exit    probe_exit
#endif

#endif // PROBE_LOG_H
