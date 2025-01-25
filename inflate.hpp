#include "license.h"
#ifndef _DEFLATE_HPP
#define _DEFLATE_HPP
#include <vector>
#include <cstdint>

#define INFLATE_OK                                  ((0 << 31) | (1 << 29) | (300 << 16) | (0))
#define INFLATE_FALSE                               ((0 << 31) | (1 << 29) | (300 << 16) | (1))
#define INFLATE_MEMORY_ACCESS_FAIL                  ((1 << 31) | (1 << 29) | (300 << 16) | (2))
#define INFLATE_INVALID_WINDOW_SIZE                 ((1 << 31) | (1 << 29) | (300 << 16) | (3))
#define INFLATE_INVALID_SYMBOL                      ((1 << 31) | (1 << 29) | (300 << 16) | (4))
#define INFLATE_RESERVED_BLOCK                      ((1 << 31) | (1 << 29) | (300 << 16) | (5))
#define INFLATE_INVALID_DISTANCE                    ((1 << 31) | (1 << 29) | (300 << 16) | (6))
#define INFLATE_INVALID_DICTIONARY_SIZE             ((1 << 31) | (1 << 29) | (300 << 16) | (7))
#define INFLATE_UNCOMPRESSED_BLOCK_LENGTH_MISMATCH  ((1 << 31) | (1 << 29) | (300 << 16) | (8))
#define INFLATE_UNCOMPRESSED_BLOCK_INVALID_LENGTH   ((1 << 31) | (1 << 29) | (300 << 16) | (9))
#define INFLATE_SYMBOL_NOT_FOUND                    ((1 << 31) | (1 << 29) | (300 << 16) | (10))
#define INFLATE_INVALID_REPEAT_CODE                 ((1 << 31) | (1 << 29) | (300 << 16) | (11))
#define INFLATE_INVALID_CODE_LENGTH                 ((1 << 31) | (1 << 29) | (300 << 16) | (12))
#define INFLATE_FINAL_BLOCK_MISPLACED               ((1 << 31) | (1 << 29) | (300 << 16) | (13))
#define INFLATE_DATA_INTEGRITY_FAIL                 ((1 << 31) | (1 << 29) | (300 << 16) | (14))
#define INFLATE_CODE_AMBIGUITY						((1 << 31) | (1 << 29) | (300 << 16) | (15))

#define CODEC_NAMESPACE_BEGIN namespace codec {
#define CODEC_NAMESPACE_END }


CODEC_NAMESPACE_BEGIN

std::int32_t
DeflateInflate(
const std::uint8_t* deflated_datastream,
const std::size_t   deflated_size,
const std::uint8_t* dictionary_data,
const std::size_t   dictionary_size,
const std::uint32_t window_size,
std::vector<std::uint8_t>& inflated_datastream
);


CODEC_NAMESPACE_END



#endif