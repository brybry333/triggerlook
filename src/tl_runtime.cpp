//=============================================================================
// triggerlook runtime: draws the current map's triggers (and player clips) in the player's own look. All client
// side: the triggers come from the map file (tl_map.cpp), the menu's ON / OFF and SHOW buttons decide what's drawn,
// no server command or plugin is needed. While ON the game's own tools/toolstrigger texture draws nothing (a
// server's !st would otherwise show its look on top).
//
// Per frame: the triggers in view (each placed once per map, meshes built once per look change, already in world
// space), ordered back to front where they can cover each other on screen, copied into dynamic meshes.
//
// Everything goes through interfaces the game exports (CreateInterface), no hooks; the same code on Linux and
// Windows (only tl_platform_*.cpp differs; the SDK's headers give each OS its own interface layout):
//   engine             VEngineClient014        level name, console commands
//                      VModelInfoClient006     model names ("*12")
//                      EngineTraceClient003    SELECT TRIGGER: where the crosshair meets the world
//   client             ClientLeafSystem002     AddRenderable (slot 4), checked against CS:S client.so
//                      VClientEntityList003    the client's entities (triggerlook_ents only)
//   materialsystem     VMaterialSystem082      materials, render context
//   filesystem_stdio   VBaseFileSystem011      maps that aren't loose files
//   vstdlib            VEngineCvar004          our console commands (tl_con.cpp), the setting-change callback
// Start: TriggerLook_Init (Linux: the loader, on the main thread; Windows: bhoptools' thread, then the game
// window's thread through a one-shot message hook) registers the commands and a global setting-change callback,
// which fires on the main thread as soon as the game changes any setting (all the time at startup and when
// joining a server); the first one after VGUI is up creates the panels, and the ticker panel's OnTick runs
// everything after that.
//=============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "tier0/dbg.h"
#include "tier0/platform.h"
#include "tier0/threadtools.h"
#include "tier0/icommandline.h"
#include "tier1/convar.h"
#include "icvar.h"
#include "cdll_int.h"
#include "iclientrenderable.h"
#include "iclientunknown.h"
#include "iclientnetworkable.h"
#include "icliententity.h"
#include "icliententitylist.h"
#include "client_class.h"
#include "basehandle.h"
#include "worldsize.h"
#include "engine/ivmodelinfo.h"
#include "engine/IClientLeafSystem.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imesh.h"
#include "mathlib/vmatrix.h"
#include "inputsystem/ButtonCode.h"
#include "engine/IEngineTrace.h"
#include "gametrace.h"
#include "bspflags.h"
#include "filesystem.h"
#include "view_shared.h"

#include "tl_map.h"
#include "tl_look.h"
#include "tl_mesh.h"
#include "tl_ui.h"
#include "tl_con.h"
#include "tl_platform.h"
#include "tl_util.h"

#ifdef _WIN32
#define TL_EXPORT extern "C" __declspec( dllexport )
#else
#define TL_EXPORT extern "C" __attribute__( ( visibility( "default" ) ) )
#endif

// The leading part of client.so's IClientLeafSystem vtable (game/client/clientleafsystem.h); only AddRenderable
// is called, the slots in between keep the layout.
abstract_class ITLLeafSystem : public IClientLeafSystemEngine
{
public:
	virtual void AddRenderable( IClientRenderable *pRenderable, RenderGroup_t group ) = 0;		// slot 4
	virtual bool IsRenderableInPVS( IClientRenderable *pRenderable ) = 0;
	virtual void SetSubSystemDataInLeaf( int leaf, int nSubSystemIdx, void *pData ) = 0;
	virtual void *GetSubSystemDataInLeaf( int leaf, int nSubSystemIdx ) = 0;
	virtual void SetDetailObjectsInLeaf( int leaf, int firstDetailObject, int detailObjectCount ) = 0;
	virtual void GetDetailObjectsInLeaf( int leaf, int &firstDetailObject, int &detailObjectCount ) = 0;
	virtual void DrawDetailObjectsInLeaf( int leaf, int frameNumber, int &firstDetailObject, int &detailObjectCount ) = 0;
	virtual bool ShouldDrawDetailObjectsInLeaf( int leaf, int frameNumber ) = 0;
	virtual void RenderableChanged( ClientRenderHandle_t handle ) = 0;							// slot 12
};

static IVEngineClient *s_pEngine;
static ITLLeafSystem *s_pLeafSystem;
static IMaterialSystem *s_pMatSys;
static IVModelInfoClient *s_pModelInfo;
static IBaseFileSystem *s_pFileSystem;
static IClientEntityList *s_pEntList;
static IBaseClientDLL *s_pClient;		// GetPlayerView: which view is the player's
static IEngineTrace *s_pTrace;			// SELECT TRIGGER (without it the world doesn't hide triggers from the pick)

#define TRIGGERLOOK_VERSION "1.1"

static bool s_bEnable = true;	// triggerlook_enable
static bool s_bCull = true;		// triggerlook_cull

CON_COMMAND( triggerlook_enable, "triggerlook_enable [0/1]: 0 = triggerlook off entirely (the game's own trigger look, the menu does nothing)" )
{
	if ( args.ArgC() > 1 )
		s_bEnable = atoi( args[1] ) != 0;
	Msg( "triggerlook_enable %d\n", s_bEnable ? 1 : 0 );
}

CON_COMMAND( triggerlook_show, "triggerlook_show [0/1]: the menu's ON / OFF (no number = switch): draw this map's triggers in your look" )
{
	g_bTLShow = args.ArgC() > 1 ? atoi( args[1] ) != 0 : !g_bTLShow;
	TL_LookChanged( 0 );	// (saved; no mesh change)
	Msg( "triggerlook_show %d\n", g_bTLShow ? 1 : 0 );
}

CON_COMMAND( triggerlook_cull, "triggerlook_cull [0/1]: skip triggers outside the view (0 = testing)" )
{
	if ( args.ArgC() > 1 )
		s_bCull = atoi( args[1] ) != 0;
	Msg( "triggerlook_cull %d\n", s_bCull ? 1 : 0 );
}

//-----------------------------------------------------------------------------
// Interfaces from already-loaded game modules
//-----------------------------------------------------------------------------
static bool ConnectInterfaces()
{
	if ( !s_pEngine )		s_pEngine = (IVEngineClient *)TLP_Interface( "engine", "VEngineClient014" );
	if ( !s_pLeafSystem )	s_pLeafSystem = (ITLLeafSystem *)TLP_Interface( "client", "ClientLeafSystem002" );
	if ( !s_pEntList )		s_pEntList = (IClientEntityList *)TLP_Interface( "client", VCLIENTENTITYLIST_INTERFACE_VERSION );
	if ( !s_pMatSys )		s_pMatSys = (IMaterialSystem *)TLP_Interface( "materialsystem", "VMaterialSystem082" );
	if ( !s_pModelInfo )	s_pModelInfo = (IVModelInfoClient *)TLP_Interface( "engine", VMODELINFO_CLIENT_INTERFACE_VERSION );
	if ( !s_pFileSystem )	s_pFileSystem = (IBaseFileSystem *)TLP_Interface( "filesystem_stdio", "VBaseFileSystem011" );
	if ( !s_pClient )		s_pClient = (IBaseClientDLL *)TLP_Interface( "client", CLIENT_DLL_INTERFACE_VERSION );
	if ( !s_pTrace )		s_pTrace = (IEngineTrace *)TLP_Interface( "engine", INTERFACEVERSION_ENGINETRACE_CLIENT );
	return s_pEngine && s_pLeafSystem && s_pEntList && s_pMatSys && s_pModelInfo;
}

// the map as the engine found it (its search order: custom/, the game folder, VPKs, download/); only the lumps the
// reader needs are read
struct GameMapFile : TLMapFile
{
	FileHandle_t h;
	explicit GameMapFile( FileHandle_t file ) : h( file ) {}
	~GameMapFile() { s_pFileSystem->Close( h ); }
	long long Size() override { return s_pFileSystem->Size( h ); }
	bool Read( long long ofs, void *dst, int len ) override
	{
		if ( ofs < 0 || ofs > 0x7fffffff )
			return false;
		s_pFileSystem->Seek( h, (int)ofs, FILESYSTEM_SEEK_HEAD );
		return s_pFileSystem->Read( dst, len, h ) == len;
	}
};

static TLMapFile *OpenGameMap( const char *level )
{
	FileHandle_t h = s_pFileSystem ? s_pFileSystem->Open( level, "rb", "GAME" ) : NULL;
	return h ? new GameMapFile( h ) : NULL;
}

//-----------------------------------------------------------------------------
// The map's triggers and their meshes
//-----------------------------------------------------------------------------
static CTLMap s_Map;
static std::vector<TLMesh_t> s_Meshes;		// by model number, built when first drawn / after a look change

static int s_nShown[TL_TYPES];				// in view and drawn, by type (last frame)
static int s_nDrawn, s_nDrawnSides;

int TL_InMap( int type ) { return type >= 0 && type < TL_TYPES ? s_Map.m_nTriggers[type] : 0; }
int TL_ModelType( int model ) { const TLModel_t *m = s_Map.Model( model ); return m ? m->type : -1; }

//-----------------------------------------------------------------------------
// Custom triggers (tl_look.h) on this map: matched to its models by number and box; a different file under the same
// map name (renumbered models) by the box alone, when only one trigger has it
//-----------------------------------------------------------------------------
static std::vector<int> s_CTOf;				// by model number: index into g_TLCT, -1 = none
static std::vector<int> s_MapCTs, s_MapCTModels;	// this map's (g_TLCT order) and their models
static int s_nCTListMatched = -1;
static std::string s_CTMapMatched;

// "maps/foo.bsp" -> "foo"
static std::string MapName( const char *level )
{
	const char *slash = strrchr( level, '/' );
	std::string n = slash ? slash + 1 : level;
	if ( n.size() > 4 && !TL_StrICmp( n.c_str() + n.size() - 4, ".bsp" ) )
		n.resize( n.size() - 4 );
	return n;
}

static bool SameBox( const TLModel_t *m, const TLCustomTrigger_t &ct )
{
	if ( !m )
		return false;
	for ( int i = 0; i < 3; i++ )
		if ( fabsf( m->mins[i] - ct.mins[i] ) > 0.05f || fabsf( m->maxs[i] - ct.maxs[i] ) > 0.05f )
			return false;
	return true;
}

static void MatchCTs()
{
	std::string map = MapName( s_Map.Level() );
	if ( s_nCTListMatched == g_nTLCTListVersion && map == s_CTMapMatched && (int)s_CTOf.size() == s_Map.NumModels() )
		return;
	s_nCTListMatched = g_nTLCTListVersion;
	s_CTMapMatched = map;
	s_CTOf.assign( s_Map.NumModels(), -1 );
	s_MapCTs.clear();
	s_MapCTModels.clear();
	for ( int i = 0; i < (int)g_TLCT.size() && !map.empty(); i++ )
	{
		const TLCustomTrigger_t &ct = g_TLCT[i];
		if ( TL_StrICmp( ct.map.c_str(), map.c_str() ) )
			continue;
		int model = SameBox( s_Map.Model( ct.model ), ct ) ? ct.model : -1;
		for ( int k = 1; model < 0 && k < s_Map.NumModels(); k++ )
			if ( SameBox( s_Map.Model( k ), ct ) )
			{
				for ( int j = k + 1; j < s_Map.NumModels(); j++ )
					if ( SameBox( s_Map.Model( j ), ct ) )
						k = s_Map.NumModels();		// two have that box: neither
				if ( k < s_Map.NumModels() )
					model = k;
			}
		if ( model >= 0 && s_CTOf[model] < 0 )
		{
			s_CTOf[model] = i;
			s_MapCTs.push_back( i );
			s_MapCTModels.push_back( model );
		}
	}
}

const std::vector<int> &TL_MapCTs() { MatchCTs(); return s_MapCTs; }

int TL_CTLabel( int index )
{
	MatchCTs();
	for ( size_t k = 0; k < s_MapCTs.size(); k++ )
		if ( s_MapCTs[k] == index )
			return (int)k + 1;
	return 0;
}

int TL_CTType( int index )
{
	int n = TL_CTLabel( index );
	return n ? TL_ModelType( s_MapCTModels[n - 1] ) : -1;
}

int TL_ModelCT( int model )
{
	MatchCTs();
	return model > 0 && model < (int)s_CTOf.size() ? s_CTOf[model] : -1;
}

// the custom trigger of a trigger of this map, made from its type's look if it has none; -1 = no such trigger
int TL_PickedCT( int model )
{
	MatchCTs();
	const TLModel_t *m = s_Map.Model( model );
	if ( !m )
		return -1;
	if ( s_CTOf[model] >= 0 )
		return s_CTOf[model];
	TLCustomTrigger_t ct;
	ct.map = MapName( s_Map.Level() );
	ct.model = model;
	ct.mins = m->mins;
	ct.maxs = m->maxs;
	ct.look = g_TLLook[m->type];
	ct.look.show = true;
	ct.version = TL_NewVersion();
	g_TLCT.push_back( ct );
	TL_CTListChanged();
	return (int)g_TLCT.size() - 1;
}

//-----------------------------------------------------------------------------
// SELECT TRIGGER: while picking, the nearest drawn trigger side under the crosshair (not behind the world) is the
// hovered one, drawn white (black when its own fill is white already)
//-----------------------------------------------------------------------------
static bool s_bPicking;
static int s_nPickHover = -1;
static double s_flPickTime;
static TLMesh_t s_HLMesh;
static int s_nHLModel = -1, s_nHLVersion = -1;
static float s_flPickWorld;			// (triggerlook_info) how far the world was along the crosshair

void TL_SetPicking( bool on ) { s_bPicking = on; s_nPickHover = -1; }
int TL_PickHover() { return s_bPicking && Plat_FloatTime() - s_flPickTime < 0.25 ? s_nPickHover : -1; }

static TLLook_t Highlight( const TLLook_t &look )
{
	TLLook_t l = look;
	unsigned char c = MIN( look.fill[0], MIN( look.fill[1], look.fill[2] ) ) >= 200 ? 0 : 255;
	l.fill[0] = l.fill[1] = l.fill[2] = c;
	l.fill[3] = 170;
	l.border[0] = l.border[1] = l.border[2] = c;
	l.border[3] = 255;
	l.width = 2;
	return l;
}

// how far along the ray the world is (a little past it, so a trigger side flush with a wall still counts)
static float WorldDist( const Vector &from, const Vector &dir )
{
	const float range = 32768.0f;
	if ( !s_pTrace )
		return range;
	Ray_t ray;
	ray.Init( from, from + dir * range );
	CTraceFilterWorldOnly filter;
	trace_t tr;
	s_pTrace->TraceRay( ray, MASK_OPAQUE, &filter, &tr );
	return tr.startsolid ? range : tr.fraction * range + 1.0f;
}

// distance to the nearest of the trigger's front sides the ray goes into, -1 = none
static float RayHit( const TLModel_t &m, const Vector &eye, const Vector &dir )
{
	float best = -1;
	for ( const TLPoly_t &p : m.polys )
	{
		const Vector &v0 = m.verts[p.first];
		float facing = DotProduct( p.normal, eye - v0 ), along = DotProduct( p.normal, dir );
		if ( facing <= 0 || along >= 0 )
			continue;
		float t = -facing / along;
		if ( best >= 0 && t >= best )
			continue;
		Vector hit = eye + dir * t;
		bool inside = true;
		for ( int k = 0; k < p.count && inside; k++ )
		{
			const Vector &a = m.verts[p.first + k], &b = m.verts[p.first + ( k + 1 ) % p.count];
			inside = DotProduct( CrossProduct( b - a, hit - a ), p.normal ) <= 0.01f;	// sides are clockwise from the front
		}
		if ( inside )
			best = t;
	}
	return best;
}

//-----------------------------------------------------------------------------
// Drawing: all visible sides into dynamic meshes, as few as fit
//-----------------------------------------------------------------------------
static IMaterial *s_pDrawMaterial;		// materials/triggerlook/trigger.vmt: unlit, vertex colour + alpha, decal depth bias
static double s_tFlush;					// (diagnosis) time in Flush this draw
static double s_tStage[4];				// (diagnosis) last draw: collect, order, copy, flush (seconds)
static Vector s_vecLastCam;
static std::vector<TLVert_t> s_BatchVerts;
static std::vector<unsigned short> s_BatchIndices;
static int s_nMaxVerts, s_nMaxIndices;

// what the batches hold this frame: chunks of at most what one dynamic mesh takes (indices count from the chunk's
// first vertex); a second view with the same camera in the same frame (the water refraction view and the main view)
// sends the same chunks again instead of building them twice
struct Chunk_t { int firstVert, numVerts, firstIndex, numIndices; };
static std::vector<Chunk_t> s_Chunks;
static int s_nFrame;					// TL_Tick counts frames
static int s_nBuiltFrame = -1, s_nReused, s_nReusedLast;
static Vector s_vecBuiltCam;

static void SubmitChunks( IMatRenderContext *ctx )
{
	const double tf = Plat_FloatTime();
	for ( const Chunk_t &c : s_Chunks )
	{
		if ( !c.numIndices )
			continue;
		IMesh *pMesh = ctx->GetDynamicMesh( true );
		CMeshBuilder mb;
		mb.Begin( pMesh, MATERIAL_TRIANGLES, c.numVerts, c.numIndices );
		for ( int i = 0; i < c.numVerts; i++ )
		{
			const TLVert_t &v = s_BatchVerts[c.firstVert + i];
			mb.Position3fv( v.pos.Base() );
			mb.Color4ubv( v.rgba );
			mb.TexCoord2f( 0, 0, 0 );
			mb.AdvanceVertex();
		}
		for ( int i = 0; i < c.numIndices; i++ )
		{
			mb.Index( s_BatchIndices[c.firstIndex + i] );
			mb.AdvanceIndex();
		}
		mb.End();
		pMesh->Draw();
	}
	s_tFlush += Plat_FloatTime() - tf;
}

static bool IsIdentity( const matrix3x4_t &m )
{
	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 4; c++ )
			if ( fabsf( m[r][c] - ( r == c ? 1.0f : 0.0f ) ) > 1e-4f )
				return false;
	return true;
}

// where each model is drawn (once per map): the map's origin / angles keys (most triggers have none), its world box
struct Place_t { matrix3x4_t xf; bool moved = false; Vector mins, maxs; std::vector<int> clusters; TLModel_t grown; };
static std::vector<Place_t> s_Place;

static void PlaceModels()
{
	s_Place.assign( s_Map.NumModels(), Place_t() );
	for ( int mi = 1; mi < s_Map.NumModels(); mi++ )
		if ( const TLModel_t *m = s_Map.Model( mi ) )
		{
			Place_t &p = s_Place[mi];
			TL_AngleMatrix( m->angles, m->origin, p.xf );
			p.moved = !IsIdentity( p.xf );
			if ( p.moved )
				TL_TransformAABB( p.xf, m->mins, m->maxs, p.mins, p.maxs );
			else
			{
				p.mins = m->mins;
				p.maxs = m->maxs;
			}
			s_Map.BoxClusters( p.mins - Vector( 1, 1, 1 ), p.maxs + Vector( 1, 1, 1 ), p.clusters );
			TL_ExpandModel( *m, TL_DRAW_GROW_X100 / 100.0f, p.grown );	// (what's drawn; picking + custom triggers use the map's own)
		}
}

// a model's mesh in its look, already where the map puts it (nothing to transform per frame)
static void BuildPlacedMesh( int index, const TLModel_t &m, const TLLook_t &look, TLMesh_t &out )
{
	const Place_t &p = s_Place[index];
	TL_BuildMesh( p.grown.polys.size() == m.polys.size() ? p.grown : m, look, out );
	if ( p.moved )
		for ( TLVert_t &v : out.verts )
		{
			Vector local = v.pos;
			TL_Transform( local, p.xf, v.pos );
		}
}

// the view frustum's four side planes (a x + b y + c z + d >= 0 inside), from projection x view
static float s_Planes[4][4];
static VMatrix s_VP;		// projection x view (screen rectangles for the draw order)
static void SetupFrustum( IMatRenderContext *ctx )
{
	VMatrix view, proj;
	ctx->GetMatrix( MATERIAL_VIEW, &view );
	ctx->GetMatrix( MATERIAL_PROJECTION, &proj );
	TL_MatMul( proj, view, s_VP );
	const VMatrix &vp = s_VP;
	for ( int k = 0; k < 4; k++ )
	{
		s_Planes[0][k] = vp.m[3][k] + vp.m[0][k];
		s_Planes[1][k] = vp.m[3][k] - vp.m[0][k];
		s_Planes[2][k] = vp.m[3][k] + vp.m[1][k];
		s_Planes[3][k] = vp.m[3][k] - vp.m[1][k];
	}
}

static bool OutsideView( const Vector &mins, const Vector &maxs )
{
	for ( int i = 0; i < 4; i++ )
	{
		const float *p = s_Planes[i];
		Vector v( p[0] >= 0 ? maxs.x : mins.x, p[1] >= 0 ? maxs.y : mins.y, p[2] >= 0 ? maxs.z : mins.z );
		if ( p[0] * v.x + p[1] * v.y + p[2] * v.z + p[3] < 0 )
			return true;
	}
	return false;
}

// The triggers in view are drawn far to near: they're see-through and don't hide each other, so the one in front
// has to go on top. Two triggers whose boxes don't overlap have a flat plane between them (one of the box faces): the
// one on the camera's side of it can't be hidden by the other, so it goes later (a big teleport floor under a box
// gets drawn first even when its middle is nearer). Boxes that overlap go by the distance to their middles.
// Only triggers that can cover each other on screen need that (their screen rectangles overlap): they're found by a
// left-to-right sweep over the rectangles, so the cost follows what's actually stacked up in view, not n x n.
struct DrawTrig_t
{
	const TLMesh_t *mesh;
	const TLModel_t *model;
	Vector eye;			// the camera in the trigger's own space (which sides face it)
	Vector mins, maxs;	// world box
	float dist;			// camera to the middle of the box, squared
};
static std::vector<DrawTrig_t> s_DrawTrigs;
static std::vector<int> s_DrawOrder;

// +1: a before b, -1: b before a, 0: their boxes overlap
static int DrawBefore( const DrawTrig_t &a, const DrawTrig_t &b, const Vector &camera )
{
	for ( int k = 0; k < 3; k++ )
	{
		if ( a.maxs[k] <= b.mins[k] + 0.1f )
			return camera[k] >= ( a.maxs[k] + b.mins[k] ) * 0.5f ? 1 : -1;
		if ( b.maxs[k] <= a.mins[k] + 0.1f )
			return camera[k] >= ( b.maxs[k] + a.mins[k] ) * 0.5f ? -1 : 1;
	}
	return 0;
}

// the part of the screen a box can cover: its corners projected (-1..1); one reaching near or behind the camera: all
struct TLRect_t { float x0, y0, x1, y1; };
static TLRect_t ScreenRect( const Vector &mins, const Vector &maxs )
{
	TLRect_t r = { 1e30f, 1e30f, -1e30f, -1e30f };
	const float *mx = s_VP.m[0], *my = s_VP.m[1], *mw = s_VP.m[3];
	for ( int c = 0; c < 8; c++ )
	{
		const float x = c & 1 ? maxs.x : mins.x, y = c & 2 ? maxs.y : mins.y, z = c & 4 ? maxs.z : mins.z;
		const float w = mw[0] * x + mw[1] * y + mw[2] * z + mw[3];
		if ( w < 1.0f )
			return { -1e30f, -1e30f, 1e30f, 1e30f };
		const float sx = ( mx[0] * x + mx[1] * y + mx[2] * z + mx[3] ) / w, sy = ( my[0] * x + my[1] * y + my[2] * z + my[3] ) / w;
		r.x0 = MIN( r.x0, sx ); r.x1 = MAX( r.x1, sx );
		r.y0 = MIN( r.y0, sy ); r.y1 = MAX( r.y1, sy );
	}
	return r;
}

static const int MAX_ORDER_PAIRS = 20000;	// pairs checked per frame at most (the rest keeps far to near)
static std::vector<TLRect_t> s_Rects;
static std::vector<int> s_ByX, s_InDeg, s_Rank, s_First, s_Next, s_Heap, s_ByDist;
static std::vector<std::pair<int, int>> s_Edges;	// (i, j): i is drawn before j
static std::vector<char> s_Done;
static int s_nOrderPairs;					// (triggerlook_info) pairs checked last frame

static void OrderTriggers( const Vector &camera )
{
	const int n = (int)s_DrawTrigs.size();
	s_DrawOrder.resize( n );
	for ( int i = 0; i < n; i++ )
		s_DrawOrder[i] = i;
	std::sort( s_DrawOrder.begin(), s_DrawOrder.end(), []( int a, int b ) { return s_DrawTrigs[a].dist > s_DrawTrigs[b].dist; } );
	s_nOrderPairs = 0;
	if ( n < 2 )
		return;
	// the pairs that can cover each other on screen -> "before" edges
	s_Rects.resize( n );
	s_ByX.resize( n );
	for ( int i = 0; i < n; i++ )
	{
		s_Rects[i] = ScreenRect( s_DrawTrigs[i].mins, s_DrawTrigs[i].maxs );
		s_ByX[i] = i;
	}
	std::sort( s_ByX.begin(), s_ByX.end(), []( int a, int b ) { return s_Rects[a].x0 < s_Rects[b].x0; } );
	s_Edges.clear();
	s_InDeg.assign( n, 0 );
	int pairs = 0;
	for ( int a = 0; a < n && pairs < MAX_ORDER_PAIRS; a++ )
	{
		const int i = s_ByX[a];
		const TLRect_t &ri = s_Rects[i];
		for ( int b = a + 1; b < n && s_Rects[s_ByX[b]].x0 <= ri.x1 && pairs < MAX_ORDER_PAIRS; b++ )
		{
			const int j = s_ByX[b];
			const TLRect_t &rj = s_Rects[j];
			if ( rj.y0 > ri.y1 || rj.y1 < ri.y0 )
				continue;
			pairs++;
			const int r = DrawBefore( s_DrawTrigs[i], s_DrawTrigs[j], camera );
			if ( r > 0 ) { s_Edges.push_back( { i, j } ); s_InDeg[j]++; }
			else if ( r < 0 ) { s_Edges.push_back( { j, i } ); s_InDeg[i]++; }
		}
	}
	s_nOrderPairs = pairs;
	if ( s_Edges.empty() )
		return;
	// edges by their first trigger
	s_First.assign( n + 1, 0 );
	for ( const auto &e : s_Edges )
		s_First[e.first + 1]++;
	for ( int i = 0; i < n; i++ )
		s_First[i + 1] += s_First[i];
	s_Next.resize( s_Edges.size() );
	{
		std::vector<int> &at = s_Rank;		// (borrowed as the fill cursor)
		at.assign( s_First.begin(), s_First.end() - 1 );
		for ( const auto &e : s_Edges )
			s_Next[at[e.first]++] = e.second;
	}
	// each step: the farthest one nothing has to come before (a loop, which boxes can make: the farthest left).
	// Heap of distance ranks (0 = farthest).
	s_ByDist = s_DrawOrder;
	s_Rank.resize( n );
	for ( int k = 0; k < n; k++ )
		s_Rank[s_ByDist[k]] = k;
	s_Done.assign( n, 0 );
	s_Heap.clear();
	const auto later = std::greater<int>();
	for ( int k = 0; k < n; k++ )
		if ( !s_InDeg[s_ByDist[k]] )
			s_Heap.push_back( k );		// (ranks in order: already a heap)
	int farLeft = 0;
	for ( int step = 0; step < n; step++ )
	{
		int pick;
		if ( !s_Heap.empty() )
		{
			std::pop_heap( s_Heap.begin(), s_Heap.end(), later );
			pick = s_ByDist[s_Heap.back()];
			s_Heap.pop_back();
		}
		else
		{
			while ( s_Done[s_ByDist[farLeft]] )
				farLeft++;
			pick = s_ByDist[farLeft];
		}
		s_Done[pick] = 1;
		s_DrawOrder[step] = pick;
		for ( int e = s_First[pick]; e < s_First[pick + 1]; e++ )
		{
			const int j = s_Next[e];
			if ( --s_InDeg[j] == 0 && !s_Done[j] )
			{
				s_Heap.push_back( s_Rank[j] );
				std::push_heap( s_Heap.begin(), s_Heap.end(), later );
			}
		}
	}
}

static bool s_bPVS = true;					// triggerlook_pvs
static int s_nCamCluster = -1, s_nPVSCluster = -2, s_nPVSCulled, s_nPVSCulledCluster;
static std::vector<unsigned char> s_PVS;	// the camera cluster's PVS row
static std::vector<char> s_ModelSeen;		// by model: the camera cluster can see it (worked out once per cluster)
static std::vector<int> s_SeenModels;		// ... the list of those
static VMatrix s_BuiltVP;

CON_COMMAND( triggerlook_pvs, "triggerlook_pvs [0/1]: leave out triggers the map's visibility says can't be seen from here (0 = testing)" )
{
	if ( args.ArgC() > 1 )
		s_bPVS = atoi( args[1] ) != 0;
	Msg( "triggerlook_pvs %d\n", s_bPVS ? 1 : 0 );
}

static void BuildChunks( IMatRenderContext *ctx, const Vector &camera, const Vector &fwd );

static void DrawTriggers( IMatRenderContext *ctx, const Vector &camera, const Vector &fwd )
{
	if ( !s_bEnable || !g_bTLShow || !s_pDrawMaterial || !s_Map.Level()[0] || (int)s_Place.size() != s_Map.NumModels() )
	{
		memset( s_nShown, 0, sizeof( s_nShown ) );
		s_nDrawn = s_nDrawnSides = 0;
		s_flPickTime = Plat_FloatTime();
		s_nPickHover = -1;
		s_nBuiltFrame = -1;
		return;
	}
	SetupFrustum( ctx );
	ctx->Bind( s_pDrawMaterial );
	IMesh *pMesh = ctx->GetDynamicMesh( true );
	ctx->GetMaxToRender( pMesh, false, &s_nMaxVerts, &s_nMaxIndices );
	s_nMaxVerts = MIN( s_nMaxVerts, 32767 );
	s_nMaxIndices = MIN( s_nMaxIndices, 32767 * 3 );
	if ( s_nMaxVerts < 64 || s_nMaxIndices < 192 )
		return;
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PushMatrix();
	ctx->LoadIdentity();
	s_tFlush = 0;
	const double t0 = Plat_FloatTime();
	if ( s_nBuiltFrame == s_nFrame && camera == s_vecBuiltCam && !memcmp( &s_BuiltVP, &s_VP, sizeof( VMatrix ) ) )
		s_nReused++;
	else
	{
		BuildChunks( ctx, camera, fwd );
		s_nBuiltFrame = s_nFrame;
		s_vecBuiltCam = camera;
		memcpy( &s_BuiltVP, &s_VP, sizeof( VMatrix ) );
	}
	const double t3 = Plat_FloatTime();
	SubmitChunks( ctx );
	s_tStage[3] = s_tFlush;
	if ( !s_nReused )
		s_tStage[2] = t3 - t0 - s_tStage[0] - s_tStage[1];
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PopMatrix();
}

// the triggers in view, ordered, into s_Chunks
static void BuildChunks( IMatRenderContext *ctx, const Vector &camera, const Vector &fwd )
{
	memset( s_nShown, 0, sizeof( s_nShown ) );
	s_nDrawn = s_nDrawnSides = s_nPVSCulled = 0;
	const bool picking = s_bPicking;
	float pickDist = picking ? WorldDist( camera, fwd ) : 0;
	s_flPickWorld = pickDist;
	int pickModel = -1;
	const int hover = s_nPickHover;		// (found last frame, lit up this frame)
	s_flPickTime = Plat_FloatTime();
	MatchCTs();
	const bool cull = s_bCull;
	// the map's visibility from the camera's cluster (none: in solid / outside / no vis data = everything)
	s_nCamCluster = s_bPVS ? s_Map.PointCluster( camera ) : -1;
	if ( s_nCamCluster != s_nPVSCluster || (int)s_ModelSeen.size() != s_Map.NumModels() )
	{
		s_nPVSCluster = s_nCamCluster;
		if ( s_nCamCluster < 0 || !s_Map.ClusterPVS( s_nCamCluster, s_PVS ) )
			s_PVS.clear();
		// each model once for this cluster: in a cluster the camera's can see (a box in no cluster at all: drawn)
		s_ModelSeen.assign( s_Map.NumModels(), 1 );
		s_SeenModels.clear();
		for ( int mi = 1; mi < s_Map.NumModels(); mi++ )
		{
			if ( !s_Map.Model( mi ) )
				continue;
			const std::vector<int> &cls = s_Place[mi].clusters;
			bool seen = s_PVS.empty() || cls.empty();
			for ( size_t k = 0; k < cls.size() && !seen; k++ )
				seen = ( s_PVS[cls[k] >> 3] & ( 1 << ( cls[k] & 7 ) ) ) != 0;
			s_ModelSeen[mi] = seen;
			if ( seen )
				s_SeenModels.push_back( mi );
		}
		s_nPVSCulledCluster = 0;
		for ( int mi = 1; mi < s_Map.NumModels(); mi++ )
			s_nPVSCulledCluster += s_Map.Model( mi ) && !s_ModelSeen[mi];
	}

	// one trigger of the map (model *index): culled, picked, drawn in its look
	auto add = [&]( int index, const TLModel_t *m )
	{
		// a custom trigger's own look (drawn even when its type is hidden), else its type's
		const int ct = s_CTOf[index];
		const TLLook_t &look = ct >= 0 ? g_TLCT[ct].look : g_TLLook[m->type];
		if ( ct < 0 && !look.show )
			return;
		const Place_t &pl = s_Place[index];
		if ( cull && OutsideView( pl.mins - Vector( 1, 1, 1 ), pl.maxs + Vector( 1, 1, 1 ) ) )
			return;
		s_nShown[m->type]++;
		const int version = ct >= 0 ? g_TLCT[ct].version : g_nTLTypeVersion[m->type];
		Vector eye = camera;
		if ( pl.moved )
			TL_ITransform( camera, pl.xf, eye );		// the camera in the trigger's own space
		if ( picking )
		{
			Vector dir = fwd;
			if ( pl.moved )
			{
				TL_ITransform( camera + fwd, pl.xf, dir );
				dir -= eye;
			}
			float d = RayHit( *m, eye, dir );
			if ( d >= 0 && d < pickDist )
			{
				pickDist = d;
				pickModel = index;
			}
		}
		TLMesh_t *pMesh = &s_Meshes[index];
		if ( picking && index == hover )
		{
			pMesh = &s_HLMesh;
			if ( s_nHLModel != index || s_nHLVersion != version )
			{
				BuildPlacedMesh( index, *m, Highlight( look ), s_HLMesh );
				s_nHLModel = index;
				s_nHLVersion = version;
			}
		}
		else if ( pMesh->version != version )
		{
			BuildPlacedMesh( index, *m, look, *pMesh );
			pMesh->version = version;
		}
		s_nDrawn++;
		s_DrawTrigs.push_back( { pMesh, m, eye, pl.mins, pl.maxs, ( ( pl.mins + pl.maxs ) * 0.5f - camera ).LengthSqr() } );
	};
	const double t0 = Plat_FloatTime();
	s_vecLastCam = camera;
	for ( int mi : s_SeenModels )
		add( mi, s_Map.Model( mi ) );
	s_nPVSCulled = s_nPVSCulledCluster;
	const double t1 = Plat_FloatTime();
	OrderTriggers( camera );
	const double t2 = Plat_FloatTime();
	s_BatchVerts.clear();
	s_BatchIndices.clear();
	s_Chunks.assign( 1, Chunk_t{ 0, 0, 0, 0 } );
	for ( int ti : s_DrawOrder )
	{
		const DrawTrig_t &d = s_DrawTrigs[ti];
		for ( size_t si = 0; si < d.mesh->sides.size(); si++ )
		{
			const TLSideMesh_t &side = d.mesh->sides[si];
			const TLPoly_t &poly = d.model->polys[si];
			// only the sides facing the camera (the GPU would drop the others anyway)
			if ( !side.numIndices || DotProduct( poly.normal, d.eye - d.model->verts[poly.first] ) <= 0 )
				continue;
			if ( side.numVerts > s_nMaxVerts || side.numIndices > s_nMaxIndices )
				continue;		// (can't happen with the reader's 128-side brushes)
			s_nDrawnSides++;
			Chunk_t *c = &s_Chunks.back();
			if ( c->numVerts + side.numVerts > s_nMaxVerts || c->numIndices + side.numIndices > s_nMaxIndices )
			{
				s_Chunks.push_back( { (int)s_BatchVerts.size(), 0, (int)s_BatchIndices.size(), 0 } );
				c = &s_Chunks.back();
			}
			const unsigned short base = (unsigned short)c->numVerts;
			s_BatchVerts.insert( s_BatchVerts.end(), d.mesh->verts.begin() + side.firstVert, d.mesh->verts.begin() + side.firstVert + side.numVerts );
			for ( int k = 0; k < side.numIndices; k++ )
				s_BatchIndices.push_back( base + d.mesh->indices[side.firstIndex + k] );
			c->numVerts += side.numVerts;
			c->numIndices += side.numIndices;
		}
	}
	s_DrawTrigs.clear();
	s_tStage[0] = t1 - t0; s_tStage[1] = t2 - t1;
	s_nPickHover = pickModel;
}

// every model's mesh in its look now (map load), so none is built mid-game the first time it comes into view
static void PrebuildMeshes()
{
	MatchCTs();
	for ( int mi = 1; mi < s_Map.NumModels(); mi++ )
		if ( const TLModel_t *m = s_Map.Model( mi ) )
		{
			const int ct = mi < (int)s_CTOf.size() ? s_CTOf[mi] : -1;
			const TLLook_t &look = ct >= 0 ? g_TLCT[ct].look : g_TLLook[m->type];
			BuildPlacedMesh( mi, *m, look, s_Meshes[mi] );
			s_Meshes[mi].version = ct >= 0 ? g_TLCT[ct].version : g_nTLTypeVersion[m->type];
		}
}

//-----------------------------------------------------------------------------
// The game's own trigger material draws nothing while we draw (and again when triggerlook_enable is 0).
// Set every tick: a material reload resets it.
//-----------------------------------------------------------------------------
static IMaterial *s_pStockTrigger;
static void HideStockTriggers( bool hide )
{
	if ( !s_pStockTrigger )
	{
		if ( !hide )
			return;
		IMaterial *pMat = s_pMatSys->FindMaterial( "tools/toolstrigger", TEXTURE_GROUP_WORLD, false );
		if ( !pMat || pMat->IsErrorMaterial() )
			return;
		pMat->IncrementReferenceCount();
		s_pStockTrigger = pMat;
	}
	if ( s_pStockTrigger->GetMaterialVarFlag( MATERIAL_VAR_NO_DRAW ) != hide )
		s_pStockTrigger->SetMaterialVarFlag( MATERIAL_VAR_NO_DRAW, hide );
}

//-----------------------------------------------------------------------------
// What drawing costs: per 2 s window, the time DrawTriggers took per frame (average, worst) and the frames
//-----------------------------------------------------------------------------
static struct { double sum, max, since; int draws, frames, reused; } s_Cost;
static struct { double avg, max, secs; int draws, frames, reused; } s_LastCost;

static void CostTick( double now )
{
	s_Cost.frames++;
	if ( s_Cost.since <= 0 )
		s_Cost.since = now;
	if ( now - s_Cost.since < 2.0 )
		return;
	s_LastCost.avg = s_Cost.draws ? s_Cost.sum / s_Cost.draws : 0;
	s_LastCost.max = s_Cost.max;
	s_LastCost.secs = now - s_Cost.since;
	s_LastCost.draws = s_Cost.draws;
	s_LastCost.frames = s_Cost.frames;
	s_LastCost.reused = s_Cost.reused;
	s_Cost.sum = s_Cost.max = 0;
	s_Cost.draws = s_Cost.frames = s_Cost.reused = 0;
	s_Cost.since = now;
}

//-----------------------------------------------------------------------------
// The layer: a translucent renderable in the leaf system, so we draw in the world
//-----------------------------------------------------------------------------
class CTLLayer : public CDefaultClientRenderable
{
public:
	CTLLayer()
	{
		m_szLevel[0] = 0;
		memset( &m_Identity, 0, sizeof( m_Identity ) );
		m_Identity[0][0] = m_Identity[1][1] = m_Identity[2][2] = 1;
	}

	// IHandleEntity / IClientUnknown: not an entity
	virtual void SetRefEHandle( const CBaseHandle &handle ) {}
	virtual const CBaseHandle &GetRefEHandle() const { static CBaseHandle h; return h; }
	virtual ICollideable *GetCollideable() { return NULL; }
	virtual IClientNetworkable *GetClientNetworkable() { return NULL; }
	virtual IClientRenderable *GetClientRenderable() { return this; }
	virtual IClientEntity *GetIClientEntity() { return NULL; }
	virtual C_BaseEntity *GetBaseEntity() { return NULL; }
	virtual IClientThinkable *GetClientThinkable() { return NULL; }
	virtual IClientUnknown *GetIClientUnknown() { return this; }

	// IClientRenderable: sorted at the camera (drawn after the other translucent things in its leaf), culled as big
	// as the world
	virtual const Vector &GetRenderOrigin() { return m_vecCamera; }
	virtual const QAngle &GetRenderAngles() { static const QAngle s_Zero( 0, 0, 0 ); return s_Zero; }
	virtual const matrix3x4_t &RenderableToWorldTransform() { return m_Identity; }
	virtual bool ShouldDraw() { return true; }
	virtual bool IsTransparent() { return true; }
	virtual void GetRenderBounds( Vector &mins, Vector &maxs )
	{
		mins.Init( -1, -1, -1 );
		maxs.Init( 1, 1, 1 );
	}
	// the base version calls DefaultRenderBoundsWorldspace(), which lives (unexported) in client.so
	virtual void GetRenderBoundsWorldspace( Vector &mins, Vector &maxs )
	{
		mins.Init( -MAX_COORD_FLOAT, -MAX_COORD_FLOAT, -MAX_COORD_FLOAT );
		maxs.Init( MAX_COORD_FLOAT, MAX_COORD_FLOAT, MAX_COORD_FLOAT );
	}

	virtual int DrawModel( int flags )
	{
		CMatRenderContextPtr pRenderContext( s_pMatSys );
		VMatrix view;
		pRenderContext->GetMatrix( MATERIAL_VIEW, &view );
		m_vecCamera = TL_ViewOrigin( view );
		// only in the player's own view: not the 3D skybox's (its camera sits in the sky room, the triggers would show
		// through the sky), water reflections or monitors - those cameras are elsewhere
		CViewSetup player;
		const bool havePlayer = s_pClient && s_pClient->GetPlayerView( player );
		if ( havePlayer && player.origin.DistToSqr( m_vecCamera ) > 1.0f )
			return 1;
		// where the crosshair points (SELECT TRIGGER)
		QAngle ang = player.angles;
		if ( !havePlayer )
			s_pEngine->GetViewAngles( ang );
		const float pitch = ang.x * ( M_PI_F / 180.0f ), yaw = ang.y * ( M_PI_F / 180.0f );
		Vector fwd( cosf( pitch ) * cosf( yaw ), cosf( pitch ) * sinf( yaw ), -sinf( pitch ) );
		const double t0 = Plat_FloatTime();
		DrawTriggers( pRenderContext, m_vecCamera, fwd );
		const double dt = Plat_FloatTime() - t0;
		s_Cost.sum += dt;
		s_Cost.max = MAX( s_Cost.max, dt );
		s_Cost.draws++;
		return 1;
	}

	// (Re)register with the leaf system on a new session (the client purged our handle) and read the new map
	bool EnsureReady()
	{
		if ( !ConnectInterfaces() )
			return false;
		if ( !s_pEngine->IsInGame() )
		{
			m_bLeftGame = true;
			return false;
		}
		const char *level = s_pEngine->GetLevelName();
		if ( RenderHandle() != INVALID_CLIENT_RENDER_HANDLE && !m_bLeftGame && !strcmp( level, m_szLevel ) )
			return true;
		if ( !s_pDrawMaterial )
		{
			// a shipped VMT (never CreateMaterial with our KeyValues: the game's layout is newer)
			IMaterial *pMat = s_pMatSys->FindMaterial( "triggerlook/trigger", TEXTURE_GROUP_OTHER, false );
			if ( pMat && !pMat->IsErrorMaterial() )
			{
				pMat->IncrementReferenceCount();
				s_pDrawMaterial = pMat;
			}
			else
				Warning( "triggerlook: materials/triggerlook/trigger.vmt is missing (run install.sh)\n" );
		}
		if ( strcmp( level, s_Map.Level() ) )
		{
			if ( !s_Map.Load( s_pEngine->GetGameDirectory(), level, OpenGameMap ) )
				s_Map.Clear();
			s_Meshes.assign( s_Map.NumModels(), TLMesh_t() );
			PlaceModels();
			PrebuildMeshes();
			s_nPVSCluster = -2;		// (the visible list belongs to the old map)
		}
		RenderHandle() = INVALID_CLIENT_RENDER_HANDLE;	// stale after a level change
		s_pLeafSystem->AddRenderable( this, RENDER_GROUP_TRANSLUCENT_ENTITY );
		TL_StrCopy( m_szLevel, level, sizeof( m_szLevel ) );
		m_bLeftGame = false;
		return true;
	}

	void LeftGame() { m_bLeftGame = true; }
	bool InGame() const { return !m_bLeftGame; }
	const char *Level() const { return m_szLevel; }

private:
	matrix3x4_t m_Identity;
	Vector m_vecCamera;
	char m_szLevel[MAX_PATH];
	bool m_bLeftGame = true;
};

static CTLLayer s_Layer;

bool TL_Active()
{
	return s_bEnable && s_Layer.InGame() && s_Map.Level()[0];
}

// (A showbrushes plugin's own look, when a player turns it on there, can't be hidden from the client: NO_DRAW on its
// material is ignored by models, its alpha modulation is set per draw, the leaf system's never-drawn group didn't stop
// the props, IMaterialVar calls broke the material (the game's layout differs), and a stand-in VMT in the game folder
// isn't used. Nobody needs it now: the menu draws every map's triggers by itself.)

//-----------------------------------------------------------------------------
// Every frame (the ticker panel)
//-----------------------------------------------------------------------------
void TL_Tick()
{
	if ( !ConnectInterfaces() )
		return;
	if ( !s_pEngine->IsInGame() )
		s_Layer.LeftGame();
	else
		s_Layer.EnsureReady();
	// the game's look stays whenever ours doesn't draw (OFF, no map read, material missing)
	HideStockTriggers( TL_Active() && g_bTLShow && s_pDrawMaterial );
	s_nFrame++;
	s_nReusedLast = s_nReused;
	s_Cost.reused += s_nReused;
	s_nReused = 0;
	CostTick( Plat_FloatTime() );
	TL_LookTick( Plat_FloatTime() );
	TLUI_Tick();
	TL_RunScript();
}

//-----------------------------------------------------------------------------
// Start: the first setting the game changes after VGUI is up (see the top)
//-----------------------------------------------------------------------------
static bool s_bStarted;

static void TryStart()
{
	if ( s_bStarted || !ConnectInterfaces() )
		return;
	if ( !TLUI_Init( s_pEngine ) )
		return;		// VGUI isn't up yet
	s_bStarted = true;
	Msg( "triggerlook " TRIGGERLOOK_VERSION ": ready (triggerlook_menu opens the trigger menu; settings in %s)\n", TL_LookPath() );
	// tests: a triggerlook_selftest script from the environment (a +command on the game's command line can run
	// before a loader on Windows has loaded us)
	if ( const char *script = getenv( "TRIGGERLOOK_SELFTEST" ) )
	{
		char cmd[1100];
		snprintf( cmd, sizeof( cmd ), "triggerlook_selftest \"%s\"\n", script );
		s_pEngine->ClientCmd_Unrestricted( cmd );
	}
}

void TL_Start() { TryStart(); }

static void OnAnyConVarChange( IConVar *var, const char *pOldValue, float flOldValue )
{
	if ( !s_bStarted && ThreadInMainThread() )
		TryStart();
}

// our game files (the trigger material, the menu font) in the game folder, where the game finds them at once
// (custom/ folders are only read at startup); rewritten when they differ
#include "tl_files.h"

static void InstallGameFiles( const char *gameDir )
{
	for ( const TLGameFile_t &f : s_GameFiles )
	{
		char path[1024], dir[1024], tmp[1100];
		snprintf( path, sizeof( path ), "%s/%s", gameDir, f.path );
		if ( FILE *in = fopen( path, "rb" ) )
		{
			std::vector<unsigned char> have( f.size + 1 );
			size_t n = fread( have.data(), 1, have.size(), in );
			fclose( in );
			if ( n == f.size && !memcmp( have.data(), f.data, f.size ) )
				continue;
		}
		TL_StrCopy( dir, path, sizeof( dir ) );
		if ( char *slash = strrchr( dir, '/' ) )
			*slash = 0;
		TLP_MakeDirs( dir );
		snprintf( tmp, sizeof( tmp ), "%s.tmp", path );
		FILE *out = fopen( tmp, "wb" );
		bool ok = out && fwrite( f.data, 1, f.size, out ) == f.size;
		if ( out )
			ok = fclose( out ) == 0 && ok;
		if ( !ok || !TLP_ReplaceFile( tmp, path ) )
			Warning( "triggerlook: can't write %s\n", path );
		else
			Msg( "triggerlook: wrote %s\n", path );
	}
}

// on the main thread: our game files, our commands, the callback, and the panels if VGUI is up already
static void MainThreadInit()
{
	if ( ConnectInterfaces() || s_pEngine )
		InstallGameFiles( s_pEngine->GetGameDirectory() );
	ICvar *pCvar = (ICvar *)TLP_Interface( "vstdlib", CVAR_INTERFACE_VERSION );
	if ( pCvar && pCvar->FindCommandBase( "triggerlook_menu" ) )
	{
		// two loaders (its own preload and bhoptools, or two folders on Windows): the first copy does it all
		Warning( "triggerlook: already loaded, this copy stays off\n" );
		return;
	}
	if ( !TL_RegisterCommands( pCvar ) )
	{
		Warning( "triggerlook: no VEngineCvar004, not started\n" );
		return;
	}
	pCvar->InstallGlobalChangeCallback( OnAnyConVarChange );
	TryStart();
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
CON_COMMAND( triggerlook_menu, "triggerlook: open / close the trigger menu (bind it to a key: bind f6 triggerlook_menu)" )
{
	TryStart();
	if ( !TLUI_Ready() )
	{
		Warning( "triggerlook: the menu isn't available yet\n" );
		return;
	}
	TLUI_SetOpen( !TLUI_IsOpen() );
}

// what the server sends: every entity with a brush model (diagnosis: a show-triggers plugin that sends them its own way)
CON_COMMAND( triggerlook_ents, "triggerlook: list the entities with brush models the client has (to the info file)" )
{
	if ( !ConnectInterfaces() )
		return;
	std::string out = "triggerlook_ents:\n";
	char line[512];
	int hi = MIN( s_pEntList->GetHighestEntityIndex(), 16383 ), n = 0, n2 = 0;
	std::string other;
	for ( int i = 1; i <= hi; i++ )
	{
		IClientEntity *e = s_pEntList->GetClientEntity( i );
		IClientRenderable *r = e ? e->GetClientRenderable() : NULL;
		const model_t *mdl = r ? r->GetModel() : NULL;
		const char *name = mdl ? s_pModelInfo->GetModelName( mdl ) : NULL;
		IClientNetworkable *net = e ? e->GetClientNetworkable() : NULL;
		ClientClass *cc = net ? net->GetClientClass() : NULL;
		if ( !name || name[0] != '*' )
		{
			// everything else too, in short (what a plugin might draw triggers with)
			if ( e && n2++ < 300 )
			{
				const Vector o = r ? r->GetRenderOrigin() : Vector( 0, 0, 0 );
				snprintf( line, sizeof( line ), "  #%d %s %s dormant %d drawn %d at %.0f %.0f %.0f\n", i, cc && cc->m_pNetworkName ? cc->m_pNetworkName : "?",
							name ? name : mdl ? "(unnamed)" : "-", net && net->IsDormant() ? 1 : 0, r && r->ShouldDraw() ? 1 : 0, o.x, o.y, o.z );
				other += line;
			}
			continue;
		}
		const TLModel_t *m = s_Map.Model( atoi( name + 1 ) );
		snprintf( line, sizeof( line ), "  #%d %s %s: map %s, dormant %d, drawn %d\n", i, cc && cc->m_pNetworkName ? cc->m_pNetworkName : "?", name,
					m ? m->classname.c_str() : "not a trigger", net && net->IsDormant() ? 1 : 0, r->ShouldDraw() ? 1 : 0 );
		if ( n++ < 200 )
			out += line;
	}
	snprintf( line, sizeof( line ), "  %d brush entities; others:\n", n );
	out += line + other;
	// the precached models a show-brushes plugin uses
	for ( int mi = 0; mi < 8192; mi++ )
	{
		const model_t *pm = s_pModelInfo->GetModel( mi );
		const char *mn = pm ? s_pModelInfo->GetModelName( pm ) : NULL;
		if ( mn && TL_StrIStr( mn, "showbrushes" ) )
		{
			snprintf( line, sizeof( line ), "  model %d: %s\n", mi, mn );
			out += line;
		}
	}
	Msg( "%s", out.c_str() );
	char path[1024];
	snprintf( path, sizeof( path ), "%s/info.txt", TLP_CacheDir() );
	if ( FILE *f = fopen( path, "a" ) )
	{
		fputs( out.c_str(), f );
		fclose( f );
	}
}

// triggerlook_set <type|all> [show 0/1] [fill r g b a] [border r g b a] [width n]  (scripts, tests)
CON_COMMAND( triggerlook_set, "triggerlook_set <teleport|booster|push|gravity|other|all> [show 0/1] [fill r g b a] [border r g b a] [width n]" )
{
	int mask = args.ArgC() > 1 ? TL_TypeMask( args[1] ) : 0;
	if ( !mask )
	{
		Msg( "usage: triggerlook_set <teleport|booster|push|gravity|other|all> [show 0/1] [fill r g b a] [border r g b a] [width 0-%d]\n", TL_MAX_WIDTH );
		return;
	}
	auto byte = []( const char *s ) { return (unsigned char)clamp( atoi( s ), 0, 255 ); };
	for ( int t = 0; t < TL_TYPES; t++ )
	{
		if ( !( mask & ( 1 << t ) ) )
			continue;
		TLLook_t &l = g_TLLook[t];
		for ( int i = 2; i < args.ArgC(); )
		{
			if ( !TL_StrICmp( args[i], "show" ) && i + 1 < args.ArgC() ) { l.show = atoi( args[i + 1] ) != 0; i += 2; }
			else if ( !TL_StrICmp( args[i], "fill" ) && i + 4 < args.ArgC() ) { for ( int k = 0; k < 4; k++ ) l.fill[k] = byte( args[i + 1 + k] ); i += 5; }
			else if ( !TL_StrICmp( args[i], "border" ) && i + 4 < args.ArgC() ) { for ( int k = 0; k < 4; k++ ) l.border[k] = byte( args[i + 1 + k] ); i += 5; }
			else if ( !TL_StrICmp( args[i], "width" ) && i + 1 < args.ArgC() ) { l.width = clamp( atoi( args[i + 1] ), 0, (int)TL_MAX_WIDTH ); i += 2; }
			else i++;
		}
	}
	TL_LookChanged( mask );
}

// also appended to TLP_CacheDir()/info.txt (tests: the game's console.log isn't always there)
CON_COMMAND( triggerlook_info, "triggerlook: show what it's doing" )
{
	bool ok = ConnectInterfaces();
	std::string out;
	char line[512];
	snprintf( line, sizeof( line ), "triggerlook " TRIGGERLOOK_VERSION ": interfaces %s, menu %s, enable %d, triggers %s, map \"%s\" (%d sides), stock trigger material %s\n",
				ok ? "OK" : "MISSING", TLUI_Ready() ? "ready" : "not ready", s_bEnable ? 1 : 0, g_bTLShow ? "ON" : "OFF", s_Map.Level(), s_Map.m_nPolys,
				!s_pStockTrigger ? "untouched" : s_pStockTrigger->GetMaterialVarFlag( MATERIAL_VAR_NO_DRAW ) ? "hidden" : "drawn" );
	out += line;
	for ( int t = 0; t < TL_TYPES; t++ )
	{
		const TLLook_t &l = g_TLLook[t];
		snprintf( line, sizeof( line ), "  %-8s %d in the map, %d in view, %s, fill %d %d %d %d, border %d %d %d %d width %d\n",
					g_TLTypeName[t], s_Map.m_nTriggers[t], s_nShown[t], l.show ? "drawn" : "hidden", l.fill[0], l.fill[1], l.fill[2], l.fill[3],
					l.border[0], l.border[1], l.border[2], l.border[3], l.width );
		out += line;
	}
	const std::vector<int> &cts = TL_MapCTs();
	for ( size_t k = 0; k < cts.size(); k++ )
	{
		const TLCustomTrigger_t &ct = g_TLCT[cts[k]];
		const TLLook_t &l = ct.look;
		snprintf( line, sizeof( line ), "  CT-%-5d *%d (%s) \"%s\", fill %d %d %d %d, border %d %d %d %d width %d\n", (int)k + 1, s_MapCTModels[k],
					g_TLTypeName[TL_ModelType( s_MapCTModels[k] )], ct.name.c_str(),
					l.fill[0], l.fill[1], l.fill[2], l.fill[3], l.border[0], l.border[1], l.border[2], l.border[3], l.width );
		out += line;
	}
	const char *mouse1 = s_pEngine ? s_pEngine->Key_BindingForKey( MOUSE_LEFT ) : NULL;
	snprintf( line, sizeof( line ), "  custom triggers: %d on this map, %d in all; picking %d, under the crosshair *%d (world at %.0f); menu %s; MOUSE1 = \"%s\"\n",
				(int)cts.size(), (int)g_TLCT.size(), s_bPicking ? 1 : 0, TL_PickHover(), s_flPickWorld, TLUI_IsOpen() ? "open" : "closed", mouse1 ? mouse1 : "" );
	out += line;
	snprintf( line, sizeof( line ), "  last frame: %d triggers drawn, %d sides, %d pairs ordered, %d left out by the map's visibility (camera cluster %d), %d view(s) reusing the first\n",
				s_nDrawn, s_nDrawnSides, s_nOrderPairs, s_nPVSCulled, s_nCamCluster, s_nReusedLast );
	out += line;
	snprintf( line, sizeof( line ), "  last draw stages: collect %.1f, order %.1f, copy %.1f, flush %.1f us; camera %.0f %.0f %.0f\n",
				s_tStage[0] * 1e6, s_tStage[1] * 1e6, s_tStage[2] * 1e6, s_tStage[3] * 1e6, s_vecLastCam.x, s_vecLastCam.y, s_vecLastCam.z );
	out += line;
	snprintf( line, sizeof( line ), "  draw cost: %.1f us average, %.1f us worst per draw (last %.1f s: %d draws, %d of them reusing, %d frames = %.0f fps)\n",
				s_LastCost.avg * 1e6, s_LastCost.max * 1e6, s_LastCost.secs, s_LastCost.draws, s_LastCost.reused, s_LastCost.frames,
				s_LastCost.secs > 0 ? s_LastCost.frames / s_LastCost.secs : 0.0 );
	out += line;
	Msg( "%s", out.c_str() );
	char path[1024];
	snprintf( path, sizeof( path ), "%s/info.txt", TLP_CacheDir() );
	if ( FILE *f = fopen( path, "a" ) )
	{
		fprintf( f, "[%.1f] %s", Plat_FloatTime(), out.c_str() );
		fclose( f );
	}
}

//-----------------------------------------------------------------------------
// Loading: Linux = the loader (libtriggerlook.so) or bhoptools.so right after the engine loaded client.so, main
// thread; Windows = DllMain's thread as soon as anything (bhoptools.exe's code) loads triggerlook.dll. Once.
//-----------------------------------------------------------------------------
TL_EXPORT int TriggerLook_Init()
{
	static std::atomic<bool> s_bInit( false );
	if ( s_bInit.exchange( true ) )
		return 1;
	if ( !CommandLine()->FindParm( "-insecure" ) )
	{
		// loading code into a VAC-secured game is how accounts get banned
		Warning( "triggerlook: the game wasn't started with -insecure, not loading\n" );
		return 0;
	}
	TL_LookLoad();
	TLP_RunOnMainThread( MainThreadInit );
	return 1;
}

// for a host mod (bhoptools): on / off without the console
TL_EXPORT void triggerlook_enable_set( bool on )
{
	s_bEnable = on;
}
