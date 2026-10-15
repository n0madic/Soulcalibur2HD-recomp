// See crypto.h. GPL-2.0-or-later.
#include "crypto.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace sc2hd {
namespace {

// --- AES tables ------------------------------------------------------------------------------------------------------
// Generated once: the S-box by the usual multiplicative-inverse walk, the round tables from it. Words are big-endian
// columns, so a block is read and written with plain byte shifts.
struct AesTables {
    uint8_t sb[256];
    uint8_t isb[256];
    uint32_t te[4][256];
    uint32_t td[4][256];

    static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b)
    {
        uint8_t r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = xt(a);
            b >>= 1;
        }
        return r;
    }
    static uint32_t ror8(uint32_t v) { return (v >> 8) | (v << 24); }

    AesTables()
    {
        uint8_t p = 1, q = 1;
        do {
            p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));   // p *= 3
            q ^= (uint8_t)(q << 1);                                   // q /= 3
            q ^= (uint8_t)(q << 2);
            q ^= (uint8_t)(q << 4);
            if (q & 0x80) q ^= 0x09;
            auto rotl = [](uint8_t v, int n) { return (uint8_t)((v << n) | (v >> (8 - n))); };
            sb[p] = (uint8_t)(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
        } while (p != 1);
        sb[0] = 0x63;
        for (int i = 0; i < 256; i++) isb[sb[i]] = (uint8_t)i;
        for (int i = 0; i < 256; i++) {
            uint8_t s = sb[i], is = isb[i];
            te[0][i] = (uint32_t)mul(s, 2) << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | mul(s, 3);
            td[0][i] = (uint32_t)mul(is, 14) << 24 | (uint32_t)mul(is, 9) << 16 | (uint32_t)mul(is, 13) << 8 | mul(is, 11);
            for (int k = 1; k < 4; k++) {
                te[k][i] = ror8(te[k - 1][i]);
                td[k][i] = ror8(td[k - 1][i]);
            }
        }
    }
};

const AesTables& tables()
{
    static const AesTables t;   // thread-safe initialisation
    return t;
}

}  // namespace

void Aes::set_key(const uint8_t* key, size_t key_bytes)
{
    if (key_bytes != 16 && key_bytes != 32) throw std::invalid_argument("AES key must be 16 or 32 bytes");
    const AesTables& t = tables();
    const int nk = (int)key_bytes / 4;
    rounds_ = nk + 6;
    const int total = 4 * (rounds_ + 1);
    for (int i = 0; i < nk; i++) enc_[i] = load_be32(key + 4 * i);
    uint32_t rcon = 1;
    auto sub = [&](uint32_t w) {
        return (uint32_t)t.sb[w >> 24] << 24 | (uint32_t)t.sb[(w >> 16) & 0xFF] << 16 | (uint32_t)t.sb[(w >> 8) & 0xFF] << 8 |
               t.sb[w & 0xFF];
    };
    for (int i = nk; i < total; i++) {
        uint32_t w = enc_[i - 1];
        if (i % nk == 0) {
            w = sub((w << 8) | (w >> 24)) ^ (rcon << 24);
            rcon = AesTables::xt((uint8_t)rcon);
        } else if (nk > 6 && i % nk == 4) {
            w = sub(w);
        }
        enc_[i] = enc_[i - nk] ^ w;
    }
    // Equivalent inverse cipher: round keys in reverse order, InvMixColumns on all but the first and last.
    for (int r = 0; r <= rounds_; r++) {
        for (int c = 0; c < 4; c++) {
            uint32_t w = enc_[4 * (rounds_ - r) + c];
            if (r != 0 && r != rounds_)
                w = t.td[0][t.sb[w >> 24]] ^ t.td[1][t.sb[(w >> 16) & 0xFF]] ^ t.td[2][t.sb[(w >> 8) & 0xFF]] ^ t.td[3][t.sb[w & 0xFF]];
            dec_[4 * r + c] = w;
        }
    }
}

void Aes::encrypt_block(const uint8_t in[16], uint8_t out[16]) const
{
    const AesTables& t = tables();
    const uint32_t* rk = enc_;
    uint32_t s0 = load_be32(in) ^ rk[0], s1 = load_be32(in + 4) ^ rk[1], s2 = load_be32(in + 8) ^ rk[2],
             s3 = load_be32(in + 12) ^ rk[3];
    for (int r = 1; r < rounds_; r++) {
        rk += 4;
        uint32_t t0 = t.te[0][s0 >> 24] ^ t.te[1][(s1 >> 16) & 0xFF] ^ t.te[2][(s2 >> 8) & 0xFF] ^ t.te[3][s3 & 0xFF] ^ rk[0];
        uint32_t t1 = t.te[0][s1 >> 24] ^ t.te[1][(s2 >> 16) & 0xFF] ^ t.te[2][(s3 >> 8) & 0xFF] ^ t.te[3][s0 & 0xFF] ^ rk[1];
        uint32_t t2 = t.te[0][s2 >> 24] ^ t.te[1][(s3 >> 16) & 0xFF] ^ t.te[2][(s0 >> 8) & 0xFF] ^ t.te[3][s1 & 0xFF] ^ rk[2];
        uint32_t t3 = t.te[0][s3 >> 24] ^ t.te[1][(s0 >> 16) & 0xFF] ^ t.te[2][(s1 >> 8) & 0xFF] ^ t.te[3][s2 & 0xFF] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    auto fin = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
        return ((uint32_t)t.sb[a >> 24] << 24 | (uint32_t)t.sb[(b >> 16) & 0xFF] << 16 | (uint32_t)t.sb[(c >> 8) & 0xFF] << 8 |
                t.sb[d & 0xFF]) ^ k;
    };
    store_be32(out, fin(s0, s1, s2, s3, rk[0]));
    store_be32(out + 4, fin(s1, s2, s3, s0, rk[1]));
    store_be32(out + 8, fin(s2, s3, s0, s1, rk[2]));
    store_be32(out + 12, fin(s3, s0, s1, s2, rk[3]));
}

void Aes::decrypt_block(const uint8_t in[16], uint8_t out[16]) const
{
    const AesTables& t = tables();
    const uint32_t* rk = dec_;
    uint32_t s0 = load_be32(in) ^ rk[0], s1 = load_be32(in + 4) ^ rk[1], s2 = load_be32(in + 8) ^ rk[2],
             s3 = load_be32(in + 12) ^ rk[3];
    for (int r = 1; r < rounds_; r++) {
        rk += 4;
        uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][(s3 >> 16) & 0xFF] ^ t.td[2][(s2 >> 8) & 0xFF] ^ t.td[3][s1 & 0xFF] ^ rk[0];
        uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][(s0 >> 16) & 0xFF] ^ t.td[2][(s3 >> 8) & 0xFF] ^ t.td[3][s2 & 0xFF] ^ rk[1];
        uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][(s1 >> 16) & 0xFF] ^ t.td[2][(s0 >> 8) & 0xFF] ^ t.td[3][s3 & 0xFF] ^ rk[2];
        uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][(s2 >> 16) & 0xFF] ^ t.td[2][(s1 >> 8) & 0xFF] ^ t.td[3][s0 & 0xFF] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    auto fin = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
        return ((uint32_t)t.isb[a >> 24] << 24 | (uint32_t)t.isb[(b >> 16) & 0xFF] << 16 | (uint32_t)t.isb[(c >> 8) & 0xFF] << 8 |
                t.isb[d & 0xFF]) ^ k;
    };
    store_be32(out, fin(s0, s3, s2, s1, rk[0]));
    store_be32(out + 4, fin(s1, s0, s3, s2, rk[1]));
    store_be32(out + 8, fin(s2, s1, s0, s3, rk[2]));
    store_be32(out + 12, fin(s3, s2, s1, s0, rk[3]));
}

void aes_cbc_decrypt(const uint8_t* key, size_t key_bytes, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len)
{
    if (len % 16) throw std::invalid_argument("CBC length must be a multiple of 16");
    Aes aes;
    aes.set_key(key, key_bytes);
    uint8_t prev[16], cur[16], plain[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i < len; i += 16) {
        memcpy(cur, in + i, 16);   // in and out may alias
        aes.decrypt_block(cur, plain);
        for (int j = 0; j < 16; j++) out[i + j] = plain[j] ^ prev[j];
        memcpy(prev, cur, 16);
    }
}

void aes_ctr(const uint8_t* key, size_t key_bytes, const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len)
{
    Aes aes;
    aes.set_key(key, key_bytes);
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (size_t i = 0; i < len; i += 16) {
        aes.encrypt_block(ctr, ks);
        size_t n = len - i < 16 ? len - i : 16;
        for (size_t j = 0; j < n; j++) out[i + j] = in[i + j] ^ ks[j];
        for (int k = 15; k >= 0; k--)
            if (++ctr[k]) break;   // 128-bit big-endian increment
    }
}

void aes_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    Aes aes;
    aes.set_key(key, 16);
    aes.encrypt_block(in, out);
}

void aes_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    Aes aes;
    aes.set_key(key, 16);
    aes.decrypt_block(in, out);
}

// --- SHA-1, HMAC, CMAC -----------------------------------------------------------------------------------------------
namespace {
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64];
    size_t fill = 0;
    uint64_t total = 0;

    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void block(const uint8_t* p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = load_be32(p + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void update(const uint8_t* p, size_t n)
    {
        total += n;
        while (n) {
            size_t take = 64 - fill < n ? 64 - fill : n;
            memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    void finish(uint8_t out[20])
    {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 5; i++) store_be32(out + 4 * i, h[i]);
    }
};
}  // namespace

void sha1(const uint8_t* data, size_t len, uint8_t out[20])
{
    Sha1 s;
    s.update(data, len);
    s.finish(out);
}

void hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* data, size_t len, uint8_t out[20])
{
    uint8_t k[64] = {};
    if (key_len > 64) sha1(key, key_len, k);
    else memcpy(k, key, key_len);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    uint8_t inner[20];
    Sha1 a;
    a.update(ipad, 64);
    a.update(data, len);
    a.finish(inner);
    Sha1 b;
    b.update(opad, 64);
    b.update(inner, 20);
    b.finish(out);
}

void aes_cmac(const uint8_t key[16], const uint8_t* data, size_t len, uint8_t out[16])
{
    Aes aes;
    aes.set_key(key, 16);
    auto dbl = [](uint8_t v[16]) {   // multiply by x in GF(2^128)
        uint8_t carry = v[0] >> 7;
        for (int i = 0; i < 15; i++) v[i] = (uint8_t)((v[i] << 1) | (v[i + 1] >> 7));
        v[15] = (uint8_t)((v[15] << 1) ^ (carry ? 0x87 : 0));
    };
    uint8_t zero[16] = {}, k1[16], k2[16];
    aes.encrypt_block(zero, k1);
    dbl(k1);
    memcpy(k2, k1, 16);
    dbl(k2);

    size_t blocks = (len + 15) / 16;
    bool complete = len != 0 && len % 16 == 0;
    if (blocks == 0) blocks = 1;
    uint8_t x[16] = {}, y[16];
    for (size_t b = 0; b + 1 < blocks; b++) {
        for (int j = 0; j < 16; j++) y[j] = x[j] ^ data[16 * b + j];
        aes.encrypt_block(y, x);
    }
    uint8_t last[16] = {};
    size_t rest = len - 16 * (blocks - 1);
    memcpy(last, data + 16 * (blocks - 1), rest);
    if (complete) {
        for (int j = 0; j < 16; j++) last[j] ^= k1[j];
    } else {
        last[rest] = 0x80;
        for (int j = 0; j < 16; j++) last[j] ^= k2[j];
    }
    for (int j = 0; j < 16; j++) y[j] = x[j] ^ last[j];
    aes.encrypt_block(y, out);
}

// --- inflate ---------------------------------------------------------------------------------------------------------
// A plain canonical-Huffman decoder in the shape of zlib's puff.c: slow, short, and enough for the few megabytes of a
// compressed SELF segment.
namespace {
struct BitReader {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    int bits(int need)
    {
        uint32_t v = bitbuf;
        while (bitcnt < need) {
            if (pos >= n) throw std::runtime_error("inflate: input ends early");
            v |= (uint32_t)p[pos++] << bitcnt;
            bitcnt += 8;
        }
        bitbuf = v >> need;
        bitcnt -= need;
        return (int)(v & ((1u << need) - 1));
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

int build(Huffman& h, const uint16_t* length, int n)
{
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[length[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;   // over-subscribed
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h.count[len]);
    for (int i = 0; i < n; i++)
        if (length[i]) h.symbol[offs[length[i]]++] = (uint16_t)i;
    return left;
}

int decode(BitReader& br, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= br.bits(1);
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw std::runtime_error("inflate: bad Huffman code");
}

void codes(BitReader& br, std::vector<uint8_t>& out, const Huffman& lencode, const Huffman& distcode)
{
    static const uint16_t lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint16_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint16_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode(br, lencode);
        if (sym < 256) {
            out.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("inflate: bad length symbol");
            int len = lens[sym] + br.bits(lext[sym]);
            int ds = decode(br, distcode);
            if (ds >= 30) throw std::runtime_error("inflate: bad distance symbol");
            size_t dist = dists[ds] + (size_t)br.bits(dext[ds]);
            if (dist > out.size()) throw std::runtime_error("inflate: distance too far back");
            size_t from = out.size() - dist;
            for (int i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
}

// A literal/length or distance code of a dynamic block: complete, or incomplete with a single code (RFC 1951 allows that
// one case; puff.c checks the same).
void build_dynamic(Huffman& h, const uint16_t* length, int n, const char* what)
{
    const int left = build(h, length, n);
    if (left < 0 || (left > 0 && n - h.count[0] != 1)) throw std::runtime_error(std::string("inflate: bad ") + what + " code");
}

uint32_t adler32(const uint8_t* p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}
}  // namespace

std::vector<uint8_t> zlib_inflate(const uint8_t* data, size_t len)
{
    if (len < 2 || (data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31) throw std::runtime_error("inflate: not a zlib stream");
    if (data[1] & 0x20) throw std::runtime_error("inflate: preset dictionaries are not supported");
    BitReader br{data + 2, len - 2};
    std::vector<uint8_t> out;
    int last;
    do {
        last = br.bits(1);
        int type = br.bits(2);
        if (type == 0) {
            br.bitbuf = 0;
            br.bitcnt = 0;
            if (br.pos + 4 > br.n) throw std::runtime_error("inflate: stored block truncated");
            unsigned n = br.p[br.pos] | (br.p[br.pos + 1] << 8);
            unsigned nn = br.p[br.pos + 2] | (br.p[br.pos + 3] << 8);
            if (n != (~nn & 0xFFFF)) throw std::runtime_error("inflate: stored block length check failed");
            br.pos += 4;
            if (br.pos + n > br.n) throw std::runtime_error("inflate: stored block truncated");
            out.insert(out.end(), br.p + br.pos, br.p + br.pos + n);
            br.pos += n;
        } else if (type == 1) {
            uint16_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            Huffman lc, dc;
            build(lc, l, 288);
            uint16_t d[30];
            for (int k = 0; k < 30; k++) d[k] = 5;
            build(dc, d, 30);
            codes(br, out, lc, dc);
        } else if (type == 2) {
            int nlen = br.bits(5) + 257, ndist = br.bits(5) + 1, ncode = br.bits(4) + 4;
            if (nlen > 286 || ndist > 30) throw std::runtime_error("inflate: bad counts");
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint16_t l[320] = {};
            for (int k = 0; k < ncode; k++) l[order[k]] = (uint16_t)br.bits(3);
            Huffman lc;
            if (build(lc, l, 19) != 0) throw std::runtime_error("inflate: incomplete code-length code");
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decode(br, lc);
                if (sym < 16) {
                    l[idx++] = (uint16_t)sym;
                } else {
                    int prev = 0, rep;
                    if (sym == 16) {
                        if (idx == 0) throw std::runtime_error("inflate: repeat with no previous length");
                        prev = l[idx - 1];
                        rep = 3 + br.bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.bits(3);
                    } else {
                        rep = 11 + br.bits(7);
                    }
                    if (idx + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                    while (rep--) l[idx++] = (uint16_t)prev;
                }
            }
            if (l[256] == 0) throw std::runtime_error("inflate: no end-of-block code");
            Huffman lcode, dcode;
            build_dynamic(lcode, l, nlen, "literal/length");
            build_dynamic(dcode, l + nlen, ndist, "distance");
            codes(br, out, lcode, dcode);
        } else {
            throw std::runtime_error("inflate: bad block type");
        }
    } while (!last);
    // The Adler-32 of the output follows on the next byte boundary (the bit reader holds less than a byte).
    if (br.pos + 4 > br.n) throw std::runtime_error("inflate: checksum missing");
    if (load_be32(br.p + br.pos) != adler32(out.data(), out.size())) throw std::runtime_error("inflate: checksum mismatch");
    return out;
}

std::vector<uint8_t> hex(const char* s)
{
    std::vector<uint8_t> v;
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (; s[0] && s[1]; s += 2) v.push_back((uint8_t)(nib(s[0]) << 4 | nib(s[1])));
    return v;
}

// --- self-test -------------------------------------------------------------------------------------------------------

const char* crypto_selftest()
{
    std::vector<uint8_t> pt = hex("00112233445566778899aabbccddeeff"), out(16), back(16);
    for (size_t kb : {16u, 32u}) {
        std::vector<uint8_t> key(kb);
        for (size_t i = 0; i < kb; i++) key[i] = (uint8_t)i;
        Aes a;
        a.set_key(key.data(), kb);
        a.encrypt_block(pt.data(), out.data());
        if (out != hex(kb == 16 ? "69c4e0d86a7b0430d8cdb78070b4c55a" : "8ea2b7ca516745bfeafc49904b496089")) return "aes-encrypt";
        a.decrypt_block(out.data(), back.data());
        if (back != pt) return "aes-decrypt";
    }
    uint8_t d[20];
    sha1((const uint8_t*)"abc", 3, d);
    if (std::vector<uint8_t>(d, d + 20) != hex("a9993e364706816aba3e25717850c26c9cd0d89d")) return "sha1";
    std::vector<uint8_t> k20(20, 0x0b);
    hmac_sha1(k20.data(), 20, (const uint8_t*)"Hi There", 8, d);
    if (std::vector<uint8_t>(d, d + 20) != hex("b617318655057264e28bc0b6fb378c8ef146be00")) return "hmac-sha1";
    std::vector<uint8_t> ck = hex("2b7e151628aed2a6abf7158809cf4f3c"), m = hex("6bc1bee22e409f96e93d7e117393172a"), mac(16);
    aes_cmac(ck.data(), nullptr, 0, mac.data());
    if (mac != hex("bb1d6929e95937287fa37d129b756746")) return "cmac-empty";
    aes_cmac(ck.data(), m.data(), 16, mac.data());
    if (mac != hex("070a16b46b4d4144f79bdd9dd04a287c")) return "cmac-16";
    std::vector<uint8_t> m40 = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411");
    aes_cmac(ck.data(), m40.data(), 40, mac.data());
    if (mac != hex("dfa66747de9ae63030ca32611497c827")) return "cmac-40";
    std::vector<uint8_t> z = hex("78dacb48cdc9c957c8402701680308b1");
    std::vector<uint8_t> text = zlib_inflate(z.data(), z.size());
    if (std::string(text.begin(), text.end()) != "hello hello hello hello") return "inflate";
    z.back() ^= 1;   // a damaged checksum must be refused
    try {
        zlib_inflate(z.data(), z.size());
        return "inflate-adler";
    } catch (const std::runtime_error&) {
    }
    // CTR: counter carries across bytes
    std::vector<uint8_t> iv = hex("000000000000000000000000000000ff"), zeros(48), ks(48);
    aes_ctr(ck.data(), 16, iv.data(), zeros.data(), ks.data(), 48);
    std::vector<uint8_t> second(16), ctr2 = hex("00000000000000000000000000000100");
    aes_ecb_encrypt(ck.data(), ctr2.data(), second.data());
    if (!std::equal(second.begin(), second.end(), ks.begin() + 16)) return "ctr-carry";
    return nullptr;
}

}  // namespace sc2hd
