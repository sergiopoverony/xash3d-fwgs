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
#include "vgui_draw.h"
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>

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

/*
==============================================================================

System keyboard

In the console and chat Cross opens the system keyboard dialog. Menu fields open
it themselves with the ps4_keyboard command and put their text into
ui_textinput_text before (mainui patch): only the menu knows that a field is
selected, text input stays enabled after leaving a field in some menus.
The dialog starts with the text being edited and replaces it.
The dialog works with UTF-16, our wchar_t is 32 bits, so buffers are uint16_t.

==============================================================================
*/
#define IME_MAX_TEXT 255

static struct
{
	qboolean wanted;       // engine has text input enabled
	qboolean open;         // dialog is running
	qboolean initialized;  // module loaded, common dialog initialized
	qboolean seen_running; // dialog reported running at least once
	uint16_t text[IME_MAX_TEXT + 1];
} ps4ime;

static CVAR_DEFINE_AUTO( ui_textinput_text, "", FCVAR_PRIVILEGED, "text of the focused menu field, set by the menu for the system keyboard" );

static CVAR_DEFINE_AUTO( ime_system_language, "0", FCVAR_ARCHIVE, "system keyboard also offers the system language; the keyboard then starts with it, not English" );

// IME languages (SceImeLanguage)
#define IME_LANGUAGE_DANISH         0x00000001ULL
#define IME_LANGUAGE_GERMAN         0x00000002ULL
#define IME_LANGUAGE_ENGLISH_US     0x00000004ULL
#define IME_LANGUAGE_SPANISH        0x00000008ULL
#define IME_LANGUAGE_FRENCH         0x00000010ULL
#define IME_LANGUAGE_ITALIAN        0x00000020ULL
#define IME_LANGUAGE_DUTCH          0x00000040ULL
#define IME_LANGUAGE_NORWEGIAN      0x00000080ULL
#define IME_LANGUAGE_POLISH         0x00000100ULL
#define IME_LANGUAGE_PORTUGUESE_PT  0x00000200ULL
#define IME_LANGUAGE_RUSSIAN        0x00000400ULL
#define IME_LANGUAGE_FINNISH        0x00000800ULL
#define IME_LANGUAGE_SWEDISH        0x00001000ULL
#define IME_LANGUAGE_JAPANESE       0x00002000ULL
#define IME_LANGUAGE_KOREAN         0x00004000ULL
#define IME_LANGUAGE_CHINESE_S      0x00008000ULL
#define IME_LANGUAGE_CHINESE_T      0x00010000ULL
#define IME_LANGUAGE_PORTUGUESE_BR  0x00020000ULL
#define IME_LANGUAGE_TURKISH        0x00080000ULL
#define IME_LANGUAGE_SPANISH_LA     0x00100000ULL

#define IME_OPTION_LANGUAGES_FORCED 0x00000008

#define SYSTEM_SERVICE_PARAM_ID_LANG 1

// system language (SceSystemParamLang) to its keyboard language
static uint64_t PS4_IME_SystemLanguage( void )
{
	static const uint64_t langs[] =
	{
		IME_LANGUAGE_JAPANESE, 0, IME_LANGUAGE_FRENCH, IME_LANGUAGE_SPANISH, IME_LANGUAGE_GERMAN,
		IME_LANGUAGE_ITALIAN, IME_LANGUAGE_DUTCH, IME_LANGUAGE_PORTUGUESE_PT, IME_LANGUAGE_RUSSIAN, IME_LANGUAGE_KOREAN,
		IME_LANGUAGE_CHINESE_T, IME_LANGUAGE_CHINESE_S, IME_LANGUAGE_FINNISH, IME_LANGUAGE_SWEDISH, IME_LANGUAGE_DANISH,
		IME_LANGUAGE_NORWEGIAN, IME_LANGUAGE_POLISH, IME_LANGUAGE_PORTUGUESE_BR, 0, IME_LANGUAGE_TURKISH,
		IME_LANGUAGE_SPANISH_LA, 0, IME_LANGUAGE_FRENCH,
	};
	int32_t lang = -1;

	if( sceSystemServiceParamGetInt( SYSTEM_SERVICE_PARAM_ID_LANG, &lang ) < 0 || lang < 0 || lang >= (int)ARRAYSIZE( langs ))
		return 0;

	return langs[lang];
}

static void PS4_SendAxis( engineAxis_t axis, short value );
static void PS4_IME_FromUtf8( uint16_t *out, size_t size, const char *in );

static qboolean PS4_IME_Init( void )
{
	int ret;

	if( ps4ime.initialized )
		return true;

	if(( ret = sceSysmoduleLoadModule( ORBIS_SYSMODULE_IME_DIALOG )) < 0 )
	{
		Con_Printf( S_ERROR "IME dialog module: 0x%08x\n", ret );
		return false;
	}

	// fails if somebody initialized it already, that's fine
	sceCommonDialogInitialize();
	ps4ime.initialized = true;
	return true;
}

static void PS4_IME_Open( void )
{
	static const uint16_t title[] = { 'X', 'a', 's', 'h', '3', 'D', 0 };
	OrbisImeDialogSetting param;
	int32_t user = -1;
	int ret;

	if( !PS4_IME_Init( ))
		return;

	sceUserServiceGetInitialUser( &user );
	PS4_IME_FromUtf8( ps4ime.text, IME_MAX_TEXT + 1, cls.key_dest == key_menu ? ui_textinput_text.string : Con_GetEditText( ));
	memset( &param, 0, sizeof( param ));
	param.userId = user;
	param.type = ORBIS_TYPE_DEFAULT;
	// the keyboard always starts with the system language if it's offered, English
	// first is only possible alone; without the forced option the list is ignored
	param.supportedLanguages = IME_LANGUAGE_ENGLISH_US;
	if( ime_system_language.value )
		param.supportedLanguages |= PS4_IME_SystemLanguage();
	param.option = IME_OPTION_LANGUAGES_FORCED;
	param.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
	param.maxTextLength = IME_MAX_TEXT;
	param.inputTextBuffer = (wchar_t *)ps4ime.text;
	param.posx = 1920.0f / 2.0f;
	param.posy = 1080.0f / 2.0f;
	param.horizontalAlignment = ORBIS_H_CENTER;
	param.verticalAlignment = ORBIS_V_CENTER;
	param.title = (const wchar_t *)title;

	if(( ret = sceImeDialogInit( &param, NULL )) < 0 )
	{
		Con_Printf( S_ERROR "sceImeDialogInit failed: 0x%08x\n", ret );
		return;
	}

	ps4ime.open = true;
	ps4ime.seen_running = false;

	// nothing reaches the game while the dialog takes the gamepad: held buttons
	// are released now, buttons still held when it closes don't press again
	for( size_t i = 0; i < ARRAYSIZE( ps4_buttons ); i++ )
	{
		if( FBitSet( ps4pad.buttons, ps4_buttons[i].mask ))
			Key_Event( ps4_buttons[i].key, false );
	}
	PS4_SendAxis( JOY_AXIS_SIDE, 0 );
	PS4_SendAxis( JOY_AXIS_FWD, 0 );
	PS4_SendAxis( JOY_AXIS_YAW, 0 );
	PS4_SendAxis( JOY_AXIS_PITCH, 0 );
	PS4_SendAxis( JOY_AXIS_LT, 0 );
	PS4_SendAxis( JOY_AXIS_RT, 0 );
}

static void PS4_IME_FromUtf8( uint16_t *out, size_t size, const char *in )
{
	const byte *p = (const byte *)in;
	size_t len = 0;

	while( p && *p && len + 2 < size )
	{
		uint32_t c = *p++;
		int extra = 0;

		if( c >= 0xf0 ) { c &= 0x07; extra = 3; }
		else if( c >= 0xe0 ) { c &= 0x0f; extra = 2; }
		else if( c >= 0xc0 ) { c &= 0x1f; extra = 1; }
		else if( c >= 0x80 ) continue; // stray continuation byte

		while( extra-- > 0 && ( *p & 0xc0 ) == 0x80 )
			c = ( c << 6 ) | ( *p++ & 0x3f );

		if( c >= 0x10000 )
		{
			c -= 0x10000;
			out[len++] = 0xd800 + ( c >> 10 );
			out[len++] = 0xdc00 + ( c & 0x3ff );
		}
		else out[len++] = c;
	}

	out[len] = 0;
}

static size_t PS4_IME_ToUtf8( char *out, size_t size, const uint16_t *in )
{
	size_t len = 0;

	while( *in && len + 5 < size )
	{
		uint32_t c = *in++;

		if( c >= 0xd800 && c < 0xdc00 && *in >= 0xdc00 && *in < 0xe000 )
			c = 0x10000 + (( c - 0xd800 ) << 10 ) + ( *in++ - 0xdc00 );

		if( c < 0x80 )
			out[len++] = c;
		else if( c < 0x800 )
		{
			out[len++] = 0xc0 | ( c >> 6 );
			out[len++] = 0x80 | ( c & 0x3f );
		}
		else if( c < 0x10000 )
		{
			out[len++] = 0xe0 | ( c >> 12 );
			out[len++] = 0x80 | (( c >> 6 ) & 0x3f );
			out[len++] = 0x80 | ( c & 0x3f );
		}
		else
		{
			out[len++] = 0xf0 | ( c >> 18 );
			out[len++] = 0x80 | (( c >> 12 ) & 0x3f );
			out[len++] = 0x80 | (( c >> 6 ) & 0x3f );
			out[len++] = 0x80 | ( c & 0x3f );
		}
	}

	out[len] = 0;
	return len;
}

static void PS4_IME_KeyPress( int key )
{
	Key_Event( key, true );
	Key_Event( key, false );
}

// typed text replaces the edited one: Ctrl+C (char 3) clears console, chat and
// menu fields, then the text goes in like text input events of SDL; console and
// chat send it right away
static void PS4_IME_Deliver( void )
{
	char text[IME_MAX_TEXT * 4 + 1];
	const qboolean menu = cls.key_dest == key_menu;
	const char *p;

	PS4_IME_ToUtf8( text, sizeof( text ), ps4ime.text );

	CL_CharEvent( 'c' - 'a' + 1 );

	VGui_ReportTextInput( text );

	for( p = text; *p; p++ )
	{
		int ch = (byte)*p;

		if( !cls.accept_utf8 )
			ch = Con_UtfProcessCharForce( ch );

		if( ch )
			CL_CharEvent( ch );
	}

	if( menu )
		Cvar_DirectSet( &ui_textinput_text, text );
	else if( text[0] )
		PS4_IME_KeyPress( K_ENTER );
}

static void PS4_IME_Update( void )
{
	OrbisDialogResult result;
	OrbisDialogStatus status = sceImeDialogGetStatus();

	// dialog may report "none" for a while after it's created and a stale state of
	// the previous one, it's done only when it finishes after running
	if( status == ORBIS_DIALOG_STATUS_RUNNING )
		ps4ime.seen_running = true;
	if( status != ORBIS_DIALOG_STATUS_STOPPED || !ps4ime.seen_running )
		return;

	memset( &result, 0, sizeof( result ));
	sceImeDialogGetResult( &result );
	sceImeDialogTerm();
	ps4ime.open = false;

	Con_Reportf( "IME dialog closed, result %d\n", result.endstatus );
	if( result.endstatus == ORBIS_DIALOG_OK )
		PS4_IME_Deliver();
}

// menu fields open the keyboard with this
static void PS4_IME_Command_f( void )
{
	if( !ps4ime.open )
		PS4_IME_Open();
}

void Platform_EnableTextInput( qboolean enable, int x, int y, int w, int h )
{
	ps4ime.wanted = enable;
}

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

	// the system keyboard has the gamepad, state is only tracked
	if( ps4ime.open )
	{
		PS4_IME_Update();
		ps4pad.buttons = data.buttons;
		return;
	}

	uint32_t changed = data.buttons ^ ps4pad.buttons;

	// Cross opens the system keyboard in the console and chat
	if( FBitSet( changed & data.buttons, ORBIS_PAD_BUTTON_CROSS ) && ps4ime.wanted && host.textmode
		&& ( cls.key_dest == key_console || cls.key_dest == key_message ))
	{
		PS4_IME_Open();
		ps4pad.buttons = data.buttons;
		return;
	}

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

	if( !Cvar_FindVar( ui_textinput_text.name ))
		Cvar_RegisterVariable( &ui_textinput_text );
	if( !Cvar_FindVar( ime_system_language.name ))
		Cvar_RegisterVariable( &ime_system_language );
	if( !Cmd_Exists( "ps4_keyboard" ))
		Cmd_AddCommand( "ps4_keyboard", PS4_IME_Command_f, "opens the system keyboard for the selected menu field" );

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
