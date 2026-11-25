//=============================================================================
// triggerlook: the custom skybox's pictures and sprays (see tl_pics.h).
//
// Adding a file runs MareTF in a background thread (p2paint's way, p2paint_pics.cpp):
//   1. decode it stretched to powers of two, RGBA, every frame      maretf create -f RGBA8888 --width W --height H
//   2. (animations over the budget) every n-th frame only
//   3. DXT1 + mipmaps, clamped edges (a 2:1 panorama wraps across)  maretf edit --set-format DXT1 --recompute-mips
// Sizes: each side the nearest power of two, at most 2048 (a 2:1 panorama 2048 x 1024); animations at most 32 MB
// (smaller, then fewer frames) and the engine's VTF rule log2 w + log2 h + log2 frames <= 25. Memory stays small: the
// picture is read whole only up to 64 MB, MareTF's decode is planned under 256 MB and read one kept frame at a time. The picture's own
// aspect is kept in its VMT (a comment line): the sky crops the stretched texture back to it.
//=============================================================================
#include "tl_pics.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>

#include "tl_platform.h"

static const long long MAX_ANIM_BYTES = 32ll << 20;	// an animation's VTF
static const long long MAX_VTF_FILE = 64ll << 20;		// a VTF taken as it is
static const long long MAX_PROBE_BYTES = 64ll << 20;	// a picture read whole to find its size and frames
static const long long MAX_DECODE_BYTES = 256ll << 20;	// MareTF's RGBA decode of all its frames (a temporary file)
static const long long MAX_SOURCE_BYTES = 1ll << 30;	// all frames at the picture's own size: MareTF holds about twice
														// that while it works (a 3000-frame 480 x 270 GIF: 3 GB)
static const int MAX_SIDE = 2048, MIN_SIDE = 64, MIN_ANIM_SIDE = 256;
static const float SPRAY_FPS = 5.0f;					// Source plays sprays at 5 fps

static std::string s_SkyDir, s_LogoDir;
static std::vector<TLPic_t> s_Pics, s_Sprays;
static std::mutex s_Lock;
static TLPicStatus_t s_Status;
static bool s_bBusy;

static bool ReadFile( const std::string &path, std::vector<uint8_t> &out, size_t limit = (size_t)-1 )
{
	FILE *f = TLP_Open( path.c_str(), "rb" );
	if ( !f )
		return false;
	out.clear();
	const long long size = TLP_FileSize( f );
	out.reserve( (size_t)std::max( 0ll, std::min( size, (long long)std::min( limit, (size_t)1 << 30 ) ) ) );	// (no doubling)
	uint8_t buf[65536];
	size_t n;
	while ( out.size() < limit && ( n = fread( buf, 1, std::min( sizeof( buf ), limit - out.size() ), f ) ) > 0 )
		out.insert( out.end(), buf, buf + n );
	const bool ok = !ferror( f );
	fclose( f );
	return ok;
}

// <path>.tmp, then renamed: the game never sees half a file
static bool WriteFile( const std::string &path, const void *data, size_t size )
{
	const std::string tmp = path + ".tmp";
	FILE *f = TLP_Open( tmp.c_str(), "wb" );
	if ( !f )
		return false;
	bool ok = fwrite( data, 1, size, f ) == size;
	ok = fclose( f ) == 0 && ok;
	if ( !ok || !TLP_ReplaceFile( tmp.c_str(), path.c_str() ) )
	{
		TLP_Remove( tmp.c_str() );
		return false;
	}
	return true;
}

static std::string Lower( std::string s )
{
	for ( char &c : s )
		if ( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';
	return s;
}
static std::string BaseName( const std::string &p ) { size_t s = p.find_last_of( "/\\" ); return s == std::string::npos ? p : p.substr( s + 1 ); }
static std::string Stem( const std::string &f ) { size_t d = f.rfind( '.' ); return d == std::string::npos || !d ? f : f.substr( 0, d ); }
static bool EndsWith( const std::string &s, const char *e ) { size_t n = strlen( e ); return s.size() > n && !Lower( s.substr( s.size() - n ) ).compare( e ); }
static int Log2( int v ) { int r = 0; while ( v > 1 ) { v >>= 1; r++; } return r; }
static uint16_t LE16( const uint8_t *p ) { return p[0] | p[1] << 8; }
static uint32_t LE24( const uint8_t *p ) { return p[0] | p[1] << 8 | p[2] << 16; }
static uint32_t LE32( const uint8_t *p ) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t BE16( const uint8_t *p ) { return p[0] << 8 | p[1]; }
static uint32_t BE32( const uint8_t *p ) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// the power of two nearest to v (log scale), within lo..hi
static int NearPow2( int v, int lo, int hi )
{
	int p = lo;
	while ( p < hi && p * 1.41421356f < v )
		p <<= 1;
	return p;
}

//-----------------------------------------------------------------------------
// VTF headers
//-----------------------------------------------------------------------------
struct VtfInfo_t { int headerSize = 0, width = 0, height = 0, frames = 1, depth = 1, format = -1, lowFormat = -1, lowW = 0, lowH = 0; };

static bool ParseVtf( const uint8_t *d, size_t n, VtfInfo_t &v )
{
	if ( n < 64 || memcmp( d, "VTF\0", 4 ) || LE32( d + 4 ) != 7 || LE32( d + 8 ) > 5 )
		return false;
	v.headerSize = LE32( d + 12 );
	v.width = LE16( d + 16 );
	v.height = LE16( d + 18 );
	v.frames = std::max( 1, (int)LE16( d + 24 ) );
	v.format = (int)LE32( d + 52 );
	v.lowFormat = (int)LE32( d + 57 );
	v.lowW = d[61];
	v.lowH = d[62];
	v.depth = LE32( d + 8 ) >= 2 && n >= 65 ? std::max( 1, (int)LE16( d + 63 ) ) : 1;
	return v.width > 0 && v.height > 0;
}

static bool ReadVtfInfo( const std::string &path, VtfInfo_t &v )
{
	std::vector<uint8_t> d;
	return ReadFile( path, d, 80 ) && ParseVtf( d.data(), d.size(), v );
}

// what CS:S's VTF loader takes (bigger shows as an error texture)
static bool EngineCanLoad( const VtfInfo_t &v ) { return Log2( v.width ) + Log2( v.height ) + Log2( v.frames ) + Log2( v.depth ) <= 25; }

// 7.2 header, no thumbnail: 80 bytes, then one mip of frames x RGBA8888
static void WriteVtfHeader( uint8_t *h, int w, int hgt, int frames )
{
	memset( h, 0, 80 );
	memcpy( h, "VTF\0", 4 );
	h[4] = 7; h[8] = 2; h[12] = 80;
	h[16] = w & 255; h[17] = w >> 8;
	h[18] = hgt & 255; h[19] = hgt >> 8;
	const uint32_t flags = 0x100 | 0x200;	// no mip (yet), no LOD
	memcpy( h + 20, &flags, 4 );
	h[24] = frames & 255; h[25] = frames >> 8;
	const float refl[3] = { 0.5f, 0.5f, 0.5f }, bump = 1.0f;
	memcpy( h + 32, refl, 12 );
	memcpy( h + 48, &bump, 4 );
	h[56] = 1;					// mips
	memset( h + 57, 0xff, 4 );	// no low-res image
	h[63] = 1;					// depth
}

//-----------------------------------------------------------------------------
// image headers: size, frames, frame rate (without decoding)
//-----------------------------------------------------------------------------
struct ImageInfo_t { int w = 0, h = 0, frames = 1; float fps = 10.0f; bool known = false; };

static void ParseGif( const std::vector<uint8_t> &d, ImageInfo_t &info )
{
	size_t n = d.size(), p = 13;
	info.w = LE16( &d[6] );
	info.h = LE16( &d[8] );
	if ( d[10] & 0x80 )
		p += 3u << ( ( d[10] & 7 ) + 1 );
	int frames = 0, delays = 0;
	double delaySum = 0;
	auto skipBlocks = [&]() { while ( p < n && d[p] ) p += d[p] + 1; p++; };
	while ( p < n )
	{
		const uint8_t b = d[p++];
		if ( b == 0x21 && p < n )
		{
			const uint8_t label = d[p++];
			if ( label == 0xF9 && p + 4 < n && d[p] == 4 )
			{
				const int cs = LE16( &d[p + 2] );
				delaySum += cs < 2 ? 10 : cs;	// browsers play 0 / 1 as 10 (1/100 s)
				delays++;
			}
			skipBlocks();
		}
		else if ( b == 0x2C && p + 9 <= n )
		{
			frames++;
			const uint8_t fl = d[p + 8];
			p += 9;
			if ( fl & 0x80 )
				p += 3u << ( ( fl & 7 ) + 1 );
			p++;	// LZW minimum code size
			skipBlocks();
		}
		else
			break;
	}
	info.frames = std::max( 1, frames );
	if ( delays )
		info.fps = (float)( 100.0 / ( delaySum / delays ) );
}

static void ParseWebp( const std::vector<uint8_t> &d, ImageInfo_t &info )
{
	const size_t n = d.size();
	if ( n < 30 )
		return;
	const uint8_t *c = &d[12];
	if ( !memcmp( c, "VP8 ", 4 ) )
	{
		info.w = LE16( &d[26] ) & 0x3FFF;
		info.h = LE16( &d[28] ) & 0x3FFF;
	}
	else if ( !memcmp( c, "VP8L", 4 ) )
	{
		const uint8_t *b = &d[21];
		info.w = 1 + ( ( b[0] | b[1] << 8 ) & 0x3FFF );
		info.h = 1 + ( ( b[1] >> 6 | b[2] << 2 | b[3] << 10 ) & 0x3FFF );
	}
	else if ( !memcmp( c, "VP8X", 4 ) )
	{
		info.w = 1 + LE24( &d[24] );
		info.h = 1 + LE24( &d[27] );
		int frames = 0;
		double ms = 0;
		for ( size_t p = 12; ( d[20] & 0x02 ) && p + 8 <= n; )	// animated: its ANMF chunks
		{
			const uint32_t len = LE32( &d[p + 4] );
			if ( !memcmp( &d[p], "ANMF", 4 ) && p + 24 <= n )
			{
				frames++;
				const uint32_t dur = LE24( &d[p + 20] );
				ms += dur ? dur : 100;
			}
			p += 8 + (size_t)len + ( len & 1 );
		}
		if ( frames )
		{
			info.frames = frames;
			info.fps = (float)( 1000.0 / ( ms / frames ) );
		}
	}
}

static void ParseJpeg( const std::vector<uint8_t> &d, ImageInfo_t &info )
{
	for ( size_t n = d.size(), p = 2; p + 4 <= n; )
	{
		if ( d[p] != 0xFF )
			return;
		const uint8_t m = d[p + 1];
		p += 2;
		if ( m == 0xD8 || m == 0x01 || ( m >= 0xD0 && m <= 0xD7 ) || m == 0xFF )
		{
			p -= m == 0xFF;
			continue;
		}
		if ( m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && p + 7 <= n )
		{
			info.h = BE16( &d[p + 3] );
			info.w = BE16( &d[p + 5] );
			return;
		}
		p += BE16( &d[p] );
	}
}

static ImageInfo_t ProbeImage( const std::vector<uint8_t> &d )
{
	ImageInfo_t info;
	const size_t n = d.size();
	if ( n >= 24 && !memcmp( d.data(), "\x89PNG\r\n\x1a\n", 8 ) )
	{
		info.w = BE32( &d[16] );
		info.h = BE32( &d[20] );
	}
	else if ( n >= 13 && ( !memcmp( d.data(), "GIF87a", 6 ) || !memcmp( d.data(), "GIF89a", 6 ) ) )
		ParseGif( d, info );
	else if ( n >= 4 && d[0] == 0xFF && d[1] == 0xD8 )
		ParseJpeg( d, info );
	else if ( n >= 16 && !memcmp( d.data(), "RIFF", 4 ) && !memcmp( &d[8], "WEBP", 4 ) )
		ParseWebp( d, info );
	info.known = info.w > 0 && info.h > 0 && info.w < 65536 && info.h < 65536;
	info.fps = std::min( 50.0f, std::max( 1.0f, info.fps ) );
	return info;
}

//-----------------------------------------------------------------------------
// materials: unlit, drawn by the sky without depth (and the menu's thumbnail)
//-----------------------------------------------------------------------------
static bool WriteMaterial( const std::string &path, const std::string &texture, float aspect, int frames, float fps )
{
	char vmt[1024];
	int n = snprintf( vmt, sizeof( vmt ), "// triggerlook sky: aspect %.4f\n\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"%s\"\n\t\"$vertexcolor\" 1\n"
					  "\t\"$ignorez\" 1\n\t\"$nocull\" 1\n\t\"$nofog\" 1\n", aspect, texture.c_str() );
	if ( frames > 1 )
		n += snprintf( vmt + n, sizeof( vmt ) - n, "\t\"Proxies\"\n\t{\n\t\t\"AnimatedTexture\"\n\t\t{\n\t\t\t\"animatedTextureVar\" \"$basetexture\"\n"
					   "\t\t\t\"animatedTextureFrameNumVar\" \"$frame\"\n\t\t\t\"animatedTextureFrameRate\" \"%.2f\"\n\t\t}\n\t}\n", fps );
	n += snprintf( vmt + n, sizeof( vmt ) - n, "}\n" );
	return WriteFile( path, vmt, n );
}

// the aspect a material was written with (0 = none)
static float MaterialAspect( const std::string &path )
{
	std::vector<uint8_t> d;
	float a = 0;
	if ( ReadFile( path, d, 64 ) )
	{
		d.push_back( 0 );
		sscanf( (const char *)d.data(), "// triggerlook sky: aspect %f", &a );
	}
	return a > 0.01f && a < 100 ? a : 0;
}

//-----------------------------------------------------------------------------
// the lists
//-----------------------------------------------------------------------------
static bool ValidName( const std::string &n )
{
	if ( n.empty() || n.size() > 47 )
		return false;
	for ( char c : n )
		if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-' ) )
			return false;
	return true;
}

void TLPics_Init( const std::string &gameDir )
{
	s_SkyDir = gameDir + "/materials/triggerlook/sky";
	s_LogoDir = gameDir + "/materials/vgui/logos";
	TLP_MakeDirs( s_SkyDir.c_str() );
}

void TLPics_Refresh()
{
	std::vector<TLPEntry_t> entries;
	std::vector<TLPic_t> pics, sprays;
	TLP_List( s_SkyDir, entries );
	for ( const TLPEntry_t &e : entries )
	{
		TLPic_t p;
		p.name = Stem( e.name );
		VtfInfo_t v;
		if ( e.dir || !EndsWith( e.name, ".vtf" ) || !ValidName( p.name ) || !ReadVtfInfo( s_SkyDir + "/" + e.name, v ) )
			continue;
		const std::string vmt = s_SkyDir + "/" + p.name + ".vmt";
		p.aspect = MaterialAspect( vmt );
		if ( !p.aspect && !WriteMaterial( vmt, "triggerlook/sky/" + p.name, p.aspect = (float)v.width / v.height, v.frames, 10.0f ) )
			continue;		// (its material went missing and can't be written)
		p.material = "triggerlook/sky/" + p.name;
		p.width = v.width;
		p.height = v.height;
		p.frames = v.frames;
		pics.push_back( p );
	}
	TLP_List( s_LogoDir, entries );
	for ( const TLPEntry_t &e : entries )
	{
		TLPic_t p;
		p.name = Stem( e.name );
		p.spray = true;
		VtfInfo_t v;
		// (a spray whose file name isn't a plain word can't be named in a material path)
		if ( e.dir || !EndsWith( e.name, ".vtf" ) || !ValidName( Lower( p.name ) ) || !ReadVtfInfo( s_LogoDir + "/" + e.name, v ) || !EngineCanLoad( v ) )
			continue;
		p.material = "triggerlook/sky/logo_" + Lower( p.name );
		const std::string vmt = s_SkyDir + "/logo_" + Lower( p.name ) + ".vmt";
		const long long made = TLP_MTime( vmt );
		if ( ( made < 0 || e.mtime > made ) && !WriteMaterial( vmt, "vgui/logos/" + p.name, (float)v.width / v.height, v.frames, SPRAY_FPS ) )
			continue;
		p.width = v.width;
		p.height = v.height;
		p.frames = v.frames;
		p.aspect = (float)v.width / v.height;
		sprays.push_back( p );
	}
	auto byName = []( const TLPic_t &a, const TLPic_t &b ) { return Lower( a.name ) < Lower( b.name ); };
	std::sort( pics.begin(), pics.end(), byName );
	std::sort( sprays.begin(), sprays.end(), byName );
	s_Pics.swap( pics );
	s_Sprays.swap( sprays );
}

const std::vector<TLPic_t> &TLPics_List( bool sprays ) { return sprays ? s_Sprays : s_Pics; }

const TLPic_t *TLPics_Find( bool spray, const std::string &name )
{
	for ( const TLPic_t &p : TLPics_List( spray ) )
		if ( !Lower( p.name ).compare( Lower( name ) ) )
			return &p;
	return NULL;
}

//-----------------------------------------------------------------------------
// adding a file (background thread)
//-----------------------------------------------------------------------------
static void SetStatus( int state, const std::string &text, const std::string &added = "" )
{
	std::lock_guard<std::mutex> lock( s_Lock );
	s_Status.state = state;
	s_Status.text = text;
	s_Status.added = added;
	s_Status.serial++;
	if ( state != 1 )
		s_bBusy = false;
}

TLPicStatus_t TLPics_Status()
{
	std::lock_guard<std::mutex> lock( s_Lock );
	return s_Status;
}

// "Cat Pic (1).PNG" -> "cat_pic_1", not taken yet
static std::string MakeName( const std::string &file )
{
	std::string s;
	for ( char c : Lower( Stem( BaseName( file ) ) ) )
		if ( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '-' )
			s += c;
		else if ( !s.empty() && s.back() != '_' )
			s += '_';
	while ( !s.empty() && s.back() == '_' )
		s.pop_back();
	s = s.empty() ? "picture" : s.compare( 0, 5, "logo_" ) ? s.substr( 0, 40 ) : "pic_" + s.substr( 0, 36 );	// (logo_ = sprays)
	std::string name = s;
	for ( int i = 2; TLP_IsFile( ( s_SkyDir + "/" + name + ".vtf" ).c_str() ) || TLP_IsFile( ( s_SkyDir + "/" + name + ".vmt" ).c_str() ); i++ )
		name = s + "_" + std::to_string( i );
	return name;
}

// MareTF, given at most MARETF_SECONDS (a hung one is killed: the next add mustn't wait for it forever)
static const int MARETF_SECONDS = 180;
static bool s_bTimedOut;
static bool MareTF( const std::vector<std::string> &args, const std::string &log )
{
	const std::string exe = TLP_FindMareTF();
	const int r = exe.empty() ? 1 : TLP_Run( exe, args, log, MARETF_SECONDS );
	s_bTimedOut = s_bTimedOut || r == 2;
	return r == 0;
}

static bool ConvertImage( const std::string &in, const std::string &name, const std::string &tmp, std::string &err )
{
	const std::string log = tmp + "/maretf.log", dec = tmp + "/decoded.vtf", mid = tmp + "/frames.vtf", out = tmp + "/out.vtf";
	const std::string vtf = s_SkyDir + "/" + name + ".vtf", vmt = s_SkyDir + "/" + name + ".vmt";
	struct Temp_t { const std::string *files[3]; ~Temp_t() { for ( const std::string *f : files ) TLP_Remove( f->c_str() ); } } temp = { { &dec, &mid, &out } };
	s_bTimedOut = false;
	auto fail = [&]( const char *e ) { err = s_bTimedOut ? "MareTF took too long" : e; return false; };
	if ( TLP_FindMareTF().empty() )
		return fail( "MareTF isn't installed" );
	TLP_Remove( log.c_str() );
	// size and frames from the file's own header (GIF: its blocks), the file read whole only up to MAX_PROBE_BYTES
	ImageInfo_t info;
	{
		std::vector<uint8_t> file;
		FILE *f = TLP_Open( in.c_str(), "rb" );
		const long long size = f ? TLP_FileSize( f ) : -1;
		if ( f )
			fclose( f );
		if ( size > MAX_PROBE_BYTES )
			return fail( "bigger than 64 MB" );
		if ( size < 0 || !ReadFile( in, file, MAX_PROBE_BYTES ) )
			return fail( "can't read the file" );
		info = ProbeImage( file );
	}
	if ( info.known && 4ll * info.w * info.h * info.frames > MAX_SOURCE_BYTES )
		return fail( "too big to decode (over 1 GB of frames)" );
	VtfInfo_t v;
	if ( !info.known )
	{
		// a format whose size we can't read: MareTF decodes it once to find out (only its header is read)
		if ( !MareTF( { "create", "-y", "-v", "7.2", "-f", "RGBA8888", "--no-mips", "--no-thumbnail", "--resize-method", "NONE", "-i", in, "-o", dec }, log ) ||
			 !ReadVtfInfo( dec, v ) )
			return fail( "MareTF can't read this file" );
		info.w = v.width;
		info.h = v.height;
		info.frames = v.frames;
		TLP_Remove( dec.c_str() );
	}
	// the texture: each side the nearest power of two (a 2:1 panorama keeps 2:1); animations smaller until they fit,
	// and until MareTF's RGBA decode of every frame stays under MAX_DECODE_BYTES
	const float aspect = (float)info.w / info.h;
	const bool pano = aspect >= 1.9f && aspect <= 2.1f;
	int H = NearPow2( info.h, MIN_SIDE, pano ? MAX_SIDE / 2 : MAX_SIDE ), W = pano ? 2 * H : NearPow2( info.w, MIN_SIDE, MAX_SIDE );
	auto bytes = []( int w, int h ) { long long t = 0; for ( ; ; w = std::max( 1, w / 2 ), h = std::max( 1, h / 2 ) ) { t += (long long)( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * 8; if ( w == 1 && h == 1 ) return t; } };
	auto maxFrames = [&]( int w, int h ) { return (int)std::min( { ( MAX_ANIM_BYTES - 4096 ) / bytes( w, h ), ( 2ll << std::max( 0, 25 - Log2( w ) - Log2( h ) ) ) - 1, 65535ll } ); };
	auto decoded = [&]( int w, int h ) { return 4ll * w * h * info.frames; };
	while ( ( info.frames > maxFrames( W, H ) && std::max( W, H ) > MIN_ANIM_SIDE ) || ( decoded( W, H ) > MAX_DECODE_BYTES && std::max( W, H ) > MIN_SIDE ) )
	{
		W /= 2;
		H /= 2;
	}
	if ( decoded( W, H ) > MAX_DECODE_BYTES )
		return fail( "too many frames" );

	// 1. decode, stretched to W x H; only its header is read, then one kept frame at a time
	if ( !MareTF( { "create", "-y", "-v", "7.2", "-f", "RGBA8888", "--no-mips", "--no-thumbnail", "--resize-method", "NONE",
					"--width", std::to_string( W ), "--height", std::to_string( H ), "-i", in, "-o", dec }, log ) )
		return fail( "MareTF can't read this file" );
	FILE *f = TLP_Open( dec.c_str(), "rb" );
	uint8_t head[80];
	const long long size = f ? TLP_FileSize( f ) : -1;
	const bool headOK = f && fread( head, 1, sizeof( head ), f ) == sizeof( head ) && ParseVtf( head, sizeof( head ), v ) && v.format == 0 &&
						v.width == W && v.height == H;
	long long offset = v.headerSize;
	if ( v.lowW && v.lowH && v.lowFormat == 13 )	// a DXT1 thumbnail (asked for none)
		offset += (long long)( ( v.lowW + 3 ) / 4 ) * ( ( v.lowH + 3 ) / 4 ) * 8;
	const size_t frameBytes = (size_t)W * H * 4;
	if ( !headOK || size < offset + (long long)frameBytes * v.frames )
	{
		if ( f )
			fclose( f );
		return fail( headOK ? "MareTF's picture is cut short" : "MareTF made something unexpected" );
	}
	// 2. too many frames (the real count can differ from the header's guess): every step-th
	const int step = ( v.frames + maxFrames( W, H ) - 1 ) / std::max( 1, maxFrames( W, H ) ), frames = ( v.frames + step - 1 ) / step;
	std::string src = dec;
	if ( step > 1 )
	{
		FILE *o = TLP_Open( mid.c_str(), "wb" );
		std::vector<uint8_t> frame( frameBytes );
		uint8_t h[80];
		WriteVtfHeader( h, W, H, frames );
		bool ok = o && fwrite( h, 1, sizeof( h ), o ) == sizeof( h );
		for ( int i = 0; i < frames && ok; i++ )
			ok = TLP_Seek( f, offset + (long long)frameBytes * i * step ) && fread( frame.data(), 1, frameBytes, f ) == frameBytes &&
				 fwrite( frame.data(), 1, frameBytes, o ) == frameBytes;
		if ( o )
			ok = fclose( o ) == 0 && ok;
		if ( !ok )
		{
			fclose( f );
			return fail( "can't write a temporary file" );
		}
		src = mid;
	}
	fclose( f );
	// 3. compressed, mipmaps, edges clamped (a panorama wraps round: no seam)
	std::vector<std::string> args = { "edit", "-y", "--set-format", "DXT1", "--recompute-mips", "--add-flag", "CLAMP_T" };
	if ( !pano )
		args.insert( args.end(), { "--add-flag", "CLAMP_S" } );
	args.insert( args.end(), { "-o", out, src } );
	VtfInfo_t o;
	if ( !MareTF( args, log ) || !ReadVtfInfo( out, o ) || o.width != W || o.height != H || o.frames != frames || !EngineCanLoad( o ) )
		return fail( "MareTF couldn't compress it" );
	// the material first: a list refresh that finds a texture without one would write its own
	if ( !WriteMaterial( vmt, "triggerlook/sky/" + name, aspect, frames, std::max( 1.0f, info.fps / step ) ) )
		return fail( "can't write its material" );
	if ( !TLP_ReplaceFile( out.c_str(), vtf.c_str() ) )
	{
		TLP_Remove( vmt.c_str() );
		return fail( "can't write to materials/triggerlook/sky" );
	}
	return true;
}

static void AddThread( void *arg )
{
	std::string *path = (std::string *)arg, file = BaseName( *path ), err, name = MakeName( file ), tmp = s_SkyDir + "/.convert";
	const std::string vtf = s_SkyDir + "/" + name + ".vtf", vmt = s_SkyDir + "/" + name + ".vmt";
	TLP_MakeDirs( tmp.c_str() );
	bool ok = false;
	std::vector<uint8_t> head;
	VtfInfo_t v;
	if ( !ReadFile( *path, head, 80 ) )
		err = "can't read the file";
	else if ( head.size() >= 4 && !memcmp( head.data(), "VTF\0", 4 ) )
	{
		// already a texture: taken as it is (the material first, as above)
		std::vector<uint8_t> all;
		if ( !ParseVtf( head.data(), head.size(), v ) || !EngineCanLoad( v ) )
			err = "not a VTF the game can show";
		else if ( !ReadFile( *path, all, MAX_VTF_FILE + 1 ) || all.size() > MAX_VTF_FILE )
			err = "bigger than 64 MB";
		else if ( !WriteMaterial( vmt, "triggerlook/sky/" + name, (float)v.width / v.height, v.frames, SPRAY_FPS ) ||
				  !WriteFile( vtf, all.data(), all.size() ) )
		{
			TLP_Remove( vmt.c_str() );
			err = "can't write to materials/triggerlook/sky";
		}
		else
			ok = true;
	}
	else
		ok = ConvertImage( *path, name, tmp, err );
	if ( ok )
	{
		TLP_Remove( ( tmp + "/maretf.log" ).c_str() );	// (kept after a failure: what MareTF said)
		SetStatus( 2, "added " + name, name );
	}
	else
		SetStatus( 3, file + ": " + err );
	delete path;
}

bool TLPics_Add( const std::string &path )
{
	{
		std::lock_guard<std::mutex> lock( s_Lock );
		if ( s_bBusy )
			return false;
		s_bBusy = true;
		s_Status.state = 1;
		s_Status.text = "adding " + BaseName( path ) + " ...";
		s_Status.added.clear();
		s_Status.serial++;
	}
	std::string *job = new std::string( path );
	if ( !TLP_StartThread( AddThread, job ) )
	{
		delete job;
		SetStatus( 3, "couldn't start" );
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// the file list
//-----------------------------------------------------------------------------
bool TLPics_ListDir( const std::string &dir, std::vector<TLDirEntry_t> &out )
{
	out.clear();
	std::vector<TLPEntry_t> entries;
	if ( !TLP_List( dir, entries ) )
		return false;
	static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".vtf" };
	for ( const TLPEntry_t &e : entries )
	{
		bool add = e.dir;
		for ( const char *x : ext )
			add = add || EndsWith( e.name, x );
		if ( add )
			out.push_back( { e.name, e.dir } );
	}
	std::sort( out.begin(), out.end(), []( const TLDirEntry_t &a, const TLDirEntry_t &b ) { return a.dir != b.dir ? a.dir : Lower( a.name ) < Lower( b.name ); } );
	return true;
}
