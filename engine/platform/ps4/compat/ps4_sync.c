/*
ps4_sync.c - working semaphore waits for PS4
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
Static musl libc from OpenOrbis toolchain implements sem_wait() as system
ksem_timedwait() with NULL timeout, which PS4 kernel rejects, so it fails immediately.
SDL timer thread waits on a semaphore forever and turned into a busy loop,
reporting the error on every iteration.

The linker redirects sem_wait and sem_timedwait here with --wrap=<function>
(see scripts/waifulib/ps4.py). Non-blocking sem_trywait() and sem_post() work fine,
so waiting is done by polling with a short sleep.
*/

#include <errno.h>
#include <semaphore.h>
#include <stdint.h>
#include <time.h>

#define PS4_SEM_POLL_USEC 1000

int32_t sceKernelUsleep( uint32_t usec );

int __wrap_sem_wait( sem_t *sem )
{
	for( ;; )
	{
		// errors are not checked, sem_trywait wraps system ksem_trywait,
		// which reports them through system errno, not musl one
		if( !sem_trywait( sem ))
			return 0;

		sceKernelUsleep( PS4_SEM_POLL_USEC );
	}
}

int __wrap_sem_timedwait( sem_t *sem, const struct timespec *abstime )
{
	for( ;; )
	{
		struct timespec now;

		// errors are not checked, sem_trywait wraps system ksem_trywait,
		// which reports them through system errno, not musl one
		if( !sem_trywait( sem ))
			return 0;

		if( !clock_gettime( CLOCK_REALTIME, &now ))
		{
			if( now.tv_sec > abstime->tv_sec || ( now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec ))
			{
				errno = ETIMEDOUT;
				return -1;
			}
		}

		sceKernelUsleep( PS4_SEM_POLL_USEC );
	}
}
