//=============================================================================
// triggerlook runtime: draws the triggers the server shows (!st) in the player's own look, instead of the game's
// tools/toolstrigger texture.
//
// The server decides what's shown, like before: a show-triggers plugin sends the trigger entities to this player
// (they're never sent otherwise). Each frame the layer walks the client's entities, picks the ones whose model is
// a trigger of this map (tl_map.cpp), and draws them itself - per type a fill and a border band (tl_mesh.cpp) -
// while the game's own trigger material is set to draw nothing.
//
// Everything goes through interfaces the game exports (CreateInterface), no hooks; the same code on Linux and
// Windows (only tl_platform_*.cpp differs; the SDK's headers give each OS its own interface layout):
//   engine             VEngineClient014        level name, console commands
//                      VModelInfoClient006     model names ("*12")
//   client             ClientLeafSystem002     AddRenderable (slot 4), checked against CS:S client.so
//                      VClientEntityList003    the client's entities
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
#include "basehandle.h"
#include "worldsize.h"
#include "engine/ivmodelinfo.h"
#include "engine/IClientLeafSystem.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imesh.h"
#include "mathlib/vmatrix.h"
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

#define TRIGGERLOOK_VERSION "1.0"

static bool s_bEnable = true;	// triggerlook_enable
static bool s_bCull = true;		// triggerlook_cull

CON_COMMAND( triggerlook_enable, "triggerlook_enable [0/1]: 1 = draw the triggers the server shows (!st) in your own look, 0 = the game's trigger texture" )
{
	if ( args.ArgC() > 1 )
		s_bEnable = atoi( args[1] ) != 0;
	Msg( "triggerlook_enable %d\n", s_bEnable ? 1 : 0 );
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

// per entity slot: the model it had last time and that model's number (*N), -1 = not a brush model
struct Seen_t { const model_t *model; int index; };
static std::vector<Seen_t> s_Seen;
static int s_nShown[TL_TYPES];				// trigger entities the server sent, by type (last frame)
static int s_nDrawn, s_nDrawnSides;

int TL_ShownNow( int type ) { return type >= 0 && type < TL_TYPES ? s_nShown[type] : 0; }

//-----------------------------------------------------------------------------
// Drawing: all visible sides into dynamic meshes, as few as fit
//-----------------------------------------------------------------------------
static IMaterial *s_pDrawMaterial;		// materials/triggerlook/trigger.vmt: unlit, vertex colour + alpha, decal depth bias
static std::vector<TLVert_t> s_BatchVerts;
static std::vector<unsigned short> s_BatchIndices;
static int s_nMaxVerts, s_nMaxIndices;

static void Flush( IMatRenderContext *ctx )
{
	if ( s_BatchIndices.empty() )
	{
		s_BatchVerts.clear();
		return;
	}
	IMesh *pMesh = ctx->GetDynamicMesh( true );
	CMeshBuilder mb;
	mb.Begin( pMesh, MATERIAL_TRIANGLES, (int)s_BatchVerts.size(), (int)s_BatchIndices.size() );
	for ( const TLVert_t &v : s_BatchVerts )
	{
		mb.Position3fv( v.pos.Base() );
		mb.Color4ubv( v.rgba );
		mb.TexCoord2f( 0, 0, 0 );
		mb.AdvanceVertex();
	}
	for ( unsigned short i : s_BatchIndices )
	{
		mb.Index( i );
		mb.AdvanceIndex();
	}
	mb.End();
	pMesh->Draw();
	s_BatchVerts.clear();
	s_BatchIndices.clear();
}

static bool IsIdentity( const matrix3x4_t &m )
{
	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 4; c++ )
			if ( fabsf( m[r][c] - ( r == c ? 1.0f : 0.0f ) ) > 1e-4f )
				return false;
	return true;
}

// the view frustum's four side planes (a x + b y + c z + d >= 0 inside), from projection x view
static float s_Planes[4][4];
static void SetupFrustum( IMatRenderContext *ctx )
{
	VMatrix view, proj, vp;
	ctx->GetMatrix( MATERIAL_VIEW, &view );
	ctx->GetMatrix( MATERIAL_PROJECTION, &proj );
	TL_MatMul( proj, view, vp );
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

static void DrawTriggers( IMatRenderContext *ctx, const Vector &camera )
{
	memset( s_nShown, 0, sizeof( s_nShown ) );
	s_nDrawn = s_nDrawnSides = 0;
	if ( !s_bEnable || !s_pDrawMaterial || !s_Map.Level()[0] )
		return;
	const bool cull = s_bCull;
	if ( cull )
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

	int hi = MIN( s_pEntList->GetHighestEntityIndex(), 16383 );
	if ( (int)s_Seen.size() < hi + 1 )
		s_Seen.resize( hi + 1, Seen_t{ NULL, -1 } );
	for ( int i = 1; i <= hi; i++ )
	{
		IClientEntity *e = s_pEntList->GetClientEntity( i );
		if ( !e )
			continue;
		IClientRenderable *r = e->GetClientRenderable();
		const model_t *mdl = r ? r->GetModel() : NULL;
		if ( !mdl )
			continue;
		Seen_t &seen = s_Seen[i];
		if ( seen.model != mdl )
		{
			const char *name = s_pModelInfo->GetModelName( mdl );
			seen.model = mdl;
			seen.index = name && name[0] == '*' ? atoi( name + 1 ) : -1;
		}
		const TLModel_t *m = s_Map.Model( seen.index );
		if ( !m )
			continue;
		IClientNetworkable *net = e->GetClientNetworkable();
		if ( ( net && net->IsDormant() ) || !r->ShouldDraw() )
			continue;
		s_nShown[m->type]++;
		const TLLook_t &look = g_TLLook[m->type];
		if ( !look.show )
			continue;
		const matrix3x4_t &xf = r->RenderableToWorldTransform();
		const bool moved = !IsIdentity( xf );
		Vector wmins = m->mins, wmaxs = m->maxs, eye = camera;
		if ( moved )
		{
			TL_TransformAABB( xf, m->mins, m->maxs, wmins, wmaxs );
			TL_ITransform( camera, xf, eye );		// the camera in the trigger's own space
		}
		if ( cull && OutsideView( wmins - Vector( 1, 1, 1 ), wmaxs + Vector( 1, 1, 1 ) ) )
			continue;
		TLMesh_t &mesh = s_Meshes[seen.index];
		if ( mesh.version != g_nTLTypeVersion[m->type] )
		{
			TL_BuildMesh( *m, look, mesh );
			mesh.version = g_nTLTypeVersion[m->type];
		}
		s_nDrawn++;
		for ( size_t si = 0; si < mesh.sides.size(); si++ )
		{
			const TLSideMesh_t &side = mesh.sides[si];
			const TLPoly_t &poly = m->polys[si];
			// only the sides facing the camera (the GPU would drop the others anyway)
			if ( !side.numIndices || DotProduct( poly.normal, eye - m->verts[poly.first] ) <= 0 )
				continue;
			if ( side.numVerts > s_nMaxVerts || side.numIndices > s_nMaxIndices )
				continue;		// (can't happen with the reader's 128-side brushes)
			s_nDrawnSides++;
			if ( (int)s_BatchVerts.size() + side.numVerts > s_nMaxVerts || (int)s_BatchIndices.size() + side.numIndices > s_nMaxIndices )
				Flush( ctx );
			unsigned short base = (unsigned short)s_BatchVerts.size();
			for ( int k = 0; k < side.numVerts; k++ )
			{
				TLVert_t v = mesh.verts[side.firstVert + k];
				if ( moved )
				{
					Vector local = v.pos;
					TL_Transform( local, xf, v.pos );
				}
				s_BatchVerts.push_back( v );
			}
			for ( int k = 0; k < side.numIndices; k++ )
				s_BatchIndices.push_back( base + mesh.indices[side.firstIndex + k] );
		}
	}
	Flush( ctx );
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PopMatrix();
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
		if ( s_pClient && s_pClient->GetPlayerView( player ) && player.origin.DistToSqr( m_vecCamera ) > 1.0f )
			return 1;
		DrawTriggers( pRenderContext, m_vecCamera );
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
		}
		s_Seen.clear();		// entity slots and model pointers belong to the old session
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
	// the game's look stays whenever ours can't draw (off, no map read, material missing)
	HideStockTriggers( TL_Active() && s_pDrawMaterial );
	TL_LookTick( Plat_FloatTime() );
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
	snprintf( line, sizeof( line ), "triggerlook " TRIGGERLOOK_VERSION ": interfaces %s, menu %s, enable %d, map \"%s\" (%d sides), stock trigger material %s\n",
				ok ? "OK" : "MISSING", TLUI_Ready() ? "ready" : "not ready", s_bEnable ? 1 : 0, s_Map.Level(), s_Map.m_nPolys,
				!s_pStockTrigger ? "untouched" : s_pStockTrigger->GetMaterialVarFlag( MATERIAL_VAR_NO_DRAW ) ? "hidden" : "drawn" );
	out += line;
	for ( int t = 0; t < TL_TYPES; t++ )
	{
		const TLLook_t &l = g_TLLook[t];
		snprintf( line, sizeof( line ), "  %-8s %d in the map, %d shown by the server, %s, fill %d %d %d %d, border %d %d %d %d width %d\n",
					g_TLTypeName[t], s_Map.m_nTriggers[t], s_nShown[t], l.show ? "drawn" : "hidden", l.fill[0], l.fill[1], l.fill[2], l.fill[3],
					l.border[0], l.border[1], l.border[2], l.border[3], l.width );
		out += line;
	}
	snprintf( line, sizeof( line ), "  last frame: %d triggers drawn, %d sides\n", s_nDrawn, s_nDrawnSides );
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
