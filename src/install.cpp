// Installs PS3 content packages into a game folder: unpacks a retail .pkg and decrypts what is licensed.
//
// Soulcalibur II HD port for ps3recomp. License: GPL-2.0-or-later.
//
// Replaces the Python tools ps3_pkg_extract.py and ps3_decrypt.py (and so their dependency on pycryptodome). The NPDRM
// algorithms and key tables are ported from RPCS3 (rpcs3/Crypto/unself.cpp, unedat.cpp, key_vault.cpp; GPL-2.0-or-later).
//
//   sc2hd install --rap <license.rap> [--to <dir>] <game.pkg> [<update.pkg> ...]
//   sc2hd selftest
//
// Each package is unpacked to <dir>/game/<TITLE ID>/. USRDIR/EBOOT.BIN comes out as USRDIR/EBOOT.elf and every .EDAT is
// decrypted under its own name (the runtime serves the decrypted archive in place of the licensed one). Packages are
// installed in the order given, so an update goes after the game it patches.
#include "crypto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  define isatty _isatty
#  define fileno _fileno
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sc2hd {
namespace {

using Bytes = std::vector<uint8_t>;

struct InstallError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// --- key material (RPCS3 key_vault.h / key_vault.cpp) ----------------------------------------------------------------

const Bytes PKG_AES_KEY = hex("2E7B71D7C9C9A14EA3221F188828B8F8");
const Bytes RAP_KEY = hex("869F7745C13FD890CCF29188E3CC3EDF");
const Bytes RAP_PBOX = hex("0C030604010B0F08020700050A0E0D09");
const Bytes RAP_E1 = hex("A93E1FD67C55A329B75FDDA62A95C7A5");
const Bytes RAP_E2 = hex("67D45DA3296D006A4E7C537BF5538C74");
const Bytes NP_KLIC_KEY = hex("F2FBCA7A75B04EDC1390638CCDFDD1EE");
const Bytes NP_KLIC_FREE = hex("72F990788F9CFF745725F08E4C128387");
const Bytes EDAT_KEY_0 = hex("BE959CA8308DEFA2E5E180C63712A9AE");
const Bytes EDAT_HASH_0 = hex("EFFE5BD1652EEBC11918CF7C04D4F011");
const Bytes EDAT_KEY_1 = hex("4CA9C14B01C95309969BEC68AA0BC081");
const Bytes EDAT_HASH_1 = hex("3D92699B705B073854D8FCC6C7672747");
const Bytes ZERO16(16, 0);

struct SelfKey {
    uint16_t revision;
    const char* erk;   // AES-256
    const char* riv;
};
const SelfKey NPDRM_SELF_KEYS[] = {
    {0x0001, "F9EDD0301F770FABBA8863D9897F0FEA6551B09431F61312654E28F43533EA6B", "A551CCB4A42C37A734A2B4F9657D5540"},
    {0x0002, "8E737230C80E66AD0162EDDD32F1F774EE5E4E187449F19079437A508FCF9C86", "7AAECC60AD12AED90C348D8C11D2BED5"},
    {0x0003, "1B715B0C3E8DC4C1A5772EBA9C5D34F7CCFE5B82025D453F3167566497239664", "E31E206FBB8AEA27FAB0D9A2FFB6B62F"},
    {0x0004, "BB4DBF66B744A33934172D9F8379A7A5EA74CB0F559BB95D0E7AECE91702B706", "ADF7B207A15AC601110E61DDFC210AF6"},
    {0x0006, "8B4C52849765D2B5FA3D5628AFB17644D52B9FFEE235B4C0DB72A62867EAA020", "05719DF1B1D0306C03910ADDCE4AF887"},
    {0x0007, "3946DFAA141718C7BE339A0D6C26301C76B568AEBC5CD52652F2E2E0297437C3", "E4897BE553AE025CDCBF2B15D1C9234E"},
    {0x0009, "0786F4B0CA5937F515BDCE188F569B2EF3109A4DA0780A7AA07BD89C3350810A", "04AD3C2F122A3B35E804850CAD142C6D"},
    {0x000A, "03C21AD78FBB6A3D425E9AAB1298F9FD70E29FD4E6E3A3C151205DA50C413DE4", "0A99D4D4F8301A88052D714AD2FB565E"},
    {0x000C, "357EBBEA265FAEC271182D571C6CD2F62CFA04D325588F213DB6B2E0ED166D92", "D26E6DD2B74CD78E866E742E5571B84F"},
    {0x000D, "337A51416105B56E40D7CAF1B954CDAF4E7645F28379904F35F27E81CA7B6957", "8405C88E042280DBD794EC7E22B74002"},
    {0x000F, "135C098CBE6A3E037EBE9F2BB9B30218DDE8D68217346F9AD33203352FBB3291", "4070C898C2EAAD1634A288AA547A35A8"},
    {0x0010, "4B3CD10F6A6AA7D99F9B3A660C35ADE08EF01C2C336B9E46D1BB5678B4261A61", "C0F2AB86E6E0457552DB50D7219371C5"},
    {0x0013, "265C93CF48562EC5D18773BEB7689B8AD10C5EB6D21421455DEBC4FB128CBF46", "8DEA5FF959682A9B98B688CEA1EF4A1D"},
    {0x0016, "7910340483E419E55F0D33E4EA5410EEEC3AF47814667ECA2AA9D75602B14D4B", "4AD981431B98DFD39B6388EDAD742A8E"},
    {0x0019, "FBDA75963FE690CFF35B7AA7B408CF631744EDEF5F7931A04D58FD6A921FFDB3", "F72C1D80FFDA2E3BF085F4133E6D2805"},
    {0x001C, "8103EA9DB790578219C4CEDF0592B43064A7D98B601B6C7BC45108C4047AA80F", "246F4B8328BE6A2D394EDE20479247C5"},
};

// --- layout constants ------------------------------------------------------------------------------------------------
constexpr uint32_t SELF_FLAG_DEBUG = 0x8000;
constexpr size_t SCE_HDR_SIZE = 0x20, EHDR64_SIZE = 0x40, PHDR64_SIZE = 0x38, SHDR64_SIZE = 0x40, NPD_SIZE = 0x80;
constexpr size_t META_INFO_SIZE = 0x40, META_HDR_SIZE = 0x20, META_SHDR_SIZE = 0x30;
constexpr uint32_t SELF_TYPE_NPDRM = 8, SUPP_HDR_NPDRM = 3, SECTION_TYPE_PHDR = 2, SECTION_ENCRYPTED = 3, SECTION_ZLIB = 2;
constexpr int LICENSE_NETWORK = 1, LICENSE_LOCAL = 2, LICENSE_FREE = 3;

constexpr uint32_t EDAT_COMPRESSED_FLAG = 0x1, EDAT_FLAG_0x02 = 0x2, EDAT_ENCRYPTED_KEY_FLAG = 0x8, EDAT_FLAG_0x10 = 0x10,
                   EDAT_FLAG_0x20 = 0x20, EDAT_SDAT_FLAG = 0x01000000, EDAT_DEBUG_DATA_FLAG = 0x80000000;
constexpr uint64_t EDAT_METADATA_OFFSET = 0x100;
constexpr uint32_t MODE_KEY_ENCRYPTED = 0x10000000, MODE_KEY_DEFAULT = 0x20000000;

// --- big-endian readers ----------------------------------------------------------------------------------------------
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return load_be32(p); }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

void need(const Bytes& b, uint64_t off, uint64_t n, const char* what)
{
    if (off > b.size() || n > b.size() - off) throw InstallError(std::string("truncated ") + what);
}

// --- random-access sources -------------------------------------------------------------------------------------------
struct Source {
    virtual ~Source() = default;
    // Reads exactly n bytes at off, or fewer at the end of the data (the return value is the count).
    virtual size_t read(uint64_t off, uint8_t* dst, size_t n) = 0;
};

class FileSource : public Source {
public:
    explicit FileSource(const fs::path& p) : f_(p, std::ios::binary)
    {
        if (!f_) throw InstallError("cannot open " + p.string());
    }
    size_t read(uint64_t off, uint8_t* dst, size_t n) override
    {
        f_.clear();
        f_.seekg((std::streamoff)off);
        f_.read((char*)dst, (std::streamsize)n);
        return (size_t)f_.gcount();
    }

private:
    std::ifstream f_;
};

// The decrypted data area of a retail PKG (AES-128-CTR, counter = IV + 16-byte block index).
class PkgData : public Source {
public:
    PkgData(Source& file, uint64_t data_off, const uint8_t iv[16]) : file_(file), data_off_(data_off) { memcpy(iv_, iv, 16); }
    size_t read(uint64_t rel, uint8_t* dst, size_t n) override
    {
        uint64_t first = rel / 16;
        size_t skip = (size_t)(rel - first * 16);
        Bytes raw(skip + n);
        size_t got = file_.read(data_off_ + first * 16, raw.data(), raw.size());
        if (got <= skip) return 0;
        uint8_t ctr[16];
        memcpy(ctr, iv_, 16);
        uint64_t carry = first;   // 128-bit big-endian add
        for (int i = 15; i >= 0 && carry; i--) {
            uint64_t v = ctr[i] + (carry & 0xFF);
            ctr[i] = (uint8_t)v;
            carry = (carry >> 8) + (v >> 8);
        }
        aes_ctr(PKG_AES_KEY.data(), 16, ctr, raw.data(), raw.data(), got);
        memcpy(dst, raw.data() + skip, got - skip);
        return got - skip;
    }

private:
    Source& file_;
    uint64_t data_off_;
    uint8_t iv_[16];
};

// A window of another source: one file of a package.
class Window : public Source {
public:
    Window(Source& s, uint64_t off, uint64_t size) : s_(s), off_(off), size_(size) {}
    size_t read(uint64_t off, uint8_t* dst, size_t n) override
    {
        if (off >= size_) return 0;
        if (n > size_ - off) n = (size_t)(size_ - off);
        return s_.read(off_ + off, dst, n);
    }
    uint64_t size() const { return size_; }

private:
    Source& s_;
    uint64_t off_, size_;
};

Bytes read_all(Source& s, uint64_t off, size_t n)
{
    Bytes b(n);
    if (s.read(off, b.data(), n) != n) throw InstallError("unexpected end of data");
    return b;
}

// --- RAP -> RIF ------------------------------------------------------------------------------------------------------
Bytes rap_to_rif(const Bytes& rap)
{
    if (rap.size() != 16) throw InstallError("a RAP file is exactly 16 bytes");
    Bytes key(16);
    aes_cbc_decrypt(RAP_KEY.data(), 16, ZERO16.data(), rap.data(), key.data(), 16);
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 16; i++) {
            int p = RAP_PBOX[i];
            key[p] ^= RAP_E1[p];
        }
        for (int i = 15; i > 0; i--) key[RAP_PBOX[i]] ^= key[RAP_PBOX[i - 1]];
        int borrow = 0;
        for (int i = 0; i < 16; i++) {
            int p = RAP_PBOX[i];
            int kc = (key[p] - borrow) & 0xFF;
            if (borrow != 1 || kc != 0xFF) borrow = kc < RAP_E2[p] ? 1 : 0;
            key[p] = (uint8_t)((kc - RAP_E2[p]) & 0xFF);
        }
    }
    return key;
}

// --- NPDRM header ----------------------------------------------------------------------------------------------------
struct Npd {
    int32_t version, license, type;
    std::string content_id;
    uint8_t digest[16], title_hash[16], dev_hash[16];
};

Npd parse_npd(const uint8_t* raw)
{
    if (memcmp(raw, "NPD\0", 4) != 0) throw InstallError("NPD header not found");
    Npd n;
    n.version = (int32_t)be32(raw + 4);
    n.license = (int32_t)be32(raw + 8);
    n.type = (int32_t)be32(raw + 12);
    n.content_id.assign((const char*)raw + 16, strnlen((const char*)raw + 16, 48));
    memcpy(n.digest, raw + 64, 16);
    memcpy(n.title_hash, raw + 80, 16);
    memcpy(n.dev_hash, raw + 96, 16);
    return n;
}

// --- SELF ------------------------------------------------------------------------------------------------------------
// The ELF is laid out by offsets from the (untrusted) headers; none may reach past this.
constexpr uint64_t MAX_ELF_SIZE = uint64_t(1) << 30;

void write_at(Bytes& buf, uint64_t off, const uint8_t* data, size_t n)
{
    if (off > MAX_ELF_SIZE || n > MAX_ELF_SIZE - off) throw InstallError("ELF layout out of range");
    if (buf.size() < off + n) buf.resize(off + n, 0);
    memcpy(buf.data() + off, data, n);
}

// The plain ELF of an NPDRM SELF.
Bytes decrypt_self(const Bytes& blob, const Bytes& rif)
{
    need(blob, 0, 0x70, "SELF header");
    if (memcmp(blob.data(), "SCE\0", 4) != 0) throw InstallError("not a SELF");
    uint16_t flags = be16(blob.data() + 8);
    uint32_t meta_off = be32(blob.data() + 12);
    uint64_t hdr_size = be64(blob.data() + 16);
    if (flags == 0x80 || flags == 0xC0 || (flags & SELF_FLAG_DEBUG)) throw InstallError("debug SELFs are not supported");

    const uint8_t* ext = blob.data() + 0x20;   // ten 64-bit offsets
    uint64_t prog_id_off = be64(ext + 8), ehdr_off = be64(ext + 16), phdr_off = be64(ext + 24), shdr_off = be64(ext + 32),
             supp_off = be64(ext + 56), supp_size = be64(ext + 64);
    need(blob, prog_id_off, 24, "program id");
    uint32_t program_type = be32(blob.data() + prog_id_off + 12);
    if (program_type != SELF_TYPE_NPDRM) throw InstallError("only NPDRM SELFs are supported");

    need(blob, ehdr_off, EHDR64_SIZE, "ELF header");
    const uint8_t* ehdr = blob.data() + ehdr_off;
    if (ehdr[4] != 2) throw InstallError("only 64-bit SELFs are supported");
    uint64_t e_phoff = be64(ehdr + 0x20), e_shoff = be64(ehdr + 0x28);
    unsigned e_phnum = be16(ehdr + 0x38), e_shnum = be16(ehdr + 0x3C);
    need(blob, phdr_off, e_phnum * PHDR64_SIZE, "program headers");
    const uint8_t* phdrs = blob.data() + phdr_off;

    uint64_t info_off = (uint64_t)meta_off + SCE_HDR_SIZE;
    need(blob, info_off, META_INFO_SIZE, "metadata info");
    uint8_t raw_info[META_INFO_SIZE];
    memcpy(raw_info, blob.data() + info_off, META_INFO_SIZE);

    // NPDRM control info among the supplemental headers.
    bool have_npd = false;
    Npd npd{};
    for (uint64_t pos = supp_off; pos < supp_off + supp_size;) {
        need(blob, pos, 16, "supplemental header");
        uint32_t typ = be32(blob.data() + pos), size = be32(blob.data() + pos + 4);
        if (size == 0) throw InstallError("corrupt supplemental header");
        if (typ == SUPP_HDR_NPDRM) {
            need(blob, pos + 16, NPD_SIZE, "NPD");
            npd = parse_npd(blob.data() + pos + 16);
            have_npd = true;
        }
        pos += size;
    }
    if (!have_npd) throw InstallError("no NPDRM control info in a SELF of NPDRM type");

    const SelfKey* key = nullptr;
    for (const SelfKey& k : NPDRM_SELF_KEYS)
        if (k.revision == flags) key = &k;
    if (!key) throw InstallError("no NPDRM SELF key for this revision");
    Bytes erk = hex(key->erk), riv = hex(key->riv);

    // The klicensee (the RIF key, or the free one) decrypted with NP_KLIC_KEY becomes the AES-128 key of the metadata.
    const Bytes* layer;
    if (npd.license == LICENSE_NETWORK || npd.license == LICENSE_LOCAL) layer = &rif;
    else if (npd.license == LICENSE_FREE) layer = &NP_KLIC_FREE;
    else throw InstallError("invalid NPDRM license type");
    uint8_t layer_key[16];
    aes_ecb_decrypt(NP_KLIC_KEY.data(), layer->data(), layer_key);
    aes_cbc_decrypt(layer_key, 16, ZERO16.data(), raw_info, raw_info, META_INFO_SIZE);

    uint8_t info[META_INFO_SIZE];
    aes_cbc_decrypt(erk.data(), 32, riv.data(), raw_info, info, META_INFO_SIZE);
    const uint8_t *meta_key = info, *key_pad = info + 16, *meta_iv = info + 32, *iv_pad = info + 48;
    if (key_pad[0] || iv_pad[0]) throw InstallError("metadata info did not decrypt (wrong RAP for this package?)");

    uint64_t enc_start = info_off + META_INFO_SIZE;
    if (hdr_size < enc_start) throw InstallError("corrupt SELF header size");
    need(blob, enc_start, hdr_size - enc_start, "metadata headers");
    Bytes headers(hdr_size - enc_start);
    aes_ctr(meta_key, 16, meta_iv, blob.data() + enc_start, headers.data(), headers.size());
    need(headers, 0, META_HDR_SIZE, "metadata header");

    uint32_t section_count = be32(headers.data() + 12), key_count = be32(headers.data() + 16);
    uint64_t keys_off = META_HDR_SIZE + (uint64_t)section_count * META_SHDR_SIZE;
    need(headers, keys_off, (uint64_t)key_count * 16, "data keys");
    const uint8_t* data_keys = headers.data() + keys_off;

    Bytes out(ehdr, ehdr + EHDR64_SIZE);
    write_at(out, e_phoff, phdrs, e_phnum * PHDR64_SIZE);
    for (uint32_t i = 0; i < section_count; i++) {
        need(headers, META_HDR_SIZE + (uint64_t)i * META_SHDR_SIZE, META_SHDR_SIZE, "section header");
        const uint8_t* s = headers.data() + META_HDR_SIZE + (size_t)i * META_SHDR_SIZE;
        uint64_t data_off = be64(s), data_size = be64(s + 8);
        uint32_t typ = be32(s + 16), prog_idx = be32(s + 20), encrypted = be32(s + 32), key_idx = be32(s + 36),
                 iv_idx = be32(s + 40), compressed = be32(s + 44);
        need(blob, data_off, data_size, "section data");
        Bytes raw(blob.begin() + data_off, blob.begin() + data_off + data_size);
        if (encrypted == SECTION_ENCRYPTED) {
            if (key_idx >= key_count || iv_idx >= key_count) throw InstallError("section key/iv index out of range");
            aes_ctr(data_keys + key_idx * 16, 16, data_keys + iv_idx * 16, raw.data(), raw.data(), raw.size());
        }
        if (typ != SECTION_TYPE_PHDR) continue;
        if (prog_idx >= e_phnum) throw InstallError("section refers to a missing program header");
        uint64_t p_offset = be64(phdrs + prog_idx * PHDR64_SIZE + 8);
        if (compressed == SECTION_ZLIB) raw = zlib_inflate(raw.data(), raw.size());
        write_at(out, p_offset, raw.data(), raw.size());
    }
    if (shdr_off) {
        need(blob, shdr_off, (uint64_t)e_shnum * SHDR64_SIZE, "section headers");
        write_at(out, e_shoff, blob.data() + shdr_off, e_shnum * SHDR64_SIZE);
    }
    if (out.size() < 4 || memcmp(out.data(), "\x7f" "ELF", 4) != 0) throw InstallError("result is not an ELF");
    return out;
}

// --- EDAT ------------------------------------------------------------------------------------------------------------
void edat_key_iv(uint32_t crypto_mode, int version, const uint8_t key[16], const uint8_t iv[16], uint8_t key_out[16],
                 uint8_t iv_out[16])
{
    const Bytes& base = version ? EDAT_KEY_1 : EDAT_KEY_0;
    uint32_t mode = crypto_mode & 0xF0000000;
    if (mode == MODE_KEY_ENCRYPTED) {
        aes_cbc_decrypt(base.data(), 16, ZERO16.data(), key, key_out, 16);
        memcpy(iv_out, iv, 16);
    } else if (mode == MODE_KEY_DEFAULT) {
        memcpy(key_out, base.data(), 16);
        memset(iv_out, 0, 16);
    } else {
        memcpy(key_out, key, 16);
        memcpy(iv_out, iv, 16);
    }
}

void edat_hash_key(uint32_t hash_mode, int version, const uint8_t hash16[16], uint8_t out[16])
{
    uint32_t mode = hash_mode & 0xF0000000;
    if (mode == MODE_KEY_ENCRYPTED) {
        aes_cbc_decrypt((version ? EDAT_KEY_1 : EDAT_KEY_0).data(), 16, ZERO16.data(), hash16, out, 16);
    } else if (mode == MODE_KEY_DEFAULT) {
        memcpy(out, (version ? EDAT_HASH_1 : EDAT_HASH_0).data(), 16);
    } else {
        memcpy(out, hash16, 16);
    }
}

bool edat_hash_ok(uint32_t hash_mode, const uint8_t key[16], const uint8_t* data, size_t len, const uint8_t* expected)
{
    uint8_t d[20];
    switch (hash_mode & 0xFF) {
        case 0x01: {   // HMAC-SHA1 over a 20-byte key (16 bytes + zero padding)
            uint8_t k[20] = {};
            memcpy(k, key, 16);
            hmac_sha1(k, 20, data, len, d);
            return memcmp(d, expected, 20) == 0;
        }
        case 0x02:   // AES-CMAC
            aes_cmac(key, data, len, d);
            return memcmp(d, expected, 16) == 0;
        case 0x04:   // HMAC-SHA1 over a 16-byte key, 16 bytes compared
            hmac_sha1(key, 16, data, len, d);
            return memcmp(d, expected, 16) == 0;
    }
    throw InstallError("unknown EDAT hashing algorithm");
}

// Decrypts an EDAT block by block into `out` (RPCS3's decrypt_block for the uncompressed layouts).
void decrypt_edat(Source& src, std::ostream& out, const Bytes& rif, const std::string& what)
{
    Bytes head = read_all(src, 0, 0x90);
    Npd npd = parse_npd(head.data());
    uint32_t flags = be32(head.data() + 0x80), block_size = be32(head.data() + 0x84);
    uint64_t file_size = be64(head.data() + 0x88);
    if (flags & (EDAT_SDAT_FLAG | EDAT_COMPRESSED_FLAG | EDAT_DEBUG_DATA_FLAG))
        throw InstallError(what + ": SDAT, compressed and debug EDATs are not supported");   // TODO: LZ decoder (rpcs3 lz.cpp)
    if ((npd.license & 3) == LICENSE_FREE) throw InstallError(what + ": free-license EDATs need their klicensee, not a RAP");
    if (block_size == 0 || block_size > (1u << 24)) throw InstallError(what + ": bad EDAT block size");

    uint64_t total_blocks = (file_size + block_size - 1) / block_size;
    size_t meta_size = (flags & (EDAT_COMPRESSED_FLAG | EDAT_FLAG_0x20)) ? 0x20 : 0x10;
    int version = npd.version == 4 ? 1 : 0;
    Bytes enc;
    uint64_t next_report = 0;
    const bool tty = isatty(fileno(stderr)) != 0;
    for (uint64_t block = 0; block < total_blocks; block++) {
        uint8_t expected[0x20];
        uint64_t data_off;
        if (flags & EDAT_FLAG_0x20) {   // metadata precedes every data block
            uint64_t meta_off = EDAT_METADATA_OFFSET + block * (meta_size + block_size);
            if (src.read(meta_off, expected, 0x20) != 0x20) throw InstallError(what + ": truncated EDAT");
            for (int j = 0; j < 0x10; j++) expected[j] ^= expected[j + 0x10];
            // bytes 0x10..0x14 of the metadata stay as they are, right behind the xor'd 16
            data_off = meta_off + 0x20;
        } else {
            if (src.read(EDAT_METADATA_OFFSET + block * meta_size, expected, 0x10) != 0x10) throw InstallError(what + ": truncated EDAT");
            data_off = EDAT_METADATA_OFFSET + block * block_size + total_blocks * meta_size;
        }
        size_t length = block_size;
        if (block == total_blocks - 1 && file_size % block_size) length = (size_t)(file_size % block_size);
        size_t padded = (length + 15) & ~(size_t)15;
        enc.assign(padded, 0);
        src.read(data_off, enc.data(), padded);   // a short last block is zero-padded

        uint8_t seed[16] = {};
        if (npd.version > 1) memcpy(seed, npd.dev_hash, 12);
        store_be32(seed + 12, (uint32_t)block);
        uint8_t block_key[16], hash_key16[16];
        aes_ecb_encrypt(rif.data(), seed, block_key);
        if (flags & EDAT_FLAG_0x10) aes_ecb_encrypt(rif.data(), block_key, hash_key16);
        else memcpy(hash_key16, block_key, 16);

        uint32_t crypto_mode = (flags & EDAT_FLAG_0x02) ? 0x1 : 0x2;
        uint32_t hash_mode = !(flags & EDAT_FLAG_0x10) ? 0x02 : !(flags & EDAT_FLAG_0x20) ? 0x04 : 0x01;
        if (flags & EDAT_ENCRYPTED_KEY_FLAG) {
            crypto_mode |= MODE_KEY_ENCRYPTED;
            hash_mode |= MODE_KEY_ENCRYPTED;
        }
        uint8_t key[16], iv[16], hkey[16];
        edat_key_iv(crypto_mode, version, block_key, npd.version <= 1 ? ZERO16.data() : npd.digest, key, iv);
        edat_hash_key(hash_mode, version, hash_key16, hkey);
        if (!edat_hash_ok(hash_mode, hkey, enc.data(), padded, expected))
            throw InstallError(what + ": block " + std::to_string(block) + " has an invalid hash (wrong RAP?)");
        if ((crypto_mode & 0xFF) != 0x1) aes_cbc_decrypt(key, 16, iv, enc.data(), enc.data(), padded);
        out.write((const char*)enc.data(), (std::streamsize)length);
        if (block >= next_report) {
            // A terminal gets a live percentage; a log gets a line per quarter.
            unsigned pct = (unsigned)(block * 100 / total_blocks);
            if (tty) fprintf(stderr, "\r  %s: %3u%%", what.c_str(), pct);
            else if (pct % 25 == 0) fprintf(stderr, "  %s: %u%%\n", what.c_str(), pct);
            next_report = block + total_blocks / 100 + 1;
        }
    }
    if (tty) fprintf(stderr, "\r");
    fprintf(stderr, "  %s: done     \n", what.c_str());
}

// --- PKG -------------------------------------------------------------------------------------------------------------
bool iends_with(const std::string& s, const char* suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if ((s[s.size() - n + i] | 0x20) != (suffix[i] | 0x20)) return false;
    return true;
}

// Joins an untrusted package path onto root, refusing anything that would leave it.
fs::path safe_join(const fs::path& root, const std::string& name)
{
    fs::path rel = fs::path(name).lexically_normal();
    if (rel.empty() || rel.is_absolute() || rel.has_root_name()) throw InstallError("unsafe path in package: " + name);
    for (const fs::path& part : rel)
        if (part == "..") throw InstallError("unsafe path in package: " + name);
    return root / rel;
}

// An output file; write errors surface when it is closed (close_checked), not as a silently short file.
std::ofstream open_out(const fs::path& p)
{
    std::ofstream o(p, std::ios::binary);
    if (!o) throw InstallError("cannot write " + p.string());
    return o;
}

void close_checked(std::ofstream& o, const fs::path& p)
{
    o.close();
    if (!o) throw InstallError("error writing " + p.string());
}

struct PkgItem {
    std::string name;
    uint64_t off, size;
    uint32_t flags;
};

void install_pkg(const fs::path& pkg, const Bytes& rif, const fs::path& dest, std::string& title_id)
{
    FileSource file(pkg);
    Bytes h = read_all(file, 0, 0x80);
    if (memcmp(h.data(), "\x7f" "PKG", 4) != 0) throw InstallError(pkg.string() + " is not a PKG");
    uint16_t rev = be16(h.data() + 4), typ = be16(h.data() + 6);
    if (rev != 0x8000 || typ != 1) throw InstallError("only retail PS3 packages are supported");
    uint32_t item_count = be32(h.data() + 0x14);
    uint64_t data_off = be64(h.data() + 0x20);
    std::string content_id((const char*)h.data() + 0x30, strnlen((const char*)h.data() + 0x30, 0x30));
    if (content_id.size() < 16 || content_id[6] != '-') throw InstallError("unexpected content id " + content_id);
    title_id = content_id.substr(7, 9);
    fprintf(stderr, "%s: %s, %u items\n", pkg.filename().string().c_str(), content_id.c_str(), item_count);

    PkgData data(file, data_off, h.data() + 0x70);
    Bytes table = read_all(data, 0, (size_t)item_count * 0x20);
    std::vector<PkgItem> items;
    for (uint32_t i = 0; i < item_count; i++) {
        const uint8_t* e = table.data() + (size_t)i * 0x20;
        uint32_t name_off = be32(e), name_len = be32(e + 4);
        if (name_len > 4096) throw InstallError("corrupt item table");
        Bytes name = read_all(data, name_off, name_len);
        items.push_back({std::string(name.begin(), name.end()), be64(e + 8), be64(e + 16), be32(e + 24)});
    }

    fs::path root = dest / "game" / title_id;
    for (const PkgItem& it : items) {
        fs::path path = safe_join(root, it.name);
        if ((it.flags & 0xFF) == 4 || (it.flags & 0xFF) == 5) {
            fs::create_directories(path);
            continue;
        }
        fs::create_directories(path.parent_path());
        Window win(data, it.off, it.size);
        if (it.name == "USRDIR/EBOOT.BIN") {
            Bytes self = read_all(win, 0, (size_t)it.size);
            Bytes elf = decrypt_self(self, rif);
            fs::path elf_path = path.parent_path() / "EBOOT.elf";
            std::ofstream o = open_out(elf_path);
            o.write((const char*)elf.data(), (std::streamsize)elf.size());
            close_checked(o, elf_path);
            fprintf(stderr, "  %s -> EBOOT.elf (%zu bytes)\n", it.name.c_str(), elf.size());
        } else if (iends_with(it.name, ".EDAT")) {
            std::ofstream o = open_out(path);
            decrypt_edat(win, o, rif, it.name);
            close_checked(o, path);
        } else {
            std::ofstream o = open_out(path);
            Bytes buf(1 << 22);
            for (uint64_t pos = 0; pos < it.size;) {
                size_t n = (size_t)std::min<uint64_t>(buf.size(), it.size - pos);
                if (win.read(pos, buf.data(), n) != n) throw InstallError("unexpected end of " + it.name);
                o.write((const char*)buf.data(), (std::streamsize)n);
                pos += n;
            }
            close_checked(o, path);
        }
    }
}

}  // namespace

int install_main(int argc, char** argv)
{
    fs::path rap_path, dest = ".";
    std::vector<fs::path> pkgs;
    for (int i = 0; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rap" && i + 1 < argc) rap_path = argv[++i];
        else if (a == "--to" && i + 1 < argc) dest = argv[++i];
        else pkgs.emplace_back(a);
    }
    if (rap_path.empty() || pkgs.empty()) {
        fprintf(stderr, "usage: sc2hd install --rap <license.rap> [--to <dir>] <game.pkg> [<update.pkg> ...]\n");
        return 2;
    }
    try {
        FileSource rap_file(rap_path);
        Bytes rap(16);
        if (rap_file.read(0, rap.data(), 16) != 16) throw InstallError("a RAP file is exactly 16 bytes");
        Bytes rif = rap_to_rif(rap);
        for (const fs::path& pkg : pkgs) {
            std::string title;
            install_pkg(pkg, rif, dest, title);
            printf("%s\n", (dest / "game" / title).string().c_str());
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "install failed: %s\n", e.what());
        return 1;
    }
    return 0;
}

int selftest_main()
{
    if (const char* bad = crypto_selftest()) {
        fprintf(stderr, "selftest FAILED: %s\n", bad);
        return 1;
    }
    printf("selftest ok\n");
    return 0;
}

}  // namespace sc2hd
