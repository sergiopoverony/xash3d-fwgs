/*
gnm_gl.h - OpenGL ES 2.0 subset over GNM for ref_gles3compat on PS4
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
#ifndef GNM_GL_H
#define GNM_GL_H

#include <stdbool.h>

// opens the display and creates the context, returns the screen size;
// flip_front swaps front face winding in case culling turns out inverted
bool PS4_GL_Init( int *width, int *height, bool flip_front );
void PS4_GL_Shutdown( void );
void PS4_GL_SwapBuffers( void );
void *PS4_GL_GetProcAddress( const char *name );

#endif // GNM_GL_H
