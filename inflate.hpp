#ifndef _INFLATE_HPP
#define _INFLATE_HPP

#include <cstdint>
#include <vector>

#define INFLATE_OK								((0 << 31) | (1 << 29) | (300 << 16) | (0))
#define INFLATE_FALSE							((0 << 31) | (1 << 29) | (300 << 16) | (1))
#define INFLATE_MEMORY_ACCESS_FAIL				((1 << 31) | (1 << 29) | (300 << 16) | (0))
#define INFLATE_SYMBOL_NOT_FOUND				((1 << 31) | (1 << 29) | (300 << 16) | (1))
#define INFLATE_INVALID_DISTANCE				((1 << 31) | (1 << 29) | (300 << 16) | (2))
#define INFLATE_INVALID_LENGTH					((1 << 31) | (1 << 29) | (300 << 16) | (3))
#define INFLATE_INVALID_DISTANCE_SYMBOL			((1 << 31) | (1 << 29) | (300 << 16) | (4))
#define INFLATE_FINAL_BLOCK_MISPLACED			((1 << 31) | (1 << 29) | (300 << 16) | (5))
#define INFLATE_DATA_INTEGRITY_FAIL				((1 << 31) | (1 << 29) | (300 << 16) | (6))
#define INFLATE_UNCOMPRESSED_LENGTH_MISMATCH	((1 << 31) | (1 << 29) | (300 << 16) | (7))
#define INFLATE_UNKNOWN_SYMBOL					((1 << 31) | (1 << 29) | (300 << 16) | (8))
#define INFLATE_REPEAT_CODE_FAIL				((1 << 31) | (1 << 29) | (300 << 16) | (9))
#define INFLATE_INVALID_WINDOW_SIZE				((1 << 31) | (1 << 29) | (300 << 16) | (10))
#define INFLATE_INVALID_BLOCK					((1 << 31) | (1 << 29) | (300 << 16) | (11))


std::int32_t
InflateDatastream(
	const std::uint8_t* const  deflated,
	const std::size_t		   deflated_size,
	const std::uint8_t* const  dictionary,
	const std::size_t		   dictionary_size,
	const std::size_t		   window_size,
	std::vector<std::uint8_t>& inflated
);


#endif

