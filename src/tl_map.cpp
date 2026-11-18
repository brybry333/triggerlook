//=============================================================================
// triggerlook: the triggers of the current map from its .bsp; see tl_map.h
//=============================================================================
#include <algorithm>
#include <float.h>
#include <math.h>
#include <stdio.h>

#include "tl_map.h"
#include "tl_platform.h"
#include "tl_util.h"
#include "tier0/dbg.h"

extern "C" {
#include "../utils/lzma/C/LzmaDec.h"
}

const char *const g_TLTypeName[TL_TYPES] = { "teleport", "booster", "push", "gravity", "other" };
const char *const g_TLTypeLabel[TL_TYPES] = { "TELEPORT", "BOOSTER", "PUSH", "GRAVITY", "OTHER" };

int TL_Classify( const char *classname, const char *outputs )
{
	if ( TL_StrNICmp( classname, "trigger_", 8 ) )
		return -1;
	if ( !TL_StrICmp( classname, "trigger_teleport" ) || !TL_StrICmp( classname, "trigger_teleport_relative" ) )
		return TL_TELEPORT;
	if ( !TL_StrICmp( classname, "trigger_push" ) )
		return TL_PUSH;
	if ( !TL_StrICmp( classname, "trigger_gravity" ) )
		return TL_GRAVITY;
	if ( outputs && TL_StrIStr( outputs, "basevelocity" ) )
		return TL_BOOSTER;
	if ( outputs && TL_StrIStr( outputs, "gravity" ) )
		return TL_GRAVITY;
	return TL_OTHER;
}

namespace
{
	enum { LUMP_ENTITIES = 0, LUMP_PLANES = 1, LUMP_NODES = 5, LUMP_LEAFS = 10, LUMP_MODELS = 14, LUMP_LEAFBRUSHES = 17,
		   LUMP_BRUSHES = 18, LUMP_BRUSHSIDES = 19, NUM_LUMPS = 64 };

	#pragma pack(push, 1)
	struct lump_t { int fileofs, filelen, version; int uncompressedSize; };
	struct dheader_t { int ident, version; lump_t lumps[NUM_LUMPS]; int mapRevision; };
	struct dplane_t { Vector normal; float dist; int type; };
	struct dmodel_t { Vector mins, maxs, origin; int headnode, firstface, numfaces; };
	struct dnode_t { int planenum; int children[2]; short mins[3], maxs[3]; unsigned short firstface, numfaces; short area, pad; };
	struct dbrush_t { int firstside, numsides, contents; };
	struct dbrushside_t { unsigned short planenum; short texinfo, dispinfo; unsigned char bevel, thin; };
	#pragma pack(pop)

	// limits against broken or hostile maps (maps come from servers): no lump this reader needs is anywhere near these
	const long long MAX_LUMP = 256ll * 1024 * 1024, MAX_UNPACKED = 64u * 1024 * 1024;
	const int MAX_BRUSH_SIDES = 128;		// vbsp's own limit
	const long long MAX_CHOPS = 2000000;	// polygon clips per map (bhop_exodus needs ~40k)

	// a lump running a little past the end of the file (seen: an entity lump 1 byte too long, the game loads the
	// map fine) is read as far as there is data, the rest zeros
	bool ReadAt( TLMapFile &f, long long size, long long ofs, void *dst, int len )
	{
		if ( ofs < 0 || len < 0 || ofs > size || ofs + len > size + 4096 )
			return false;
		int have = (int)MIN( (long long)len, size - ofs );
		memset( (char *)dst + have, 0, len - have );
		return !have || f.Read( ofs, dst, have );
	}

	// Valve's LZMA lump: "LZMA", actual size, compressed size, 5 property bytes, then the stream
	void *LzmaAlloc( void *, size_t n ) { return malloc( n ); }
	void LzmaFree( void *, void *p ) { free( p ); }

	bool Unlzma( const std::vector<unsigned char> &in, std::vector<unsigned char> &out )
	{
		unsigned int actual, packed;
		memcpy( &actual, in.data() + 4, 4 );
		memcpy( &packed, in.data() + 8, 4 );
		if ( (size_t)packed + 17 > in.size() || actual > MAX_UNPACKED )
			return false;
		out.resize( actual );
		SizeT outLen = actual, inLen = packed;
		ELzmaStatus status;
		ISzAlloc alloc = { LzmaAlloc, LzmaFree };
		return LzmaDecode( out.data(), &outLen, in.data() + 17, &inLen, in.data() + 12, 5, LZMA_FINISH_ANY, &status, &alloc ) == SZ_OK &&
			   outLen == actual;
	}

	// one lump, decompressed if needed; sizes checked before anything is allocated
	bool ReadLump( TLMapFile &f, long long size, const dheader_t &h, int lump, std::vector<unsigned char> &out )
	{
		const lump_t &l = h.lumps[lump];
		out.clear();
		if ( l.filelen <= 0 )
			return l.filelen == 0;
		if ( l.fileofs < 0 || l.filelen > MAX_LUMP || (long long)l.fileofs + l.filelen > size + 4096 )
			return false;
		std::vector<unsigned char> raw( l.filelen );
		if ( !ReadAt( f, size, l.fileofs, raw.data(), l.filelen ) )
			return false;
		if ( l.filelen >= 17 && !memcmp( raw.data(), "LZMA", 4 ) )
			return Unlzma( raw, out );
		out.swap( raw );
		return true;
	}

	// a loose map file (offline tools; in the game the map comes through the game's filesystem)
	struct LooseFile : TLMapFile
	{
		FILE *f;
		explicit LooseFile( FILE *file ) : f( file ) {}
		~LooseFile() { fclose( f ); }
		long long Size() override { return TLP_FileSize( f ); }
		bool Read( long long ofs, void *dst, int len ) override { return TLP_Seek( f, ofs ) && fread( dst, 1, len, f ) == (size_t)len; }
	};

	// the engine's order: custom/<folder>/ first, then the game folder, then download/
	struct FindInCustom_t { const char *customDir, *level; char *found; int foundLen; bool ok; };
	void TryCustomFolder( const char *name, void *ctx )
	{
		FindInCustom_t *f = (FindInCustom_t *)ctx;
		if ( f->ok )
			return;
		snprintf( f->found, f->foundLen, "%s/%s/%s", f->customDir, name, f->level );
		f->ok = TLP_IsFile( f->found );
	}

	bool FindMap( const char *gameDir, const char *level, char *found, int foundLen )
	{
		char customDir[1024];
		snprintf( customDir, sizeof( customDir ), "%s/custom", gameDir );
		FindInCustom_t f = { customDir, level, found, foundLen, false };
		TLP_ListDir( customDir, TryCustomFolder, &f );
		if ( f.ok ) return true;
		snprintf( found, foundLen, "%s/%s", gameDir, level );
		if ( TLP_IsFile( found ) ) return true;
		snprintf( found, foundLen, "%s/download/%s", gameDir, level );
		return TLP_IsFile( found );
	}

	// the entity lump: brush entities (model "*N") -> classname + every output value
	struct Ent_t { std::string classname, outputs; };
	void ParseEntities( const char *text, int len, std::vector<Ent_t> &models )
	{
		const char *p = text, *end = text + len;
		while ( p < end )
		{
			while ( p < end && *p != '{' ) p++;
			if ( p >= end ) break;
			p++;
			Ent_t e;
			int model = -1;
			while ( p < end && *p != '}' )
			{
				char key[128], value[2048];
				char *dst[2] = { key, value };
				int sizes[2] = { sizeof( key ), sizeof( value ) };
				bool ok = true;
				for ( int k = 0; k < 2 && ok; k++ )
				{
					while ( p < end && *p != '"' && *p != '}' ) p++;
					if ( p >= end || *p == '}' ) { ok = false; break; }
					p++;
					int n = 0;
					while ( p < end && *p != '"' ) { if ( n < sizes[k] - 1 ) dst[k][n++] = *p; p++; }
					dst[k][n] = 0;
					p++;
				}
				if ( !ok ) break;
				if ( !TL_StrICmp( key, "classname" ) ) e.classname = value;
				else if ( !TL_StrICmp( key, "model" ) && value[0] == '*' ) model = atoi( value + 1 );
				else if ( !TL_StrNICmp( key, "On", 2 ) ) { e.outputs += value; e.outputs += '\n'; }
			}
			if ( model > 0 && model < 65536 )
			{
				if ( (int)models.size() <= model )
					models.resize( model + 1 );
				models[model] = e;
			}
		}
	}

	// ---- brush -> polygons (doubles: the starting polygon is the size of the world) ----------------------
	struct DV { double x, y, z; };
	inline DV operator+( const DV &a, const DV &b ) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline DV operator-( const DV &a, const DV &b ) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline DV operator*( const DV &a, double s ) { return { a.x * s, a.y * s, a.z * s }; }
	inline double Dot( const DV &a, const DV &b ) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline DV Cross( const DV &a, const DV &b ) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline DV Norm( const DV &a ) { double l = sqrt( Dot( a, a ) ); return l > 0 ? a * ( 1.0 / l ) : a; }

	const double ON_EPS = 0.01;

	// a huge square on the plane dot( p, n ) == d
	void BaseWinding( const DV &n, double d, std::vector<DV> &w )
	{
		DV up = fabs( n.z ) > fabs( n.x ) && fabs( n.z ) > fabs( n.y ) ? DV{ 1, 0, 0 } : DV{ 0, 0, 1 };
		up = Norm( up - n * Dot( up, n ) );
		DV right = Cross( up, n );
		DV org = n * d;
		const double S = 131072;
		up = up * S;
		right = right * S;
		w.clear();
		w.push_back( org - right + up );
		w.push_back( org + right + up );
		w.push_back( org + right - up );
		w.push_back( org - right - up );
	}

	// keeps the part behind the plane (dot( p, n ) <= d): the inside of a brush side
	void Chop( std::vector<DV> &w, const DV &n, double d )
	{
		int count = (int)w.size(), front = 0, back = 0;
		std::vector<double> dist( count );
		std::vector<int> side( count );
		for ( int i = 0; i < count; i++ )
		{
			dist[i] = Dot( w[i], n ) - d;
			side[i] = dist[i] > ON_EPS ? 1 : dist[i] < -ON_EPS ? -1 : 0;
			front += side[i] > 0;
			back += side[i] < 0;
		}
		if ( !front )
			return;
		if ( !back )
		{
			w.clear();
			return;
		}
		std::vector<DV> out;
		for ( int i = 0; i < count; i++ )
		{
			const DV &p = w[i], &q = w[( i + 1 ) % count];
			int s = side[i], sn = side[( i + 1 ) % count];
			if ( s <= 0 )
				out.push_back( p );
			if ( s == 0 || sn == 0 || s == sn )
				continue;
			double t = dist[i] / ( dist[i] - dist[( i + 1 ) % count] );
			out.push_back( p + ( q - p ) * t );
		}
		w.swap( out );
	}

	// grid maps: put values within 0.01 of a 1/32 step on it
	inline float Snap( double v )
	{
		double s = floor( v * 32 + 0.5 ) / 32;
		return (float)( fabs( s - v ) < 0.01 ? s : v );
	}

	void AddBrush( const dbrush_t &b, const dbrushside_t *pSides, int nSides, const dplane_t *pPlanes, int nPlanes, TLModel_t &m,
				   long long &chops )
	{
		if ( b.firstside < 0 || b.numsides <= 0 || b.numsides > MAX_BRUSH_SIDES || b.firstside > nSides - b.numsides )
			return;
		const dbrushside_t *sides = pSides + b.firstside;
		std::vector<DV> w;
		for ( int i = 0; i < b.numsides; i++ )
		{
			if ( sides[i].bevel || sides[i].planenum >= nPlanes )
				continue;
			bool dup = false;
			for ( int j = 0; j < i && !dup; j++ )
				dup = !sides[j].bevel && sides[j].planenum == sides[i].planenum;
			if ( dup )
				continue;
			const dplane_t &pl = pPlanes[sides[i].planenum];
			DV n = { pl.normal.x, pl.normal.y, pl.normal.z };
			BaseWinding( n, pl.dist, w );
			for ( int j = 0; j < b.numsides && w.size() >= 3; j++ )
			{
				if ( j == i || sides[j].bevel || sides[j].planenum >= nPlanes || sides[j].planenum == sides[i].planenum )
					continue;
				if ( ++chops > MAX_CHOPS )
					return;
				const dplane_t &cp = pPlanes[sides[j].planenum];
				Chop( w, DV{ cp.normal.x, cp.normal.y, cp.normal.z }, cp.dist );
			}
			// drop repeated points, then the tiny / degenerate polygons
			std::vector<DV> clean;
			for ( size_t k = 0; k < w.size(); k++ )
			{
				const DV &p = w[k], &q = w[( k + 1 ) % w.size()];
				DV dd = q - p;
				if ( Dot( dd, dd ) > ON_EPS * ON_EPS )
					clean.push_back( p );
			}
			if ( clean.size() < 3 )
				continue;
			DV area = { 0, 0, 0 };
			for ( size_t k = 0; k < clean.size(); k++ )
				area = area + Cross( clean[k], clean[( k + 1 ) % clean.size()] );
			double a2 = Dot( area, n );	// twice the area, signed: > 0 = counter-clockwise seen from the front
			if ( fabs( a2 ) < 0.1 )
				continue;
			if ( a2 > 0 )
				std::reverse( clean.begin(), clean.end() );		// the map's faces are clockwise from the front
			TLPoly_t poly;
			poly.first = (int)m.verts.size();
			poly.count = (int)clean.size();
			poly.normal = pl.normal;
			for ( const DV &p : clean )
			{
				Vector v( Snap( p.x ), Snap( p.y ), Snap( p.z ) );
				m.verts.push_back( v );
				VectorMin( m.mins, v, m.mins );
				VectorMax( m.maxs, v, m.maxs );
			}
			m.polys.push_back( poly );
		}
	}
}

void CTLMap::Clear()
{
	m_Models.clear();
	memset( m_nTriggers, 0, sizeof( m_nTriggers ) );
	m_nPolys = 0;
	m_szLevel[0] = 0;
}

bool CTLMap::Load( const char *pszGameDir, const char *pszLevel, TLOpenMapFn pfnOpen )
{
	Clear();
	char path[1024];
	TLMapFile *file = pfnOpen ? pfnOpen( pszLevel ) : NULL;
	if ( file )
		snprintf( path, sizeof( path ), "%s", pszLevel );
	else if ( FILE *f = FindMap( pszGameDir, pszLevel, path, sizeof( path ) ) ? fopen( path, "rb" ) : NULL )
		file = new LooseFile( f );
	else
	{
		Warning( "triggerlook: can't read %s\n", pszLevel );
		return false;
	}
	const long long size = file->Size();
	dheader_t h;
	bool ok = size >= (long long)sizeof( h ) && ReadAt( *file, size, 0, &h, sizeof( h ) );
	if ( !ok || h.ident != ( ( 'P' << 24 ) | ( 'S' << 16 ) | ( 'B' << 8 ) | 'V' ) || h.version < 19 || h.version > 21 )
	{
		delete file;
		Warning( "triggerlook: %s is not a supported BSP (version %d)\n", path, ok ? h.version : -1 );
		return false;
	}
	std::vector<unsigned char> ents, planes, nodes, leafs, models, leafbrushes, brushes, brushsides;
	ok = ReadLump( *file, size, h, LUMP_ENTITIES, ents ) && ReadLump( *file, size, h, LUMP_PLANES, planes ) &&
		 ReadLump( *file, size, h, LUMP_NODES, nodes ) && ReadLump( *file, size, h, LUMP_LEAFS, leafs ) &&
		 ReadLump( *file, size, h, LUMP_MODELS, models ) && ReadLump( *file, size, h, LUMP_LEAFBRUSHES, leafbrushes ) &&
		 ReadLump( *file, size, h, LUMP_BRUSHES, brushes ) && ReadLump( *file, size, h, LUMP_BRUSHSIDES, brushsides );
	delete file;
	// leaf layout: version 0 has the ambient light cube (56 bytes), version 1 doesn't (32); the brush list is at 24
	int leafSize = h.lumps[LUMP_LEAFS].version == 0 ? 56 : h.lumps[LUMP_LEAFS].version == 1 ? 32 : 0;
	if ( !ok || !leafSize )
	{
		Warning( "triggerlook: %s: unreadable lumps\n", path );
		return false;
	}

	std::vector<Ent_t> ent;
	ParseEntities( (const char *)ents.data(), (int)ents.size(), ent );

	const dplane_t *pPlanes = (const dplane_t *)planes.data();
	const dnode_t *pNodes = (const dnode_t *)nodes.data();
	const dmodel_t *pModels = (const dmodel_t *)models.data();
	const unsigned short *pLeafBrushes = (const unsigned short *)leafbrushes.data();
	const dbrush_t *pBrushes = (const dbrush_t *)brushes.data();
	const dbrushside_t *pSides = (const dbrushside_t *)brushsides.data();
	int nPlanes = (int)planes.size() / sizeof( dplane_t ), nNodes = (int)nodes.size() / sizeof( dnode_t ), nLeafs = (int)leafs.size() / leafSize;
	int nModels = (int)models.size() / sizeof( dmodel_t ), nLeafBrushes = (int)leafbrushes.size() / 2;
	int nBrushes = (int)brushes.size() / sizeof( dbrush_t ), nSides = (int)brushsides.size() / sizeof( dbrushside_t );

	m_Models.resize( nModels );
	std::vector<char> seen( nBrushes, 0 ), nodeSeen( nNodes, 0 );
	std::vector<int> stack, used;
	long long chops = 0;
	for ( int mi = 1; mi < nModels && mi < (int)ent.size(); mi++ )
	{
		int type = TL_Classify( ent[mi].classname.c_str(), ent[mi].outputs.c_str() );
		if ( type < 0 )
			continue;
		TLModel_t &m = m_Models[mi];
		m.type = type;
		m.classname = ent[mi].classname;
		m.mins.Init( FLT_MAX, FLT_MAX, FLT_MAX );
		m.maxs.Init( -FLT_MAX, -FLT_MAX, -FLT_MAX );
		// the model's brushes: every leaf under its head node
		used.clear();
		stack.assign( 1, pModels[mi].headnode );
		while ( !stack.empty() )
		{
			int n = stack.back();
			stack.pop_back();
			if ( n >= 0 )
			{
				// each node once (a hostile tree could point back up, or share subtrees)
				if ( n < nNodes && !nodeSeen[n] )
				{
					nodeSeen[n] = 1;
					stack.push_back( pNodes[n].children[0] );
					stack.push_back( pNodes[n].children[1] );
				}
				continue;
			}
			int leaf = -1 - n;
			if ( leaf >= nLeafs )
				continue;
			const unsigned char *l = (const unsigned char *)leafs.data() + (size_t)leaf * leafSize;
			unsigned short first, num;
			memcpy( &first, l + 24, 2 );
			memcpy( &num, l + 26, 2 );
			for ( int k = first; k < first + num && k < nLeafBrushes; k++ )
			{
				int b = pLeafBrushes[k];
				if ( b < nBrushes && !seen[b] )
				{
					seen[b] = 1;
					used.push_back( b );
				}
			}
		}
		for ( int b : used )
		{
			seen[b] = 0;
			AddBrush( pBrushes[b], pSides, nSides, pPlanes, nPlanes, m, chops );
		}
		if ( m.polys.empty() )
		{
			m.mins.Init();
			m.maxs.Init();
			continue;
		}
		m_nTriggers[type]++;
		m_nPolys += (int)m.polys.size();
	}
	if ( chops > MAX_CHOPS )
		Warning( "triggerlook: %s: too many trigger brush sides, the rest are left out\n", path );
	TL_StrCopy( m_szLevel, pszLevel, sizeof( m_szLevel ) );
	Msg( "triggerlook: %s: %d teleport, %d booster, %d push, %d gravity, %d other triggers (%d sides)\n", path,
		 m_nTriggers[TL_TELEPORT], m_nTriggers[TL_BOOSTER], m_nTriggers[TL_PUSH], m_nTriggers[TL_GRAVITY], m_nTriggers[TL_OTHER], m_nPolys );
	return true;
}
