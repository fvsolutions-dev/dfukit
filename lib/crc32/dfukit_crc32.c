// CRC-32 (dfukit_crc32.h), a nibble at a time: a table of 64 bytes

#include "dfukit_crc32.h"

// MSVC has no weak. An application's own still wins there: the linker takes this file from the
// static library only for a name nothing else defines
#if defined(_MSC_VER)
#define DFUKIT_WEAK
#else
#define DFUKIT_WEAK __attribute__((weak))
#endif

/** @brief The default implementation; weak, so that an application's own replaces it. */
DFUKIT_WEAK uint32_t dfukit_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
	static const uint32_t table[16] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
		0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
		0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
	};

	crc = ~crc;
	for (size_t index = 0; index < len; index++) {
		crc = table[(crc ^ data[index]) & 0x0f] ^ (crc >> 4);
		crc = table[(crc ^ (data[index] >> 4)) & 0x0f] ^ (crc >> 4);
	}
	return ~crc;
}
