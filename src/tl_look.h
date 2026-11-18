//=============================================================================
// triggerlook: how each trigger type looks (the menu edits these, the layer draws with them), saved in
// ~/.config/triggerlook/settings.txt (Linux) / %APPDATA%\triggerlook\settings.txt (Windows): the player's PC, any server
//=============================================================================
#ifndef TL_LOOK_H
#define TL_LOOK_H

#include "tl_map.h"

struct TLLook_t
{
	bool			show;			// draw this type at all
	unsigned char	fill[4];		// rgba of the sides
	unsigned char	border[4];		// rgba of the band along the edges
	int				width;			// border width in units, 0 = no border
};

enum { TL_MAX_WIDTH = 16 };

extern TLLook_t g_TLLook[TL_TYPES];
extern int g_TLCustom[3];			// the last custom colour (the menu's custom block)
extern int g_nTLTypeVersion[TL_TYPES];	// bumped when a type's look changes (its meshes rebuild)

void TL_LookDefaults();
void TL_LookChanged( int typeMask = ( 1 << TL_TYPES ) - 1 );	// after editing g_TLLook / g_TLCustom: rebuild those types + save soon
void TL_LookTick( double now );		// saves a while after the last change
void TL_LookFlush();				// saves now if anything changed (the menu closing)
bool TL_LookLoad();
bool TL_LookSave();
const char *TL_LookPath();

// "teleport", "all", ... -> a bit mask of types (0 = unknown)
int TL_TypeMask( const char *name );

#endif // TL_LOOK_H
