//=============================================================================
// triggerlook: what differs between Linux and Windows. Plain C++ only here (windows.h and the Source SDK headers
// don't mix): tl_platform_linux.cpp / tl_platform_win.cpp implement it.
//=============================================================================
#ifndef TL_PLATFORM_H
#define TL_PLATFORM_H

#include <stdio.h>

// a game module's interface: module = "engine", "client", "materialsystem", "vgui2", "vguimatsurface",
// "inputsystem", "filesystem_stdio", "vstdlib" (engine.so / engine.dll, libvstdlib.so / vstdlib.dll, ...)
void *TLP_Interface( const char *module, const char *name );

bool TLP_IsFile( const char *path );
// calls fn( name ) for every entry of a directory (no "." entries); false = can't read it
bool TLP_ListDir( const char *dir, void ( *fn )( const char *name, void *ctx ), void *ctx );
// a folder of ours, created on first use, without the trailing slash:
//   config: ~/.config/triggerlook | %APPDATA%\triggerlook       cache: ~/.cache/triggerlook | %TEMP%\triggerlook
const char *TLP_ConfigDir();
const char *TLP_CacheDir();
const char TLP_SLASH = '/';	// (Windows takes / too)

// creates a folder and its parents (path with / separators)
void TLP_MakeDirs( const char *dir );
// tmp replaces path in one step (the old file stays if it fails)
bool TLP_ReplaceFile( const char *tmp, const char *path );

bool TLP_Seek( FILE *f, long long ofs );
long long TLP_FileSize( FILE *f );

// runs fn once on the game's main thread: at once when called there (Linux: the loader calls us there), else as
// soon as the main thread pumps its window messages (Windows: loaded by bhoptools' thread)
void TLP_RunOnMainThread( void ( *fn )() );

#endif // TL_PLATFORM_H
