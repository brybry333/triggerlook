// geomtest: offline check of the map reader + mesh builder (no game needed)
//   build/geomtest <game dir> <maps/x.bsp> [out.obj]
// Prints the triggers per type, checks every side (flat, convex, clockwise from the front, closed solids), builds
// each trigger's mesh with a border and checks the triangles; with an .obj path writes the sides + meshes.
#include <stdio.h>
#include <time.h>
#include <map>

#include "tl_map.h"
#include "tl_mesh.h"
#include "tier1/strtools.h"

static double Now() { timespec t; clock_gettime( CLOCK_MONOTONIC, &t ); return t.tv_sec + t.tv_nsec * 1e-9; }

int main( int argc, char **argv )
{
	if ( argc < 3 )
	{
		fprintf( stderr, "usage: geomtest <game dir> <maps/x.bsp> [out.obj]\n" );
		return 2;
	}
	CTLMap map;
	double t0 = Now();
	if ( !map.Load( argv[1], argv[2] ) )
		return 1;
	double t1 = Now();
	int bad = 0, sides = 0, open = 0, grownOff = 0;
	TLLook_t look = { true, { 40, 40, 40, 128 }, { 255, 255, 255, 255 }, getenv( "GEOM_WIDTH" ) ? atoi( getenv( "GEOM_WIDTH" ) ) : 4 };
	TLMesh_t mesh;
	size_t verts = 0, indices = 0;
	double tm = 0;
	FILE *obj = argc > 3 ? fopen( argv[3], "w" ) : NULL;
	int objBase = 1;
	for ( int mi = 1; mi < map.NumModels(); mi++ )
	{
		const TLModel_t *m = map.Model( mi );
		if ( !m )
			continue;
		if ( getenv( "GEOM_LIST" ) )
			printf( "  *%d %-8s %-28s mins %g %g %g  maxs %g %g %g\n", mi, g_TLTypeName[m->type], m->classname.c_str(), m->mins.x, m->mins.y,
					m->mins.z, m->maxs.x, m->maxs.y, m->maxs.z );
		// every side: flat on its plane, convex, clockwise seen from the front (cross . normal < 0 at every corner)
		std::map<std::pair<long long, long long>, int> edges;	// directed edge -> count (closed solids: every edge once each way)
		for ( const TLPoly_t &p : m->polys )
		{
			sides++;
			const Vector *v = &m->verts[p.first];
			float d = DotProduct( v[0], p.normal );
			for ( int i = 0; i < p.count; i++ )
			{
				const Vector &a = v[i], &b = v[( i + 1 ) % p.count], &c = v[( i + 2 ) % p.count];
				if ( fabsf( DotProduct( a, p.normal ) - d ) > 0.05f )
				{
					bad++;
					printf( "  *%d %s: side off its plane\n", mi, m->classname.c_str() );
					break;
				}
				if ( DotProduct( CrossProduct( b - a, c - b ), p.normal ) > 0.01f )
				{
					bad++;
					printf( "  *%d %s: side not convex / not clockwise\n", mi, m->classname.c_str() );
					break;
				}
				auto key = []( const Vector &q ) { return ( (long long)lroundf( q.x * 8 ) * 2000003LL + lroundf( q.y * 8 ) ) * 2000003LL + lroundf( q.z * 8 ); };
				edges[{ key( a ), key( b ) }]++;
			}
		}
		for ( auto &e : edges )
			if ( !edges.count( { e.first.second, e.first.first } ) )
				open++;
		double ta = Now();
		TL_BuildMesh( *m, look, mesh );
		tm += Now() - ta;
		verts += mesh.verts.size();
		indices += mesh.indices.size();
		for ( const TLSideMesh_t &s : mesh.sides )
			for ( int k = 0; k < s.numIndices; k++ )
				if ( mesh.indices[s.firstIndex + k] >= s.numVerts )
				{
					bad++;
					printf( "  *%d: index out of its side\n", mi );
					break;
				}
		// triangles keep the side's turning sense (else the GPU would cull them)
		for ( size_t si = 0; si < mesh.sides.size(); si++ )
		{
			const TLSideMesh_t &s = mesh.sides[si];
			const Vector &n = m->polys[si].normal;
			for ( int k = 0; k + 2 < s.numIndices; k += 3 )
			{
				const Vector &a = mesh.verts[s.firstVert + mesh.indices[s.firstIndex + k]].pos;
				const Vector &b = mesh.verts[s.firstVert + mesh.indices[s.firstIndex + k + 1]].pos;
				const Vector &c = mesh.verts[s.firstVert + mesh.indices[s.firstIndex + k + 2]].pos;
				Vector cr = CrossProduct( b - a, c - a );
				if ( cr.Length() > 0.01f && DotProduct( cr, n ) > 0.5f * cr.Length() )	// clearly backwards (slivers aside)
				{
					bad++;
					printf( "  *%d: triangle facing the wrong way (side %zu, %d corners, tri %d of %d):", mi, si, m->polys[si].count, k / 3, s.numIndices / 3 );
					for ( int q = 0; q < m->polys[si].count; q++ ) { const Vector &v = m->verts[m->polys[si].first + q]; printf( " (%g %g %g)", v.x, v.y, v.z ); }
					printf( "\n" );
					k = s.numIndices;
				}
			}
		}
		// the drawn (grown) shape: every side still flat and exactly 0.5 out from its own
		{
			TLModel_t g;
			const float eps = TL_DRAW_GROW_X100 / 100.0f;
			TL_ExpandModel( *m, eps, g );
			if ( getenv( "GEOM_DEBUG" ) && atoi( getenv( "GEOM_DEBUG" ) ) == mi )
				for ( size_t si = 0; si < m->polys.size(); si++ )
				{
					const TLPoly_t &p = m->polys[si];
					printf( "  side %zu brush %d n %.4f %.4f %.4f:", si, p.brush, p.normal.x, p.normal.y, p.normal.z );
					for ( int k = 0; k < p.count; k++ )
					{
						const Vector &a = m->verts[p.first + k], &b = g.verts[p.first + k];
						printf( " (%.3f %.3f %.3f -> %+.3f %+.3f %+.3f)", a.x, a.y, a.z, b.x - a.x, b.y - a.y, b.z - a.z );
					}
					printf( "\n" );
				}
			for ( size_t si = 0; si < m->polys.size(); si++ )
			{
				const TLPoly_t &poly = m->polys[si];
				// every corner of the side has to end up in front of the side's own plane (at least 0.2: a side lying on a
				// floor must win against it); sharp corners move less than eps (capped), that's fine
				float least = 1e9f;
				for ( int k = 0; k < poly.count; k++ )
					least = MIN( least, DotProduct( poly.normal, g.verts[poly.first + k] - m->verts[poly.first + k] ) );
				if ( least < 0.2f )
				{
					grownOff++;
					if ( getenv( "GEOM_LIST" ) )
						printf( "  *%d: grown side %zu only %.3f out\n", mi, si, least );
				}
			}
		}
		// band + fill cover each side exactly once: their triangles add up to the side's area
		for ( size_t si = 0; si < mesh.sides.size(); si++ )
		{
			const TLSideMesh_t &s = mesh.sides[si];
			const TLPoly_t &poly = m->polys[si];
			Vector a2( 0, 0, 0 );
			const Vector &o = m->verts[poly.first];	// (relative to a corner: clips sit far out, floats lose the area)
			for ( int i = 0; i < poly.count; i++ )
				a2 += CrossProduct( m->verts[poly.first + i] - o, m->verts[poly.first + ( i + 1 ) % poly.count] - o );
			float sideArea = fabsf( DotProduct( a2, poly.normal ) ) * 0.5f, tris = 0;
			for ( int k = 0; k + 2 < s.numIndices; k += 3 )
			{
				const Vector &a = mesh.verts[s.firstVert + mesh.indices[s.firstIndex + k]].pos;
				const Vector &b = mesh.verts[s.firstVert + mesh.indices[s.firstIndex + k + 1]].pos;
				const Vector &c = mesh.verts[s.firstVert + mesh.indices[s.firstIndex + k + 2]].pos;
				tris += CrossProduct( b - a, c - a ).Length() * 0.5f;
			}
			if ( fabsf( tris - sideArea ) > 0.01f * sideArea + 0.5f )
			{
				bad++;
				printf( "  *%d: side %zu covered %.1f of %.1f\n", mi, si, tris, sideArea );
			}
		}
		if ( obj )
		{
			fprintf( obj, "o m%d_%s\n", mi, g_TLTypeName[m->type] );
			for ( const TLVert_t &v : mesh.verts )
				fprintf( obj, "v %g %g %g %g %g %g\n", v.pos.x, v.pos.y, v.pos.z, v.rgba[0] / 255.0, v.rgba[1] / 255.0, v.rgba[2] / 255.0 );
			for ( const TLSideMesh_t &s : mesh.sides )
				for ( int k = 0; k + 2 < s.numIndices; k += 3 )
					fprintf( obj, "f %d %d %d\n", objBase + s.firstVert + mesh.indices[s.firstIndex + k],
							 objBase + s.firstVert + mesh.indices[s.firstIndex + k + 1], objBase + s.firstVert + mesh.indices[s.firstIndex + k + 2] );
			objBase += (int)mesh.verts.size();
		}
	}
	if ( obj )
		fclose( obj );
	printf( "%s: load %.1f ms, %d sides, mesh with 4-unit border: %zu verts %zu indices (%.2f ms to build all)\n", argv[2],
			( t1 - t0 ) * 1000, sides, verts, indices, tm * 1000 );
	printf( "%d open edges (sides of a solid that don't meet another side), %d problems, %d grown sides not out far enough\n", open, bad, grownOff );
	return bad ? 1 : 0;
}
