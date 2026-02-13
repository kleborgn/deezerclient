#ifndef BLOWFISH_JUKEBOX_H
#define BLOWFISH_JUKEBOX_H

#include <QtGlobal>

// Decrypt one 2048-byte chunk with jukebox Blowfish CBC (same as decrypter.js + blowfish-cbc.js).
// key16: 16-byte key (bfKey from generateBlowfishKey)
// iv8: 8-byte IV (same per chunk: 0,1,2,3,4,5,6,7)
// data: 2048 bytes in/out (decrypted in place)
void blowfishCbcDecryptChunk(const quint8* key16, const quint8* iv8, quint8* data);

#endif
