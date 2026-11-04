// CRC-32 (IEEE 802.3, as zlib's): the check of each image. The implementation is weak: define
// dfukit_crc32() in your application, in an object file that gets linked, to replace it (a hardware
// engine, say). Same result, or the host's check fails

#ifndef DFUKIT_CRC32_H
#define DFUKIT_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The CRC-32 of data. crc: 0 to begin, or the last result to go on with more data */
uint32_t dfukit_crc32(uint32_t crc, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_CRC32_H
