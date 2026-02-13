// Blowfish CBC decoder matching jukebox-player-vlc blowfish-cbc.js + decrypter.js exactly.
// Key expansion: expand key to 72 bytes by repetition, XOR into P as big-endian 32-bit words.
// Decrypt: same IV [0,1,2,3,4,5,6,7] per chunk; CBC within chunk.

#include "blowfish_jukebox.h"
#include <QtCore/QtGlobal>
#include <cstring>
#include <algorithm>

namespace {

using U32 = quint32;

static const U32 bf_p[18] = {
    0x243f6a88U, 0x85a308d3U, 0x13198a2eU, 0x03707344U, 0xa4093822U, 0x299f31d0U,
    0x082efa98U, 0xec4e6c89U, 0x452821e6U, 0x38d01377U, 0xbe5466cfU, 0x34e90c6cU,
    0xc0ac29b7U, 0xc97c50ddU, 0x3f84d5b5U, 0xb5470917U, 0x9216d5d9U, 0x8979fb1bU
};

#include "blowfish_jukebox_tables.inc"

static const U32* const bf_s[4] = { bf_s0, bf_s1, bf_s2, bf_s3 };

inline U32 pack32(quint8 a, quint8 b, quint8 c, quint8 d) {
    return (static_cast<U32>(a) << 24) | (static_cast<U32>(b) << 16) | (static_cast<U32>(c) << 8) | static_cast<U32>(d);
}

inline void unpack32(U32 n, quint8* out) {
    out[0] = static_cast<quint8>(n >> 24);
    out[1] = static_cast<quint8>(n >> 16);
    out[2] = static_cast<quint8>(n >> 8);
    out[3] = static_cast<quint8>(n);
}

inline U32 xor32(U32 a, U32 b) { return a ^ b; }
inline U32 addMod32(U32 a, U32 b) { return static_cast<U32>(static_cast<quint32>(a) + static_cast<quint32>(b)); }

struct BlowfishCtx {
    U32 P[18];
    U32 S[4][256];

    U32 F(U32 xL) const {
        U32 a = (xL >> 24) & 0xFF, b = (xL >> 16) & 0xFF, c = (xL >> 8) & 0xFF, d = xL & 0xFF;
        U32 res = addMod32(S[0][a], S[1][b]);
        res = xor32(res, S[2][c]);
        return addMod32(res, S[3][d]);
    }

    void encryptBlock(U32& xL, U32& xR) const {
        for (int t = 0; t < 16; t++) {
            xL = xor32(xL, P[t]);
            xR = xor32(xR, F(xL));
            std::swap(xL, xR);
        }
        std::swap(xL, xR);
        xL = xor32(xL, P[17]);
        xR = xor32(xR, P[16]);
    }

    void decryptBlock(U32& xL, U32& xR) const {
        for (int t = 17; t > 1; t--) {
            xL = xor32(xL, P[t]);
            xR = xor32(xR, F(xL));
            std::swap(xL, xR);
        }
        std::swap(xL, xR);
        xL = xor32(xL, P[0]);
        xR = xor32(xR, P[1]);
    }

    void initKey(const quint8* key16) {
        memcpy(P, bf_p, sizeof(bf_p));
        for (int i = 0; i < 4; i++)
            memcpy(S[i], bf_s[i], 256 * sizeof(U32));
        quint8 expanded[72];
        for (int i = 0; i < 72; i++)
            expanded[i] = key16[i % 16];
        for (int n = 0, i = 0; n < 18; n++, i += 4) {
            P[n] = xor32(P[n], pack32(expanded[i], expanded[i+1], expanded[i+2], expanded[i+3]));
        }
        U32 xL = 0, xR = 0;
        for (int f = 0; f < 18; f += 2) {
            encryptBlock(xL, xR);
            P[f] = xL;
            P[f + 1] = xR;
        }
        for (int a = 0; a < 4; a++) {
            for (int c = 0; c < 256; c += 2) {
                encryptBlock(xL, xR);
                S[a][c] = xL;
                S[a][c + 1] = xR;
            }
        }
    }

    void decodeChunk(const quint8* iv8, quint8* data, int len) {
        U32 ivL = pack32(iv8[0], iv8[1], iv8[2], iv8[3]);
        U32 ivR = pack32(iv8[4], iv8[5], iv8[6], iv8[7]);
        for (int i = 0; i < len; i += 8) {
            U32 cL = pack32(data[i], data[i+1], data[i+2], data[i+3]);
            U32 cR = pack32(data[i+4], data[i+5], data[i+6], data[i+7]);
            U32 xL = cL, xR = cR;
            decryptBlock(xL, xR);
            U32 pL = xor32(ivL, xL), pR = xor32(ivR, xR);
            unpack32(pL, data + i);
            unpack32(pR, data + i + 4);
            ivL = cL;
            ivR = cR;
        }
    }
};

} // namespace

void blowfishCbcDecryptChunk(const quint8* key16, const quint8* iv8, quint8* data) {
    BlowfishCtx ctx;
    ctx.initKey(key16);
    ctx.decodeChunk(iv8, data, 2048);
}
