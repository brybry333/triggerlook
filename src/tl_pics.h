//=============================================================================
// triggerlook: the custom skybox's pictures and sprays (the menu's SKYBOX page). Plain C++ (no engine calls).
//
//   materials/triggerlook/sky/<name>.vtf + .vmt     a picture added in the menu (ADD A FILE), made with MareTF
//   materials/triggerlook/sky/logo_<spray>.vmt      a spray of this PC (materials/vgui/logos/<spray>.vtf): the
//                                                   material points at the spray's own texture (nothing copied)
// Both materials also give the menu's thumbnails. Adapted from p2paint's pictures (p2paint_pics.cpp).
//=============================================================================
#ifndef TL_PICS_H
#define TL_PICS_H

#include <string>
#include <vector>

struct TLPic_t
{
	std::string name;			// a picture: its file name; a spray: its file name in materials/vgui/logos (no .vtf)
	std::string material;		// "triggerlook/sky/<name>" | "triggerlook/sky/logo_<spray>"
	int width = 0, height = 0, frames = 1;
	float aspect = 1;			// the picture's own width / height (the texture is stretched to powers of two)
	bool spray = false;
};

struct TLPicStatus_t
{
	int state = 0;			// 0 idle, 1 busy, 2 done, 3 failed
	std::string text;		// "adding cat.gif ...", "added cat", "cat.gif: MareTF can't read this file"
	std::string added;		// the new picture's name (done)
	int serial = 0;			// a new number on every change
};

void TLPics_Init( const std::string &gameDir );
void TLPics_Refresh();		// both lists again; materials for new / changed sprays are written here (main thread)
const std::vector<TLPic_t> &TLPics_List( bool sprays );
const TLPic_t *TLPics_Find( bool spray, const std::string &name );

// a file (JPEG / PNG / GIF / WEBP / VTF) into a picture, in a background thread; false = one is still being made
bool TLPics_Add( const std::string &path );
TLPicStatus_t TLPics_Status();

// the in-game file list: sub folders, then the files ADD A FILE takes, sorted (hidden ones left out)
struct TLDirEntry_t { std::string name; bool dir; };
bool TLPics_ListDir( const std::string &dir, std::vector<TLDirEntry_t> &out );

#endif // TL_PICS_H
