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
int g_nTLTypeVersion[TL_TYPES] = { 1, 2, 3, 4, 5, 6 };
bool g_bTLShow;		// the menu's ON / OFF: off = the game's own trigger look, nothing of ours
std::vector<TLCustomTrigger_t> g_TLCT;
int g_nTLCTListVersion;

static int s_nVersion = TL_TYPES;	// mesh versions: every type and custom trigger gets its own number
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
	// clips: hidden until asked for (there are many), pink so they don't pass for triggers
	TLLook_t &c = g_TLLook[TL_CLIP];
	c.show = false;
	c.fill[0] = 255; c.fill[1] = 80; c.fill[2] = 200; c.fill[3] = 50;
	g_bTLShow = false;
	g_TLCustom[0] = 255; g_TLCustom[1] = 165; g_TLCustom[2] = 0;
	g_TLCT.clear();
	g_nTLCTListVersion++;
}

int TL_NewVersion() { return ++s_nVersion; }

void TL_LookChanged( int typeMask )
{
	for ( int t = 0; t < TL_TYPES; t++ )
		if ( typeMask & ( 1 << t ) )
			g_nTLTypeVersion[t] = TL_NewVersion();
	s_bDirty = true;
	s_flChanged = -1;	// TL_LookTick starts the clock
}

void TL_CTChanged( int index )
{
	if ( index >= 0 && index < (int)g_TLCT.size() )
		g_TLCT[index].version = TL_NewVersion();
	TL_LookChanged( 0 );
}

void TL_CTListChanged()
{
	g_nTLCTListVersion++;
	TL_LookChanged( 0 );
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

// "fill r g b a border r g b a width n" (and "show 0/1") from tok[i] on
static void ParseLook( char **tok, int i, int n, TLLook_t &l )
{
	while ( i < n )
	{
		if ( !TL_StrICmp( tok[i], "show" ) && i + 1 < n ) { l.show = atoi( tok[i + 1] ) != 0; i += 2; }
		else if ( !TL_StrICmp( tok[i], "fill" ) && i + 4 < n ) { for ( int k = 0; k < 4; k++ ) l.fill[k] = Byte( tok[i + 1 + k] ); i += 5; }
		else if ( !TL_StrICmp( tok[i], "border" ) && i + 4 < n ) { for ( int k = 0; k < 4; k++ ) l.border[k] = Byte( tok[i + 1 + k] ); i += 5; }
		else if ( !TL_StrICmp( tok[i], "width" ) && i + 1 < n ) { int w = atoi( tok[i + 1] ); l.width = w < 0 ? 0 : w > TL_MAX_WIDTH ? TL_MAX_WIDTH : w; i += 2; }
		else i++;
	}
}

std::string TL_CleanName( const std::string &name )
{
	std::string out;
	for ( char c : name )
		if ( c >= 32 && c < 127 && out.size() < TL_NAME_MAX )
			out += c;
	size_t a = out.find_first_not_of( ' ' ), b = out.find_last_not_of( ' ' );
	return a == std::string::npos ? "" : out.substr( a, b - a + 1 );
}

// lines: "triggers 0/1" (ON / OFF), "<type> show 1 fill r g b a border r g b a width n", "custom r g b",
// "ctname <name>" (the custom trigger on the line before) and
// "ct <map> <model> <mins x y z> <maxs x y z> fill ... border ... width n"; anything unknown is skipped
bool TL_LookLoad()
{
	TL_LookDefaults();
	FILE *f = fopen( TL_LookPath(), "r" );
	if ( !f )
		return false;
	char line[512];
	bool afterCT = false;		// the line before made a custom trigger (a ctname belongs to it)
	while ( fgets( line, sizeof( line ), f ) )
	{
		const bool ctBefore = afterCT;
		afterCT = false;
		if ( !strncmp( line, "ctname ", 7 ) )
		{
			std::string name = line + 7;
			while ( !name.empty() && ( name.back() == '\n' || name.back() == '\r' ) )
				name.pop_back();
			if ( ctBefore )
				g_TLCT.back().name = TL_CleanName( name );
			continue;
		}
		char *tok[32];
		int n = 0;
		for ( char *p = strtok( line, " \t\r\n" ); p && n < 32; p = strtok( NULL, " \t\r\n" ) )
			tok[n++] = p;
		if ( n < 1 || tok[0][0] == '#' )
			continue;
		if ( !TL_StrICmp( tok[0], "triggers" ) && n >= 2 )
		{
			g_bTLShow = atoi( tok[1] ) != 0;
			continue;
		}
		if ( !TL_StrICmp( tok[0], "custom" ) && n >= 4 )
		{
			for ( int k = 0; k < 3; k++ )
				g_TLCustom[k] = Byte( tok[1 + k] );
			continue;
		}
		if ( !TL_StrICmp( tok[0], "ct" ) && n >= 9 && atoi( tok[2] ) > 0 )
		{
			TLCustomTrigger_t ct;
			ct.map = tok[1];
			ct.model = atoi( tok[2] );
			ct.mins.Init( (float)atof( tok[3] ), (float)atof( tok[4] ), (float)atof( tok[5] ) );
			ct.maxs.Init( (float)atof( tok[6] ), (float)atof( tok[7] ), (float)atof( tok[8] ) );
			ct.look = g_TLLook[TL_OTHER];
			ct.look.show = true;
			ParseLook( tok, 9, n, ct.look );
			ct.version = TL_NewVersion();
			g_TLCT.push_back( ct );
			afterCT = true;
			continue;
		}
		int mask = TL_TypeMask( tok[0] );
		if ( !mask || mask == ( 1 << TL_TYPES ) - 1 )
			continue;
		ParseLook( tok, 1, n, g_TLLook[__builtin_ctz( mask )] );
	}
	fclose( f );
	for ( int t = 0; t < TL_TYPES; t++ )
		g_nTLTypeVersion[t] = TL_NewVersion();
	g_nTLCTListVersion++;
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
	fprintf( f, "triggers %d\n", g_bTLShow ? 1 : 0 );
	for ( int t = 0; t < TL_TYPES; t++ )
	{
		const TLLook_t &l = g_TLLook[t];
		fprintf( f, "%s show %d fill %d %d %d %d border %d %d %d %d width %d\n", g_TLTypeName[t], l.show ? 1 : 0, l.fill[0], l.fill[1],
				 l.fill[2], l.fill[3], l.border[0], l.border[1], l.border[2], l.border[3], l.width );
	}
	fprintf( f, "custom %d %d %d\n", g_TLCustom[0], g_TLCustom[1], g_TLCustom[2] );
	for ( const TLCustomTrigger_t &ct : g_TLCT )
	{
		const TLLook_t &l = ct.look;
		fprintf( f, "ct %s %d %.2f %.2f %.2f %.2f %.2f %.2f fill %d %d %d %d border %d %d %d %d width %d\n", ct.map.c_str(), ct.model,
				 ct.mins.x, ct.mins.y, ct.mins.z, ct.maxs.x, ct.maxs.y, ct.maxs.z, l.fill[0], l.fill[1], l.fill[2], l.fill[3],
				 l.border[0], l.border[1], l.border[2], l.border[3], l.width );
		if ( !ct.name.empty() )
			fprintf( f, "ctname %s\n", ct.name.c_str() );
	}
	bool ok = fclose( f ) == 0 && TLP_ReplaceFile( tmp, TL_LookPath() );
	if ( !ok )
		Warning( "triggerlook: can't write %s\n", TL_LookPath() );
	return ok;
}
