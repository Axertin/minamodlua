// handle.hpp - engine pointers as generation-stamped Lua userdata.
//
// A mod stashes an entity in a global, the engine destroys it on a room
// transition, and the next access is a use-after-free inside the game. So
// userdata never holds a raw pointer: it holds {slot, generation} indexing a
// host-side table, validation is an integer compare, and everything can be
// retired at once.
//
// Deliberately NOT "wrap everything in MM_WeakPtr": that is an engine allocation
// per handle plus a call per access, far too heavy for a scene traversal touching
// a thousand entities a frame. It also only covers entities and components -
// World, SpawnPoint, ycTexture and GameCamera have no weak-pointer story at all.
//
// Never validate by dereferencing, and never wrap engine calls in SEH or a signal
// handler: catching an access violation leaves the engine in an unknown state,
// which is worse than crashing.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <unordered_map>
#include <vector>

namespace mml
{

// What lives in the userdata. 8 bytes, no pointer.
struct HandleRef
{
    uint32_t slot;
    uint32_t generation;
};

class HandleTable
{
  public:
    // Returns a stable ref for `ptr`, reusing the existing slot if this pointer
    // is already known, so two lookups of the same entity compare equal.
    HandleRef acquire( void* ptr, uint32_t typeId );

    // Null if the handle is stale or was never valid. Callers must check.
    void* resolve( HandleRef ref, uint32_t typeId ) const;

    // True if the slot is live, regardless of type - backs the `.valid` property.
    bool is_live( HandleRef ref ) const;

    // Survivors in Lua become stale refs that error on next use instead of
    // dereferencing freed memory.
    void invalidate_all();

  private:
    struct Slot
    {
        void* ptr = nullptr;
        uint32_t generation = 1;  // 0 is reserved as "never valid"
        uint32_t typeId = 0;
        bool live = false;
    };

    void retire( uint32_t slot );

    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_free;

    // Pointer -> slot, so `==` works in Lua. Cleared per slot on retirement: the
    // engine reuses addresses, and the generation stamp is what makes that safe.
    std::unordered_map<void*, uint32_t> m_index;
};

// Process-wide table. Single-threaded by construction: the probe confirmed the
// render callback runs on the update thread.
HandleTable& handles();

}  // namespace mml
