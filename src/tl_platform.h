//=============================================================================
// triggerlook: what differs between Linux and Windows. Plain C++ only here (windows.h and the Source SDK headers
// don't mix): tl_platform_linux.cpp / tl_platform_win.cpp implement it.
//=============================================================================
#ifndef TL_PLATFORM_H
#define TL_PLATFORM_H

#include <stdio.h>

#include <string>
#include <vector>

// a game module's interface: module = "engine", "client", "materialsystem", "vgui2", "vguimatsurface",
// "inputsystem", "filesystem_stdio", "vstdlib" (engine.so / engine.dll, libvstdlib.so / vstdlib.dll, ...)
void *TLP_Interface( const char *module, const char *name );

bool TLP_IsFile( const char *path );
bool TLP_IsDir( const std::string &path );
long long TLP_MTime( const std::string &path );		// -1 = no such file
bool TLP_Remove( const char *path );
// the entries of a folder (no "." ones); false = can't read it. Windows: "" = the drives ("C:", ...)
struct TLPEntry_t { std::string name; bool dir; long long mtime; };
bool TLP_List( const std::string &dir, std::vector<TLPEntry_t> &out );
// a folder of ours, created on first use, without the trailing slash:
//   config: ~/.config/triggerlook | %APPDATA%\triggerlook       cache: ~/.cache/triggerlook | %TEMP%\triggerlook
const char *TLP_ConfigDir();
const char *TLP_CacheDir();
const char TLP_SLASH = '/';	// (Windows takes / too)
// fopen for paths in those folders (UTF-8: Windows opens them with the wide call)
FILE *TLP_Open( const char *path, const char *mode );

// creates a folder and its parents (path with / separators)
void TLP_MakeDirs( const char *dir );
// tmp replaces path in one step (the old file stays if it fails)
bool TLP_ReplaceFile( const char *tmp, const char *path );

bool TLP_Seek( FILE *f, long long ofs );
long long TLP_FileSize( FILE *f );

// the sky pictures (tl_pics.cpp): the player's home (~ | %USERPROFILE%, / separators), the root a path starts on
// ("/" | "C:/"), MareTF ("" = none: Linux the host's, the game runs in Steam's container; Windows maretf.exe next to
// us or on PATH), running it (output appended to log; 0 = exit code 0, 1 = failed, 2 = killed after timeout
// seconds), a detached thread. Paths are UTF-8 (Windows uses the wide calls).
std::string TLP_Home();
std::string TLP_Root( const std::string &path );
std::string TLP_FindMareTF();
int TLP_Run( const std::string &exe, const std::vector<std::string> &args, const std::string &log, int timeout );
bool TLP_StartThread( void ( *fn )( void * ), void *arg );

// runs fn once on the game's main thread: at once when called there (Linux: the loader calls us there), else as
// soon as the main thread pumps its window messages (Windows: loaded by bhoptools' thread)
void TLP_RunOnMainThread( void ( *fn )() );

#endif // TL_PLATFORM_H
