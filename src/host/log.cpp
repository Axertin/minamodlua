#include "log.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace mml::log
{

namespace
{

MinaModAPI* g_mm = nullptr;
FILE* g_file = nullptr;

}  // namespace

void init( MinaModAPI* mm )
{
    g_mm = mm;

    // Beside the game's own mod.log, which is where a mod author already looks.
#if defined( _WIN32 )
    const char* base = getenv( "APPDATA" );
    const char* tail = "/Yacht Club Games/Mina the Hollower/luamods.log";
#else
    const char* base = getenv( "HOME" );
    const char* tail = "/.local/share/Yacht Club Games/Mina the Hollower/luamods.log";
#endif
    if ( !base ) return;

    char path[1024];
    snprintf( path, sizeof path, "%s%s", base, tail );
    g_file = fopen( path, "w" );  // truncate: this run only
}

void shutdown()
{
    if ( g_file )
    {
        fclose( g_file );
        g_file = nullptr;
    }
    g_mm = nullptr;
}

void write( const char* fmt, ... )
{
    // Two bytes held back for the newline Log does not add, and the terminator.
    char buf[2048];
    const int cap = (int)sizeof buf - 2;

    int n = snprintf( buf, cap, "[minamodlua] " );

    va_list args;
    va_start( args, fmt );
    const int m = vsnprintf( buf + n, (size_t)( cap - n ), fmt, args );
    va_end( args );

    if ( m > 0 ) n += m;
    if ( n > cap ) n = cap;
    buf[n] = '\n';
    buf[n + 1] = '\0';

    // "%s" and never the caller's string: Log is printf-style, and mod-supplied
    // text containing a stray %s would otherwise read off the stack.
    if ( g_mm && g_mm->Log ) g_mm->Log( "%s", buf );
    if ( g_file )
    {
        fputs( buf, g_file );
        fflush( g_file );  // the game may terminate without unwinding
    }
}

}  // namespace mml::log
