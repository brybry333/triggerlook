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
#include <wchar.h>

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

// UTF-8 (our paths; the game folder from the engine may be in the code page) <-> UTF-16 for the wide calls (a user
// or folder name outside the code page: the A calls can't open it)
static std::wstring W( const std::string &s )
{
	for ( UINT cp : { (UINT)CP_UTF8, (UINT)CP_ACP } )
	{
		const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
		const int n = MultiByteToWideChar( cp, flags, s.c_str(), -1, NULL, 0 );
		if ( n > 0 )
		{
			std::wstring w( n, 0 );
			MultiByteToWideChar( cp, flags, s.c_str(), -1, &w[0], n );
			w.resize( n - 1 );
			return w;
		}
	}
	return L"";
}

static std::string U( const wchar_t *w )
{
	const int n = WideCharToMultiByte( CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL );
	std::string s( n > 0 ? n : 1, 0 );
	if ( n > 0 )
		WideCharToMultiByte( CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL );
	s.resize( n > 0 ? n - 1 : 0 );
	return s;
}

static std::string Slashes( std::string s )
{
	for ( char &c : s )
		if ( c == '\\' )
			c = '/';
	return s;
}

bool TLP_IsFile( const char *path )
{
	DWORD a = GetFileAttributesW( W( path ).c_str() );
	return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
}

bool TLP_IsDir( const std::string &path )
{
	DWORD a = GetFileAttributesW( W( path ).c_str() );
	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
}

bool TLP_Remove( const char *path ) { return DeleteFileW( W( path ).c_str() ) != 0; }

static long long UnixTime( const FILETIME &ft )
{
	ULARGE_INTEGER t;
	t.LowPart = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	return (long long)( t.QuadPart / 10000000ULL ) - 11644473600LL;
}

long long TLP_MTime( const std::string &path )
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	return GetFileAttributesExW( W( path ).c_str(), GetFileExInfoStandard, &a ) ? UnixTime( a.ftLastWriteTime ) : -1;
}

bool TLP_List( const std::string &dir, std::vector<TLPEntry_t> &out )
{
	out.clear();
	if ( dir.empty() )
	{
		// above a drive's root: the drives
		const DWORD drives = GetLogicalDrives();
		for ( int i = 0; i < 26; i++ )
			if ( drives & ( 1u << i ) )
				out.push_back( { std::string( 1, (char)( 'A' + i ) ) + ":", true, 0 } );
		return true;
	}
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW( W( dir + ( dir.back() == '/' || dir.back() == '\\' ? "*" : "/*" ) ).c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE )
		return false;
	do
	{
		if ( fd.cFileName[0] != L'.' )
			out.push_back( { U( fd.cFileName ), ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0, UnixTime( fd.ftLastWriteTime ) } );
	} while ( FindNextFileW( h, &fd ) );
	FindClose( h );
	return true;
}

std::string TLP_Home()
{
	const wchar_t *h = _wgetenv( L"USERPROFILE" );
	return h && h[0] ? Slashes( U( h ) ) : "C:/";
}

// "C:/..." -> "C:/"; above it there's "" (the drives)
std::string TLP_Root( const std::string &path ) { return path.size() >= 2 && path[1] == ':' ? path.substr( 0, 2 ) + "/" : ""; }

std::string TLP_FindMareTF()
{
	wchar_t buf[MAX_PATH];
	HMODULE self = NULL;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)&TLP_FindMareTF, &self );
	const DWORD n = GetModuleFileNameW( self, buf, MAX_PATH );
	buf[n < MAX_PATH ? n : MAX_PATH - 1] = 0;
	std::string p = Slashes( U( buf ) );
	p = p.substr( 0, p.rfind( '/' ) + 1 ) + "maretf.exe";
	if ( TLP_IsFile( p.c_str() ) )
		return p;
	return SearchPathW( NULL, L"maretf.exe", NULL, MAX_PATH, buf, NULL ) ? Slashes( U( buf ) ) : "";
}

// one command-line argument the way CommandLineToArgvW / the CRT read it back
static void Quote( std::string &cmd, const std::string &a )
{
	cmd += '"';
	int slashes = 0;
	for ( char c : a )
	{
		if ( c == '\\' )
			slashes++;
		else
		{
			if ( c == '"' )
				cmd.append( slashes + 1, '\\' );
			slashes = 0;
		}
		cmd += c;
	}
	cmd.append( slashes, '\\' );
	cmd += "\" ";
}

int TLP_Run( const std::string &exe, const std::vector<std::string> &args, const std::string &log, int timeout )
{
	std::string cmd;
	Quote( cmd, exe );
	for ( const std::string &a : args )
		Quote( cmd, a );
	std::wstring wcmd = W( cmd );
	SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
	HANDLE out = CreateFileW( W( log ).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	STARTUPINFOW si = { sizeof( si ) };
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = out;
	PROCESS_INFORMATION pi;
	const BOOL ok = CreateProcessW( NULL, &wcmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi );
	if ( out != INVALID_HANDLE_VALUE )
		CloseHandle( out );
	if ( !ok )
		return 1;
	int result = 1;
	if ( WaitForSingleObject( pi.hProcess, (DWORD)timeout * 1000 ) == WAIT_TIMEOUT )
	{
		TerminateProcess( pi.hProcess, 1 );
		WaitForSingleObject( pi.hProcess, INFINITE );
		result = 2;
	}
	else
	{
		DWORD code = 1;
		GetExitCodeProcess( pi.hProcess, &code );
		result = code == 0 ? 0 : 1;
	}
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return result;
}

struct Thread_t { void ( *fn )( void * ); void *arg; };
static DWORD WINAPI ThreadMain( LPVOID p )
{
	Thread_t t = *(Thread_t *)p;
	delete (Thread_t *)p;
	t.fn( t.arg );
	return 0;
}

bool TLP_StartThread( void ( *fn )( void * ), void *arg )
{
	Thread_t *t = new Thread_t{ fn, arg };
	HANDLE h = CreateThread( NULL, 0, ThreadMain, t, 0, NULL );
	if ( !h )
		delete t;
	else
		CloseHandle( h );
	return h != NULL;
}

static const char *OwnDir( char *buf, size_t len, const wchar_t *env )
{
	if ( !buf[0] )
	{
		const wchar_t *base = _wgetenv( env );
		wchar_t dir[1024];
		swprintf( dir, 1024, L"%ls/triggerlook", base && base[0] ? base : L"." );
		CreateDirectoryW( dir, NULL );
		if ( !WideCharToMultiByte( CP_UTF8, 0, dir, -1, buf, (int)len, NULL, NULL ) )
			buf[0] = 0;
	}
	return buf;
}

const char *TLP_ConfigDir() { static char s[1024]; return OwnDir( s, sizeof( s ), L"APPDATA" ); }
const char *TLP_CacheDir() { static char s[1024]; return OwnDir( s, sizeof( s ), L"TEMP" ); }

FILE *TLP_Open( const char *path, const char *mode ) { return _wfopen( W( path ).c_str(), W( mode ).c_str() ); }

void TLP_MakeDirs( const char *dir )
{
	const std::string p = dir;
	for ( size_t i = 1; i <= p.size(); i++ )
		if ( i == p.size() || ( ( p[i] == '/' || p[i] == '\\' ) && p[i - 1] != ':' ) )
			CreateDirectoryW( W( p.substr( 0, i ) ).c_str(), NULL );
}

bool TLP_ReplaceFile( const char *tmp, const char *path ) { return MoveFileExW( W( tmp ).c_str(), W( path ).c_str(), MOVEFILE_REPLACE_EXISTING ) != 0; }

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
