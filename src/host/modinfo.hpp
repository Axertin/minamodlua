#pragma once

#include <stdint.h>

#include <filesystem>
#include <string>
#include <vector>

namespace mml
{

// One mod folder: mods/<id>/ holding a mod.yc manifest and a main.lua.
struct ModInfo
{
    std::string id;
    std::string name;
    uint32_t modVersion = 1;
    uint32_t minGameVersion = 0;  // 0 means unconstrained
    uint32_t maxGameVersion = 999999;
    int32_t loadPriority = 0;
    std::filesystem::path dir;
};

// Parses the fields we care about out of a mod.yc. The format is the game's own
// "YCD" text, and only a handful of scalar keys matter here, so this reads
// `key: value` pairs and ignores structure rather than pretending to be a
// parser for the whole format.
bool parse_manifest( const std::filesystem::path& file, ModInfo& out, std::string& error );

// Every folder under `modsDir` holding both a mod.yc and a main.lua, filtered
// against `gameRevision` and ordered by loadPriority (descending, matching the
// engine's own hook priority convention), then by id for stability.
//
// The scan is our own rather than a list from the game: a Lua mod ships no
// binary, and nothing in the SDK reports which folders the game registered.
std::vector<ModInfo> discover_mods( const std::filesystem::path& modsDir, uint32_t gameRevision );

}  // namespace mml
