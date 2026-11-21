//=============================================================================
// triggerlook: the triangles of one trigger in its look - per side a fill and a border band along its edges
// (inside the side, so the band follows the real edges and nothing is drawn twice)
//=============================================================================
#ifndef TL_MESH_H
#define TL_MESH_H

#include "tl_map.h"
#include "tl_look.h"

struct TLVert_t
{
	Vector			pos;
	unsigned char	rgba[4];
};

// one side: its vertices and triangles (indices count from the side's own first vertex)
struct TLSideMesh_t
{
	int firstVert, numVerts, firstIndex, numIndices;
};

struct TLMesh_t
{
	int version = -1;					// g_nTLLookVersion it was built for
	std::vector<TLVert_t> verts;
	std::vector<unsigned short> indices;
	std::vector<TLSideMesh_t> sides;	// one per TLModel_t::polys entry, same order (empty sides: numIndices 0)
};

void TL_BuildMesh( const TLModel_t &m, const TLLook_t &look, TLMesh_t &out );

// The model grown by eps on every side (each corner moved so every side through it moves out by eps): what's drawn,
// so a side lying on a floor shows in front of it without a depth bias, and nothing opens up along the edges
enum { TL_DRAW_GROW_X100 = 50 };	// 0.5 units
void TL_ExpandModel( const TLModel_t &in, float eps, TLModel_t &out );

#endif // TL_MESH_H
