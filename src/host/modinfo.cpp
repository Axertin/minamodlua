#include "modinfo.hpp"

#include "log.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace mml
{

namespace
{

std::string trim( std::string s )
{
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of( ws );
    if ( b == std::string::npos ) return {};
    return s.substr( b, s.find_last_not_of( ws ) - b + 1 );
}

// Values are either quoted strings or bare integers; trailing commas are part
// of the format.
std::string clean_value( std::string v )
{
    v = trim( v );
    if ( !v.empty() && v.back() == ',' ) v.pop_back();
    v = trim( v );
    if ( v.size() >= 2 && v.front() == '"' && v.back() == '"' ) v = v.substr( 1, v.size() - 2 );
    return v;
}

bool to_u32( const std::string& s, uint32_t& out )
{
    try
    {
        out = (uint32_t)std::stoul( s );
        return true;
    }
    catch ( ... )
    {
        return false;
    }
}

}  // namespace

bool parse_manifest( const fs::path& file, ModInfo& out, std::string& error )
{
    std::ifstream in( file );
    if ( !in )
    {
        error = "cannot open " + file.string();
        return false;
    }

    std::string line;
    while ( std::getline( in, line ) )
    {
        const size_t colon = line.find( ':' );
        if ( colon == std::string::npos ) continue;

        const std::string key = trim( line.substr( 0, colon ) );
        const std::string value = clean_value( line.substr( colon + 1 ) );
        if ( key.empty() || value.empty() ) continue;

        if ( key == "id" )
            out.id = value;
        else if ( key == "name" )
            out.name = value;
        else if ( key == "modVersion" )
            to_u32( value, out.modVersion );
        else if ( key == "minGameVersion" )
            to_u32( value, out.minGameVersion );
        else if ( key == "maxGameVersion" )
            to_u32( value, out.maxGameVersion );
        else if ( key == "loadPriority" )
            out.loadPriority = (int32_t)std::strtol( value.c_str(), nullptr, 10 );
    }

    if ( out.id.empty() )
    {
        error = "no id field";
        return false;
    }
    if ( out.name.empty() ) out.name = out.id;
    return true;
}

std::vector<ModInfo> discover_mods( const fs::path& modsDir, uint32_t gameRevision )
{
    std::vector<ModInfo> mods;

    std::error_code ec;
    if ( !fs::is_directory( modsDir, ec ) )
    {
        log::write( "no mods directory at %s", modsDir.string().c_str() );
        return mods;
    }

    for ( const fs::directory_entry& entry : fs::directory_iterator( modsDir, ec ) )
    {
        if ( !entry.is_directory( ec ) ) continue;

        const fs::path dir = entry.path();
        const fs::path manifest = dir / "mod.yc";
        const fs::path main = dir / "main.lua";

        // A folder without main.lua is a native mod or the game's own - not ours.
        if ( !fs::exists( main, ec ) ) continue;

        if ( !fs::exists( manifest, ec ) )
        {
            log::write( "%s has main.lua but no mod.yc, skipping", dir.filename().string().c_str() );
            continue;
        }

        ModInfo info;
        std::string error;
        info.dir = dir;
        if ( !parse_manifest( manifest, info, error ) )
        {
            log::write( "%s: bad mod.yc (%s), skipping", dir.filename().string().c_str(), error.c_str() );
            continue;
        }

        // The game gates its own mods on these; a Lua mod ships no binary, so
        // nothing would otherwise apply them. 0 means unconstrained.
        if ( info.minGameVersion && gameRevision < info.minGameVersion )
        {
            log::write( "%s needs game revision >= %u, this is %u - skipping", info.id.c_str(), info.minGameVersion,
                        gameRevision );
            continue;
        }
        if ( info.maxGameVersion && gameRevision > info.maxGameVersion )
        {
            log::write( "%s needs game revision <= %u, this is %u - skipping", info.id.c_str(), info.maxGameVersion,
                        gameRevision );
            continue;
        }

        mods.push_back( std::move( info ) );
    }

    std::sort( mods.begin(), mods.end(),
               []( const ModInfo& a, const ModInfo& b )
               {
                   if ( a.loadPriority != b.loadPriority ) return a.loadPriority > b.loadPriority;
                   return a.id < b.id;
               } );

    return mods;
}

}  // namespace mml
