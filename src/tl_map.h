//=============================================================================
// triggerlook: the triggers of the current map, read from its .bsp (version 20, CS:S).
//
// Every brush entity whose class starts with "trigger_" gets its shape rebuilt from its brushes (each brush =
// the convex solid inside its side planes), so each side is one polygon with its real edges - the drawn faces
// of the map are cut into pieces by the compiler, which would draw borders across the middle of a side.
// Each trigger also gets a type (TLType_t) from its class and outputs.
//=============================================================================
#ifndef TL_MAP_H
#define TL_MAP_H

#include "mathlib/vector.h"

#include <string>
#include <vector>

enum TLType_t
{
	TL_TELEPORT,	// trigger_teleport, trigger_teleport_relative
	TL_BOOSTER,		// trigger_multiple / trigger_once with a basevelocity output
	TL_PUSH,		// trigger_push
	TL_GRAVITY,		// trigger_gravity, or a gravity output
	TL_OTHER,		// every other trigger
	TL_TYPES
};
extern const char *const g_TLTypeName[TL_TYPES];	// "teleport", ... (settings file, console)
extern const char *const g_TLTypeLabel[TL_TYPES];	// "TELEPORT", ... (menu)

// the type of a brush entity (-1 = not a trigger); outputs = all its output values, any separator
int TL_Classify( const char *classname, const char *outputs );

// one side of a trigger: a convex polygon, clockwise seen from the front (outside), like the map's faces
struct TLPoly_t
{
	int		first, count;	// into TLModel_t::verts
	Vector	normal;			// outward
};

struct TLModel_t
{
	int		type = -1;		// TLType_t, -1 = not a trigger
	std::string classname;
	Vector	mins, maxs;		// model space (the entity's own; most triggers are never moved, so = world)
	std::vector<Vector> verts;
	std::vector<TLPoly_t> polys;
};

// a map file to read lumps from
struct TLMapFile
{
	virtual ~TLMapFile() {}
	virtual long long Size() = 0;
	virtual bool Read( long long ofs, void *dst, int len ) = 0;		// exactly len bytes at ofs
};
// opens a map the way the engine finds it (its filesystem: custom/, the game folder, VPKs, download/); NULL = no
typedef TLMapFile *( *TLOpenMapFn )( const char *level );

class CTLMap
{
public:
	CTLMap() { m_szLevel[0] = 0; }

	// pszLevel = engine->GetLevelName() ("maps/foo.bsp"), opened with pfnOpen (in the game: its filesystem); without
	// one (offline tools) the loose file is looked up in pszGameDir the engine's way. False on error.
	bool Load( const char *pszGameDir, const char *pszLevel, TLOpenMapFn pfnOpen = NULL );
	void Clear();

	// brush model *index if it's a trigger with a shape, else NULL
	const TLModel_t *Model( int index ) const
	{
		return index > 0 && index < (int)m_Models.size() && m_Models[index].type >= 0 && !m_Models[index].polys.empty() ? &m_Models[index] : NULL;
	}
	int NumModels() const { return (int)m_Models.size(); }
	const char *Level() const { return m_szLevel; }

	int		m_nTriggers[TL_TYPES] = {};	// trigger models per type
	int		m_nPolys = 0;

private:
	std::vector<TLModel_t> m_Models;	// by brush model number (*N); 0 = the world, never a trigger
	char	m_szLevel[260];
};

#endif // TL_MAP_H
