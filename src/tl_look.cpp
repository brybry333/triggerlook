//=============================================================================
// triggerlook: the look of each trigger type and its settings file; see tl_look.h
//=============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tl_look.h"
#include "tl_platform.h"
#include "tl_util.h"
#include "tier0/dbg.h"

TLLook_t g_TLLook[TL_TYPES];
int g_TLCustom[3] = { 255, 165, 0 };
int g_nTLTypeVersion[TL_TYPES] = { 1, 1, 1, 1, 1 };

static bool s_bDirty;
static double s_flChanged = -1;

// dark grey, about half see-through, no border: the stock look without its orange and its "TRIGGER" text
void TL_LookDefaults()
{
	for ( int t = 0; t < TL_TYPES; t++ )
	{
		TLLook_t &l = g_TLLook[t];
		l.show = true;
		l.fill[0] = l.fill[1] = l.fill[2] = 24;
		l.fill[3] = 115;
		l.border[0] = l.border[1] = l.border[2] = l.border[3] = 255;
		l.width = 0;
	}
	g_TLCustom[0] = 255; g_TLCustom[1] = 165; g_TLCustom[2] = 0;
}

void TL_LookChanged( int typeMask )
{
	for ( int t = 0; t < TL_TYPES; t++ )
		if ( typeMask & ( 1 << t ) )
			g_nTLTypeVersion[t]++;
	s_bDirty = true;
	s_flChanged = -1;	// TL_LookTick starts the clock
}

void TL_LookTick( double now )
{
	if ( !s_bDirty )
		return;
	if ( s_flChanged < 0 )
		s_flChanged = now;
	else if ( now - s_flChanged > 0.5 )
	{
		s_bDirty = false;
		TL_LookSave();
	}
}

void TL_LookFlush()
{
	if ( s_bDirty )
	{
		s_bDirty = false;
		TL_LookSave();
	}
}

const char *TL_LookPath()
{
	static char s_szPath[1024];
	if ( !s_szPath[0] )
		snprintf( s_szPath, sizeof( s_szPath ), "%s/settings.txt", TLP_ConfigDir() );
	return s_szPath;
}

int TL_TypeMask( const char *name )
{
	if ( !TL_StrICmp( name, "all" ) )
		return ( 1 << TL_TYPES ) - 1;
	for ( int t = 0; t < TL_TYPES; t++ )
		if ( !TL_StrICmp( name, g_TLTypeName[t] ) )
			return 1 << t;
	return 0;
}

static unsigned char Byte( const char *s ) { int v = atoi( s ); return (unsigned char)( v < 0 ? 0 : v > 255 ? 255 : v ); }

// lines: "<type> show 1 fill r g b a border r g b a width n" and "custom r g b"; anything unknown is skipped
bool TL_LookLoad()
{
	TL_LookDefaults();
	FILE *f = fopen( TL_LookPath(), "r" );
	if ( !f )
		return false;
	char line[512];
	while ( fgets( line, sizeof( line ), f ) )
	{
		char *tok[32];
		int n = 0;
		for ( char *p = strtok( line, " \t\r\n" ); p && n < 32; p = strtok( NULL, " \t\r\n" ) )
			tok[n++] = p;
		if ( n < 1 || tok[0][0] == '#' )
			continue;
		if ( !TL_StrICmp( tok[0], "custom" ) && n >= 4 )
		{
			for ( int k = 0; k < 3; k++ )
				g_TLCustom[k] = Byte( tok[1 + k] );
			continue;
		}
		int mask = TL_TypeMask( tok[0] );
		if ( !mask || mask == ( 1 << TL_TYPES ) - 1 )
			continue;
		TLLook_t &l = g_TLLook[__builtin_ctz( mask )];
		for ( int i = 1; i < n; )
		{
			if ( !TL_StrICmp( tok[i], "show" ) && i + 1 < n ) { l.show = atoi( tok[i + 1] ) != 0; i += 2; }
			else if ( !TL_StrICmp( tok[i], "fill" ) && i + 4 < n ) { for ( int k = 0; k < 4; k++ ) l.fill[k] = Byte( tok[i + 1 + k] ); i += 5; }
			else if ( !TL_StrICmp( tok[i], "border" ) && i + 4 < n ) { for ( int k = 0; k < 4; k++ ) l.border[k] = Byte( tok[i + 1 + k] ); i += 5; }
			else if ( !TL_StrICmp( tok[i], "width" ) && i + 1 < n ) { int w = atoi( tok[i + 1] ); l.width = w < 0 ? 0 : w > TL_MAX_WIDTH ? TL_MAX_WIDTH : w; i += 2; }
			else i++;
		}
	}
	fclose( f );
	for ( int t = 0; t < TL_TYPES; t++ )
		g_nTLTypeVersion[t]++;
	return true;
}

bool TL_LookSave()
{
	char tmp[1100];
	snprintf( tmp, sizeof( tmp ), "%s.tmp", TL_LookPath() );
	FILE *f = fopen( tmp, "w" );
	if ( !f )
	{
		Warning( "triggerlook: can't write %s\n", tmp );
		return false;
	}
	fprintf( f, "# triggerlook settings (the trigger menu writes this file)\n" );
	for ( int t = 0; t < TL_TYPES; t++ )
	{
		const TLLook_t &l = g_TLLook[t];
		fprintf( f, "%s show %d fill %d %d %d %d border %d %d %d %d width %d\n", g_TLTypeName[t], l.show ? 1 : 0, l.fill[0], l.fill[1],
				 l.fill[2], l.fill[3], l.border[0], l.border[1], l.border[2], l.border[3], l.width );
	}
	fprintf( f, "custom %d %d %d\n", g_TLCustom[0], g_TLCustom[1], g_TLCustom[2] );
	bool ok = fclose( f ) == 0 && TLP_ReplaceFile( tmp, TL_LookPath() );
	if ( !ok )
		Warning( "triggerlook: can't write %s\n", TL_LookPath() );
	return ok;
}
