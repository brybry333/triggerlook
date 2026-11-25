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

const char *const g_TLTypeName[TL_TYPES] = { "teleport", "booster", "push", "gravity", "other", "clip", "invisible", "nodraw" };
const char *const g_TLTypeLabel[TL_TYPES] = { "TELEPORT", "BOOSTER", "PUSH", "GRAVITY", "OTHER", "CLIPS", "INVISIBLES", "NODRAWS" };

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
	enum { LUMP_ENTITIES = 0, LUMP_PLANES = 1, LUMP_TEXDATA = 2, LUMP_VERTEXES = 3, LUMP_VISIBILITY = 4, LUMP_NODES = 5, LUMP_TEXINFO = 6,
		   LUMP_FACES = 7, LUMP_LEAFS = 10, LUMP_EDGES = 12, LUMP_SURFEDGES = 13, LUMP_MODELS = 14, LUMP_LEAFBRUSHES = 17,
		   LUMP_BRUSHES = 18, LUMP_BRUSHSIDES = 19, LUMP_AREAS = 20, LUMP_AREAPORTALS = 21, LUMP_TEXDATA_STRING_DATA = 43,
		   LUMP_TEXDATA_STRING_TABLE = 44, NUM_LUMPS = 64 };

	#pragma pack(push, 1)
	struct lump_t { int fileofs, filelen, version; int uncompressedSize; };
	struct dheader_t { int ident, version; lump_t lumps[NUM_LUMPS]; int mapRevision; };
	struct dplane_t { Vector normal; float dist; int type; };
	struct dmodel_t { Vector mins, maxs, origin; int headnode, firstface, numfaces; };
	struct dnode_t { int planenum; int children[2]; short mins[3], maxs[3]; unsigned short firstface, numfaces; short area, pad; };
	struct dbrush_t { int firstside, numsides, contents; };
	struct dbrushside_t { unsigned short planenum; short texinfo, dispinfo; unsigned char bevel, thin; };
	struct dface_t { unsigned short planenum; unsigned char side, onNode; int firstedge; short numedges, texinfo, dispinfo, fogVolume;
					 unsigned char styles[4]; int lightofs; float area; int lmMins[2], lmSize[2]; int origFace;
					 unsigned short numPrims, firstPrimID; unsigned int smoothingGroups; };
	struct dedge_t { unsigned short v[2]; };
	struct darea_t { int numareaportals, firstareaportal; };
	struct dareaportal_t { unsigned short portalKey, otherArea, firstClipVert, numClipVerts; int planenum; };
	static_assert( sizeof( dface_t ) == 56, "dface_t layout" );
	static_assert( sizeof( dareaportal_t ) == 12, "dareaportal_t layout" );
	#pragma pack(pop)

	// limits against broken or hostile maps (maps come from servers): no lump this reader needs is anywhere near these
	const long long MAX_LUMP = 256ll * 1024 * 1024, MAX_UNPACKED = 64u * 1024 * 1024;
	const int MAX_BRUSH_SIDES = 128;		// vbsp's own limit
	const int CONTENTS_PLAYERCLIP = 0x10000;	// tools/toolsclip, tools/toolsplayerclip
	const int CONTENTS_BLOCKS = 0x1 | 0x2 | 0x8 | 0x4000;	// solid, window, grate (toolsinvisible), moveable: stops players
	const long long MAX_CHOPS = 2000000;	// polygon clips per map (bhop_exodus needs ~40k)
	const long long MAX_WALK = 8000000;		// tree steps per map for the load's point lookups (nodraw sides facing open
											// space, sky faces' areas): the local maps need at most 1.9M (bhop_mellifluous);
											// a crafted map's looping tree costs 4096 a point

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
	bool FindMap( const char *gameDir, const char *level, char *found, int foundLen )
	{
		std::vector<TLPEntry_t> custom;
		TLP_List( std::string( gameDir ) + "/custom", custom );
		for ( const TLPEntry_t &e : custom )
		{
			snprintf( found, foundLen, "%s/custom/%s/%s", gameDir, e.name.c_str(), level );
			if ( TLP_IsFile( found ) )
				return true;
		}
		snprintf( found, foundLen, "%s/%s", gameDir, level );
		if ( TLP_IsFile( found ) ) return true;
		snprintf( found, foundLen, "%s/download/%s", gameDir, level );
		return TLP_IsFile( found );
	}

	// the entity lump: brush entities (model "*N") -> classname + every output value; area portals (portalnumber N)
	// -> portalOpen[N]: 0 = a func_areaportal that starts closed and nothing can open (no name, no door: the map's
	// logic can't reach it), else 1 (open, or opened / closed while playing: counted as open)
	struct Ent_t { std::string classname, outputs; Vector origin = Vector( 0, 0, 0 ), angles = Vector( 0, 0, 0 ); };
	void ParseEntities( const char *text, int len, std::vector<Ent_t> &models, std::vector<char> &portalOpen )
	{
		const char *p = text, *end = text + len;
		while ( p < end )
		{
			while ( p < end && *p != '{' ) p++;
			if ( p >= end ) break;
			p++;
			Ent_t e;
			int model = -1, portal = -1;
			bool named = false, startOpen = true;
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
				else if ( !TL_StrICmp( key, "portalnumber" ) ) portal = atoi( value );
				else if ( !TL_StrICmp( key, "StartOpen" ) ) startOpen = atoi( value ) != 0;
				else if ( !TL_StrICmp( key, "targetname" ) || !TL_StrICmp( key, "target" ) ) named = named || value[0];
				else if ( !TL_StrICmp( key, "origin" ) || !TL_StrICmp( key, "angles" ) )
				{
					float v[3] = { 0, 0, 0 };
					if ( sscanf( value, "%f %f %f", &v[0], &v[1], &v[2] ) == 3 && fabsf( v[0] ) < 1e6f && fabsf( v[1] ) < 1e6f && fabsf( v[2] ) < 1e6f )
						( key[0] == 'o' || key[0] == 'O' ? e.origin : e.angles ).Init( v[0], v[1], v[2] );
				}
			}
			if ( model > 0 && model < 65536 )
			{
				if ( (int)models.size() <= model )
					models.resize( model + 1 );
				models[model] = e;
			}
			if ( portal > 0 && portal < 65536 )
			{
				if ( (int)portalOpen.size() <= portal )
					portalOpen.resize( portal + 1, 1 );
				portalOpen[portal] = TL_StrICmp( e.classname.c_str(), "func_areaportal" ) || named || startOpen;
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

	// want( side index ) before a side is cut out, keep( side index, its polygon, normal ) after: which sides to add
	// (all for triggers, clips and invisible walls)
	struct AllSides
	{
		bool operator()( int ) const { return true; }
		bool operator()( int, const std::vector<DV> &, const Vector & ) const { return true; }
	};
	template <class Want = AllSides, class Keep = AllSides>
	void AddBrush( const dbrush_t &b, const dbrushside_t *pSides, int nSides, const dplane_t *pPlanes, int nPlanes, TLModel_t &m,
				   long long &chops, int brushId, Want want = Want(), Keep keep = Keep() )
	{
		if ( b.firstside < 0 || b.numsides <= 0 || b.numsides > MAX_BRUSH_SIDES || b.firstside > nSides - b.numsides )
			return;
		const dbrushside_t *sides = pSides + b.firstside;
		std::vector<DV> w;
		for ( int i = 0; i < b.numsides; i++ )
		{
			if ( sides[i].bevel || sides[i].planenum >= nPlanes || !want( i ) )
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
			bool outside = false;
			for ( const DV &p : clean )
				outside = outside || !( fabs( p.x ) < 33000 && fabs( p.y ) < 33000 && fabs( p.z ) < 33000 );
			if ( outside || ++chops > MAX_CHOPS || !keep( i, clean, pl.normal ) )
				continue;
			DV area = { 0, 0, 0 };
			for ( size_t k = 0; k < clean.size(); k++ )
				area = area + Cross( clean[k], clean[( k + 1 ) % clean.size()] );
			double a2 = Dot( area, n );	// twice the area, signed: > 0 = counter-clockwise seen from the front
			if ( fabs( a2 ) < 0.1 )
				continue;
			if ( a2 > 0 )
				std::reverse( clean.begin(), clean.end() );		// the map's faces are clockwise from the front
			// on the grid; then repeated corners and corners turning the wrong way (rounding noise where many planes nearly
			// meet: a cone's tip) dropped
			std::vector<Vector> pts;
			for ( const DV &p : clean )
				pts.push_back( Vector( Snap( p.x ), Snap( p.y ), Snap( p.z ) ) );
			for ( size_t k = 0; pts.size() > 3 && k < pts.size(); )
			{
				const Vector &pa = pts[( k + pts.size() - 1 ) % pts.size()], &pb = pts[k], &pc = pts[( k + 1 ) % pts.size()];
				if ( pb.DistToSqr( pc ) < 1e-4f || DotProduct( CrossProduct( pb - pa, pc - pb ), pl.normal ) > 0 )
				{
					pts.erase( pts.begin() + k );
					k = 0;
				}
				else
					k++;
			}
			TLPoly_t poly;
			poly.first = (int)m.verts.size();
			poly.count = (int)pts.size();
			poly.normal = pl.normal;
			poly.brush = brushId;
			for ( const Vector &v : pts )
			{
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
	m_Planes.clear();
	m_Nodes.clear();
	m_LeafCluster.clear();
	m_LeafArea.clear();
	m_Vis.clear();
	m_Areas.clear();
	m_Portals.clear();
	m_AreaMark.clear();
	m_nClusters = m_nWorldHead = m_nPortalsShut = 0;
	m_OccVerts.clear();
	m_OccFirst.clear();
	m_OccMaterials.clear();
	m_SkyVerts.clear();
	m_SkyFirst.clear();
	m_SkyArea.clear();
	m_WaterVerts.clear();
	m_WaterFirst.clear();
	m_ClusterMark.clear();
	m_NodeMark.clear();
	memset( m_nTriggers, 0, sizeof( m_nTriggers ) );
	m_nPolys = 0;
	m_szLevel[0] = 0;
}

bool CTLMap::Load( const char *pszGameDir, const char *pszLevel, TLOpenMapFn pfnOpen, TLSolidLookingFn pfnSolidLooking )
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
	std::vector<unsigned char> ents, planes, nodes, leafs, models, leafbrushes, brushes, brushsides, vis;
	ok = ReadLump( *file, size, h, LUMP_ENTITIES, ents ) && ReadLump( *file, size, h, LUMP_PLANES, planes ) &&
		 ReadLump( *file, size, h, LUMP_NODES, nodes ) && ReadLump( *file, size, h, LUMP_LEAFS, leafs ) &&
		 ReadLump( *file, size, h, LUMP_MODELS, models ) && ReadLump( *file, size, h, LUMP_LEAFBRUSHES, leafbrushes ) &&
		 ReadLump( *file, size, h, LUMP_BRUSHES, brushes ) && ReadLump( *file, size, h, LUMP_BRUSHSIDES, brushsides );
	if ( ok && !ReadLump( *file, size, h, LUMP_VISIBILITY, vis ) )
		vis.clear();		// (no culling then, not an error)
	std::vector<unsigned char> areas, areaportals;
	if ( ok && !( ReadLump( *file, size, h, LUMP_AREAS, areas ) && ReadLump( *file, size, h, LUMP_AREAPORTALS, areaportals ) ) )
		areas.clear();		// (same)
	// the materials (brush sides: invisible walls, nodraw) and the world's faces (sky faces, solid-looking see-through ones)
	std::vector<unsigned char> faces, texinfo, texdata, strdata, strtable, vertexes, edgesL, surfedges;
	if ( ok && !( ReadLump( *file, size, h, LUMP_TEXINFO, texinfo ) && ReadLump( *file, size, h, LUMP_TEXDATA, texdata ) &&
				  ReadLump( *file, size, h, LUMP_TEXDATA_STRING_DATA, strdata ) && ReadLump( *file, size, h, LUMP_TEXDATA_STRING_TABLE, strtable ) ) )
		texinfo.clear();	// (no invisible walls / nodraw / sky then)
	const bool facesOk = ok && !texinfo.empty() && ReadLump( *file, size, h, LUMP_FACES, faces ) && ReadLump( *file, size, h, LUMP_VERTEXES, vertexes ) &&
						 ReadLump( *file, size, h, LUMP_EDGES, edgesL ) && ReadLump( *file, size, h, LUMP_SURFEDGES, surfedges );
	const bool occOk = facesOk && pfnSolidLooking;
	delete file;
	// leaf layout: version 0 has the ambient light cube (56 bytes), version 1 doesn't (32); the brush list is at 24
	int leafSize = h.lumps[LUMP_LEAFS].version == 0 ? 56 : h.lumps[LUMP_LEAFS].version == 1 ? 32 : 0;
	if ( !ok || !leafSize )
	{
		Warning( "triggerlook: %s: unreadable lumps\n", path );
		return false;
	}

	std::vector<Ent_t> ent;
	std::vector<char> portalOpen;
	ParseEntities( (const char *)ents.data(), (int)ents.size(), ent, portalOpen );

	const dplane_t *pPlanes = (const dplane_t *)planes.data();
	const dnode_t *pNodes = (const dnode_t *)nodes.data();
	const dmodel_t *pModels = (const dmodel_t *)models.data();
	const unsigned short *pLeafBrushes = (const unsigned short *)leafbrushes.data();
	const dbrush_t *pBrushes = (const dbrush_t *)brushes.data();
	const dbrushside_t *pSides = (const dbrushside_t *)brushsides.data();
	int nPlanes = (int)planes.size() / sizeof( dplane_t ), nNodes = (int)nodes.size() / sizeof( dnode_t ), nLeafs = (int)leafs.size() / leafSize;
	int nModels = (int)models.size() / sizeof( dmodel_t ), nLeafBrushes = (int)leafbrushes.size() / 2;
	int nBrushes = (int)brushes.size() / sizeof( dbrush_t ), nSides = (int)brushsides.size() / sizeof( dbrushside_t );
	const int nTexinfo = (int)( texinfo.size() / 72 ), nTexdata = (int)( texdata.size() / 32 ), nStr = (int)( strtable.size() / 4 );

	// a material's name ("TOOLS/TOOLSNODRAW", as in the map) by texdata index; "" = unknown
	auto materialOf = [&]( int td, char *name, int len )
	{
		int sid, so = -1, n = 0;
		if ( td >= 0 && td < nTexdata )
		{
			memcpy( &sid, texdata.data() + (size_t)td * 32 + 12, 4 );
			if ( sid >= 0 && sid < nStr )
				memcpy( &so, strtable.data() + (size_t)sid * 4, 4 );
		}
		for ( ; so >= 0 && so + n < (int)strdata.size() && strdata[so + n] && n < len - 1; n++ )
			name[n] = (char)strdata[so + n];
		name[n] = 0;
	};
	// what a brush side's material is: 0 anything else, 1 toolsinvisible(ladder), 2 toolsnodraw, 3 toolsskip / toolshint
	// (by its name in any folder: maps pack their own copies of the tool textures)
	std::vector<signed char> toolOf( nTexinfo, -1 );
	auto toolKind = [&]( int ti ) -> int
	{
		if ( ti < 0 || ti >= nTexinfo )
			return 0;
		if ( toolOf[ti] < 0 )
		{
			int td;
			char name[260];
			memcpy( &td, texinfo.data() + (size_t)ti * 72 + 68, 4 );
			materialOf( td, name, sizeof( name ) );
			const char *base = name;
			for ( const char *c = name; *c; c++ )
				if ( *c == '/' || *c == '\\' )
					base = c + 1;
			toolOf[ti] = !TL_StrICmp( base, "toolsinvisible" ) || !TL_StrICmp( base, "toolsinvisibleladder" ) ? 1
					   : !TL_StrICmp( base, "toolsnodraw" ) ? 2 : !TL_StrICmp( base, "toolsskip" ) || !TL_StrICmp( base, "toolshint" ) ? 3 : 0;
		}
		return toolOf[ti];
	};
	// a brush: TL_CLIP (player clip), TL_INVISIBLE (stops players, toolsinvisible sides, the rest never drawn),
	// TL_NODRAW (has nodraw sides), else -1
	auto brushKind = [&]( const dbrush_t &br )
	{
		if ( br.contents & CONTENTS_PLAYERCLIP )
			return (int)TL_CLIP;
		if ( br.firstside < 0 || br.numsides <= 0 || br.numsides > MAX_BRUSH_SIDES || br.firstside > nSides - br.numsides )
			return -1;
		int count[4] = {};
		for ( int i = 0; i < br.numsides; i++ )
			if ( !pSides[br.firstside + i].bevel )
				count[toolKind( pSides[br.firstside + i].texinfo )]++;
		return count[1] && !count[0] && ( br.contents & CONTENTS_BLOCKS ) ? (int)TL_INVISIBLE : count[2] ? (int)TL_NODRAW : -1;
	};

	m_Models.resize( nModels );
	std::vector<char> seen( nBrushes, 0 ), nodeSeen( nNodes, 0 );
	std::vector<int> stack, used;
	long long chops = 0;
	long long leafBudget = 2ll * nLeafBrushes + 65536;	// real maps read each leaf's list about once
	// the brushes of brush model mi: every leaf under its head node -> used
	auto modelBrushes = [&]( int mi )
	{
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
			for ( int k = first; k < first + num && k < nLeafBrushes && --leafBudget >= 0; k++ )
			{
				int b = pLeafBrushes[k];
				if ( b < nBrushes && !seen[b] )
				{
					seen[b] = 1;
					used.push_back( b );
				}
			}
		}
		// (seen stays: in a compiled map a brush belongs to one model only)
	};
	// a finished model: counted, or emptied when no side survived
	auto finish = [&]( TLModel_t &m )
	{
		if ( m.polys.empty() )
		{
			m.mins.Init();
			m.maxs.Init();
			return;
		}
		m_nTriggers[m.type]++;
		m_nPolys += (int)m.polys.size();
	};
	for ( int mi = 1; mi < nModels && mi < (int)ent.size(); mi++ )
	{
		int type = TL_Classify( ent[mi].classname.c_str(), ent[mi].outputs.c_str() );
		if ( type < 0 && ( ent[mi].classname.empty() || !TL_StrICmp( ent[mi].classname.c_str(), "func_illusionary" ) ) )
			continue;
		modelBrushes( mi );
		// any other brush entity made of invisible walls only (an invisible func_door / func_brush ...) is one too
		for ( size_t k = 0; k < used.size() && type < 0; k++ )
			if ( brushKind( pBrushes[used[k]] ) != TL_INVISIBLE )
				break;
			else if ( k + 1 == used.size() )
				type = TL_INVISIBLE;
		if ( type < 0 )
			continue;
		TLModel_t &m = m_Models[mi];
		m.type = type;
		m.classname = ent[mi].classname;
		m.origin = ent[mi].origin;
		m.angles = ent[mi].angles;
		m.mins.Init( FLT_MAX, FLT_MAX, FLT_MAX );
		m.maxs.Init( -FLT_MAX, -FLT_MAX, -FLT_MAX );
		for ( size_t k = 0; k < used.size(); k++ )
			AddBrush( pBrushes[used[k]], pSides, nSides, pPlanes, nPlanes, m, chops, (int)k );
		finish( m );
	}
	// does a brush side face open space (not buried in a wall): the world leaf just in front of its middle or of a
	// point near a corner isn't solid. Past the map's walk budget: "solid" (a looping tree mustn't stall the load)
	long long walk = MAX_WALK;
	auto leafSolid = [&]( const DV &p )
	{
		int n = nModels > 0 ? pModels[0].headnode : -1;
		for ( int steps = 0; n >= 0 && steps < 4096; steps++ )
		{
			if ( --walk < 0 )
				return true;
			if ( n >= nNodes || pNodes[n].planenum < 0 || pNodes[n].planenum >= nPlanes )
				return true;
			const dplane_t &pl = pPlanes[pNodes[n].planenum];
			n = pNodes[n].children[p.x * pl.normal.x + p.y * pl.normal.y + p.z * pl.normal.z - pl.dist >= 0 ? 0 : 1];
		}
		int contents = 1;
		if ( n < 0 && -1 - n < nLeafs )
			memcpy( &contents, leafs.data() + (size_t)( -1 - n ) * leafSize, 4 );
		return ( contents & 1 ) != 0;
	};
	auto facesOpen = [&]( const std::vector<DV> &w, const Vector &normal )
	{
		DV c = { 0, 0, 0 }, out = { normal.x, normal.y, normal.z };
		for ( const DV &p : w )
			c = c + p;
		c = c * ( 1.0 / w.size() );
		for ( int k = -1; k < (int)w.size(); k++ )
			if ( !leafSolid( ( k < 0 ? c : w[k] + ( c - w[k] ) * 0.2 ) + out ) )
				return true;
		return false;
	};
	// the world's player clips, invisible walls and brushes with nodraw sides: one model each, after the brush models,
	// kind by kind in the world's brush order (the numbers stay the same for the same file: custom triggers keep them)
	if ( nModels > 0 )
	{
		modelBrushes( 0 );
		std::sort( used.begin(), used.end() );
		std::vector<signed char> kindOf( used.size() );
		for ( size_t u = 0; u < used.size(); u++ )
			kindOf[u] = (signed char)brushKind( pBrushes[used[u]] );
		static const char *const s_Class[] = { "playerclip", "toolsinvisible", "toolsnodraw" };
		for ( int type = TL_CLIP; type <= TL_NODRAW; type++ )
			for ( size_t u = 0; u < used.size(); u++ )
			{
				if ( kindOf[u] != type )
					continue;
				const dbrush_t &br = pBrushes[used[u]];
				TLModel_t m;
				m.type = type;
				m.classname = s_Class[type - TL_CLIP];
				m.origin.Init();
				m.angles.Init();
				m.mins.Init( FLT_MAX, FLT_MAX, FLT_MAX );
				m.maxs.Init( -FLT_MAX, -FLT_MAX, -FLT_MAX );
				if ( type == TL_NODRAW )		// only its nodraw sides a player could see
					AddBrush( br, pSides, nSides, pPlanes, nPlanes, m, chops, 0, [&]( int i ) { return toolKind( pSides[br.firstside + i].texinfo ) == 2; },
							  [&]( int, const std::vector<DV> &w, const Vector &n ) { return facesOpen( w, n ); } );
				else
					AddBrush( br, pSides, nSides, pPlanes, nPlanes, m, chops, 0 );
				if ( m.polys.empty() )
					continue;
				m_Models.push_back( std::move( m ) );
				finish( m_Models.back() );
			}
	}
	// visibility: the world's tree (planes, nodes, each leaf's cluster + area) + the PVS lump + the areas and their
	// portals, checked so lookups stay inside
	int nc = 0;
	if ( vis.size() >= 4 )
		memcpy( &nc, vis.data(), 4 );
	if ( !( nc > 0 && nc < 65536 && (size_t)8 * nc + 4 <= vis.size() ) )
		nc = 0;
	const int nAreas = (int)( areas.size() / sizeof( darea_t ) ), nPortals = (int)( areaportals.size() / sizeof( dareaportal_t ) );
	const bool useAreas = nAreas > 2 && nAreas <= 65536 && nPortals > 0;	// (area 0 = solid; one area = nothing to leave out)
	if ( nModels > 0 && ( nc || useAreas ) )
	{
		m_Planes.resize( nPlanes );
		for ( int i = 0; i < nPlanes; i++ )
			m_Planes[i] = { pPlanes[i].normal, pPlanes[i].dist };
		m_Nodes.resize( nNodes );
		for ( int i = 0; i < nNodes; i++ )
			m_Nodes[i] = { pNodes[i].planenum, { pNodes[i].children[0], pNodes[i].children[1] } };
		m_LeafCluster.resize( nLeafs );
		m_LeafArea.resize( nLeafs );
		for ( int i = 0; i < nLeafs; i++ )
		{
			unsigned short areaFlags;	// area = the low 9 bits
			memcpy( &m_LeafCluster[i], leafs.data() + (size_t)i * leafSize + 4, 2 );
			memcpy( &areaFlags, leafs.data() + (size_t)i * leafSize + 6, 2 );
			m_LeafArea[i] = areaFlags & 0x1FF;
		}
		if ( nc )
		{
			m_Vis.swap( vis );
			m_nClusters = nc;
		}
		m_nWorldHead = pModels[0].headnode;
		if ( useAreas )
		{
			const darea_t *pAreas = (const darea_t *)areas.data();
			const dareaportal_t *pPortals = (const dareaportal_t *)areaportals.data();
			m_Areas.resize( nAreas );
			for ( int i = 0; i < nAreas; i++ )
			{
				const long long first = pAreas[i].firstareaportal, count = pAreas[i].numareaportals;
				const bool fits = first >= 0 && count >= 0 && first + count <= nPortals;
				m_Areas[i] = { fits ? (int)first : 0, fits ? (int)count : 0 };
			}
			m_Portals.resize( nPortals );
			for ( int i = 0; i < nPortals; i++ )
			{
				const int key = pPortals[i].portalKey;
				m_Portals[i] = { pPortals[i].otherArea, key >= (int)portalOpen.size() || portalOpen[key] };
				m_nPortalsShut += !m_Portals[i].open;
			}
			m_nPortalsShut /= 2;	// (each portal is listed by both of its areas)
		}
	}
	if ( chops > MAX_CHOPS )
		Warning( "triggerlook: %s: too many brush sides, the rest are left out\n", path );
	if ( walk < 0 )
		Warning( "triggerlook: %s: its tree is too deep or loops: nodraw sides / sky areas left out\n", path );

	// the world's faces: sky faces (their area: where they're seen from), and the solid-looking see-through surfaces
	// (not sky / nodraw / displacement) whose material the callback names, as polygons. Everything bounds checked (maps
	// come from servers).
	if ( facesOk && nModels > 0 )
	{
		const int nFaces = (int)( faces.size() / sizeof( dface_t ) ), nVerts = (int)( vertexes.size() / 12 );
		const int nEdges = (int)( edgesL.size() / sizeof( dedge_t ) ), nSurf = (int)( surfedges.size() / 4 );
		std::vector<signed char> solidOf( nTexdata, -1 );	// per material: -1 not asked yet, 0 no, 1 yes
		// a face's corners onto the end of out (false: broken, nothing added)
		auto corners = [&]( const dface_t &f, std::vector<Vector> &out )
		{
			const size_t start = out.size();
			for ( int k = 0; k < f.numedges; k++ )
			{
				int se;
				memcpy( &se, surfedges.data() + (size_t)( f.firstedge + k ) * 4, 4 );
				const unsigned int e = se < 0 ? 0u - (unsigned int)se : (unsigned int)se;
				dedge_t ed;
				if ( e < (unsigned int)nEdges )
					memcpy( &ed, edgesL.data() + (size_t)e * sizeof( dedge_t ), sizeof( ed ) );
				const int v = e < (unsigned int)nEdges ? ( se >= 0 ? ed.v[0] : ed.v[1] ) : nVerts;
				if ( v >= nVerts )
				{
					out.resize( start );
					return false;
				}
				Vector p;
				memcpy( &p, vertexes.data() + (size_t)v * 12, 12 );
				out.push_back( p );
			}
			return true;
		};
		const int first = pModels[0].firstface, count = pModels[0].numfaces;
		for ( int fi = MAX( first, 0 ); fi < first + count && fi < nFaces && (int)( m_OccVerts.size() + m_SkyVerts.size() + m_WaterVerts.size() ) < 1000000; fi++ )
		{
			dface_t f;
			memcpy( &f, faces.data() + (size_t)fi * sizeof( dface_t ), sizeof( f ) );
			if ( f.dispinfo != -1 || f.texinfo < 0 || f.texinfo >= nTexinfo || f.numedges < 3 || f.numedges > 64 ||
				 f.firstedge < 0 || f.firstedge > nSurf - f.numedges )
				continue;
			int tiFlags, td;
			memcpy( &tiFlags, texinfo.data() + (size_t)f.texinfo * 72 + 64, 4 );
			memcpy( &td, texinfo.data() + (size_t)f.texinfo * 72 + 68, 4 );
			if ( ( tiFlags & ( 0x2 | 0x4 ) ) && f.planenum < nPlanes )
			{
				// 2D / 3D sky: the area of the point just in front of its middle
				const int start = (int)m_SkyVerts.size();
				if ( !corners( f, m_SkyVerts ) )
					continue;
				Vector c( 0, 0, 0 );
				for ( int k = start; k < (int)m_SkyVerts.size(); k++ )
					c += m_SkyVerts[k];
				c /= (float)f.numedges;
				m_SkyFirst.push_back( start );
				m_SkyArea.push_back( PointArea( c + pPlanes[f.planenum].normal, &walk ) );	// (planenum is the face's own, facing the room)
				continue;
			}
			if ( tiFlags & 0x8 )
			{
				const int start = (int)m_WaterVerts.size();	// water (warp)
				if ( corners( f, m_WaterVerts ) )
					m_WaterFirst.push_back( start );
				continue;
			}
			if ( !occOk || ( tiFlags & ( 0x2 | 0x4 | 0x80 | 0x100 | 0x200 ) ) || td < 0 || td >= nTexdata )
				continue;		// 2D / 3D sky, nodraw, hint, skip
			if ( solidOf[td] < 0 )
			{
				char name[260];
				materialOf( td, name, sizeof( name ) );
				solidOf[td] = name[0] && pfnSolidLooking( name ) ? 1 : 0;
				if ( solidOf[td] )
					m_OccMaterials.push_back( name );
			}
			if ( !solidOf[td] )
				continue;
			const int start = (int)m_OccVerts.size();
			if ( corners( f, m_OccVerts ) )
				m_OccFirst.push_back( start );
		}
		if ( !m_OccFirst.empty() )
			m_OccFirst.push_back( (int)m_OccVerts.size() );
		if ( !m_SkyFirst.empty() )
			m_SkyFirst.push_back( (int)m_SkyVerts.size() );
		if ( !m_WaterFirst.empty() )
			m_WaterFirst.push_back( (int)m_WaterVerts.size() );
	}
	TL_StrCopy( m_szLevel, pszLevel, sizeof( m_szLevel ) );
	Msg( "triggerlook: %s: %d teleport, %d booster, %d push, %d gravity, %d other triggers, %d clips, %d invisible walls, %d with nodraw "
		 "sides (%d sides), %d sky faces, %d vis clusters, %d areas (%d portals closed for good)\n", path,
		 m_nTriggers[TL_TELEPORT], m_nTriggers[TL_BOOSTER], m_nTriggers[TL_PUSH], m_nTriggers[TL_GRAVITY], m_nTriggers[TL_OTHER],
		 m_nTriggers[TL_CLIP], m_nTriggers[TL_INVISIBLE], m_nTriggers[TL_NODRAW], m_nPolys, (int)m_SkyArea.size(), m_nClusters,
		 (int)m_Areas.size(), m_nPortalsShut );
	if ( !m_OccFirst.empty() )
		Msg( "triggerlook: %s: %d surfaces look solid but are drawn see-through (%d materials, first: %s): they hide the triggers behind them\n",
			 path, (int)m_OccFirst.size() - 1, (int)m_OccMaterials.size(), m_OccMaterials[0].c_str() );
	return true;
}

// the leaves of the world tree a box reaches -> their clusters (each once); none = drawn whenever in view (also a box
// over more than MAX_BOX_CLUSTERS: it's everywhere anyway); areas = their areas (each once, none = in every area)
void CTLMap::BoxClusters( const Vector &mins, const Vector &maxs, std::vector<int> &out, std::vector<int> *areas ) const
{
	const size_t MAX_BOX_CLUSTERS = 1024;
	out.clear();
	if ( areas )
		areas->clear();
	const bool wantAreas = areas && !m_Areas.empty();
	if ( !m_nClusters && !wantAreas )
		return;
	if ( m_ClusterMark.size() != (size_t)m_nClusters )
		m_ClusterMark.assign( m_nClusters, 0 );
	if ( m_NodeMark.size() != m_Nodes.size() )
		m_NodeMark.assign( m_Nodes.size(), 0 );
	if ( wantAreas && m_AreaMark.size() != m_Areas.size() )
		m_AreaMark.assign( m_Areas.size(), 0 );
	const Vector c = ( mins + maxs ) * 0.5f, e = ( maxs - mins ) * 0.5f;
	m_Stack.assign( 1, m_nWorldHead );
	m_Visited.clear();
	while ( !m_Stack.empty() && out.size() <= MAX_BOX_CLUSTERS )
	{
		int n = m_Stack.back();
		m_Stack.pop_back();
		if ( n < 0 )
		{
			int leaf = -1 - n;
			int cl = leaf < (int)m_LeafCluster.size() ? m_LeafCluster[leaf] : -1;
			if ( cl >= 0 && cl < m_nClusters && !m_ClusterMark[cl] )
			{
				m_ClusterMark[cl] = 1;
				out.push_back( cl );
			}
			const int a = wantAreas && leaf < (int)m_LeafArea.size() ? m_LeafArea[leaf] : 0;
			if ( a > 0 && a < (int)m_Areas.size() && !m_AreaMark[a] )
			{
				m_AreaMark[a] = 1;
				areas->push_back( a );
			}
			continue;
		}
		if ( n >= (int)m_Nodes.size() || m_NodeMark[n] || m_Nodes[n].plane < 0 || m_Nodes[n].plane >= (int)m_Planes.size() )
			continue;
		m_NodeMark[n] = 1;
		m_Visited.push_back( n );
		const Plane_t &pl = m_Planes[m_Nodes[n].plane];
		const float d = DotProduct( pl.n, c ) - pl.d, r = fabsf( pl.n.x ) * e.x + fabsf( pl.n.y ) * e.y + fabsf( pl.n.z ) * e.z;
		if ( d > -r )
			m_Stack.push_back( m_Nodes[n].child[0] );		// (in front)
		if ( d < r )
			m_Stack.push_back( m_Nodes[n].child[1] );
	}
	for ( int cl : out )
		m_ClusterMark[cl] = 0;
	for ( int n : m_Visited )
		m_NodeMark[n] = 0;
	if ( areas )
		for ( int a : *areas )
			m_AreaMark[a] = 0;
	if ( out.size() > MAX_BOX_CLUSTERS )
	{
		out.clear();
		if ( areas )
			areas->clear();		// (the walk stopped early: not all of them)
	}
}

int CTLMap::PointCluster( const Vector &p ) const
{
	if ( !m_nClusters )
		return -1;
	int leaf = PointLeaf( p );
	int cl = leaf >= 0 && leaf < (int)m_LeafCluster.size() ? m_LeafCluster[leaf] : -1;
	return cl < m_nClusters ? cl : -1;
}

int CTLMap::PointArea( const Vector &p, long long *budget ) const
{
	if ( m_Areas.empty() )
		return 0;
	int leaf = PointLeaf( p, budget );
	return leaf >= 0 && leaf < (int)m_LeafArea.size() ? m_LeafArea[leaf] : 0;
}

// the areas the game draws from area a (bit per area): a + every area joined to it through portals that aren't
// closed for good. False = all of them (area 0: in solid / outside the map, like the game; or the map has no areas)
bool CTLMap::AreaFlood( int area, std::vector<unsigned char> &bits ) const
{
	if ( area <= 0 || area >= (int)m_Areas.size() )
		return false;
	bits.assign( ( m_Areas.size() + 7 ) / 8, 0 );
	bits[area >> 3] |= 1 << ( area & 7 );
	m_Stack.assign( 1, area );
	while ( !m_Stack.empty() )
	{
		const Area_t &ar = m_Areas[m_Stack.back()];
		m_Stack.pop_back();
		for ( int k = ar.first; k < ar.first + ar.count; k++ )
		{
			const int o = m_Portals[k].other;
			if ( m_Portals[k].open && o < (int)m_Areas.size() && !( bits[o >> 3] & ( 1 << ( o & 7 ) ) ) )
			{
				bits[o >> 3] |= 1 << ( o & 7 );
				m_Stack.push_back( o );
			}
		}
	}
	return true;
}

// the leaf a point is in (-1 = no tree, or the budget ran out)
int CTLMap::PointLeaf( const Vector &p, long long *budget ) const
{
	if ( m_Nodes.empty() )
		return -1;
	int n = m_nWorldHead;
	for ( int steps = 0; n >= 0 && steps < 4096; steps++ )
	{
		if ( budget && --*budget < 0 )
			return -1;
		if ( n >= (int)m_Nodes.size() || m_Nodes[n].plane < 0 || m_Nodes[n].plane >= (int)m_Planes.size() )
			return -1;
		const Plane_t &pl = m_Planes[m_Nodes[n].plane];
		n = m_Nodes[n].child[DotProduct( pl.n, p ) - pl.d >= 0 ? 0 : 1];
	}
	return n >= 0 ? -1 : -1 - n;
}

// PVS row of a cluster: run-length coded (a zero byte = that many zero bytes follow)
bool CTLMap::ClusterPVS( int cluster, std::vector<unsigned char> &bits ) const
{
	const int bytes = ( m_nClusters + 7 ) / 8;
	bits.assign( bytes, 0 );
	if ( cluster < 0 || cluster >= m_nClusters )
		return false;
	int ofs;
	memcpy( &ofs, m_Vis.data() + 4 + (size_t)8 * cluster, 4 );
	if ( ofs < 4 || (size_t)ofs >= m_Vis.size() )
		return false;
	const unsigned char *in = m_Vis.data() + ofs, *end = m_Vis.data() + m_Vis.size();
	for ( int o = 0; o < bytes && in < end; )
	{
		if ( *in )
		{
			bits[o++] = *in++;
			continue;
		}
		if ( in + 1 >= end )
			break;
		o += in[1];
		in += 2;
	}
	return true;
}
