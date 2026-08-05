// log.hpp - diagnostics for mod authors.
//
// The game's mod.log is append-only across launches, so results from three
// sessions ago sit above the current ones. The host therefore owns a file it
// truncates per run and writes to both.
//
// Two things about MinaModAPI::Log that the bindings must also honour: it is
// printf-style, so a runtime string must never be passed as the format (a stray
// %s in mod-supplied text reads garbage off the stack), and it appends no
// newline.

#pragma once

#include "MinaModAPI.h"

namespace mml::log
{

// `mm` may be null; logging then goes to the host file only.
void init( MinaModAPI* mm );
void shutdown();

void write( const char* fmt, ... );

}  // namespace mml::log
