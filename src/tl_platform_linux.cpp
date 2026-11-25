//=============================================================================
// triggerlook: tl_platform.h for Linux (libtriggerlook_mod.so, dlopened by the loader)
//=============================================================================
#ifndef _WIN32
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tl_platform.h"

typedef void *( *CreateInterfaceFn_t )( const char *name, int *ret );
extern "C" bool ThreadInMainThread();	// tier0
extern "C" void Warning( const char *fmt, ... );

struct FindModule_t { const char *suffix; char path[1024]; bool found; };

static int FindModuleCB( struct dl_phdr_info *info, size_t, void *data )
{
	FindModule_t *m = (FindModule_t *)data;
	size_t n = strlen( info->dlpi_name ), s = strlen( m->suffix );
	if ( n < s || strcmp( info->dlpi_name + n - s, m->suffix ) )
		return 0;
	snprintf( m->path, sizeof( m->path ), "%s", info->dlpi_name );
	m->found = true;
	return 1;
}

void *TLP_Interface( const char *module, const char *name )
{
	char suffix[128];
	snprintf( suffix, sizeof( suffix ), !strcmp( module, "vstdlib" ) ? "/lib%s.so" : "/%s.so", module );
	FindModule_t m = { suffix, "", false };
	dl_iterate_phdr( FindModuleCB, &m );
	if ( !m.found )
		return NULL;
	void *h = dlopen( m.path, RTLD_NOW | RTLD_NOLOAD );
	if ( !h )
		return NULL;
	CreateInterfaceFn_t fn = (CreateInterfaceFn_t)dlsym( h, "CreateInterface" );
	dlclose( h );	// NOLOAD handle; the module stays loaded
	return fn ? fn( name, NULL ) : NULL;
}

bool TLP_IsFile( const char *path )
{
	struct stat st;
	return stat( path, &st ) == 0 && S_ISREG( st.st_mode );
}

bool TLP_Remove( const char *path ) { return remove( path ) == 0; }

bool TLP_IsDir( const std::string &path )
{
	struct stat st;
	return stat( path.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
}

long long TLP_MTime( const std::string &path )
{
	struct stat st;
	return stat( path.c_str(), &st ) == 0 ? (long long)st.st_mtime : -1;
}

bool TLP_List( const std::string &dir, std::vector<TLPEntry_t> &out )
{
	out.clear();
	DIR *d = opendir( dir.c_str() );
	if ( !d )
		return false;
	while ( struct dirent *e = readdir( d ) )
	{
		struct stat st;
		if ( e->d_name[0] != '.' && stat( ( dir + "/" + e->d_name ).c_str(), &st ) == 0 )
			out.push_back( { e->d_name, S_ISDIR( st.st_mode ), (long long)st.st_mtime } );
	}
	closedir( d );
	return true;
}

std::string TLP_Home()
{
	const char *h = getenv( "HOME" );
	return h && h[0] ? h : "/";
}

std::string TLP_Root( const std::string & ) { return "/"; }

std::string TLP_FindMareTF()
{
	static const char *const paths[] = { "/run/host/usr/bin/maretf", "/run/host/usr/local/bin/maretf", "/usr/bin/maretf", "/usr/local/bin/maretf" };
	for ( const char *p : paths )
		if ( access( p, X_OK ) == 0 )
			return p;
	std::string p = TLP_Home() + "/.local/bin/maretf";
	return access( p.c_str(), X_OK ) == 0 ? p : "";
}

// with the game's environment minus its preloads
extern char **environ;
int TLP_Run( const std::string &exe, const std::vector<std::string> &args, const std::string &log, int timeout )
{
	std::vector<char *> argv( 1, (char *)exe.c_str() ), envp;
	for ( const std::string &a : args )
		argv.push_back( (char *)a.c_str() );
	argv.push_back( NULL );
	for ( char **e = environ; *e; e++ )
		if ( strncmp( *e, "LD_PRELOAD=", 11 ) )
			envp.push_back( *e );
	envp.push_back( NULL );
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init( &fa );
	posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
	posix_spawn_file_actions_addopen( &fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644 );
	posix_spawn_file_actions_adddup2( &fa, 1, 2 );
	pid_t pid;
	const int rc = posix_spawn( &pid, exe.c_str(), &fa, NULL, argv.data(), envp.data() );
	posix_spawn_file_actions_destroy( &fa );
	if ( rc != 0 )
		return 1;
	int status = 0;
	for ( int ms = 0; ; ms += 50 )
	{
		const pid_t r = waitpid( pid, &status, WNOHANG );
		if ( r == pid || ( r < 0 && errno != EINTR ) )
			break;
		if ( ms >= timeout * 1000 )
		{
			kill( pid, SIGKILL );
			while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
				;
			return 2;
		}
		usleep( 50000 );
	}
	return WIFEXITED( status ) && WEXITSTATUS( status ) == 0 ? 0 : 1;
}

struct Thread_t { void ( *fn )( void * ); void *arg; };
static void *ThreadMain( void *p )
{
	Thread_t t = *(Thread_t *)p;
	delete (Thread_t *)p;
	t.fn( t.arg );
	return NULL;
}

bool TLP_StartThread( void ( *fn )( void * ), void *arg )
{
	Thread_t *t = new Thread_t{ fn, arg };
	pthread_t id;
	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
	const int rc = pthread_create( &id, &attr, ThreadMain, t );
	pthread_attr_destroy( &attr );
	if ( rc != 0 )
		delete t;
	return rc == 0;
}

static const char *OwnDir( char *buf, size_t len, const char *base )
{
	if ( !buf[0] )
	{
		const char *home = getenv( "HOME" );
		snprintf( buf, len, "%s/%s", home ? home : ".", base );
		char parent[1024];
		snprintf( parent, sizeof( parent ), "%s", buf );
		if ( char *slash = strrchr( parent, '/' ) )
		{
			*slash = 0;
			mkdir( parent, 0755 );
		}
		mkdir( buf, 0755 );
	}
	return buf;
}

const char *TLP_ConfigDir() { static char s[1024]; return OwnDir( s, sizeof( s ), ".config/triggerlook" ); }
const char *TLP_CacheDir() { static char s[1024]; return OwnDir( s, sizeof( s ), ".cache/triggerlook" ); }

FILE *TLP_Open( const char *path, const char *mode ) { return fopen( path, mode ); }

void TLP_MakeDirs( const char *dir )
{
	char p[1024];
	snprintf( p, sizeof( p ), "%s", dir );
	for ( char *s = p + 1; *s; s++ )
		if ( *s == '/' )
		{
			*s = 0;
			mkdir( p, 0755 );
			*s = '/';
		}
	mkdir( p, 0755 );
}

bool TLP_ReplaceFile( const char *tmp, const char *path ) { return rename( tmp, path ) == 0; }

bool TLP_Seek( FILE *f, long long ofs ) { return fseeko( f, ofs, SEEK_SET ) == 0; }

long long TLP_FileSize( FILE *f )
{
	struct stat st;
	return fstat( fileno( f ), &st ) == 0 ? (long long)st.st_size : -1;
}

void TLP_RunOnMainThread( void ( *fn )() )
{
	// the loader / bhoptools.so call TriggerLook_Init on the main thread (right after the engine loaded client.so)
	if ( ThreadInMainThread() )
		fn();
	else
		Warning( "triggerlook: started off the game's main thread, not loading\n" );
}
#endif
