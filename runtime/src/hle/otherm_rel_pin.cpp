// Pins game.rel to its pre-linked address.
//
// main.dol's loader (0x80006E58) reads files/rel/Release/game.rel into memory taken from the bottom of the MEM1
// arena with OSAllocFromMEM1ArenaLo(round32(file length), 0x20), then OSLinkFixed places the BSS right after the
// module's fixed part and hands the rest back with OSSetArenaLo (0x80006F80). The translation of game.rel
// (project/tools/prelink_otherm_rel.py) is linked for one image address, so this override of
// OSAllocFromMEM1ArenaLo raises the arena's low end to that address first when the REL's allocation would land
// below it. Every other allocation behaves exactly like the original: a bump of the arena's low end.
//
// The image address was read off the first boot that reached the REL ("[relpin] game.rel natural image
// 0x8052B280"), so the pin only confirms the layout; a mismatch here means something before the REL load changed
// (prelink at the new address and update kRelImage, recomp.yml inputs.rel.load_address and the REL entry points).
#include "hle_stubs.h"

#include "memory.h"
#include "runtime_log.h"

namespace {

constexpr uint32_t kArenaLoMem1 = 0x80331DA8u;   // __OSArenaLo (r13-0x7C38): OSGetArenaLo 0x80109E20 reads it
constexpr uint32_t kRelImage = 0x8052B280u;      // game/otherm_rel_layout.json "image": the address the game chose itself (boot 3)
constexpr uint32_t kRelAllocSize = 0x008C95C0u;  // game.rel's length rounded up to 0x20 (layout "alloc_size")

uint32_t AlignUp(uint32_t value, uint32_t align) { return (value + align - 1u) & ~(align - 1u); }

} // namespace

// 0x80109E90 OSAllocFromMEM1ArenaLo(size, align)
extern "C" uint32_t OtherM_OSAllocFromMEM1ArenaLo(uint32_t size, uint32_t align)
{
    uint32_t start = AlignUp(Memory::Read32(kArenaLoMem1), align);
    if (size == kRelAllocSize && align == 0x20u) {
        RT_LOGF(RT_TAG_HLE, "[relpin] game.rel natural image 0x%08X, pre-linked for 0x%08X\n", start, kRelImage);
        if (start < kRelImage) {
            start = kRelImage;  // the gap below stays unused, as if the arena had started higher
        } else if (start > kRelImage) {
            RT_LOGF(RT_TAG_HLE, "[relpin] MISMATCH: the arena is already above the pre-linked image; "
                                "translated game.rel code will run against the wrong addresses\n");
        }
    }
    Memory::Write32(kArenaLoMem1, AlignUp(start + size, align));
    return start;
}

PPC_NATIVE_OVERRIDE(80109E90, OtherM_OSAllocFromMEM1ArenaLo, uint32_t, (uint32_t size, uint32_t align),
                    (size, align));
