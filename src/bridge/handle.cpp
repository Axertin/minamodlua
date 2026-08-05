#include "handle.hpp"

namespace mml
{

HandleRef HandleTable::acquire( void* ptr, uint32_t typeId )
{
    if ( !ptr ) return { 0, 0 };  // generation 0 never validates

    // Same pointer, same slot - so two lookups of one entity are `==` in Lua and
    // work as the same table key.
    if ( auto it = m_index.find( ptr ); it != m_index.end() )
    {
        const uint32_t slot = it->second;
        if ( slot < m_slots.size() && m_slots[slot].live && m_slots[slot].typeId == typeId )
            return { slot, m_slots[slot].generation };
    }

    uint32_t slot;
    if ( !m_free.empty() )
    {
        slot = m_free.back();
        m_free.pop_back();
    }
    else
    {
        slot = (uint32_t)m_slots.size();
        m_slots.emplace_back();
    }

    Slot& s = m_slots[slot];
    s.ptr = ptr;
    s.typeId = typeId;
    s.live = true;
    // s.generation carries over from the previous occupant, already bumped by
    // retire(), so any surviving ref to that occupant stays stale.

    m_index[ptr] = slot;
    return { slot, s.generation };
}

void* HandleTable::resolve( HandleRef ref, uint32_t typeId ) const
{
    if ( ref.generation == 0 || ref.slot >= m_slots.size() ) return nullptr;

    const Slot& s = m_slots[ref.slot];
    if ( !s.live || s.generation != ref.generation || s.typeId != typeId ) return nullptr;
    return s.ptr;
}

bool HandleTable::is_live( HandleRef ref ) const
{
    if ( ref.generation == 0 || ref.slot >= m_slots.size() ) return false;
    const Slot& s = m_slots[ref.slot];
    return s.live && s.generation == ref.generation;
}

void HandleTable::retire( uint32_t slot )
{
    Slot& s = m_slots[slot];
    if ( !s.live ) return;

    if ( auto it = m_index.find( s.ptr ); it != m_index.end() && it->second == slot ) m_index.erase( it );

    s.ptr = nullptr;
    s.live = false;

    // Bumped on retirement, not on reuse, so a stale ref is detectable
    // immediately. 0 stays reserved as "never valid".
    if ( ++s.generation == 0 ) s.generation = 1;

    m_free.push_back( slot );
}

void HandleTable::invalidate_all()
{
    for ( uint32_t i = 0; i < (uint32_t)m_slots.size(); ++i )
        if ( m_slots[i].live ) retire( i );
}

HandleTable& handles()
{
    static HandleTable table;
    return table;
}

}  // namespace mml
