//=============================================================================
// triggerlook: one trigger's triangles in its look; see tl_mesh.h
//=============================================================================
#include <float.h>
#include <math.h>

#include "tl_mesh.h"
#include "tl_util.h"

static void Push( TLMesh_t &out, const Vector &p, const unsigned char *rgba )
{
	TLVert_t v;
	v.pos = p;
	memcpy( v.rgba, rgba, 4 );
	out.verts.push_back( v );
}

// keeps the part of a convex polygon where dot( x, n ) >= d. tags[k] names the outer edge that edge k (corner k to
// k + 1) lies on; the new edge along the cut gets 'tag'.
static void ChopInside( std::vector<Vector> &poly, std::vector<int> &tags, const Vector &n, float d, int tag )
{
	std::vector<Vector> out;
	std::vector<int> outTags;
	const int c = (int)poly.size();
	for ( int i = 0; i < c; i++ )
	{
		const Vector &a = poly[i], &b = poly[( i + 1 ) % c];
		float da = DotProduct( a, n ) - d, db = DotProduct( b, n ) - d;
		Vector cut = a + ( b - a ) * ( da / ( da - db ) );
		if ( da >= 0 && db < 0 )
		{
			if ( da > 0 )
			{
				out.push_back( a ); outTags.push_back( tags[i] );
				out.push_back( cut ); outTags.push_back( tag );
			}
			else
			{
				out.push_back( a ); outTags.push_back( tag );
			}
		}
		else if ( da >= 0 )
		{
			out.push_back( a ); outTags.push_back( tags[i] );
		}
		else if ( db > 0 )
		{
			out.push_back( cut ); outTags.push_back( tags[i] );
		}
	}
	// corners on the cut line can come out twice (rounding): keep the second, whose edge goes on
	poly.clear();
	tags.clear();
	for ( size_t i = 0; i < out.size(); i++ )
		if ( out[i].DistToSqr( out[( i + 1 ) % out.size()] ) > 0.01f * 0.01f )
		{
			poly.push_back( out[i] );
			tags.push_back( outTags[i] );
		}
}

void TL_BuildMesh( const TLModel_t &m, const TLLook_t &look, TLMesh_t &out )
{
	out.verts.clear();
	out.indices.clear();
	out.sides.clear();
	const bool fill = look.fill[3] > 0;
	const bool border = look.width > 0 && look.border[3] > 0;
	std::vector<Vector> inner, inward;
	std::vector<int> tags, edgeOf;
	for ( const TLPoly_t &poly : m.polys )
	{
		TLSideMesh_t side = { (int)out.verts.size(), 0, (int)out.indices.size(), 0 };
		const Vector *p = &m.verts[poly.first];
		const int n = poly.count;
		// the band's inner edge: every edge moved inwards by the width (less on small sides, so it never folds over)
		inner.assign( p, p + n );
		if ( border )
		{
			Vector centre( 0, 0, 0 );
			for ( int i = 0; i < n; i++ )
				centre += p[i];
			centre /= (float)n;
			inward.resize( n );
			float room = FLT_MAX;	// distance from the centre to the nearest edge
			for ( int i = 0; i < n; i++ )
			{
				Vector e = p[( i + 1 ) % n] - p[i];
				Vector in = CrossProduct( poly.normal, e );
				TL_Normalize( in );
				if ( DotProduct( in, centre - p[i] ) < 0 )
					in = -in;
				inward[i] = in;
				room = MIN( room, DotProduct( in, centre - p[i] ) );
			}
			// the inner outline: the side cut along every edge moved inwards by the width; short edges can vanish
			float w = MIN( (float)look.width, room * 0.45f );
			tags.assign( n, -1 );
			for ( int i = 0; i < n && inner.size() >= 3; i++ )
				ChopInside( inner, tags, inward[i], DotProduct( inward[i], p[i] ) + w, i );
			const int m2 = (int)inner.size();
			edgeOf.assign( n, -1 );		// outer edge -> the inner edge on its moved line
			bool ok = m2 >= 3;
			for ( int k = 0; k < m2 && ok; k++ )
			{
				ok = tags[k] >= 0 && tags[k] < n && edgeOf[tags[k]] < 0;
				if ( ok )
					edgeOf[tags[k]] = k;
			}
			if ( !ok )
			{
				inner.assign( p, p + n );	// (shouldn't happen: the width stays under the room) no band
				goto fillSide;
			}
			// the band: per outer edge a trapezoid to its inner edge, or (edge vanished) a triangle to the inner corner
			// where its neighbours' inner edges meet
			int base = (int)out.verts.size() - side.firstVert;
			for ( int i = 0; i < n; i++ )
				Push( out, p[i], look.border );
			for ( int k = 0; k < m2; k++ )
				Push( out, inner[k], look.border );
			for ( int i = 0; i < n; i++ )
			{
				unsigned short P0 = base + i, P1 = base + ( i + 1 ) % n;
				if ( edgeOf[i] >= 0 )
				{
					unsigned short Q0 = base + n + edgeOf[i], Q1 = base + n + ( edgeOf[i] + 1 ) % m2;
					unsigned short t[6] = { P0, P1, Q1, P0, Q1, Q0 };
					out.indices.insert( out.indices.end(), t, t + 6 );
				}
				else
				{
					int j = ( i + 1 ) % n;
					while ( edgeOf[j] < 0 )
						j = ( j + 1 ) % n;
					unsigned short t[3] = { P0, P1, (unsigned short)( base + n + edgeOf[j] ) };
					out.indices.insert( out.indices.end(), t, t + 3 );
				}
			}
		}
	fillSide:
		if ( fill )
		{
			int base = (int)out.verts.size() - side.firstVert, k = (int)inner.size();
			for ( int i = 0; i < k; i++ )
				Push( out, inner[i], look.fill );
			for ( int i = 1; i + 1 < k; i++ )
			{
				unsigned short t[3] = { (unsigned short)base, (unsigned short)( base + i ), (unsigned short)( base + i + 1 ) };
				out.indices.insert( out.indices.end(), t, t + 3 );
			}
		}
		side.numVerts = (int)out.verts.size() - side.firstVert;
		side.numIndices = (int)out.indices.size() - side.firstIndex;
		out.sides.push_back( side );
	}
}
