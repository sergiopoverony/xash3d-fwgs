/*
probe.c - checks whether GPU rendering through opengnm works on this console
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

// Two stages, every step is logged to /data/xash/gnmprobe.txt with fsync,
// so the log survives a GPU hang:
//   A. bare submit: default hardware state + EOP label write, no shaders
//      (same as opengnm hardware smoke test)
//   B. triangle: per-frame command buffer with clear and a triangle drawn
//      with shaders compiled by opengnm-psbc, flipped to the screen
// Based on the triangle sample from freegnm-examples (MIT).

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/SystemService.h>

#include <gnm/drawcommandbuffer.h>
#include <gnm/gpuaddr/gpuaddr.h>
#include <gnm/platform.h>

#include "displayctx.h"
#include "memalloc.h"
#include "misc.h"

#define LOG_DIR   "/data/xash"
#define LOG_PATH  LOG_DIR "/gnmprobe.txt"
#define FRAMES    600     // ~10 seconds at 60 Hz
#define TIMEOUT_US 2000000 // GPU must finish a frame within 2 seconds

static int log_fd = -1;
static uint64_t log_start;

static void log_write( const char *buf, size_t len )
{
	if( log_fd < 0 )
		return;

	sceKernelWrite( log_fd, buf, len );
	sceKernelFsync( log_fd );
}

static void log_open( void )
{
	sceKernelMkdir( LOG_DIR, 0777 );
	log_fd = sceKernelOpen( LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666 );
	log_start = sceKernelGetProcessTime();
}

int probe_vprintf( const char *fmt, va_list ap )
{
	char buf[1024];
	uint64_t ms = ( sceKernelGetProcessTime() - log_start ) / 1000;
	int prefix = snprintf( buf, sizeof( buf ), "[%6llu] ", (unsigned long long)ms );
	int len = vsnprintf( buf + prefix, sizeof( buf ) - prefix - 1, fmt, ap );

	if( len < 0 )
		return len;

	len += prefix;
	if( len > (int)sizeof( buf ) - 2 )
		len = sizeof( buf ) - 2;

	// every message is a separate line, example code mixes printf with and without newline
	if( buf[len - 1] != '\n' )
		buf[len++] = '\n';

	log_write( buf, len );
	return len;
}

int probe_printf( const char *fmt, ... )
{
	va_list ap;
	int ret;

	va_start( ap, fmt );
	ret = probe_vprintf( fmt, ap );
	va_end( ap );
	return ret;
}

int probe_fprintf( FILE *f, const char *fmt, ... )
{
	va_list ap;
	int ret;

	(void)f;
	va_start( ap, fmt );
	ret = probe_vprintf( fmt, ap );
	va_end( ap );
	return ret;
}

int probe_puts( const char *s )
{
	return probe_printf( "%s", s );
}

int probe_putc( int c, FILE *f )
{
	(void)f;
	// lone newlines from fatalf(), each message already ends with one
	return c;
}

_Noreturn void probe_exit( int code )
{
	probe_printf( "exit(%d), returning to the system", code );

	if( log_fd >= 0 )
		sceKernelClose( log_fd );
	log_fd = -1;

	sceSystemServiceLoadExec( "exit", NULL );
	for( ;; )
		sceKernelUsleep( 1000000 );
}

static bool wait_label( volatile uint64_t *label, uint64_t value, uint64_t *waited_us )
{
	uint64_t start = sceKernelGetProcessTime();

	for( ;; )
	{
		uint64_t now;

		if( *label == value )
		{
			*waited_us = sceKernelGetProcessTime() - start;
			return true;
		}

		now = sceKernelGetProcessTime();
		if( now - start > TIMEOUT_US )
		{
			*waited_us = now - start;
			return false;
		}

		sceKernelUsleep( 50 );
	}
}

static void log_system_info( void )
{
	OrbisKernelSwVersion ver;

	memset( &ver, 0, sizeof( ver ));
	ver.Size = sizeof( ver );
	if( sceKernelGetSystemSwVersion( &ver ) == 0 )
		printf( "firmware: %.28s (0x%08x)", ver.VersionString, ver.Version );
	else
		printf( "firmware: unknown" );

	printf( "neo mode (PS4 Pro): %d, gpu mode: %d", sceKernelIsNeoMode(), (int)gnmGpuMode());
	printf( "direct memory size: 0x%llx", (unsigned long long)sceKernelGetDirectMemorySize());
}

// stage A: does the GPU execute anything we submit at all
static bool stage_bare_submit( MemoryAllocator *garlic )
{
	const uint32_t cmdsize = 64 * 1024;
	const uint64_t magic = 0x4f50474e534d4b45ULL;
	volatile uint64_t *label;
	GnmCommandBuffer cmd;
	void *cmdmem;
	void *dcb[1];
	uint32_t dcbsize[1];
	uint64_t waited;
	int res;

	printf( "=== stage A: bare submit" );

	cmdmem = memalloc_alloc( garlic, cmdsize, GNM_ALIGNMENT_BUFFER_BYTES );
	label = memalloc_alloc( garlic, sizeof( uint64_t ), sizeof( uint64_t ));
	if( !cmdmem || !label )
	{
		printf( "A: garlic allocation failed" );
		return false;
	}
	*label = 0;

	cmd = gnmCmdInit( cmdmem, cmdsize, NULL, NULL );
	gnmDrawCmdInitDefaultHardwareState( &cmd );
	gnmDrawCmdDrawIndexAuto( &cmd, 0 );
	gnmDrawCmdEventWriteEop( &cmd, GNM_CACHE_FLUSH_AND_INV_TS_EVENT, (uint64_t)(uintptr_t)label, GNM_DATA_SEL_SEND_DATA64, magic );

	dcb[0] = cmd.beginptr;
	dcbsize[0] = (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr );
	printf( "A: command buffer %u bytes", dcbsize[0] );

	res = sceGnmSubmitCommandBuffers( 1, dcb, dcbsize, NULL, NULL );
	printf( "A: sceGnmSubmitCommandBuffers = 0x%x", res );
	if( res < 0 )
		return false;

	res = sceGnmSubmitDone();
	printf( "A: sceGnmSubmitDone = 0x%x", res );

	if( !wait_label( label, magic, &waited ))
	{
		printf( "A: FAIL, EOP label not written in %llu us, value 0x%llx",
			(unsigned long long)waited, (unsigned long long)*label );
		return false;
	}

	printf( "A: OK, GPU wrote EOP label in %llu us", (unsigned long long)waited );
	return true;
}

// stage B: shaders, render target, flip
static bool stage_triangle( MemoryAllocator *garlic )
{
	const uint32_t cmdsize = 1024 * 1024;
	volatile uint64_t *label;
	DisplayContext display;
	GnmRenderTarget fb;
	GnmVsShader *vs = NULL;
	GnmPsShader *ps = NULL;
	void *cmdmem;
	uint64_t max_wait = 0, total_wait = 0;
	uint64_t start;
	int frame;

	printf( "=== stage B: triangle" );

	if( !initclearutility( garlic ))
	{
		printf( "B: FAIL, clear shader not loaded" );
		return false;
	}
	printf( "B: clear shader loaded" );

	if( !loadvshader( &vs, garlic, "/app0/assets/misc/tri.vert.sb" ) || !loadpshader( &ps, garlic, "/app0/assets/misc/tri.frag.sb" ))
	{
		printf( "B: FAIL, triangle shaders not loaded" );
		return false;
	}
	printf( "B: triangle shaders loaded, vs exports %u, ps inputs %u", vs->numexportsemantics, ps->numinputsemantics );

	memset( &display, 0, sizeof( display ));
	if( !displayctx_init( &display ))
	{
		printf( "B: FAIL, VideoOut init" );
		return false;
	}
	printf( "B: VideoOut handle %d, resolution %ux%u", display.videohandle, display.screenw, display.screenh );

	memset( &fb, 0, sizeof( fb ));
	if( !initcolortarget( &fb, garlic, display.screenw, display.screenh, GNM_FMT_R8G8B8A8_SRGB, gnmGpuMode()))
	{
		printf( "B: FAIL, color render target" );
		return false;
	}
	printf( "B: render target %ux%u pitch %u at %p", fb.size.width, fb.size.height, gnmRtGetPitch( &fb ), gnmRtGetBaseAddr( &fb ));

	if( !displayctx_setrts( &display, &fb, 1 ))
	{
		printf( "B: FAIL, VideoOut buffer registration" );
		return false;
	}
	printf( "B: VideoOut buffer registered" );

	cmdmem = memalloc_alloc( garlic, cmdsize, GNM_ALIGNMENT_BUFFER_BYTES );
	label = memalloc_alloc( garlic, sizeof( uint64_t ), sizeof( uint64_t ));
	if( !cmdmem || !label )
	{
		printf( "B: FAIL, garlic allocation" );
		return false;
	}

	start = sceKernelGetProcessTime();

	for( frame = 0; frame < FRAMES; frame++ )
	{
		// background cycles through colors, so a frozen picture is visible
		const float t = (float)( frame % 120 ) / 120.0f;
		const float clearcolor[4] = { t, 0.2f, 1.0f - t, 1.0f };
		const GnmPrimitiveSetup primsetup = {
			.cullmode = GNM_CULL_NONE,
			.frontface = GNM_FACE_CCW,
			.frontmode = GNM_FILL_SOLID,
			.backmode = GNM_FILL_SOLID,
			.provokemode = GNM_PROVOKINGVTX_FIRST,
		};
		GnmCommandBuffer cmd = gnmCmdInit( cmdmem, cmdsize, NULL, NULL );
		void *dcb[1];
		uint32_t dcbsize[1];
		uint64_t waited;
		int res;

		*label = 0;

		gnmDrawCmdInitDefaultHardwareState( &cmd );

		clearcolortarget( &cmd, &fb, clearcolor );
		gnmDrawCmdWaitGraphicsWrite( &cmd, GNM_ACQUIRE_TARGET_CB0 | GNM_ACQUIRE_TARGET_DB );

		gnmDrawCmdSetPrimitiveSetup( &cmd, &primsetup );
		gnmDrawCmdSetRenderTarget( &cmd, 0, &fb );
		gnmDrawCmdSetRenderTargetMask( &cmd, 0xf );
		setupviewport( &cmd, 0, 0, display.screenw, display.screenh, 0.5f, 0.5f );

		gnmDrawCmdSetVsShader( &cmd, &vs->registers, 0 );
		gnmDrawCmdSetPsShader( &cmd, &ps->registers );
		gnmDrawCmdSetPsInputUsage( &cmd, gnmVsShaderExportSemanticTable( vs ), vs->numexportsemantics,
			gnmPsShaderInputSemanticTable( ps ), ps->numinputsemantics );

		gnmDrawCmdSetPrimitiveType( &cmd, GNM_PT_TRILIST );
		gnmDrawCmdDrawIndexAuto( &cmd, 3 );

		gnmDrawCmdEventWriteEop( &cmd, GNM_CACHE_FLUSH_AND_INV_TS_EVENT, (uint64_t)(uintptr_t)label, GNM_DATA_SEL_SEND_DATA64, 1 );

		dcb[0] = cmd.beginptr;
		dcbsize[0] = (uint32_t)((uintptr_t)cmd.cmdptr - (uintptr_t)cmd.beginptr );

		res = sceGnmSubmitCommandBuffers( 1, dcb, dcbsize, NULL, NULL );
		if( frame == 0 || res < 0 )
			printf( "B: frame %d: command buffer %u bytes, submit = 0x%x", frame, dcbsize[0], res );
		if( res < 0 )
			return false;

		if( !wait_label( label, 1, &waited ))
		{
			printf( "B: FAIL, frame %d: GPU did not finish in %llu us", frame, (unsigned long long)waited );
			return false;
		}

		total_wait += waited;
		if( waited > max_wait )
			max_wait = waited;

		if( !displayctx_flip( &display, 0 ))
		{
			printf( "B: FAIL, frame %d: flip", frame );
			return false;
		}

		res = sceGnmSubmitDone();
		if( frame == 0 || res < 0 )
			printf( "B: frame %d: sceGnmSubmitDone = 0x%x", frame, res );

		// picture is on screen, the system splash can go away
		if( frame == 1 )
			printf( "B: hide splash = 0x%x", sceSystemServiceHideSplashScreen());

		if( frame % 120 == 0 )
			printf( "B: frame %d, GPU time %llu us", frame, (unsigned long long)waited );
	}

	printf( "B: OK, %d frames in %llu ms, GPU wait avg %llu us, max %llu us", FRAMES,
		(unsigned long long)(( sceKernelGetProcessTime() - start ) / 1000 ),
		(unsigned long long)( total_wait / FRAMES ), (unsigned long long)max_wait );

	displayctx_destroy( &display );
	return true;
}

int main( void )
{
	MemoryAllocator garlic;
	bool a, b = false;

	log_open();
	printf( "gnm-probe started" );
	log_system_info();

	garlic = memalloc_init( 64 * 1024 * 1024,
		ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE,
		ORBIS_KERNEL_WC_GARLIC );

	a = stage_bare_submit( &garlic );
	if( a )
		b = stage_triangle( &garlic );

	printf( "=== RESULT: stage A %s, stage B %s", a ? "OK" : "FAIL", a ? ( b ? "OK" : "FAIL" ) : "skipped" );
	exit( a && b ? 0 : 1 );
}
