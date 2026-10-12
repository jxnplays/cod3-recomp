#pragma once

// A table from a key of a few words to an integer handle, for the caches:
// open addressing over a power of two, the keys kept whole so a hit is a
// real match, grown by doubling when half full. Handle 0 is "none" and is
// never stored, so an empty slot is one whose handle is 0. Nothing here
// allocates on a hit, and a miss allocates only when the table grows.

#include <cstdint>
#include <cstring>
#include <vector>

namespace RenderTable
{
    template <int KeyWords>
    class KeyTable
    {
    public:
        KeyTable() { Reset(256); }

        // The handle for the key, or 0.
        uint32_t Find(const uint32_t* key) const
        {
            const size_t mask = m_slots.size() - 1;
            size_t slot = HashOf(key) & mask;
            for (;;)
            {
                const Slot& entry = m_slots[slot];
                if (entry.handle == 0) return 0;
                if (memcmp(entry.key, key, sizeof(entry.key)) == 0) return entry.handle;
                slot = (slot + 1) & mask;
            }
        }

        // Puts the key in under the handle. The key must not be in already.
        void Insert(const uint32_t* key, uint32_t handle)
        {
            if ((m_used + 1) * 2 > m_slots.size()) Grow();
            const size_t mask = m_slots.size() - 1;
            size_t slot = HashOf(key) & mask;
            while (m_slots[slot].handle != 0) slot = (slot + 1) & mask;
            memcpy(m_slots[slot].key, key, sizeof(m_slots[slot].key));
            m_slots[slot].handle = handle;
            m_used++;
        }

        // Takes the key out, if it is in: the slot is tombstoned by
        // rebuilding the run after it, which keeps Find's early exit true.
        void Erase(const uint32_t* key)
        {
            const size_t mask = m_slots.size() - 1;
            size_t slot = HashOf(key) & mask;
            for (;;)
            {
                if (m_slots[slot].handle == 0) return;
                if (memcmp(m_slots[slot].key, key, sizeof(m_slots[slot].key)) == 0) break;
                slot = (slot + 1) & mask;
            }
            m_slots[slot].handle = 0;
            m_used--;
            // Reinsert what follows in the run, so nothing is cut off.
            size_t next = (slot + 1) & mask;
            while (m_slots[next].handle != 0)
            {
                const Slot moved = m_slots[next];
                m_slots[next].handle = 0;
                m_used--;
                Insert(moved.key, moved.handle);
                next = (next + 1) & mask;
            }
        }

        void Clear() { Reset(m_slots.size()); }
        uint32_t Size() const { return m_used; }

    private:
        struct Slot { uint32_t key[KeyWords]; uint32_t handle; };
        std::vector<Slot> m_slots;
        uint32_t m_used = 0;

        static uint32_t HashOf(const uint32_t* key)
        {
            uint64_t hash = 1469598103934665603ull;
            for (int i = 0; i < KeyWords; i++) { hash ^= key[i]; hash *= 1099511628211ull; }
            return uint32_t(hash ^ (hash >> 32));
        }

        void Reset(size_t size)
        {
            m_slots.assign(size, Slot{});
            m_used = 0;
        }

        void Grow()
        {
            std::vector<Slot> old;
            old.swap(m_slots);
            m_slots.assign(old.size() * 2, Slot{});
            m_used = 0;
            for (const Slot& slot : old)
                if (slot.handle != 0) Insert(slot.key, slot.handle);
        }
    };
}
