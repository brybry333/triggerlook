//=============================================================================
// triggerlook: tl_platform.h for Linux (libtriggerlook_mod.so, dlopened by the loader)
//=============================================================================
#ifndef _WIN32
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

bool TLP_ListDir( const char *dir, void ( *fn )( const char *name, void *ctx ), void *ctx )
{
	DIR *d = opendir( dir );
	if ( !d )
		return false;
	while ( struct dirent *e = readdir( d ) )
		if ( e->d_name[0] != '.' )
			fn( e->d_name, ctx );
	closedir( d );
	return true;
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
