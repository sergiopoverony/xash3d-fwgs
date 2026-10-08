/*
in_ps4.c - PlayStation 4 native gamepad input
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
OpenOrbis SDL2 joystick driver doesn't report sticks returning to center
and drops one of the triggers, so DualShock 4 is read directly through libScePad
*/

#include "platform/platform.h"
#include "input.h"
#include "keydefs.h"
#include "client.h"
#include <orbis/Pad.h>
#include <orbis/UserService.h>

#ifndef ORBIS_PAD_PORT_TYPE_STANDARD
#define ORBIS_PAD_PORT_TYPE_STANDARD 0
#endif

static const struct
{
	uint32_t mask;
	int key;
} ps4_buttons[] =
{
{ ORBIS_PAD_BUTTON_CROSS,     K_A_BUTTON },
{ ORBIS_PAD_BUTTON_CIRCLE,    K_B_BUTTON },
{ ORBIS_PAD_BUTTON_SQUARE,    K_X_BUTTON },
{ ORBIS_PAD_BUTTON_TRIANGLE,  K_Y_BUTTON },
{ ORBIS_PAD_BUTTON_L1,        K_L1_BUTTON },
{ ORBIS_PAD_BUTTON_R1,        K_R1_BUTTON },
{ ORBIS_PAD_BUTTON_L3,        K_LSTICK },
{ ORBIS_PAD_BUTTON_R3,        K_RSTICK },
{ ORBIS_PAD_BUTTON_OPTIONS,   K_START_BUTTON },
{ ORBIS_PAD_BUTTON_TOUCH_PAD, K_BACK_BUTTON },
{ ORBIS_PAD_BUTTON_UP,        K_DPAD_UP },
{ ORBIS_PAD_BUTTON_DOWN,      K_DPAD_DOWN },
{ ORBIS_PAD_BUTTON_LEFT,      K_DPAD_LEFT },
{ ORBIS_PAD_BUTTON_RIGHT,     K_DPAD_RIGHT },
};

static struct
{
	int32_t  handle;
	uint32_t buttons;
	short    axes[MAX_AXES];
	float    vibrate_until;
	qboolean vibrating;
} ps4pad = { -1 };

static short PS4_StickToAxis( uint8_t value )
{
	// 0..255 to -32768..32767, 128 is center
	int v = ((int)value - 128 ) * 256;

	return (short)bound( -32768, v, 32767 );
}

static short PS4_TriggerToAxis( uint8_t value )
{
	return (short)( value * 32767 / 255 );
}

static void PS4_SendAxis( engineAxis_t axis, short value )
{
	if( ps4pad.axes[axis] == value )
		return;

	ps4pad.axes[axis] = value;
	Joy_AxisMotionEvent( axis, value );
}

static void PS4_StopVibration( void )
{
	OrbisPadVibeParam vibe = { 0 };

	if( ps4pad.handle >= 0 && ps4pad.vibrating )
		scePadSetVibration( ps4pad.handle, &vibe );

	ps4pad.vibrating = false;
}

void PS4_InputUpdate( void )
{
	OrbisPadData data;

	if( ps4pad.handle < 0 )
		return;

	if( ps4pad.vibrating && host.realtime > ps4pad.vibrate_until )
		PS4_StopVibration( );

	memset( &data, 0, sizeof( data ));

	if( scePadReadState( ps4pad.handle, &data ) < 0 || !data.connected )
		return;

	uint32_t changed = data.buttons ^ ps4pad.buttons;

	for( size_t i = 0; changed && i < ARRAYSIZE( ps4_buttons ); i++ )
	{
		if( FBitSet( changed, ps4_buttons[i].mask ))
			Key_Event( ps4_buttons[i].key, FBitSet( data.buttons, ps4_buttons[i].mask ) ? true : false );
	}

	ps4pad.buttons = data.buttons;

	// same layout as SDL GameController code uses
	PS4_SendAxis( JOY_AXIS_SIDE, PS4_StickToAxis( data.leftStick.x ));
	PS4_SendAxis( JOY_AXIS_FWD, PS4_StickToAxis( data.leftStick.y ));
	PS4_SendAxis( JOY_AXIS_YAW, PS4_StickToAxis( data.rightStick.x ));
	PS4_SendAxis( JOY_AXIS_PITCH, PS4_StickToAxis( data.rightStick.y ));
	PS4_SendAxis( JOY_AXIS_LT, PS4_TriggerToAxis( data.analogButtons.l2 ));
	PS4_SendAxis( JOY_AXIS_RT, PS4_TriggerToAxis( data.analogButtons.r2 ));
}

int Platform_JoyInit( void )
{
	int32_t user = -1;
	int ret;

	if( ps4pad.handle >= 0 )
		return 1;

	Con_Reportf( "Joystick: PS4 libScePad\n" );

	if(( ret = scePadInit( )) < 0 )
	{
		Con_Printf( S_ERROR "scePadInit failed: 0x%08x\n", ret );
		return 0;
	}

	if(( ret = sceUserServiceGetInitialUser( &user )) < 0 )
	{
		Con_Printf( S_ERROR "sceUserServiceGetInitialUser failed: 0x%08x\n", ret );
		return 0;
	}

	ps4pad.handle = scePadOpen( user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL );

	// might be already opened by somebody else
	if( ps4pad.handle < 0 )
		ps4pad.handle = scePadGetHandle( user, ORBIS_PAD_PORT_TYPE_STANDARD, 0 );

	if( ps4pad.handle < 0 )
	{
		Con_Printf( S_ERROR "scePadOpen failed: 0x%08x\n", ps4pad.handle );
		return 0;
	}

	ps4pad.buttons = 0;
	memset( ps4pad.axes, 0, sizeof( ps4pad.axes ));

	return 1;
}

void Platform_JoyShutdown( void )
{
	PS4_StopVibration( );

	if( ps4pad.handle >= 0 )
		scePadClose( ps4pad.handle );

	ps4pad.handle = -1;
}

void Platform_CalibrateGamepadGyro( void )
{
}

void Platform_Vibrate2( float time, int val1, int val2, uint flags )
{
	OrbisPadVibeParam vibe;

	if( ps4pad.handle < 0 )
		return;

	if( val1 < 0 )
		val1 = 0xFFFF;
	if( val2 < 0 )
		val2 = 0xFFFF;

	vibe.lgMotor = (uint8_t)( bound( 0, val1, 0xFFFF ) >> 8 );
	vibe.smMotor = (uint8_t)( bound( 0, val2, 0xFFFF ) >> 8 );

	if( scePadSetVibration( ps4pad.handle, &vibe ) >= 0 )
	{
		ps4pad.vibrating = true;
		ps4pad.vibrate_until = host.realtime + time;
	}
}

void Platform_Vibrate( float time, char flags )
{
	Platform_Vibrate2( time, -1, -1, flags );
}
