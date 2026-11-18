//=============================================================================
// triggerlook: tl_platform.h for Windows (triggerlook.dll, loaded by bhoptools.exe's code in the game)
//=============================================================================
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

#include "tl_platform.h"

typedef void *( *CreateInterfaceFn_t )( const char *name, int *ret );
extern "C" __declspec( dllimport ) bool ThreadInMainThread();	// tier0

void *TLP_Interface( const char *module, const char *name )
{
	char file[64];
	snprintf( file, sizeof( file ), "%s.dll", module );
	HMODULE h = GetModuleHandleA( file );
	if ( !h )
		return NULL;
	CreateInterfaceFn_t fn = (CreateInterfaceFn_t)(void *)GetProcAddress( h, "CreateInterface" );
	return fn ? fn( name, NULL ) : NULL;
}

bool TLP_IsFile( const char *path )
{
	DWORD a = GetFileAttributesA( path );
	return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
}

bool TLP_ListDir( const char *dir, void ( *fn )( const char *name, void *ctx ), void *ctx )
{
	char pattern[1024];
	snprintf( pattern, sizeof( pattern ), "%s\\*", dir );
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA( pattern, &fd );
	if ( h == INVALID_HANDLE_VALUE )
		return false;
	do
	{
		if ( fd.cFileName[0] != '.' )
			fn( fd.cFileName, ctx );
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
	return true;
}

static const char *OwnDir( char *buf, size_t len, const char *env, const char *fallback )
{
	if ( !buf[0] )
	{
		const char *base = getenv( env );
		snprintf( buf, len, "%s/triggerlook", base && base[0] ? base : fallback );
		CreateDirectoryA( buf, NULL );
	}
	return buf;
}

const char *TLP_ConfigDir() { static char s[1024]; return OwnDir( s, sizeof( s ), "APPDATA", "." ); }
const char *TLP_CacheDir() { static char s[1024]; return OwnDir( s, sizeof( s ), "TEMP", "." ); }

void TLP_MakeDirs( const char *dir )
{
	char p[1024];
	snprintf( p, sizeof( p ), "%s", dir );
	for ( char *s = p + 1; *s; s++ )
		if ( ( *s == '/' || *s == '\\' ) && s[-1] != ':' )
		{
			char c = *s;
			*s = 0;
			CreateDirectoryA( p, NULL );
			*s = c;
		}
	CreateDirectoryA( p, NULL );
}

bool TLP_ReplaceFile( const char *tmp, const char *path )
{
	return MoveFileExA( tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) != 0;
}

bool TLP_Seek( FILE *f, long long ofs ) { return _fseeki64( f, ofs, SEEK_SET ) == 0; }

long long TLP_FileSize( FILE *f )
{
	LARGE_INTEGER size;
	HANDLE h = (HANDLE)_get_osfhandle( _fileno( f ) );
	return h != INVALID_HANDLE_VALUE && GetFileSizeEx( h, &size ) ? (long long)size.QuadPart : -1;
}

//-----------------------------------------------------------------------------
// The main thread from another one: one-shot message hooks on the threads that own the game's windows (SDL pumps
// the main window's messages every frame); only the one tier0 calls the main thread runs fn. No game code is touched.
//-----------------------------------------------------------------------------
extern "C" __declspec( dllimport ) void Warning( const char *fmt, ... );	// tier0

static void ( *volatile s_pfnMain )();
enum { MAX_HOOKS = 16 };
static HHOOK s_Hooks[MAX_HOOKS];
static DWORD s_HookThreads[MAX_HOOKS];
static int s_nHooks;

static LRESULT CALLBACK MessageHook( int code, WPARAM wParam, LPARAM lParam )
{
	if ( ThreadInMainThread() )
	{
		void ( *fn )() = (void ( * )())InterlockedExchangePointer( (void *volatile *)&s_pfnMain, NULL );
		if ( fn )
			fn();
	}
	return CallNextHookEx( NULL, code, wParam, lParam );
}

struct FindWindows_t { DWORD pid; DWORD tids[MAX_HOOKS]; int n; };

static BOOL CALLBACK FindGameWindows( HWND hwnd, LPARAM lp )
{
	FindWindows_t *f = (FindWindows_t *)lp;
	DWORD pid = 0, tid = GetWindowThreadProcessId( hwnd, &pid );
	if ( pid != f->pid || f->n >= MAX_HOOKS )
		return TRUE;
	for ( int i = 0; i < f->n; i++ )
		if ( f->tids[i] == tid )
			return TRUE;
	f->tids[f->n++] = tid;
	return TRUE;
}

static DWORD WINAPI WaitForMainThread( LPVOID )
{
	HMODULE self = NULL;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)&MessageHook, &self );
	for ( int i = 0; s_pfnMain; i++ )
	{
		// hook every thread that owns a window of the game (new ones too), nudge them, until the main one ran fn
		FindWindows_t f = { GetCurrentProcessId(), {}, 0 };
		EnumWindows( FindGameWindows, (LPARAM)&f );
		for ( int k = 0; k < f.n; k++ )
		{
			bool have = false;
			for ( int j = 0; j < s_nHooks && !have; j++ )
				have = s_HookThreads[j] == f.tids[k];
			if ( !have && s_nHooks < MAX_HOOKS )
				if ( HHOOK h = SetWindowsHookExW( WH_GETMESSAGE, MessageHook, self, f.tids[k] ) )
				{
					s_Hooks[s_nHooks] = h;
					s_HookThreads[s_nHooks++] = f.tids[k];
				}
		}
		for ( int j = 0; j < s_nHooks; j++ )
			PostThreadMessageW( s_HookThreads[j], WM_NULL, 0, 0 );
		if ( i == 1200 )
			Warning( "triggerlook: still waiting for the game's main thread (2 minutes)\n" );
		Sleep( 100 );
	}
	for ( int j = 0; j < s_nHooks; j++ )
		UnhookWindowsHookEx( s_Hooks[j] );
	s_nHooks = 0;
	return 0;
}

// LoadLibrary is all a loader has to do (bhoptools.exe's code, or any injector): we start ourselves. Not in
// DllMain itself (loader lock): a thread that runs right after it.
extern "C" int TriggerLook_Init();

static DWORD WINAPI StartThread( LPVOID )
{
	TriggerLook_Init();
	return 0;
}

BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, LPVOID )
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		DisableThreadLibraryCalls( hinst );
		if ( HANDLE t = CreateThread( NULL, 0, StartThread, NULL, 0, NULL ) )
			CloseHandle( t );
	}
	return TRUE;
}

void TLP_RunOnMainThread( void ( *fn )() )
{
	if ( ThreadInMainThread() )
	{
		fn();
		return;
	}
	s_pfnMain = fn;
	if ( HANDLE t = CreateThread( NULL, 0, WaitForMainThread, NULL, 0, NULL ) )
		CloseHandle( t );
}
#endif
