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
//   B. triangle: render target state, a triangle drawn with shaders compiled
//      by opengnm-psbc and a clear are first submitted one by one
//   C. textured quad: uniform buffers in VS and PS, vertices pulled from a
//      storage buffer, texture with sampler; all of them are then drawn in
//      per-frame command buffers flipped to the screen
// Parts of stage B can be turned off with /data/xash/gnmprobe.cfg, see load_cfg().
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

static char probe_cfg[256];

// /data/xash/gnmprobe.cfg: words that turn parts of stage B off without rebuilding,
// "nodefault" - no default hardware state, "notri" - no triangle, "noclear" - no clear,
// "noquad" - no textured quad (stage C)
static void load_cfg( void )
{
	int fd = sceKernelOpen( LOG_DIR "/gnmprobe.cfg", O_RDONLY, 0 );
	int n;
	char *p;

	if( fd < 0 )
	{
		printf( "config: none" );
		return;
	}

	n = sceKernelRead( fd, probe_cfg, sizeof( probe_cfg ) - 1 );
	sceKernelClose( fd );
	probe_cfg[n > 0 ? n : 0] = 0;

	for( p = probe_cfg; *p; p++ )
	{
		if( *p == '\n' || *p == '\r' )
			*p = ' ';
	}

	printf( "config: %s", probe_cfg );
}

static bool cfg( const char *word )
{
	return strstr( probe_cfg, word ) != NULL;
}

// logs a heartbeat while waiting: if heartbeats stop, the process was killed or frozen
static bool wait_label( volatile uint64_t *label, uint64_t value, uint64_t *waited_us )
{
	uint64_t start = sceKernelGetProcessTime();
	uint64_t beat = start + 500000;

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

		if( now >= beat )
		{
			printf( "  still waiting, %llu ms, label 0x%llx", (unsigned long long)(( now - start ) / 1000 ), (unsigned long long)*label );
			beat += 500000;
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

// opengnm-psbc compiles shaders with address32_hi = 0: pointer to a descriptor table
// is a single 32-bit user SGPR, so the table must be mapped below 4 GB. Buffers and
// textures themselves can be anywhere, their descriptors hold full 48-bit addresses.
#define LOW_POOL_SIZE ( 64 * 1024 )
#define MAP_FIXED_NO_OVERWRITE ( 0x10 | 0x80 ) // SCE_KERNEL_MAP_FIXED | SCE_KERNEL_MAP_NO_OVERWRITE

typedef struct
{
	uint8_t *base;
	uint32_t used;
} lowpool_t;

static bool lowpool_init( lowpool_t *p )
{
	static const uint64_t hints[] = { 0x80000000ULL, 0x40000000ULL, 0xc0000000ULL, 0x10000000ULL };
	static const int flags[] = { 0, MAP_FIXED_NO_OVERWRITE };
	const int prot = ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE;
	off_t off = 0;
	size_t f, h;
	int res;

	res = sceKernelAllocateDirectMemory( 0, sceKernelGetDirectMemorySize(), LOW_POOL_SIZE, LOW_POOL_SIZE, ORBIS_KERNEL_WC_GARLIC, &off );
	if( res < 0 )
	{
		printf( "low pool: AllocateDirectMemory = 0x%x", res );
		return false;
	}

	for( f = 0; f < sizeof( flags ) / sizeof( flags[0] ); f++ )
	{
		for( h = 0; h < sizeof( hints ) / sizeof( hints[0] ); h++ )
		{
			void *addr = (void *)(uintptr_t)hints[h];

			res = sceKernelMapDirectMemory( &addr, LOW_POOL_SIZE, prot, flags[f], off, LOW_POOL_SIZE );
			printf( "low pool: map hint 0x%llx flags 0x%x = 0x%x, got %p", (unsigned long long)hints[h], flags[f], res, addr );
			if( res < 0 )
				continue;

			if( (uintptr_t)addr + LOW_POOL_SIZE <= 0x100000000ULL )
			{
				p->base = addr;
				p->used = 0;
				return true;
			}

			sceKernelMunmap( addr, LOW_POOL_SIZE );
		}
	}

	return false;
}

static void *lowpool_alloc( lowpool_t *p, uint32_t size, uint32_t align )
{
	uint32_t start = ( p->used + align - 1 ) & ~( align - 1 );

	if( start + size > LOW_POOL_SIZE )
		return NULL;

	p->used = start + size;
	return p->base + start;
}

// stage B: shaders, render target, flip
typedef struct
{
	DisplayContext display;
	GnmRenderTarget fb;
	GnmVsShader *vs;
	GnmPsShader *ps;
	GnmVsShader *fullvs;   // fullscreen triangle, firmware embedded VS faults on newer firmware
	GnmPsShader *clearps;
	int clearreg; // user SGPR with pointer to resource table of clear shader
	void *cmdmem;
	uint32_t cmdsize;
	volatile uint64_t *label;
	lowpool_t low; // descriptor tables, reused by every command buffer

	// stage C: textured quad with resources in both stages
	GnmVsShader *quadvs;
	GnmPsShader *quadps;
	int quadvsreg, quadpsreg;
	GnmTexture tex;
	GnmSampler samp;
	float *verts;
} scene_t;

static GnmCommandBuffer begin_cmd( scene_t *s )
{
	GnmCommandBuffer cmd = gnmCmdInit( s->cmdmem, s->cmdsize, NULL, NULL );

	// previous submit is finished by now, so its descriptor tables can be reused
	s->low.used = 0;

	if( !cfg( "nodefault" ))
		gnmDrawCmdInitDefaultHardwareState( &cmd );

	return cmd;
}

static void emit_target( scene_t *s, GnmCommandBuffer *cmd )
{
	const GnmPrimitiveSetup primsetup = {
		.cullmode = GNM_CULL_NONE,
		.frontface = GNM_FACE_CCW,
		.frontmode = GNM_FILL_SOLID,
		.backmode = GNM_FILL_SOLID,
		.provokemode = GNM_PROVOKINGVTX_FIRST,
	};

	gnmDrawCmdSetPrimitiveSetup( cmd, &primsetup );
	gnmDrawCmdSetRenderTarget( cmd, 0, &s->fb );
	gnmDrawCmdSetRenderTargetMask( cmd, 0xf );
	setupviewport( cmd, 0, 0, s->display.screenw, s->display.screenh, 0.5f, 0.5f );
}

static void emit_triangle( scene_t *s, GnmCommandBuffer *cmd )
{
	gnmDrawCmdSetVsShader( cmd, &s->vs->registers, 0 );
	gnmDrawCmdSetPsShader( cmd, &s->ps->registers );
	gnmDrawCmdSetPsInputUsage( cmd, gnmVsShaderExportSemanticTable( s->vs ), s->vs->numexportsemantics,
		gnmPsShaderInputSemanticTable( s->ps ), s->ps->numinputsemantics );
	gnmDrawCmdSetPrimitiveType( cmd, GNM_PT_TRILIST );
	gnmDrawCmdDrawIndexAuto( cmd, 3 );
}

// opengnm-psbc puts pointer to descriptor set 0 into a user SGPR chosen by the compiler,
// it's not always register 0, so it must be taken from the shader input usage table
static int find_table_register( const GnmInputUsageSlot *slots, unsigned count )
{
	unsigned i;

	for( i = 0; i < count; i++ )
	{
		if( slots[i].usagetype == GNM_SHINPUTUSAGE_PTR_INDIRECTRESOURCETABLE )
			return slots[i].startregister;
	}

	return -1;
}

// like clearcolortarget() from freegnm-examples, but with correct resource table register
// and our own fullscreen VS instead of the firmware embedded one
// parts of the clear, B3 adds them one by one to find which one kills the process
#define CLEAR_TABLE ( 1 << 0 ) // resource table pointer in user SGPR
#define CLEAR_ALLOC ( 1 << 1 ) // color data allocated inside the command buffer
#define CLEAR_DB    ( 1 << 2 ) // depth/stencil controls
#define CLEAR_WAIT  ( 1 << 3 ) // wait for color and depth writes
#define CLEAR_ALL   ( CLEAR_TABLE | CLEAR_ALLOC | CLEAR_DB | CLEAR_WAIT )

static void emit_clear( scene_t *s, GnmCommandBuffer *cmd, const float color[4], int parts )
{
	const GnmDbRenderControl dbrenderctrl = { 0 };
	const GnmDepthStencilControl depthstencilctrl = {
		.zfunc = GNM_DEPTH_COMPARE_NEVER,
		.stencilfunc = GNM_DEPTH_COMPARE_NEVER,
		.stencilbackfunc = GNM_DEPTH_COMPARE_NEVER,
	};
	int i;

	if( parts & CLEAR_DB )
	{
		gnmDrawCmdSetDbRenderControl( cmd, &dbrenderctrl );
		gnmDrawCmdSetDepthStencilControl( cmd, &depthstencilctrl );
	}

	gnmDrawCmdSetVsShader( cmd, &s->fullvs->registers, 0 );
	gnmDrawCmdSetPsShader( cmd, &s->clearps->registers );
	gnmDrawCmdSetPsInputUsage( cmd, gnmVsShaderExportSemanticTable( s->fullvs ), s->fullvs->numexportsemantics,
		gnmPsShaderInputSemanticTable( s->clearps ), s->clearps->numinputsemantics );

	// clear shader doesn't read resources for now (see shaders/clear.frag.glsl),
	// then only the allocation inside the command buffer is checked
	if(( parts & CLEAR_TABLE ) && s->clearreg >= 0 )
	{
		// table with the descriptor is below 4 GB, color data inside the command buffer or in the same pool
		float *colorbuf = ( parts & CLEAR_ALLOC ) ? gnmCmdAllocInside( cmd, sizeof( float ) * 4, 4 ) : lowpool_alloc( &s->low, sizeof( float ) * 4, 16 );
		GnmBuffer *table = lowpool_alloc( &s->low, sizeof( GnmBuffer ), 16 );

		for( i = 0; i < 4; i++ )
			colorbuf[i] = color[i];
		*table = gnmCreateConstBuffer( colorbuf, sizeof( float ) * 4 );
		gnmDrawCmdSetPointerUserData( cmd, GNM_STAGE_PS, s->clearreg, table );
	}
	else if( parts & CLEAR_ALLOC )
	{
		gnmCmdAllocInside( cmd, sizeof( float ) * 4, 4 );
	}

	setupviewport( cmd, 0, 0, s->fb.size.width, s->fb.size.height, 0.5f, 0.5f );
	gnmDrawCmdSetRenderTarget( cmd, 0, &s->fb );
	gnmDrawCmdSetRenderTargetMask( cmd, 0xf );

	gnmDrawCmdSetPrimitiveType( cmd, GNM_PT_TRILIST );
	gnmDrawCmdDrawIndexAuto( cmd, 3 );

	if( parts & CLEAR_WAIT )
		gnmDrawCmdWaitGraphicsWrite( cmd, GNM_ACQUIRE_TARGET_CB0 | GNM_ACQUIRE_TARGET_DB );
}

_Static_assert( sizeof( GnmBuffer ) == 16, "V# size" );
_Static_assert( sizeof( GnmTexture ) == 32, "T# size" );
_Static_assert( sizeof( GnmSampler ) == 16, "S# size" );

// writes one user SGPR: psbc passes descriptor table address in a single 32-bit SGPR,
// SetPointerUserData writes two and could overwrite the next shader argument
static void set_user_sgpr( GnmCommandBuffer *cmd, GnmShaderStage stage, uint32_t reg, uint32_t value )
{
	// SPI_SHADER_USER_DATA_PS_0 and SPI_SHADER_USER_DATA_VS_0 relative to SH register space
	const uint32_t base = stage == GNM_STAGE_PS ? 0x0c : 0x4c;

	if( cmd->cmdptr + 3 > cmd->endptr )
		return;

	cmd->cmdptr[0] = 0xc0017600; // PKT3( SET_SH_REG, 1 )
	cmd->cmdptr[1] = base + reg;
	cmd->cmdptr[2] = value;
	cmd->cmdptr += 3;
}

// buffer descriptor like radv makes: raw, size in bytes, works for uniform and storage buffers
static GnmBuffer raw_buffer( void *base, uint32_t size )
{
	GnmBuffer b = gnmCreateConstBuffer( base, size );

	b.stride = 0;
	b.numrecords = size;
	return b;
}

static bool init_quad( scene_t *s, MemoryAllocator *garlic )
{
	// two triangles, xy - position, zw - texture coordinates
	static const float quad[6][4] = {
		{ -1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, -1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 0.0f },
		{ -1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f, 0.0f },
	};
	const uint32_t size = 64;
	GnmTextureCreateInfo ci;
	uint32_t *pixels;
	GnmError err;
	uint32_t x, y;

	if( !loadvshader( &s->quadvs, garlic, "/app0/assets/misc/quad.vert.sb" ) || !loadpshader( &s->quadps, garlic, "/app0/assets/misc/quad.frag.sb" ))
	{
		printf( "C: FAIL, quad shaders not loaded" );
		return false;
	}

	s->quadvsreg = find_table_register( gnmVsShaderInputUsageSlotTable( s->quadvs ), s->quadvs->common.numinputusageslots );
	s->quadpsreg = find_table_register( gnmPsShaderInputUsageSlotTable( s->quadps ), s->quadps->common.numinputusageslots );
	printf( "C: quad shaders loaded, resource tables in VS SGPR %d, PS SGPR %d, vs exports %u, ps inputs %u",
		s->quadvsreg, s->quadpsreg, s->quadvs->numexportsemantics, s->quadps->numinputsemantics );
	if( s->quadvsreg < 0 || s->quadpsreg < 0 )
	{
		printf( "C: FAIL, quad shaders don't read resource tables" );
		return false;
	}

	// orange and dark gray checkerboard, 8x8 cells
	pixels = memalloc_alloc( garlic, size * size * 4, 256 );
	s->verts = memalloc_alloc( garlic, sizeof( quad ), 256 );
	if( !pixels || !s->verts )
	{
		printf( "C: FAIL, garlic allocation" );
		return false;
	}

	for( y = 0; y < size; y++ )
	{
		for( x = 0; x < size; x++ )
			pixels[y * size + x] = (( x / 8 + y / 8 ) & 1 ) ? 0xff008cffu : 0xff282828u; // A B G R
	}
	memcpy( s->verts, quad, sizeof( quad ));

	memset( &ci, 0, sizeof( ci ));
	ci.texturetype = GNM_TEXTURE_2D;
	ci.width = size;
	ci.height = size;
	ci.depth = 1;
	ci.pitch = size;
	ci.nummiplevels = 1;
	ci.numslices = 1;
	ci.format = GNM_FMT_R8G8B8A8_UNORM;
	ci.tilemodehint = GNM_TM_DISPLAY_LINEAR_GENERAL;
	ci.mingpumode = GNM_GPU_BASE;
	ci.numfragments = 1;

	err = gnmCreateTexture( &s->tex, &ci );
	if( err != GNM_ERROR_OK )
	{
		printf( "C: FAIL, texture: %s", gnmStrError( err ));
		return false;
	}
	gnmTexSetBaseAddress( &s->tex, pixels );

	memset( &s->samp, 0, sizeof( s->samp ));
	s->samp.clampx = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
	s->samp.clampy = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
	s->samp.clampz = GNM_TEX_CLAMP_CLAMP_LAST_TEXEL;
	s->samp.xymagfilter = GNM_FILTER_POINT;
	s->samp.xyminfilter = GNM_FILTER_POINT;
	s->samp.maxlod = 0xfff;

	printf( "C: texture %ux%u at %p, vertices at %p", size, size, (void *)pixels, (void *)s->verts );
	return true;
}

// descriptor set 0 layouts made by patched psbc (scripts/ps4/psbc):
//   quad.vert: binding 0 uniform buffer at 0, binding 1 storage buffer at 16
//   quad.frag: binding 0 uniform buffer at 0, binding 1 combined image sampler at 16 (T#) and 48 (S#)
static bool emit_quad( scene_t *s, GnmCommandBuffer *cmd, int frame )
{
	// moves left and right, color pulses, so per-frame uniform updates are visible
	const float t = (float)( frame % 240 ) / 120.0f;
	const float k = t < 1.0f ? t : 2.0f - t;
	const float aspect = (float)s->display.screenw / (float)s->display.screenh;
	float *vsdata = lowpool_alloc( &s->low, 16, 16 );
	float *psdata = lowpool_alloc( &s->low, 16, 16 );
	uint8_t *vstable = lowpool_alloc( &s->low, 32, 16 );
	uint8_t *pstable = lowpool_alloc( &s->low, 64, 16 );
	GnmBuffer b;

	if( !vsdata || !psdata || !vstable || !pstable )
		return false;

	vsdata[0] = 0.25f;
	vsdata[1] = 0.25f * aspect;
	vsdata[2] = -0.5f + k;
	vsdata[3] = -0.3f;

	psdata[0] = 1.0f;
	psdata[1] = 1.0f - 0.5f * k;
	psdata[2] = 0.5f + 0.5f * k;
	psdata[3] = 1.0f;

	b = raw_buffer( vsdata, 16 );
	memcpy( vstable + 0, &b, sizeof( b ));
	b = raw_buffer( s->verts, 6 * 16 );
	memcpy( vstable + 16, &b, sizeof( b ));

	b = raw_buffer( psdata, 16 );
	memcpy( pstable + 0, &b, sizeof( b ));
	memcpy( pstable + 16, &s->tex, sizeof( s->tex ));
	memcpy( pstable + 48, &s->samp, sizeof( s->samp ));

	gnmDrawCmdSetVsShader( cmd, &s->quadvs->registers, 0 );
	gnmDrawCmdSetPsShader( cmd, &s->quadps->registers );
	gnmDrawCmdSetPsInputUsage( cmd, gnmVsShaderExportSemanticTable( s->quadvs ), s->quadvs->numexportsemantics,
		gnmPsShaderInputSemanticTable( s->quadps ), s->quadps->numinputsemantics );
	set_user_sgpr( cmd, GNM_STAGE_VS, s->quadvsreg, (uint32_t)(uintptr_t)vstable );
	set_user_sgpr( cmd, GNM_STAGE_PS, s->quadpsreg, (uint32_t)(uintptr_t)pstable );

	gnmDrawCmdSetPrimitiveType( cmd, GNM_PT_TRILIST );
	gnmDrawCmdDrawIndexAuto( cmd, 6 );
	return true;
}

// name is logged before submit, so after a crash the last line tells what killed it
static bool submit_and_wait( scene_t *s, const char *name, GnmCommandBuffer *cmd, bool verbose, uint64_t *waited )
{
	void *dcb[1];
	uint32_t dcbsize[1];
	uint64_t w;
	int res;

	gnmDrawCmdEventWriteEop( cmd, GNM_CACHE_FLUSH_AND_INV_TS_EVENT, (uint64_t)(uintptr_t)s->label, GNM_DATA_SEL_SEND_DATA64, 1 );
	dcb[0] = cmd->beginptr;
	dcbsize[0] = (uint32_t)((uintptr_t)cmd->cmdptr - (uintptr_t)cmd->beginptr );
	*s->label = 0;

	if( verbose )
		printf( "%s: submitting %u bytes", name, dcbsize[0] );

	res = sceGnmSubmitCommandBuffers( 1, dcb, dcbsize, NULL, NULL );
	if( res < 0 )
	{
		printf( "%s: FAIL, submit = 0x%x", name, res );
		return false;
	}

	if( !wait_label( s->label, 1, &w ))
	{
		printf( "%s: FAIL, GPU did not finish in %llu us", name, (unsigned long long)w );
		return false;
	}

	res = sceGnmSubmitDone();
	if( res < 0 )
	{
		printf( "%s: FAIL, sceGnmSubmitDone = 0x%x", name, res );
		return false;
	}

	if( verbose )
		printf( "%s: OK in %llu us", name, (unsigned long long)w );
	if( waited )
		*waited = w;
	return true;
}

static bool stage_triangle( MemoryAllocator *garlic )
{
	static scene_t scene;
	scene_t *s = &scene;
	const bool tri = !cfg( "notri" ), clear = !cfg( "noclear" ), quad = !cfg( "noquad" );
	const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	uint64_t max_wait = 0, total_wait = 0;
	uint64_t start;
	GnmCommandBuffer cmd;
	int frame;

	printf( "=== stage B: triangle (default state %s, triangle %s, clear %s, quad %s)",
		cfg( "nodefault" ) ? "off" : "on", tri ? "on" : "off", clear ? "on" : "off", quad ? "on" : "off" );

	if( !loadvshader( &s->vs, garlic, "/app0/assets/misc/tri.vert.sb" ) || !loadpshader( &s->ps, garlic, "/app0/assets/misc/tri.frag.sb" )
		|| !loadvshader( &s->fullvs, garlic, "/app0/assets/misc/fullscreen.vert.sb" )
		|| !loadpshader( &s->clearps, garlic, "/app0/assets/misc/clear.frag.sb" ))
	{
		printf( "B: FAIL, shaders not loaded" );
		return false;
	}
	s->clearreg = find_table_register( gnmPsShaderInputUsageSlotTable( s->clearps ), s->clearps->common.numinputusageslots );
	printf( "B: shaders loaded, vs exports %u, ps inputs %u, clear resource table in user SGPR %d",
		s->vs->numexportsemantics, s->ps->numinputsemantics, s->clearreg );

	if( !displayctx_init( &s->display ))
	{
		printf( "B: FAIL, VideoOut init" );
		return false;
	}
	printf( "B: VideoOut handle %d, resolution %ux%u", s->display.videohandle, s->display.screenw, s->display.screenh );

	if( !initcolortarget( &s->fb, garlic, s->display.screenw, s->display.screenh, GNM_FMT_R8G8B8A8_SRGB, gnmGpuMode()))
	{
		printf( "B: FAIL, color render target" );
		return false;
	}
	printf( "B: render target %ux%u pitch %u at %p", s->fb.size.width, s->fb.size.height, gnmRtGetPitch( &s->fb ), gnmRtGetBaseAddr( &s->fb ));

	if( !displayctx_setrts( &s->display, &s->fb, 1 ))
	{
		printf( "B: FAIL, VideoOut buffer registration" );
		return false;
	}

	if( !lowpool_init( &s->low ))
	{
		printf( "B: FAIL, can't map memory for descriptor tables below 4 GB" );
		return false;
	}
	printf( "B: descriptor tables at %p", s->low.base );

	s->cmdsize = 1024 * 1024;
	s->cmdmem = memalloc_alloc( garlic, s->cmdsize, GNM_ALIGNMENT_BUFFER_BYTES );
	s->label = memalloc_alloc( garlic, sizeof( uint64_t ), sizeof( uint64_t ));
	if( !s->cmdmem || !s->label )
	{
		printf( "B: FAIL, garlic allocation" );
		return false;
	}

	// each part separately first
	cmd = begin_cmd( s );
	emit_target( s, &cmd );
	if( !submit_and_wait( s, "B1 state", &cmd, true, NULL ))
		return false;

	if( tri )
	{
		cmd = begin_cmd( s );
		emit_target( s, &cmd );
		emit_triangle( s, &cmd );
		if( !submit_and_wait( s, "B2 triangle", &cmd, true, NULL ))
			return false;
	}

	if( clear )
	{
		cmd = begin_cmd( s );
		emit_clear( s, &cmd, white, CLEAR_ALL );
		if( !submit_and_wait( s, "B3 clear", &cmd, true, NULL ))
			return false;
	}

	if( quad )
	{
		printf( "=== stage C: textured quad" );
		if( !init_quad( s, garlic ))
			return false;

		cmd = begin_cmd( s );
		emit_clear( s, &cmd, white, CLEAR_ALL );
		emit_target( s, &cmd );
		if( !emit_quad( s, &cmd, 0 ))
		{
			printf( "C: FAIL, descriptor pool is full" );
			return false;
		}
		if( !submit_and_wait( s, "C1 textured quad", &cmd, true, NULL ))
			return false;
	}

	// what is drawn so far goes to the screen
	if( !displayctx_flip( &s->display, 0 ))
	{
		printf( "B: FAIL, first flip" );
		return false;
	}
	printf( "B: first flip OK, hide splash = 0x%x", sceSystemServiceHideSplashScreen());

	start = sceKernelGetProcessTime();

	for( frame = 0; frame < FRAMES; frame++ )
	{
		// background cycles through colors, so a frozen picture is visible
		const float t = (float)( frame % 120 ) / 120.0f;
		const float color[4] = { t, 0.2f, 1.0f - t, 1.0f };
		uint64_t waited;

		cmd = begin_cmd( s );
		if( clear )
			emit_clear( s, &cmd, color, CLEAR_ALL );
		emit_target( s, &cmd );
		if( tri )
			emit_triangle( s, &cmd );
		if( quad )
			emit_quad( s, &cmd, frame );

		if( !submit_and_wait( s, "B4 frame", &cmd, frame == 0, &waited ))
		{
			printf( "B: failed at frame %d", frame );
			return false;
		}

		total_wait += waited;
		if( waited > max_wait )
			max_wait = waited;

		if( !displayctx_flip( &s->display, 0 ))
		{
			printf( "B: FAIL, frame %d: flip", frame );
			return false;
		}

		if( frame % 120 == 0 )
			printf( "B: frame %d, GPU time %llu us", frame, (unsigned long long)waited );
	}

	printf( "B: OK, %d frames in %llu ms, GPU wait avg %llu us, max %llu us", FRAMES,
		(unsigned long long)(( sceKernelGetProcessTime() - start ) / 1000 ),
		(unsigned long long)( total_wait / FRAMES ), (unsigned long long)max_wait );

	displayctx_destroy( &s->display );
	return true;
}

int main( void )
{
	MemoryAllocator garlic;
	bool a, b = false;

	log_open();
	printf( "gnm-probe started" );
	log_system_info();
	load_cfg();

	garlic = memalloc_init( 64 * 1024 * 1024,
		ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_READ | ORBIS_KERNEL_PROT_GPU_WRITE,
		ORBIS_KERNEL_WC_GARLIC );

	a = stage_bare_submit( &garlic );
	if( a )
		b = stage_triangle( &garlic );

	printf( "=== RESULT: stage A %s, stage B %s", a ? "OK" : "FAIL", a ? ( b ? "OK" : "FAIL" ) : "skipped" );
	exit( a && b ? 0 : 1 );
}
