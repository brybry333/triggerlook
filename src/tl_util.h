//=============================================================================
// triggerlook: the few tier1 / mathlib bits it used, inline, so the module needs nothing of the SDK but its
// headers and tier0 (the same on Linux and Windows; the SDK ships no Windows tier1 / mathlib)
//=============================================================================
#ifndef TL_UTIL_H
#define TL_UTIL_H

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "mathlib/vector.h"
#include "mathlib/vmatrix.h"

inline void TL_StrCopy( char *dst, const char *src, size_t n )
{
	if ( !n )
		return;
	size_t i = 0;
	for ( ; i + 1 < n && src[i]; i++ )
		dst[i] = src[i];
	dst[i] = 0;
}

inline int TL_StrNICmp( const char *a, const char *b, size_t n )
{
	for ( size_t i = 0; i < n; i++ )
	{
		int ca = tolower( (unsigned char)a[i] ), cb = tolower( (unsigned char)b[i] );
		if ( ca != cb || !ca )
			return ca - cb;
	}
	return 0;
}

inline int TL_StrICmp( const char *a, const char *b ) { return TL_StrNICmp( a, b, (size_t)-1 ); }

inline const char *TL_StrIStr( const char *hay, const char *needle )
{
	size_t n = strlen( needle );
	for ( ; *hay; hay++ )
		if ( !TL_StrNICmp( hay, needle, n ) )
			return hay;
	return n ? NULL : hay;
}

inline float TL_Normalize( Vector &v )
{
	float l = sqrtf( v.x * v.x + v.y * v.y + v.z * v.z );
	if ( l > 0 )
		v *= 1.0f / l;
	return l;
}

// point into / out of an entity's space (rotation + translation, as RenderableToWorldTransform gives)
inline void TL_Transform( const Vector &in, const matrix3x4_t &m, Vector &out )
{
	out.x = in.x * m[0][0] + in.y * m[0][1] + in.z * m[0][2] + m[0][3];
	out.y = in.x * m[1][0] + in.y * m[1][1] + in.z * m[1][2] + m[1][3];
	out.z = in.x * m[2][0] + in.y * m[2][1] + in.z * m[2][2] + m[2][3];
}

inline void TL_ITransform( const Vector &in, const matrix3x4_t &m, Vector &out )
{
	Vector d( in.x - m[0][3], in.y - m[1][3], in.z - m[2][3] );
	out.x = d.x * m[0][0] + d.y * m[1][0] + d.z * m[2][0];
	out.y = d.x * m[0][1] + d.y * m[1][1] + d.z * m[2][1];
	out.z = d.x * m[0][2] + d.y * m[1][2] + d.z * m[2][2];
}

// the world box of a box in an entity's space
inline void TL_TransformAABB( const matrix3x4_t &m, const Vector &mins, const Vector &maxs, Vector &omins, Vector &omaxs )
{
	Vector c = ( mins + maxs ) * 0.5f, e = ( maxs - mins ) * 0.5f, wc;
	TL_Transform( c, m, wc );
	for ( int i = 0; i < 3; i++ )
	{
		float r = fabsf( m[i][0] ) * e.x + fabsf( m[i][1] ) * e.y + fabsf( m[i][2] ) * e.z;
		omins[i] = wc[i] - r;
		omaxs[i] = wc[i] + r;
	}
}

inline void TL_MatMul( const VMatrix &a, const VMatrix &b, VMatrix &out )
{
	for ( int r = 0; r < 4; r++ )
		for ( int c = 0; c < 4; c++ )
			out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
}

// the camera position from a world -> view matrix (rotation + translation): -R^T t
inline Vector TL_ViewOrigin( const VMatrix &v )
{
	Vector o;
	for ( int i = 0; i < 3; i++ )
		o[i] = -( v.m[0][i] * v.m[0][3] + v.m[1][i] * v.m[1][3] + v.m[2][i] * v.m[2][3] );
	return o;
}

#endif // TL_UTIL_H
