/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl_scrn.c -- master for refresh, status bar, console, chat, notify, etc

#include "client.h"

#include <errno.h>
#include <limits.h>

qboolean	scr_initialized;		// ready to draw

cvar_t		*cl_timegraph;
cvar_t		*cl_debuggraph;
cvar_t		*cl_graphheight;
cvar_t		*cl_graphscale;
cvar_t		*cl_graphshift;

// ======================================================================
// Optional JSONL benchmark instrumentation (opt-in, see tests/benchmark/).
//
// IOQ3_BENCH_JSONL=1 enables it; IOQ3_BENCH_WARMUP and IOQ3_BENCH_SAMPLES
// select how many frames to run. Every completed active (CA_ACTIVE)
// SCR_UpdateScreen frame emits one JSONL sample to stdout:
//
//   {"event":"sample","producer_seconds":0.01,"finalize_seconds":0.01}
//
// producer_seconds is the CPU time spent producing the frame, from
// SCR_UpdateScreen entry to immediately before re.EndFrame.
// finalize_seconds is the time re.EndFrame takes, measured immediately
// after it returns. The first IOQ3_BENCH_WARMUP samples are warmup (the
// harness discards them); once IOQ3_BENCH_SAMPLES measured samples have
// been emitted the engine requests a normal quit. Startup (non-CA_ACTIVE)
// and recursively re-entered frames are never measured. When the
// environment variable is absent this is a complete no-op.
// ======================================================================

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/time.h>
#endif

typedef struct {
	qboolean	checked;	// environment read once
	qboolean	enabled;	// IOQ3_BENCH_JSONL is active
	qboolean	done;		// requested quit, stop measuring
	int		warmup;		// samples to emit before the measured ones
	int		samples;	// measured samples to emit, then quit
	int		count;		// samples emitted so far
	int		depth;		// > 0 while inside the outermost measured frame
	double		frameStart;	// SCR_UpdateScreen entry time
	double		beforeEnd;	// time immediately before re.EndFrame
} scrBenchState_t;

static scrBenchState_t scrBench;

/*
================
SCR_BenchNow

High-resolution wall clock in seconds. Windows uses the QPC; other
platforms use gettimeofday like the engine's Sys_Milliseconds does.
================
*/
static double SCR_BenchNow( void ) {
#if defined(_WIN32)
	static LARGE_INTEGER freq;
	LARGE_INTEGER counter;

	if( !freq.QuadPart ) {
		QueryPerformanceFrequency( &freq );
		if( !freq.QuadPart ) {
			freq.QuadPart = 1;	// fallback: raw counter ticks
		}
	}
	QueryPerformanceCounter( &counter );
	return (double)counter.QuadPart / (double)freq.QuadPart;
#else
	struct timeval tv;

	gettimeofday( &tv, NULL );
	return (double)tv.tv_sec + (double)tv.tv_usec * 0.000001;
#endif
}

/*
================
SCR_BenchEnvInt
================
*/
static int SCR_BenchEnvInt( const char *name, int def ) {
	const char *value = getenv( name );
	char *end;
	long parsed;

	// Keep hostile or accidental environment values from wrapping an int or
	// making the engine run an effectively unbounded benchmark. The harness
	// uses ordinary small counts; values outside this explicit bound fall back
	// to the safe default and are clamped again by SCR_BenchInit for negatives.
	if( !value || !*value ) {
		return def;
	}
	errno = 0;
	end = NULL;
	parsed = strtol( value, &end, 10 );
	if( errno == ERANGE || end == value || !end || *end != '\0' ||
		parsed < (long)-1000000 || parsed > (long)1000000 ) {
		return def;
	}
	if( parsed < (long)INT_MIN || parsed > (long)INT_MAX ) {
		return def;
	}
	return (int)parsed;
}

/*
================
SCR_BenchInit

Read the IOQ3_BENCH_* environment once, on the first frame.
================
*/
static void SCR_BenchInit( void ) {
	const char *value = getenv( "IOQ3_BENCH_JSONL" );

	scrBench.enabled = qfalse;
	if( !value || !*value || !strcmp( value, "0" ) ) {
		return;
	}
	scrBench.enabled = qtrue;
	scrBench.warmup = SCR_BenchEnvInt( "IOQ3_BENCH_WARMUP", 0 );
	scrBench.samples = SCR_BenchEnvInt( "IOQ3_BENCH_SAMPLES", 1 );
	if( scrBench.warmup < 0 ) {
		scrBench.warmup = 0;
	}
	if( scrBench.samples < 1 ) {
		scrBench.samples = 1;
	}
}

/*
================
SCR_BenchFrameBegin

Start timing the frame. Only outermost, active (CA_ACTIVE) frames are
measured; startup and recursively re-entered frames are skipped.
================
*/
static void SCR_BenchFrameBegin( void ) {
	if( !scrBench.checked ) {
		scrBench.checked = qtrue;
		SCR_BenchInit();
	}
	if( !scrBench.enabled || scrBench.done ) {
		return;
	}
	if( clc.state != CA_ACTIVE ) {
		return;			// startup guard: not in a game yet
	}
	if( scrBench.depth++ > 0 ) {
		return;			// recursive frame: the outer frame owns the sample
	}
	scrBench.frameStart = SCR_BenchNow();
}

/*
================
SCR_BenchBeforeEndFrame

The producer phase ends immediately before re.EndFrame.
================
*/
static void SCR_BenchBeforeEndFrame( void ) {
	if( scrBench.depth != 1 || scrBench.done ) {
		return;
	}
	scrBench.beforeEnd = SCR_BenchNow();
}

/*
================
SCR_BenchAfterEndFrame

The finalize phase ends immediately after re.EndFrame returns. Emits the
JSONL sample and requests a normal engine quit once the requested number
of samples has been reached.
================
*/
static void SCR_BenchAfterEndFrame( void ) {
	char	line[256];
	double	now;
	double	producer;
	double	finalize;

	if( scrBench.depth != 1 || scrBench.done ) {
		return;
	}
	now = SCR_BenchNow();

	producer = now - scrBench.frameStart;
	finalize = now - scrBench.beforeEnd;
	if( producer < 0.0 ) {
		producer = 0.0;
	}
	if( finalize < 0.0 ) {
		finalize = 0.0;
	}

	Com_sprintf( line, sizeof( line ),
		"{\"event\":\"sample\",\"producer_seconds\":%.9f,\"finalize_seconds\":%.9f}\n",
		producer, finalize );
	fputs( line, stdout );
	fflush( stdout );

	scrBench.count++;
	if( scrBench.count >= scrBench.warmup + scrBench.samples ) {
		scrBench.done = qtrue;
		Cbuf_AddText( "quit\n" );	// normal engine quit
	}
}

/*
================
SCR_BenchFrameEnd

Leave the outermost measured frame.
================
*/
static void SCR_BenchFrameEnd( void ) {
	if( !scrBench.enabled ) {
		return;
	}
	if( scrBench.depth > 0 ) {
		scrBench.depth--;
	}
}

/*
================
SCR_DrawNamedPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawNamedPic( float x, float y, float width, float height, const char *picname ) {
	qhandle_t	hShader;

	assert( width != 0 );

	hShader = re.RegisterShader( picname );
	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}


/*
================
SCR_AdjustFrom640

Adjusted for resolution and screen aspect ratio
================
*/
void SCR_AdjustFrom640( float *x, float *y, float *w, float *h ) {
	float	xscale;
	float	yscale;

#if 0
		// adjust for wide screens
		if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
			*x += 0.5 * ( cls.glconfig.vidWidth - ( cls.glconfig.vidHeight * 640 / 480 ) );
		}
#endif

	// scale for screen sizes
	xscale = cls.glconfig.vidWidth / 640.0;
	yscale = cls.glconfig.vidHeight / 480.0;
	if ( x ) {
		*x *= xscale;
	}
	if ( y ) {
		*y *= yscale;
	}
	if ( w ) {
		*w *= xscale;
	}
	if ( h ) {
		*h *= yscale;
	}
}

/*
================
SCR_FillRect

Coordinates are 640*480 virtual values
=================
*/
void SCR_FillRect( float x, float y, float width, float height, const float *color ) {
	re.SetColor( color );

	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 0, 0, cls.whiteShader );

	re.SetColor( NULL );
}


/*
================
SCR_DrawPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawPic( float x, float y, float width, float height, qhandle_t hShader ) {
	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}



/*
** SCR_DrawChar
** chars are drawn at 640*480 virtual screen size
*/
static void SCR_DrawChar( int x, int y, float size, int ch ) {
	int row, col;
	float frow, fcol;
	float	ax, ay, aw, ah;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -size ) {
		return;
	}

	ax = x;
	ay = y;
	aw = size;
	ah = size;
	SCR_AdjustFrom640( &ax, &ay, &aw, &ah );

	row = ch>>4;
	col = ch&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	re.DrawStretchPic( ax, ay, aw, ah,
					   fcol, frow, 
					   fcol + size, frow + size, 
					   cls.charSetShader );
}

/*
** SCR_DrawSmallChar
** small chars are drawn at native screen resolution
*/
void SCR_DrawSmallChar( int x, int y, int ch ) {
	int row, col;
	float frow, fcol;
	float size;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -g_smallchar_height ) {
		return;
	}

	row = ch>>4;
	col = ch&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	re.DrawStretchPic( x, y, g_smallchar_width, g_smallchar_height,
					   fcol, frow, 
					   fcol + size, frow + size, 
					   cls.charSetShader );
}


/*
==================
SCR_DrawBigString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.

Coordinates are at 640 by 480 virtual resolution
==================
*/
void SCR_DrawStringExt( int x, int y, float size, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the drop shadow
	color[0] = color[1] = color[2] = 0;
	color[3] = setColor[3];
	re.SetColor( color );
	s = string;
	xx = x;
	while ( *s ) {
		if ( !noColorEscape && Q_IsColorString( s ) ) {
			s += 2;
			continue;
		}
		SCR_DrawChar( xx+2, y+2, size, *s );
		xx += size;
		s++;
	}


	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				Com_Memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawChar( xx, y, size, *s );
		xx += size;
		s++;
	}
	re.SetColor( NULL );
}


void SCR_DrawBigString( int x, int y, const char *s, float alpha, qboolean noColorEscape ) {
	float	color[4];

	color[0] = color[1] = color[2] = 1.0;
	color[3] = alpha;
	SCR_DrawStringExt( x, y, BIGCHAR_WIDTH, s, color, qfalse, noColorEscape );
}

void SCR_DrawBigStringColor( int x, int y, const char *s, vec4_t color, qboolean noColorEscape ) {
	SCR_DrawStringExt( x, y, BIGCHAR_WIDTH, s, color, qtrue, noColorEscape );
}


/*
==================
SCR_DrawSmallString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.
==================
*/
void SCR_DrawSmallStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				Com_Memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawSmallChar( xx, y, *s );
		xx += g_smallchar_width;
		s++;
	}
	re.SetColor( NULL );
}



/*
** SCR_Strlen -- skips color escape codes
*/
static int SCR_Strlen( const char *str ) {
	const char *s = str;
	int count = 0;

	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
		} else {
			count++;
			s++;
		}
	}

	return count;
}

/*
** SCR_GetBigStringWidth
*/ 
int	SCR_GetBigStringWidth( const char *str ) {
	return SCR_Strlen( str ) * BIGCHAR_WIDTH;
}


//===============================================================================

/*
=================
SCR_DrawDemoRecording
=================
*/
void SCR_DrawDemoRecording( void ) {
	char	string[1024];
	int		pos;

	if ( !clc.demorecording ) {
		return;
	}
	if ( clc.spDemoRecording ) {
		return;
	}

	pos = FS_FTell( clc.demofile );
	sprintf( string, "RECORDING %s: %ik", clc.demoName, pos / 1024 );

	SCR_DrawStringExt( 320 - strlen( string ) * 4, 20, 8, string, g_color_table[7], qtrue, qfalse );
}


#ifdef USE_VOIP
/*
=================
SCR_DrawVoipMeter
=================
*/
void SCR_DrawVoipMeter( void ) {
	char	buffer[16];
	char	string[256];
	int limit, i;

	if (!cl_voipShowMeter->integer)
		return;  // player doesn't want to show meter at all.
	else if (!cl_voipSend->integer)
		return;  // not recording at the moment.
	else if (clc.state != CA_ACTIVE)
		return;  // not connected to a server.
	else if (!clc.voipEnabled)
		return;  // server doesn't support VoIP.
	else if (clc.demoplaying)
		return;  // playing back a demo.
	else if (!cl_voip->integer)
		return;  // client has VoIP support disabled.

	limit = (int) (clc.voipPower * 10.0f);
	if (limit > 10)
		limit = 10;

	for (i = 0; i < limit; i++)
		buffer[i] = '*';
	while (i < 10)
		buffer[i++] = ' ';
	buffer[i] = '\0';

	sprintf( string, "VoIP: [%s]", buffer );
	SCR_DrawStringExt( 320 - strlen( string ) * 4, 10, 8, string, g_color_table[7], qtrue, qfalse );
}
#endif




/*
===============================================================================

DEBUG GRAPH

===============================================================================
*/

static	int			current;
static	float		values[1024];

/*
==============
SCR_DebugGraph
==============
*/
void SCR_DebugGraph (float value)
{
	values[current] = value;
	current = (current + 1) % ARRAY_LEN(values);
}

/*
==============
SCR_DrawDebugGraph
==============
*/
void SCR_DrawDebugGraph (void)
{
	int		a, x, y, w, i, h;
	float	v;

	//
	// draw the graph
	//
	w = cls.glconfig.vidWidth;
	x = 0;
	y = cls.glconfig.vidHeight;
	re.SetColor( g_color_table[0] );
	re.DrawStretchPic(x, y - cl_graphheight->integer, 
		w, cl_graphheight->integer, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );

	for (a=0 ; a<w ; a++)
	{
		i = (ARRAY_LEN(values)+current-1-(a % ARRAY_LEN(values))) % ARRAY_LEN(values);
		v = values[i];
		v = v * cl_graphscale->integer + cl_graphshift->integer;
		
		if (v < 0)
			v += cl_graphheight->integer * (1+(int)(-v / cl_graphheight->integer));
		h = (int)v % cl_graphheight->integer;
		re.DrawStretchPic( x+w-1-a, y - h, 1, h, 0, 0, 0, 0, cls.whiteShader );
	}
}

//=============================================================================

/*
==================
SCR_Init
==================
*/
void SCR_Init( void ) {
	cl_timegraph = Cvar_Get ("timegraph", "0", CVAR_CHEAT);
	cl_debuggraph = Cvar_Get ("debuggraph", "0", CVAR_CHEAT);
	cl_graphheight = Cvar_Get ("graphheight", "32", CVAR_CHEAT);
	cl_graphscale = Cvar_Get ("graphscale", "1", CVAR_CHEAT);
	cl_graphshift = Cvar_Get ("graphshift", "0", CVAR_CHEAT);

	scr_initialized = qtrue;
}


//=======================================================

/*
==================
SCR_DrawScreenField

This will be called twice if rendering in stereo mode
==================
*/
void SCR_DrawScreenField( stereoFrame_t stereoFrame ) {
	qboolean uiFullscreen;

	re.BeginFrame( stereoFrame );

	uiFullscreen = (uivm && VM_Call( uivm, UI_IS_FULLSCREEN ));

	// wide aspect ratio screens need to have the sides cleared
	// unless they are displaying game renderings
	if ( uiFullscreen || clc.state < CA_LOADING ) {
		if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
			re.SetColor( g_color_table[0] );
			re.DrawStretchPic( 0, 0, cls.glconfig.vidWidth, cls.glconfig.vidHeight, 0, 0, 0, 0, cls.whiteShader );
			re.SetColor( NULL );
		}
	}

	// if the menu is going to cover the entire screen, we
	// don't need to render anything under it
	if ( uivm && !uiFullscreen ) {
		switch( clc.state ) {
		default:
			Com_Error( ERR_FATAL, "SCR_DrawScreenField: bad clc.state" );
			break;
		case CA_CINEMATIC:
			SCR_DrawCinematic();
			break;
		case CA_DISCONNECTED:
			// force menu up
			S_StopAllSounds();
			VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			break;
		case CA_CONNECTING:
		case CA_CHALLENGING:
		case CA_CONNECTED:
			// connecting clients will only show the connection dialog
			// refresh to update the time
			VM_Call( uivm, UI_REFRESH, cls.realtime );
			VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, qfalse );
			break;
		case CA_LOADING:
		case CA_PRIMED:
			// draw the game information screen and loading progress
			CL_CGameRendering(stereoFrame);

			// also draw the connection information, so it doesn't
			// flash away too briefly on local or lan games
			// refresh to update the time
			VM_Call( uivm, UI_REFRESH, cls.realtime );
			VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, qtrue );
			break;
		case CA_ACTIVE:
			// always supply STEREO_CENTER as vieworg offset is now done by the engine.
			CL_CGameRendering(stereoFrame);
			SCR_DrawDemoRecording();
#ifdef USE_VOIP
			SCR_DrawVoipMeter();
#endif
			break;
		}
	}

	// the menu draws next
	if ( Key_GetCatcher( ) & KEYCATCH_UI && uivm ) {
		VM_Call( uivm, UI_REFRESH, cls.realtime );
	}

	// console draws next
	Con_DrawConsole ();

	// debug graph can be drawn on top of anything
	if ( cl_debuggraph->integer || cl_timegraph->integer || cl_debugMove->integer ) {
		SCR_DrawDebugGraph ();
	}
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen( void ) {
	static int	recursive;

	if ( !scr_initialized ) {
		return;				// not initialized yet
	}

	if ( ++recursive > 2 ) {
		Com_Error( ERR_FATAL, "SCR_UpdateScreen: recursively called" );
	}
	recursive = 1;

	SCR_BenchFrameBegin();

	// If there is no VM, there are also no rendering commands issued. Stop the renderer in
	// that case.
	if( uivm || com_dedicated->integer )
	{
		// XXX
		int in_anaglyphMode = Cvar_VariableIntegerValue("r_anaglyphMode");
		// if running in stereo, we need to draw the frame twice
		if ( cls.glconfig.stereoEnabled || in_anaglyphMode) {
			SCR_DrawScreenField( STEREO_LEFT );
			SCR_DrawScreenField( STEREO_RIGHT );
		} else {
			SCR_DrawScreenField( STEREO_CENTER );
		}

		SCR_BenchBeforeEndFrame();
		if ( com_speeds->integer ) {
			re.EndFrame( &time_frontend, &time_backend );
		} else {
			re.EndFrame( NULL, NULL );
		}
		SCR_BenchAfterEndFrame();
	}
	
	SCR_BenchFrameEnd();
	recursive = 0;
}

