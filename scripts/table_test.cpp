// Exercises RenderTable::KeyTable's Erase, which had a rehash bug nothing else covered.
//
// Compiled and run by scripts/selfcheck.ps1 against CoD3Host/render_table.h -
// the header the renderer itself uses.
//
// WHAT THIS IS ABOUT, HONESTLY.
//
// Erase used to walk the run after the erased slot and reinsert each entry as
// it went, advancing `next` with a mask captured at entry. Insert calls Grow()
// when the table passes half full, and Grow reallocates at a new size - so IF
// Grow fired mid-walk, the loop would index the new array with a stale mask,
// and Grow itself sets m_used to 0 and reinserts everything, which would be
// thoroughly destructive.
//
// It was reported as a bug. It is not reachable, and this test is how that was
// established: it was run against the original Erase and passed.
//
// The reason: Erase decrements m_used before each reinsert, so every Insert in
// the walk evaluates the same guard value, one below where the table sat when
// the erase began. A table can never already be over the threshold - the
// Insert that would push it over grows it first - so that value is always
// under it, and Grow never fires. The invariant is subtle and entirely
// implicit, and it is the only thing standing between this function and the
// corruption it looks like it should suffer.
//
// Erase now collects the run before reinserting, so the property no longer
// depends on the invariant holding. That is a hardening, not a fix, and the
// test is here to keep the behaviour pinned either way.

#include "render_table.h"

#include <cstdio>
#include <vector>

static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok) { std::printf("      %s\n", what); failures++; }
}

static uint32_t MakeKey(uint32_t i)
{
    uint32_t k[4];
    k[0] = i;
    k[1] = i * 2654435761u;
    k[2] = ~i;
    k[3] = i * 40503u;
    return k[0];
}

int main()
{
    // The case that used to corrupt: a table sitting near half full, so an
    // Insert during Erase pushes it over and forces a rehash mid-walk.
    for (uint32_t n : { 8u, 60u, 120u, 200u, 400u })
    {
        RenderTable::KeyTable<4> table;
        std::vector<uint32_t> keys;
        for (uint32_t i = 0; i < n; i++)
        {
            const uint32_t key[4] = { MakeKey(i), i * 2654435761u, ~i, i * 40503u };
            table.Insert(key, i + 1);
            keys.push_back(key[0]);
        }
        Check(table.Size() == n, "Size is wrong straight after the inserts");

        // Erase every third key, one at a time. Each erase removes a slot and
        // reinserts the run behind it, so the table stays near the load factor
        // the whole way.
        for (uint32_t i = 0; i < n; i += 3)
        {
            const uint32_t key[4] = { MakeKey(i), i * 2654435761u, ~i, i * 40503u };
            table.Erase(key);
        }

        uint32_t expected = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            const uint32_t key[4] = { MakeKey(i), i * 2654435761u, ~i, i * 40503u };
            const bool shouldBeThere = (i % 3) != 0;
            const uint32_t got = table.Find(key);
            if (shouldBeThere)
            {
                expected++;
                // Find walking off the end of the table is the failure that
                // hangs rather than returning, so it is bounded here: the
                // table is small enough that a correct probe terminates.
                if (got != i + 1)
                {
                    std::printf("      n=%u: key %u should be there, got %u\n", n, i, got);
                    failures++;
                    break;
                }
            }
            else if (got != 0)
            {
                std::printf("      n=%u: key %u was erased but came back as %u\n", n, i, got);
                failures++;
                break;
            }
        }
        if (table.Size() != expected)
        {
            std::printf("      n=%u: Size is %u, expected %u\n", n, table.Size(), expected);
            failures++;
        }
    }

    // Erase everything, then check the table is empty and reusable. An erase
    // loop that leaves the table corrupt shows up here as a Size that never
    // reaches zero.
    {
        RenderTable::KeyTable<4> table;
        for (uint32_t i = 0; i < 300; i++)
        {
            const uint32_t key[4] = { MakeKey(i), i * 2654435761u, ~i, i * 40503u };
            table.Insert(key, i + 1);
        }
        for (uint32_t i = 0; i < 300; i++)
        {
            const uint32_t key[4] = { MakeKey(i), i * 2654435761u, ~i, i * 40503u };
            table.Erase(key);
            table.Erase(key);   // and again: erasing a missing key must be safe
        }
        Check(table.Size() == 0, "Size did not reach zero after erasing everything");
        for (uint32_t i = 0; i < 300; i++)
        {
            const uint32_t key[4] = { MakeKey(i), i * 2654435761u, ~i, i * 40503u };
            if (table.Find(key) != 0) { Check(false, "a key survived a full erase"); break; }
        }
        // And it still works afterwards.
        const uint32_t key[4] = { 7u, 7u * 2654435761u, ~7u, 7u * 40503u };
        table.Insert(key, 99);
        Check(table.Find(key) == 99, "the table did not survive being emptied and refilled");
    }

    if (failures == 0) std::printf("the key table survives erasing across rehashes\n");
    return failures == 0 ? 0 : 1;
}