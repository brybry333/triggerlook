//=============================================================================
// triggerlook: how each trigger type looks (the menu edits these, the layer draws with them), saved in
// ~/.config/triggerlook/settings.txt (Linux) / %APPDATA%\triggerlook\settings.txt (Windows): the player's PC, any server
//=============================================================================
#ifndef TL_LOOK_H
#define TL_LOOK_H

#include "tl_map.h"

#include <string>
#include <vector>

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
extern int g_nTLTypeVersion[TL_TYPES];	// a new number when a type's look changes (its meshes rebuild)
extern std::string g_TLPickKey, g_TLPickBind;	// SELECT TRIGGER running: the borrowed key and its own bind (saved at once)
extern bool g_bTLShow;				// ON / OFF (the menu's switch, triggerlook_show): draw the map's triggers at all

// A trigger picked in the world (the menu's SELECT TRIGGER): its own look on its map, over its type's (also when the
// type is hidden). The menu calls the ones of the current map CT-1, CT-2, ... in this order.
struct TLCustomTrigger_t
{
	std::string	map;			// "bhop_foo"
	int			model;			// its brush model (*N)
	Vector		mins, maxs;		// that model's box when picked: another file under the same map name is matched by it
	TLLook_t	look;			// (show: unused)
	int			version;		// a new number when the look changes
	std::string	name;			// the player's name for it (double click in the menu), empty = "CT-n"
};
enum { TL_NAME_MAX = 24 };
// a name as kept: printable ASCII, no spaces at the ends, at most TL_NAME_MAX characters
std::string TL_CleanName( const std::string &name );
extern std::vector<TLCustomTrigger_t> g_TLCT;	// every map's
extern int g_nTLCTListVersion;					// bumped when one is added or deleted

// the custom skybox (the menu's SKYBOX, triggerlook_sky): every map, whether the triggers are ON or OFF. Settings line
// "sky off" | "sky colour r g b" | "sky spray <name>" | "sky picture <name>" (sprays: materials/vgui/logos/<name>.vtf,
// pictures: materials/triggerlook/sky/<name>.vtf, added in the menu)
enum { TL_SKY_OFF, TL_SKY_COLOUR, TL_SKY_SPRAY, TL_SKY_PICTURE };
struct TLSky_t
{
	int				mode = TL_SKY_OFF;
	unsigned char	rgb[3] = { 40, 40, 40 };
	std::string		spray, picture;		// (all kept while playing: switching back finds the last one)
};
extern TLSky_t g_TLSky;
extern int g_nTLSkyVersion;			// a new number on every change (the runtime picks its material again)
void TL_SkyChanged();				// after editing g_TLSky: saved soon

void TL_LookDefaults();
void TL_LookChanged( int typeMask = ( 1 << TL_TYPES ) - 1 );	// after editing g_TLLook / g_TLCustom: rebuild those types + save soon
void TL_CTChanged( int index );			// after editing g_TLCT[index].look
void TL_CTListChanged();				// after adding to / deleting from g_TLCT
int TL_NewVersion();
void TL_LookTick( double now );		// saves a while after the last change
void TL_LookFlush();				// saves now if anything changed (the menu closing)
bool TL_LookLoad();
bool TL_LookSave();
const char *TL_LookPath();

// "teleport", "all", ... -> a bit mask of types (0 = unknown)
int TL_TypeMask( const char *name );

#endif // TL_LOOK_H
