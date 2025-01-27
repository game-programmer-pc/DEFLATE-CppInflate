#include <cstdint>
#include <array>
#include <limits>
#include <memory>
#include <concepts>
#include <map>
#include <algorithm>
#include "inflate.hpp"

#define INFLATE_EOF											    ((0 << 31) | (1 << 29) | (300 << 16) | (2))
#define INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MAX_BIT_LENGTH		(9)
#define INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MIN_BIT_LENGTH		(7)
#define INFLATE_FIXED_DISTANCE_BIT_LENGTH						(5)
#define INFLATE_UNCOMPRESSED									(0)
#define INFLATE_FIXED											(1)
#define INFLATE_DYNAMIC											(2)
#define INFLATE_RESERVED										(3)
#define INFLATE_EOB												(256)

#define ADLER32_MODULO											(65521)



struct datastream {
	constexpr datastream(const std::uint8_t* pointer, const std::size_t size) noexcept : data(pointer), datastream_size(size), datastream_ptr(0) {}
	const std::uint8_t* const data;
	const std::size_t		  datastream_size;
	std::uintptr_t			  datastream_ptr;
};

struct sliding_window {
	sliding_window(std::uint8_t* pointer, const std::size_t size) noexcept : window(pointer), window_size(size), window_ptr(0), window_count(0) {}
	std::unique_ptr<std::uint8_t[]>		  window;
	const std::size_t					  window_size;
	std::uintptr_t						  window_ptr;
	std::size_t							  window_count;
};

struct dynamic_alphabet {
	constexpr dynamic_alphabet() noexcept : blen(0) {}
	std::size_t blen;
	std::vector<std::uint32_t> symbols;
	std::vector<std::uint32_t> codes;
	bool operator<(const dynamic_alphabet& operand) const noexcept {
		return blen < operand.blen;
	}
};


template <typename T>
concept endian_swap_compatible = std::is_integral_v<T> || std::is_floating_point_v<T>;

template <endian_swap_compatible T>
T EndianSwap(T value) noexcept {
	static_assert(sizeof(T) > 1);
	std::uint8_t* begin = reinterpret_cast<std::uint8_t*>(&value);
	std::uint8_t* end = begin + (sizeof(T) - 1);
	while (begin < end)
	{
		std::swap(*begin, *end);
		begin++;
		end--;
	}
	return value;
}

static
std::int32_t
VerifyWindowSize(
	const std::size_t				size
) {
	switch (size)
	{
	case 256:
	case 512:
	case 1024:
	case 2048:
	case 4096:
	case 8192:
	case 16384:
	case 32768:
		return INFLATE_OK;
		break;
	default:
		return INFLATE_INVALID_WINDOW_SIZE;
		break;
	}
}


static
std::int32_t
GetDuplicatedStringFromSlidingWindow(
	sliding_window&					window,
	const std::size_t				length,
	const std::size_t				distance,
	std::vector<std::uint8_t>&		duplicated_string
) {
	if (distance > window.window_size)
	{
		return INFLATE_INVALID_DISTANCE;
	}
	else if (length > window.window_size)
	{
		return INFLATE_INVALID_LENGTH;
	}
	else if (window.window_ptr < distance && window.window_count != window.window_size)
	{
		return INFLATE_INVALID_DISTANCE;
	}
	else
	{
		const std::uint8_t* begin;
		if (window.window_ptr >= distance)
			begin = window.window.get() + window.window_ptr - distance;
		else
			begin = window.window.get() + window.window_size - (distance - (window.window_ptr));

		if (length > distance)
		{
			const std::uintptr_t quotient = length / distance;
			const std::uintptr_t remainder = length % distance;
			if (window.window.get() + window.window_size < begin + distance)
			{
				const std::uintptr_t low = window.window_size - distance;
				const std::uintptr_t hi = distance - low;
				for (std::size_t i = 0; i < quotient; i++)
				{
					duplicated_string.insert(duplicated_string.end(), begin, begin + low);
					duplicated_string.insert(duplicated_string.end(), window.window.get(), window.window.get() + hi);
				}
				if (remainder != 0)
				{
					if (remainder + begin > window.window.get() + window.window_size)
					{
						duplicated_string.insert(duplicated_string.end(), begin, begin + low);
						duplicated_string.insert(duplicated_string.end(), window.window.get(), window.window.get() + remainder);
					}
					else
					{
						duplicated_string.insert(duplicated_string.end(), begin, begin + remainder);
					}
				}
			}
			else
			{
				for (std::uintptr_t i = 0; i < quotient; i++)
				{
					duplicated_string.insert(duplicated_string.end(), begin, begin + distance);
				}
				if (remainder != 0)
				{
					duplicated_string.insert(duplicated_string.end(), begin, begin + remainder);
				}
			}
		}
		else
		{
			if (begin + length > window.window.get() + window.window_size)
			{
				const std::uintptr_t low = window.window_size - window.window_ptr;
				duplicated_string.insert(duplicated_string.end(), begin, begin + low);
				duplicated_string.insert(duplicated_string.end(), window.window.get(), window.window.get() + (length - low));
			}
			else
			{
				duplicated_string.insert(duplicated_string.end(), begin, begin + length);
			}
		}
		return INFLATE_OK;
	}
}

static
void
AppendLiteralByteToSlidingWindow(
	sliding_window&				window,
	const std::uint8_t			byte
) {
	if (window.window_ptr == window.window_size)
	{
		window.window_ptr = 0;
	}
	window.window.get()[window.window_ptr] = byte;
	window.window_ptr++;
	if (window.window_count != window.window_size)
		window.window_count++;
}

static
void
AppendDuplicatedStringToSlidingWindow(
	sliding_window&					 window,
	const std::vector<std::uint8_t>& duplicated_string
) {
	if (window.window_ptr + duplicated_string.size() < window.window_size)
	{
		memcpy(window.window.get() + window.window_ptr, duplicated_string.data(), sizeof(std::uint8_t) * duplicated_string.size());
		window.window_ptr += duplicated_string.size();
		if (window.window_count != window.window_size)
			window.window_count += duplicated_string.size();
	}
	else
	{
		const std::size_t low = window.window_size - window.window_ptr;
		const std::size_t hi = duplicated_string.size() - low;
		memcpy(window.window.get() + window.window_ptr, duplicated_string.data(), sizeof(std::uint8_t) * low);
		memcpy(window.window.get(), duplicated_string.data() + low, sizeof(std::uint8_t) * hi);
		window.window_ptr = 0 + hi;
		if (window.window_count != window.window_size)
			window.window_count = window.window_size;
	}
}

static
std::int32_t
ReadDatastream(
	std::uint8_t*		buffer,
	const std::size_t	size,
	datastream&			istream
) {
	const std::uintptr_t quotient = istream.datastream_ptr >> 3;
	const std::uintptr_t remainder = istream.datastream_ptr & 0x7;
	const std::uintptr_t bstream = istream.datastream_size << 3;

	if (quotient + remainder + size < bstream)
	{
		for (std::size_t i = 0; i < size; i++)
		{
			buffer[i] = (istream.data[(istream.datastream_ptr >> 3)] >> (istream.datastream_ptr & 0x7)) & 0x1;
			istream.datastream_ptr++;
		}

		return INFLATE_OK;
	}
	else
	{
		return INFLATE_MEMORY_ACCESS_FAIL;
	}
}

static
std::int32_t
AlignPointer(
	datastream& istream
) {
	std::uintptr_t pointer = ((istream.datastream_ptr + 7) & ~7) >> 3;
	if (pointer < istream.datastream_size)
	{
		istream.datastream_ptr = pointer << 3;
		return INFLATE_OK;
	}
	else
	{
		return INFLATE_MEMORY_ACCESS_FAIL;
	}
}

static
std::uint32_t
NonHuffmanIntegerValue(
	const std::uint8_t* const buffer,
	const std::size_t		  size
) {
	std::uint32_t value{ 0 };
	for (std::size_t i = 0; i < size; i++)
	{
		value |= (buffer[i] << i);
	}
	return value;
}

static
std::uint32_t
HuffmanIntegerValue(
	const std::uint8_t* const buffer,
	const std::size_t		  size
) {
	std::uint32_t value{ 0 };
	for (std::size_t i = 0; i < size; i++)
	{
		value = (value << 1) | buffer[i];
	}
	return value;
}

static
std::int32_t
GetBlockType(
	datastream&			istream,
	bool&				bfinal,
	std::uint8_t&		btype
) {
	std::array<std::uint8_t, 3> buffer;
	std::int32_t result = ReadDatastream(buffer.data(), buffer.size(), istream);
	if (result != INFLATE_OK) return result;
	bfinal = buffer[0];
	btype = NonHuffmanIntegerValue(buffer.data() + 1, buffer.size() - 1);
	return result;
}

static
std::int32_t
GetFixedLiteralLengthSymbol(
	datastream&				dstream,
	std::uint32_t&			symbol
) {
	std::array<std::uint8_t, INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MAX_BIT_LENGTH> bits;
	std::uint32_t value;
	std::int32_t result = INFLATE_OK;
	result = ReadDatastream(
		bits.data(),
		INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MIN_BIT_LENGTH,
		dstream
	);
	if (result != INFLATE_OK) return result;
	value = HuffmanIntegerValue(
		bits.data(),
		INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MIN_BIT_LENGTH
	);
	if (value <= 23)
	{
		symbol = value + 256;
		return result;
	}
	else
	{
		result = ReadDatastream(
			bits.data() + INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MIN_BIT_LENGTH,
			1,
			dstream
		);
		if (result != INFLATE_OK) return result;
		value = HuffmanIntegerValue(bits.data(), INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MIN_BIT_LENGTH + 1);
		if (value >= 48 && value <= 191)
		{ 
			symbol = value - 48;
			return result;
		}
		else if (value >= 192 && value <= 199)
		{
			symbol = value + 88;
			return result;
		}
		else
		{
			result = ReadDatastream(
				bits.data() + INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MIN_BIT_LENGTH + 1,
				1,
				dstream
			);
			if (result != INFLATE_OK) return result;
			value = HuffmanIntegerValue(bits.data(), INFLATE_FIXED_LITERAL_LENGTH_SYMBOL_MAX_BIT_LENGTH);
			if (value >= 400 && value <= 511)
			{
				symbol = value - 256;
				return result;
			}
			else
			{
				return INFLATE_SYMBOL_NOT_FOUND;
			}
		}
	}
}

static
std::int32_t
ParseLiteralLengthSymbol(
	datastream&				   dstream,
	sliding_window&			   window,
	std::vector<std::uint8_t>& istream,
	const std::uint32_t		   symbol,
	std::size_t&			   length
) {
	if (symbol < 256)
	{
		istream.push_back(static_cast<std::uint8_t>(symbol));
		AppendLiteralByteToSlidingWindow(window, static_cast<std::uint8_t>(symbol));
		length = 0;
		return INFLATE_FALSE;
	}
	else if (symbol == 256)
	{
		return INFLATE_EOB;
	}
	else
	{
		if (symbol > 256 && symbol <= 264)
		{
			length = static_cast<std::size_t>(symbol) - 254;
			return INFLATE_OK;
		}
		else if (symbol == 285)
		{
			length = 258;
			return INFLATE_OK;
		}
		else
		{
			std::int32_t result = INFLATE_OK;
			std::array<std::uint8_t, 5> bits;
			std::size_t bits_needed = ((static_cast<std::size_t>(symbol) - 265) >> 2) + 1;
			result = ReadDatastream(bits.data(), bits_needed, dstream);
			if (result != INFLATE_OK) return result;
			length = HuffmanIntegerValue(bits.data(), bits_needed);
			const std::array<const std::size_t, 20> length_offsets = {
				11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227
			};
			length += length_offsets[static_cast<std::size_t>(symbol) - 265];
			return result;
		}
	}
}

static
std::int32_t
ParseDistanceSymbol(
	datastream&			dstream,
	const std::uint32_t symbol,
	std::size_t&		distance
) {
	std::array<std::uint8_t, 13> bits;
	if (symbol <= 3)
	{
		distance = static_cast<std::size_t>(symbol) + 1;
		return INFLATE_OK;
	}
	else if (symbol > 29)
	{
		return INFLATE_INVALID_DISTANCE_SYMBOL;
	}
	const std::size_t bits_needed = ((static_cast<std::size_t>(symbol) - 4) >> 1) + 1;
	std::int32_t result = INFLATE_OK;
	result = ReadDatastream(bits.data(), bits_needed, dstream);
	if (result != INFLATE_OK) return result;
	const std::array<const std::size_t, 26> distance_offsets =
	{
		5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
	};
	distance = distance_offsets[static_cast<std::size_t>(symbol) - 4] + NonHuffmanIntegerValue(bits.data(), bits_needed);
	return result;
}

static
std::int32_t
GetFixedDistanceSymbol(
	datastream&			dstream,
	std::uint32_t&	    dist
) {
	std::array<std::uint8_t, INFLATE_FIXED_DISTANCE_BIT_LENGTH> bits;
	std::int32_t result = INFLATE_OK;
	result = ReadDatastream(bits.data(), INFLATE_FIXED_DISTANCE_BIT_LENGTH, dstream);
	if (result != INFLATE_OK) return result;
	dist = HuffmanIntegerValue(bits.data(), INFLATE_FIXED_DISTANCE_BIT_LENGTH);
	return INFLATE_OK;
}



static
std::int32_t
DecodeFixedBlock(
	datastream&				   dstream,
	sliding_window&			   window,
	std::vector<std::uint8_t>& istream
) {
	std::uint32_t literal_length_symbol = 0;
	std::size_t length;
	std::uint32_t distance_symbol;
	std::size_t distance;
	std::int32_t result = INFLATE_OK;
	do
	{
		result = GetFixedLiteralLengthSymbol(dstream, literal_length_symbol);
		if (result != INFLATE_OK) return result;
		result = ParseLiteralLengthSymbol(dstream, window, istream, literal_length_symbol, length);
		if (result == INFLATE_OK)
		{
			result = GetFixedDistanceSymbol(dstream, distance_symbol);
			if (result != INFLATE_OK) return result;
			result = ParseDistanceSymbol(dstream, distance_symbol, distance);
			if (result != INFLATE_OK) return result;
			std::vector<std::uint8_t> duplicated_string;
			result = GetDuplicatedStringFromSlidingWindow(window, length, distance, duplicated_string);
			if (result != INFLATE_OK) return result;
			istream.insert(istream.end(), duplicated_string.begin(), duplicated_string.end());
			AppendDuplicatedStringToSlidingWindow(window, duplicated_string);
		}
		else if (result == INFLATE_FALSE)
		{
			continue;
		}
		else if (result != INFLATE_EOF)
		{
			return result;
		}
		else
			continue;
	} while (literal_length_symbol != INFLATE_EOB);
	
	
	
	return result;
}

static
std::uint32_t
Adler32ComputeChecksum(
	const std::vector<std::uint8_t>& istream
) {
	std::uint32_t s1{ 1 };
	std::uint32_t s2{ 0 };
	for (std::size_t i = 0; i < istream.size(); i++)
	{
		s1 = (s1 + istream[i]) % ADLER32_MODULO;
		s2 = (s2 + s1) % ADLER32_MODULO;
	}

	return (s2 << 16) | s1;
}

static
std::int32_t
Adler32CompareChecksum(
	datastream&						 dstream,
	const std::vector<std::uint8_t>& istream
) {
	std::int32_t result = INFLATE_OK;
	result = AlignPointer(dstream);
	if (result != INFLATE_OK) return result;

	const std::uintptr_t index = dstream.datastream_ptr >> 3;

	if (index + 4 != dstream.datastream_size)
	{
		return INFLATE_FINAL_BLOCK_MISPLACED;
	}
	else
	{
		const std::uint32_t checksum = EndianSwap(*(reinterpret_cast<const std::uint32_t*>(dstream.data + index)));
		if (checksum != Adler32ComputeChecksum(istream))
		{
			return INFLATE_DATA_INTEGRITY_FAIL;
		}
		else
		{
			return result;
		}
	}
}

static
std::int32_t
ParseAndGetUncompressedData(
	datastream&				   dstream,
	std::vector<std::uint8_t>& inflated
) {
	std::int32_t result = INFLATE_OK;
	result = AlignPointer(dstream);
	const std::uintptr_t index = dstream.datastream_ptr >> 3;
	if (index + 4 < dstream.datastream_size)
	{
		std::array<std::uint16_t, 2> data;
		data[0] = EndianSwap(*(reinterpret_cast<const std::uint16_t*>(dstream.data + index)));
		data[1] = EndianSwap(*(reinterpret_cast<const std::uint16_t*>(dstream.data + index + 2)));
		if (~data[0] != data[1])
		{
			return INFLATE_UNCOMPRESSED_LENGTH_MISMATCH;
		}
		else
		{
			if (index + 4 + data[0] < dstream.datastream_size)
			{
				inflated.insert(inflated.end(), dstream.data + index + 4, dstream.data + index + 4 + data[0]);
				return result;
			}
			else
			{
				return INFLATE_MEMORY_ACCESS_FAIL;
			}
		}
	}
	else
	{
		return INFLATE_FINAL_BLOCK_MISPLACED;
	}
}

static
std::int32_t
ParseDynamicHeader(
	datastream&				dstream,
	std::uint32_t&			hlit,
	std::uint32_t&			hdist,
	std::uint32_t&			hclen
) {
	std::int32_t result = INFLATE_OK;
	std::array<std::uint8_t, 5> bits;
	result = ReadDatastream(bits.data(), bits.size(), dstream);
	if (result != INFLATE_OK) return result;
	hlit = NonHuffmanIntegerValue(bits.data(), bits.size());
	result = ReadDatastream(bits.data(), bits.size(), dstream);
	if (result != INFLATE_OK) return result;
	hdist = NonHuffmanIntegerValue(bits.data(), bits.size());
	if (result != INFLATE_OK) return result;
	result = ReadDatastream(bits.data(), bits.size() - 1, dstream);
	if (result != INFLATE_OK) return result;
	hclen = NonHuffmanIntegerValue(bits.data(), bits.size() - 1);
	hlit += 257;
	hdist += 1;
	hclen += 4;
	return result;
}

static
std::int32_t
GetCodeLengthBitLength(
	datastream&						dstream,
	const std::uint32_t             hclen,
	std::vector<dynamic_alphabet>& alphabet
) {
	std::int32_t result = INFLATE_OK;
	const std::array<const std::uint32_t, 19> code_length_alphabet =
	{
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	for (std::size_t i = 0; i < hclen; i++)
	{
		std::array<std::uint8_t, 3> bits;
		result = ReadDatastream(bits.data(), bits.size(), dstream);
		if (result != INFLATE_OK) return result;
		const std::size_t blen = static_cast<std::size_t>(NonHuffmanIntegerValue(bits.data(), bits.size()));
		if (blen == 0)
			continue;
		bool found = false;
		for (std::size_t j = 0; j < alphabet.size(); j++)
		{
			if (alphabet[j].blen == blen)
			{
				found = true;
				alphabet[j].symbols.push_back(code_length_alphabet[i]);
				break;
			}
		}
		if (found == false)
		{
			dynamic_alphabet entry;
			entry.blen = blen;
			entry.symbols.push_back(code_length_alphabet[i]);
			alphabet.push_back(entry);
		}
	}
	return result;
}

static
void
PadDynamicAlphabet(
	std::vector<dynamic_alphabet>& alphabet,
	const std::size_t			   max_blen
) {
	dynamic_alphabet p;
	p.blen = 0;
	std::size_t diff = (max_blen + 1) - alphabet.size();
	alphabet.insert(alphabet.begin(), diff, dynamic_alphabet());
	for (std::size_t i = 1; i < max_blen; i++)
	{
		if (alphabet[i].blen != i)
		{
			if (alphabet[i].blen == 0)
				alphabet[i].blen = i;
			else
			{
				for (std::size_t j = 1; j < alphabet.size(); j++)
				{
					if (alphabet[j].blen == i)
					{
						std::swap(alphabet[i], alphabet[j]);
						break;
					}
				}
			}
		}
	}
}

static
void
AssignHuffmanCodesFromSymbolsAndBitLengths(
	std::vector<dynamic_alphabet>&	alphabet
) {
	std::vector<std::uint32_t> minium_codes;
	std::uint32_t code = 0;
	std::sort(alphabet.begin(), alphabet.end());
	PadDynamicAlphabet(alphabet, alphabet.back().blen);
	minium_codes.resize(alphabet.size() + 1);
	for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(alphabet.size()); i++)
	{
		if (alphabet[static_cast<std::size_t>(i) - 1].symbols.size() != 0)
			std::sort(alphabet[static_cast<std::size_t>(i)-1].symbols.begin(), alphabet[static_cast<std::size_t>(i)-1].symbols.end());
		const std::uint32_t size = static_cast<std::uint32_t>(alphabet[static_cast<std::size_t>(i) - 1].symbols.size());
		code = (code + size) << 1;
		minium_codes[i] = code;
	}

	for (std::size_t i = 0; i < alphabet.size(); i++)
	{
		if (alphabet[i].symbols.size() != 0)
		{
			alphabet[i].codes.resize(alphabet[i].symbols.size());
			for (std::size_t j = 0; j < alphabet[i].symbols.size(); j++)
			{
				alphabet[i].codes[j] = minium_codes[i]++;
			}
		}
	}
	for (std::size_t i = 0; i < alphabet.size();)
	{
		if (alphabet[i].blen == 0 || alphabet[i].symbols.size() == 0)
		{
			alphabet.erase(alphabet.begin() + i);
		}
		else
			i++;
	}
}

static
std::int32_t
GetDynamicSymbolCodeLength(
	datastream& dstream,
	std::size_t& previous_length,
	std::size_t& current_length,
	const std::uint32_t   symbol
) {
	std::int32_t result = INFLATE_OK;
	if (symbol <= 15)
	{
		previous_length = symbol;
		current_length = symbol;
		return result;
	}
	else if (symbol == 16)
	{
		std::array<std::uint8_t, 2> bits;
		result = ReadDatastream(bits.data(), bits.size(), dstream);
		if (result != INFLATE_OK) return result;
		if (previous_length == std::numeric_limits<std::size_t>::max())
			return INFLATE_REPEAT_CODE_FAIL;
		current_length = previous_length * static_cast<std::size_t>(3 + (NonHuffmanIntegerValue(bits.data(), bits.size())));
		return result;
	}
	else if (symbol == 17)
	{
		std::array<std::uint8_t, 3> bits;
		result = ReadDatastream(bits.data(), bits.size(), dstream);
		if (result != INFLATE_OK) return result;
		current_length = static_cast<std::size_t>(3) + NonHuffmanIntegerValue(bits.data(), bits.size());
		if (current_length > 10)
		{
			return INFLATE_INVALID_LENGTH;
		}
		return result;
	}
	else if (symbol == 18)
	{
		std::array<std::uint8_t, 7> bits;
		result = ReadDatastream(bits.data(), bits.size(), dstream);
		if (result != INFLATE_OK) return result;
		current_length = static_cast<std::size_t>(11) + NonHuffmanIntegerValue(bits.data(), bits.size());
		if (current_length > 138)
		{
			return INFLATE_INVALID_LENGTH;
		}
		return result;
	}
	else
	{
		return INFLATE_UNKNOWN_SYMBOL;
	}
}

static
std::int32_t
DynamicGetSymbol(
	datastream&							 dstream,
	const std::vector<dynamic_alphabet>& alphabet,
	std::uint32_t&						 symbol,
	std::vector<std::uint8_t>&			 bits
) {
	std::int32_t result = INFLATE_OK;
	std::uint32_t psymbol = 0;
	std::size_t previous_blen = 0;
	std::uint32_t value = 0;
	for (std::size_t i = 0; i < alphabet.size(); i++)
	{
		const std::size_t offset = alphabet[i].blen - previous_blen;
		result = ReadDatastream(bits.data() + previous_blen, offset, dstream);
		value = (value << alphabet[i].blen - previous_blen) + HuffmanIntegerValue(bits.data() + previous_blen, offset);
		for (std::size_t j = 0; j < alphabet[i].symbols.size(); j++)
		{
			if (value == alphabet[i].codes[j])
			{
				symbol = alphabet[i].symbols[j];
				return result;
			}
		}
		previous_blen = alphabet[i].blen;
	}
	return INFLATE_SYMBOL_NOT_FOUND;
}

static
std::int32_t
GetDynamicBlockHuffmanCodes(
	datastream&									  dstream,
	std::vector<dynamic_alphabet>&				  code_length_alphabet,
	const std::uint32_t							  hlit,
	const std::uint32_t							  hdist,
	std::array<std::vector<dynamic_alphabet>, 2>& alphabet
) {
	std::vector<std::uint8_t> bits;
	bits.resize(code_length_alphabet.back().blen);
	std::int32_t result = INFLATE_OK;
	std::size_t previous_length = std::numeric_limits<std::size_t>::max();
	std::size_t current_length = 0;
	for (std::uint32_t i = 0; i < hlit + hdist;)
	{
		std::uint32_t symbol;
		result = DynamicGetSymbol(dstream, code_length_alphabet, symbol, bits);
		if (result != INFLATE_OK) return result;

		result = GetDynamicSymbolCodeLength(dstream, previous_length, current_length, symbol);
		if (result != INFLATE_OK) return result;

		std::size_t index;
		std::uint32_t letter;

		if (i < hlit)
		{
			letter = i;
			index = 0;
		}
		else
		{
			letter = i - hlit;
			index = 1;
		}

		if (current_length != 0)
		{
			if (symbol == 17 || symbol == 18)
			{
				i += (current_length);
			}
			else
			{
				bool found = false;
				for (std::size_t j = 0; j < alphabet[index].size(); j++)
				{
					if (current_length == alphabet[index][j].blen)
					{
						alphabet[index][j].symbols.push_back(letter);
						found = true;
						break;
					}
				}
				if (found == false)
				{
					dynamic_alphabet palphabet;
					palphabet.blen = current_length;
					palphabet.symbols.push_back(letter);
					alphabet[index].push_back(palphabet);
				}
				i++;
			}
		}
		else
			i++;
	}
	for (auto& i : alphabet)
	{
		AssignHuffmanCodesFromSymbolsAndBitLengths(i);
	}
	return result;
}

static
std::int32_t
ParseDynamicBlock(
	datastream&				   dstream,
	sliding_window&			   window,
	std::vector<std::uint8_t>& istream,
	const std::vector<dynamic_alphabet>& literal_length_alphabet,
	const std::vector<dynamic_alphabet>& distance_alphabet
) {
	std::uint32_t lng_symbol{ 0 };
	std::size_t lng{ 0 };
	std::uint32_t dist_symbol{ 0 };
	std::size_t dist{ 0 };
	std::int32_t result = INFLATE_OK;
	std::vector<std::uint8_t> literal_length_bits;
	literal_length_bits.resize(literal_length_alphabet.back().blen);
	std::vector<std::uint8_t> distance_bits;
	distance_bits.resize(distance_alphabet.back().blen);
	do
	{
		result = DynamicGetSymbol(dstream, literal_length_alphabet, lng_symbol, literal_length_bits);
		if (result != INFLATE_OK) return result;
		result = ParseLiteralLengthSymbol(dstream, window, istream, lng_symbol, lng);
		if (result == INFLATE_OK)
		{
			result = DynamicGetSymbol(dstream, distance_alphabet, dist_symbol, distance_bits);
			if (result != INFLATE_OK) return result;
			result = ParseDistanceSymbol(dstream, dist_symbol, dist);
			if (result != INFLATE_OK) return result;
			std::vector<std::uint8_t> duplicated_string;
			result = GetDuplicatedStringFromSlidingWindow(window, lng, dist, duplicated_string);
			if (result != INFLATE_OK) return result;
			istream.insert(istream.end(), duplicated_string.begin(), duplicated_string.end());
			AppendDuplicatedStringToSlidingWindow(window, duplicated_string);
		}
		else if (result == INFLATE_FALSE)
		{
			continue;
		}
		else if (result != INFLATE_EOB)
		{
			return result;
		}
		else
		{
			continue;
		}
	} while (lng_symbol != INFLATE_EOB);
	return result;
}


std::int32_t
InflateDatastream(
	const std::uint8_t* const  deflated,
	const std::size_t		   deflated_size,
	const std::uint8_t* const  dictionary,
	const std::size_t		   dictionary_size,
	const std::size_t		   window_size,
	std::vector<std::uint8_t>& inflated
) {
	datastream dstream(deflated, deflated_size);
	bool bfinal;
	std::int32_t result = INFLATE_OK;
	result = VerifyWindowSize(window_size);
	if (result != INFLATE_OK) return result;
	sliding_window window(new std::uint8_t[window_size], window_size);
	std::vector<std::uint8_t> istream;
	do
	{
		std::uint8_t btype;
		result = GetBlockType(
			dstream,
			bfinal,
			btype
		);
		if (result != INFLATE_OK) return result;
		switch (btype)
		{
		case INFLATE_UNCOMPRESSED:
			result = ParseAndGetUncompressedData(dstream, inflated);
			break;
		case INFLATE_FIXED:
			result = DecodeFixedBlock(dstream, window, istream);
			if (result != INFLATE_EOB) return result;
			break;
		case INFLATE_DYNAMIC:
		{
			std::int32_t result = INFLATE_OK;
			std::uint32_t hlit;
			std::uint32_t hdist;
			std::uint32_t hclen;
			result = ParseDynamicHeader(
				dstream,
				hlit,
				hdist,
				hclen
			);
			if (result != INFLATE_OK) return result;
			std::vector<dynamic_alphabet> code_length_alphabet;
			std::array<std::vector<dynamic_alphabet>, 2> alphabets;
			result = GetCodeLengthBitLength(
				dstream,
				hclen,
				code_length_alphabet
			);
			if (result != INFLATE_OK) return result;
			AssignHuffmanCodesFromSymbolsAndBitLengths(code_length_alphabet);
			result = GetDynamicBlockHuffmanCodes(
				dstream,
				code_length_alphabet,
				hlit,
				hdist,
				alphabets
			);
			if (result != INFLATE_OK) return result;
			result = ParseDynamicBlock(
				dstream,
				window,
				istream,
				alphabets[0],
				alphabets[1]
			);
			if (result != INFLATE_OK && result != INFLATE_EOB) return result;
			break;
		}
		case INFLATE_RESERVED:
			result = INFLATE_INVALID_BLOCK;
			break;
		}
	} while (bfinal != true);
	result = Adler32CompareChecksum(dstream, istream);
	if (result != INFLATE_OK) return result;
	inflated.swap(istream);
	return result;
}

