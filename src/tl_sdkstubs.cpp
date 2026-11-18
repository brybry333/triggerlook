//=============================================================================
// triggerlook: the few things the SDK's headers reference that live in its libraries (mathlib, tier2), which
// this mod doesn't link (the SDK ships no Windows tier1 / mathlib): the mesh builder's index generators, as tier2 has them.
//=============================================================================
#include "mathlib/vector.h"
#include "tier2/meshutils.h"

// CMeshBuilder::End generates indices for primitive types that come without them (ours always have them)
void GenerateSequentialIndexBuffer( unsigned short *pIndices, int nIndexCount, int nFirstVertex )
{
	for ( int i = 0; i < nIndexCount; i++ )
		pIndices[i] = (unsigned short)( nFirstVertex + i );
}

void GenerateQuadIndexBuffer( unsigned short *pIndices, int nIndexCount, int nFirstVertex )
{
	for ( int q = 0, v = nFirstVertex; q < nIndexCount / 6; q++, v += 4 )
	{
		unsigned short *p = pIndices + q * 6;
		p[0] = v; p[1] = v + 1; p[2] = v + 2; p[3] = v; p[4] = v + 2; p[5] = v + 3;
	}
}

void GeneratePolygonIndexBuffer( unsigned short *pIndices, int nIndexCount, int nFirstVertex )
{
	for ( int i = 0; i < nIndexCount / 3; i++ )
	{
		pIndices[i * 3] = nFirstVertex;
		pIndices[i * 3 + 1] = nFirstVertex + i + 1;
		pIndices[i * 3 + 2] = nFirstVertex + i + 2;
	}
}

void GenerateLineStripIndexBuffer( unsigned short *pIndices, int nIndexCount, int nFirstVertex )
{
	for ( int i = 0; i < nIndexCount / 2; i++ )
	{
		pIndices[i * 2] = nFirstVertex + i;
		pIndices[i * 2 + 1] = nFirstVertex + i + 1;
	}
}

void GenerateLineLoopIndexBuffer( unsigned short *pIndices, int nIndexCount, int nFirstVertex )
{
	int n = nIndexCount / 2;
	if ( n <= 0 )
		return;
	pIndices[0] = nFirstVertex + n - 1;
	pIndices[1] = nFirstVertex;
	for ( int i = 1; i < n; i++ )
	{
		pIndices[i * 2] = nFirstVertex + i - 1;
		pIndices[i * 2 + 1] = nFirstVertex + i;
	}
}
