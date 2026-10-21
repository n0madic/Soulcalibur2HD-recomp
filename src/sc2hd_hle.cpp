/*
 * Soulcalibur II HD Online (NPUB31366, versions 01.00 and 01.01) -- title-specific HLE.
 *
 * Firmware imports the game uses that ps3recomp v0.17.0 does not implement, or implements otherwise than the game
 * needs: the *WithRevision font entry points, SBCSstoUTF8s, _sys_spu_printf_initialize, sceNpDrmIsAvailable and
 * cellSpursLFQueueDetachLv2EventQueue.
 *
 * Handlers use the generic HLE ABI (runtime/ppu/ppu_hle.cpp): up to eight integer
 * arguments r3..r10 as uint64_t, result in r3. Pointers are guest addresses.
 */

#include <cstdint>

extern "C" {
int32_t  cellFontInit(const void* config);                          /* libs/font/cellFont.c */
int32_t  cellFontFTInit(const void* config, void* lib);             /* libs/font/cellFontFT.c */
void     ps3_hle_register(uint32_t nid, const char* name, void* handler);
uint32_t vm_read32(uint64_t addr);
void     vm_write32(uint64_t addr, uint32_t val);
extern uint8_t* vm_base;
}

namespace {

constexpr uint64_t CELL_OK = 0;

uint64_t h_nop(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    return CELL_OK;
}

/* ---- fonts and text conversion: the *WithRevision entry points the SDK emits ------------- */

/* int cellFontInitializeWithRevision(uint64_t revisionFlags, CellFontConfig* config) */
uint64_t h_font_init_rev(uint64_t, uint64_t config, uint64_t, uint64_t, uint64_t, uint64_t,
                         uint64_t, uint64_t)
{
    return static_cast<uint64_t>(static_cast<int64_t>(cellFontInit(reinterpret_cast<const void*>(config))));
}

/* int cellFontInitLibraryFreeTypeWithRevision(uint64_t revisionFlags, config, CellFontLibrary** lib) */
uint64_t h_font_ft_init_rev(uint64_t, uint64_t config, uint64_t lib, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t)
{
    return static_cast<uint64_t>(static_cast<int64_t>(
        cellFontFTInit(reinterpret_cast<const void*>(config), reinterpret_cast<void*>(lib))));
}

/* int SBCSstoUTF8s(const uint8_t* src, uint32_t* srcLen, uint8_t* dst, uint32_t* dstLen, uint32_t codePage)
 * Single-byte text to UTF-8. The game feeds Latin text (ASCII / ISO-8859-1 / Windows-1252 range, code
 * page 0x1D); bytes >= 0x80 are mapped as ISO-8859-1. *srcLen returns the bytes consumed, *dstLen the
 * bytes written. With dst == NULL nothing is written and *dstLen returns the size the conversion needs
 * (the game sizes its buffer that way). The result is an L10nResult, not a CELL error code. */
uint64_t h_sbcs_to_utf8(uint64_t src, uint64_t src_len_ea, uint64_t dst, uint64_t dst_len_ea,
                        uint64_t, uint64_t, uint64_t, uint64_t)
{
    constexpr uint64_t L10N_CONVERSION_OK = 0, L10N_SRC_ILLEGAL = 1, L10N_DST_EXHAUSTED = 2;   /* L10nResult */
    if (!src || !src_len_ea || !dst_len_ea) return L10N_SRC_ILLEGAL;
    const uint32_t in_len  = vm_read32(src_len_ea);
    const uint32_t out_cap = vm_read32(dst_len_ea);
    uint32_t in = 0, out = 0;
    uint64_t result = L10N_CONVERSION_OK;
    while (in < in_len) {
        const uint8_t c = vm_base[src + in];
        const uint32_t need = c < 0x80 ? 1 : 2;
        if (dst && out + need > out_cap) { result = L10N_DST_EXHAUSTED; break; }
        if (dst) {
            if (c < 0x80) {
                vm_base[dst + out] = c;
            } else {
                vm_base[dst + out]     = static_cast<uint8_t>(0xC0 | (c >> 6));
                vm_base[dst + out + 1] = static_cast<uint8_t>(0x80 | (c & 0x3F));
            }
        }
        out += need;
        in++;
    }
    vm_write32(src_len_ea, in);
    vm_write32(dst_len_ea, out);
    return result;
}

}  // namespace

/* NIDs are SHA-1(name + suffix) as in tools/nid_database.py; they are written
 * out here so the table does not depend on the generator. */
extern "C" void sc2hd_sail_register(void);   /* sail_hle.cpp */
extern "C" void sc2hd_lfqueue_register(void);   /* spurs_lfqueue.cpp */

extern "C" void sc2hd_hle_register(void)
{
    struct E { uint32_t nid; const char* name;
               uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t); };
    static const E table[] = {
        { 0xF03DCC29u, "cellFontInitializeWithRevision",            h_font_init_rev },
        { 0x7A0A83C4u, "cellFontInitLibraryFreeTypeWithRevision",   h_font_ft_init_rev },
        { 0x72632E53u, "SBCSstoUTF8s",                              h_sbcs_to_utf8 },
        /* sysPrxForUser: registers the SPU printf callbacks; nothing prints. */
        { 0x45FE2FCEu, "_sys_spu_printf_initialize",                h_nop },
        /* sceNp: the content is served already decrypted, see README. */
        { 0xAD218FAFu, "sceNpDrmIsAvailable",                       h_nop },
        /* The queue's Lv2 event queue is HLE-only (patches/0002): nothing to detach. */
        { 0x73E06F91u, "cellSpursLFQueueDetachLv2EventQueue",       h_nop },
    };
    for (const E& e : table)
        ps3_hle_register(e.nid, e.name, reinterpret_cast<void*>(e.fn));
    sc2hd_sail_register();
    sc2hd_lfqueue_register();
}
