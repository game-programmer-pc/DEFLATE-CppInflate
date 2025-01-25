#include "inflate.hpp"
#include <array>
#include <cstring>
#include <memory>
#include <algorithm>
#include <map>
#include <cmath>
#include <bit>
#include <cassert>

#define INFLATE_NO_COMPRESSION_BLOCK_ID         (0x00)
#define INFLATE_FIXED_COMPRESSION_BLOCK_ID      (0x01)
#define INFLATE_DYNAMIC_COMPRESSION_BLOCK_ID    (0x02)
#define INFLATE_RESERVED_COMPRESSION_BLOCK_ID   (0x03)

#define INFLATE_CODE_TYPE_LITERAL_LENGTH        (0)
#define INFLATE_CODE_TYPE_DISTANCE              (1)

static
std::uint32_t
InflateAdler32ComputeChecksum(
    const std::uint8_t*     inflated_datastream,
    const std::size_t       inflated_datastream_size
) {
    std::uint32_t s1 = 1;
    std::uint32_t s2 = 0;
    for (std::size_t i = 0; i < inflated_datastream_size; i++)
    {
        s1 = (s1 + inflated_datastream[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return (s2 << 16) | s1;
}

template <class T>
static T InflateReverseEndian(
    T value
) {
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

struct dynamic_alphabet {
    std::uint32_t blen = 0;
    std::vector<std::uint32_t> symbols;
    std::vector<std::uint32_t> symbol_codes;
    constexpr bool operator<(const dynamic_alphabet& operand) const noexcept {
        return this->blen < operand.blen;
    }
};

struct sliding_window {
    sliding_window(std::uint8_t* dynamic_allocated_memory, const std::size_t size) : size(size), ptr(0), window(dynamic_allocated_memory) {}
    std::unique_ptr<std::uint8_t[]> window;
    const std::size_t size;
    std::uintptr_t    ptr;
};

struct datastream {
    constexpr datastream(const std::uint8_t* data, const std::size_t size) : cstream(data), size(size), ptr(0) {}
    const std::uint8_t* cstream;
    const std::size_t   size;
    std::uintptr_t      ptr;
};

struct huffman_alphabet {
    std::map<std::uint32_t, std::uint32_t> alphabet;
    std::size_t minimum_blen;
    std::size_t maximum_blen;
};

struct huffman_codes {
    huffman_alphabet literal_length;
    huffman_alphabet distance;
};

static
std::int32_t
InflateGetDeflatedDatastreamIndex(
    datastream& dstream,
    std::uint8_t& bit
) {
    std::uintptr_t quotient = dstream.ptr >> 0x3;
    std::uintptr_t remainder = dstream.ptr & 0x7;
    if (quotient < dstream.size)
    {
        bit = (dstream.cstream[quotient] >> remainder) & 0x1;
        dstream.ptr++;
        return INFLATE_OK;
    }
    else
    {
        return INFLATE_MEMORY_ACCESS_FAIL;
    }
}

static
std::int32_t
InflateReadDatastreamBits(
    datastream& dstream,
    std::uint8_t* bit_stream,
    const std::size_t           bit_stream_size
) {
    std::int32_t result = INFLATE_OK;
    for (std::size_t i = 0; i < bit_stream_size; i++)
    {
        result = InflateGetDeflatedDatastreamIndex(dstream, bit_stream[i]);
        if (result != INFLATE_OK) return result;
    }
    return result;
}

static
std::uint32_t
InflateConcatenateNonHuffmanBits(
    const std::uint8_t* bits,
    const std::size_t           bit_count
) {

    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bit_count; i++)
    {
        value |= bits[i] << i;
    }
    return value;
}

static
std::uint32_t
InflateConcatenateHuffmanBits(
    const std::uint8_t* bits,
    const std::size_t          bit_count
) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bit_count; i++)
    {
        value = (value << 1) | bits[i];
    }
    return value;
}

static 
void
InflateAddLiteralByteToSlidingWindow(
    sliding_window&                window,
    const std::uint8_t             byte
) {
    window.window[window.ptr++] = byte;
    if (window.ptr >= window.size)
        window.ptr = 0;
}

static
std::int32_t
InflateGetNonHuffmanIntegerValue(
    datastream&                    dstream,
    std::uint8_t*                  bstream,
    const std::size_t              bstream_size,
    std::uint32_t&                 value
) {
    std::int32_t result = INFLATE_OK;
    result = InflateReadDatastreamBits(dstream, bstream, bstream_size);
    if (result != INFLATE_OK) return result;
    value = InflateConcatenateNonHuffmanBits(bstream, bstream_size);
    return result;
}

static
void
InflateAppendDuplicatedStringToSlidingWindow(
    sliding_window&                  window,
    const std::vector<std::uint8_t>& duplicated_string
) {
    if (duplicated_string.size() + window.ptr < window.size)
    {
        memcpy(window.window.get() + window.ptr, duplicated_string.data(), sizeof(std::uint8_t) * duplicated_string.size());
        window.ptr += duplicated_string.size();
    }
    else
    {
        std::size_t dist_to_end = window.size - window.ptr;
        std::size_t offset_next = duplicated_string.size() - dist_to_end;
        memcpy(window.window.get() + window.ptr, duplicated_string.data(), sizeof(std::uint8_t) * dist_to_end);
        window.ptr = 0;
        memcpy(window.window.get() + window.ptr, duplicated_string.data() + dist_to_end, sizeof(std::uint8_t) * offset_next);
        window.ptr += offset_next;
    }
    if (window.ptr >= window.size)
        window.ptr -= window.size;
}

static
std::int32_t
InflateGetDuplicatedStringFromSlidingWindow(
    const sliding_window&       window,
    const std::uint32_t         length,
    const std::uint32_t         distance,
    std::vector<std::uint8_t>&  duplicated_string
) {
    std::uint32_t read_begin = static_cast<std::uint32_t>(window.ptr) - distance;

    if (distance > window.size)
        return INFLATE_INVALID_DISTANCE;

    if (distance > length)
    {

        std::uint32_t read_end = read_begin + length;
        if (read_end < window.size)
            duplicated_string.insert(duplicated_string.end(), window.window.get() + read_begin, window.window.get() + read_begin + length);
        else
        {
            std::uint32_t read_slide_end = static_cast<std::uint32_t>(window.size) - read_begin;
            std::uint32_t diff = length - read_slide_end;
            duplicated_string.insert(duplicated_string.end(), window.window.get() + read_begin, window.window.get() + read_begin + read_slide_end);
            duplicated_string.insert(duplicated_string.end(), window.window.get(), window.window.get() + read_slide_end);
        }
    }
    else 
    {
        const std::size_t quotient = length / distance;
        const std::size_t remainder = length % distance;
        if (static_cast<std::uintptr_t>(read_begin) + distance > window.size)
        {
            
            std::size_t low = window.size - read_begin;
            std::size_t hi = distance - low;
            for (std::size_t i = 0; i < quotient; i++)
            {
                duplicated_string.insert(duplicated_string.end(), window.window.get() + read_begin, window.window.get() + read_begin + low);
                duplicated_string.insert(duplicated_string.end(), window.window.get(), window.window.get() + hi);
            }

            if (remainder)
            {
                if (read_begin + remainder > window.size)
                {
                    hi = remainder - low;
                    duplicated_string.insert(duplicated_string.end(), window.window.get() + read_begin, window.window.get() + read_begin + low);
                    duplicated_string.insert(duplicated_string.end(), window.window.get(), window.window.get() + hi);
                }
                else
                {
                    duplicated_string.insert(duplicated_string.end(), window.window.get() + read_begin, window.window.get() + read_begin + remainder);
                }
            }
        }
        else
        {
            for (std::size_t i = 0; i < quotient; i++)
            {
                duplicated_string.insert(duplicated_string.end(), window.window.get() + read_begin, window.window.get() + read_begin + distance);
            }
            if (remainder)
            {
                duplicated_string.insert(duplicated_string.end(), window.window.get() + read_begin, window.window.get() + read_begin + remainder);
            }
        }
    }
    return INFLATE_OK;
}


static
void
InflateDynamicSortByBitLength(
    std::vector <dynamic_alphabet>& alphabet
) {
    std::sort(alphabet.begin(), alphabet.end());
}

static
std::int32_t
InflateAddPresetDictionaryToSlidingWindowIfPresent(
    const std::uint8_t*     preset_dictionary_datastream,
    const std::size_t       preset_dictionary_size,
    sliding_window&         window
) {
    if (preset_dictionary_datastream != nullptr)
    {
        if (preset_dictionary_size > window.size)
            return INFLATE_INVALID_DICTIONARY_SIZE;
        memcpy(window.window.get(), preset_dictionary_datastream, sizeof(std::uint8_t) * preset_dictionary_size);
        window.ptr += preset_dictionary_size;
        return INFLATE_OK;
    }
    else
    {
        return INFLATE_FALSE;
    }
}

static 
std::int32_t
InflateGetBlockType(
    datastream&             dstream,
    std::uint8_t&           bfinal,
    std::uint8_t&           btype
) {
    std::int32_t result = InflateGetDeflatedDatastreamIndex(dstream, bfinal);
    if (result != INFLATE_OK) return result;
    std::array<std::uint8_t, 2> bits;
    result = InflateReadDatastreamBits(dstream, bits.data(), bits.size());
    if (result != INFLATE_OK) return result;
    btype = InflateConcatenateNonHuffmanBits(bits.data(), bits.size());
    return INFLATE_OK;
}

static 
std::int32_t
InflateAlignPointer(
    datastream&         dstream
) {
    std::uintptr_t alignment = ((dstream.ptr + 7) & ~7) >> 3;
    if (alignment < dstream.size)
    {
        dstream.ptr = alignment;
        return INFLATE_OK;
    }
    else
        return INFLATE_FALSE;
}

static
std::int32_t
InflateFixedParseDistanceSymbol(
    datastream&           dstream,
    const std::uint32_t   distance_symbol,
    std::uint32_t&        distance
) {
    std::int32_t result = INFLATE_OK;
    const std::array<const std::uint32_t, 30> distance_offsets =
    {
        1, 2, 3, 4,5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };

    if (distance_symbol >= 0 && distance_symbol <= 3)
    {
        distance = distance_offsets[distance_symbol];
        return result;
    }
    std::array<std::uint8_t, 13> extra_bits;
    std::uint32_t bits_needed = ((distance_symbol - 4) >> 1) + 1;
    result = InflateReadDatastreamBits(dstream, extra_bits.data(), bits_needed);
    if (result != INFLATE_OK) return result;
    std::uint32_t computed_distance = InflateConcatenateNonHuffmanBits(extra_bits.data(), bits_needed);
    distance = distance_offsets[static_cast<std::size_t>(distance_symbol)] + computed_distance;
    return result;
}


static 
void
InflateDynamicAddBitLengthToDynamicAlphabet(
    const std::size_t length,
    std::vector<dynamic_alphabet>& alphabet
) {
    dynamic_alphabet dalphabet;
    dalphabet.blen = static_cast<std::uint32_t>(length);
    alphabet.push_back(dalphabet);
}

template <typename T>
static
void
InflateDeallocateVector(
    T& vector
) {
    T empty{};
    vector.swap(empty);
}

static
void 
InflateDynamicAddDummyBitLengths(
    std::vector<dynamic_alphabet>& alphabet
) {
    std::size_t min = 0;
    std::size_t max = alphabet.back().blen;

    for (std::size_t i = min; i < max; i++)
    {
        bool found = false;
        for (std::size_t j = 0; j < alphabet.size(); j++)
        {
            if (i == alphabet[j].blen)
            {
                found = true;
                break;
            }
        }
        if (found != true)
        {
            dynamic_alphabet dummy;
            dummy.blen = static_cast<std::uint32_t>(i);
            alphabet.push_back(dummy);
        }
    }
    InflateDynamicSortByBitLength(alphabet);
}


static 
std::int32_t
InflateGetUncompressedData(
    datastream&                     dstream,
    std::vector<std::uint8_t>&      inflated_stream
) {
    std::int32_t result = InflateAlignPointer(dstream);
    if (result != INFLATE_OK) return result;
    std::uintptr_t byte_index = dstream.ptr >> 3;
    if (byte_index + 3 < dstream.size)
    {
        std::uint32_t len = *((const std::uint32_t*)dstream.cstream + byte_index);
        byte_index += 3;
        if (len == 0 || len + byte_index >= dstream.size || (~(len & 0xFF) != (len & 0xFF00)))
        {
            return INFLATE_UNCOMPRESSED_BLOCK_INVALID_LENGTH;
        }
        else
        {
            inflated_stream.insert(inflated_stream.end(), dstream.cstream + byte_index, dstream.cstream + byte_index + (len & 0xFF));
            return INFLATE_OK;
        }
    }
    return result;
}

static
std::int32_t
InflateFixedParseLengthSymbol(
    datastream&                 dstream,
    const std::uint32_t         symbol,
    std::vector<std::uint8_t>&  istream,
    sliding_window&             window,
    std::uint32_t&              length
) {
    std::int32_t result = INFLATE_OK;
    assert(symbol != 286 && symbol != 287);

    if (symbol < 256)
    {
        InflateAddLiteralByteToSlidingWindow(window, static_cast<std::uint8_t>(symbol));
        istream.push_back(symbol);
        result = INFLATE_FALSE;
        return result;
    }
    else
    {
        const std::array<const std::uint32_t, 30> length_offsets =
        {
            0,3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
        };
        length = length_offsets[static_cast<std::size_t>(symbol) - 256];
        if (symbol >= 256 && symbol <= 264 || symbol == 285)
        {
            return INFLATE_OK;
        }
        else
        {
            std::array<std::uint8_t, 5> extra_bits;
            std::size_t bits_needed = ((static_cast<std::size_t>(symbol) - 264) >> 2) + 1;
            result = InflateReadDatastreamBits(dstream, extra_bits.data(), extra_bits.size());
            if (result != INFLATE_OK) return result;
            length += InflateConcatenateHuffmanBits(extra_bits.data(), bits_needed);
            return result;
        }
    }
}

static 
void
InflateFixedHuffmanGetSymbols(
    huffman_codes& alphabet
) {
    for (std::uint32_t i = 0; i < 30; i++)
    {
        alphabet.distance.alphabet[i] = i;
    }
    for (std::uint32_t i = 0; i < 24; i++)
    {
        alphabet.literal_length.alphabet[i] = i + 256;
    }
    for (std::uint32_t i = 48; i < 192; i++)
    {
        alphabet.literal_length.alphabet[i] = i - 48;
    }
    for (std::uint32_t i = 192; i < 200; i++)
    {
        alphabet.literal_length.alphabet[i] = i + 88;
    }
    for (std::uint32_t i = 400; i < 512; i++)
    {
        alphabet.literal_length.alphabet[i] = i - 256;
    }
    alphabet.distance.minimum_blen = 5;
    alphabet.distance.maximum_blen = 5;
    alphabet.literal_length.minimum_blen = 7;
    alphabet.literal_length.maximum_blen = 9;
}

static
void
InflateDynamicAlphabetAssignCodes(
    std::vector<dynamic_alphabet>& alphabet
) {
    std::uint32_t code = 0;
    std::uint32_t next_code = 0;
    for (std::size_t i = 1; i <= alphabet.back().blen; i++)
    {
        next_code <<= 1;
        std::uint32_t start_bit = 1 << i;
        for (std::size_t j = 0; j < alphabet[i].symbols.size(); j++)
        {
            alphabet[i].symbol_codes.resize(alphabet[i].symbols.size());
            alphabet[i].symbol_codes[j] = next_code | start_bit;
            next_code++;
        }
    }
}

static
void
InflateDynamicSortSymbolsByBitLength(
    std::vector<dynamic_alphabet>& alphabet
) {
    for (std::size_t i = 0; i < alphabet.size(); i++)
    {
        std::sort(alphabet[i].symbols.begin(), alphabet[i].symbols.end());
    }
}

static
void
InflateDynamicEraseUnassignedBitLengths(
    std::vector<dynamic_alphabet>& alphabet
) {
    for (std::size_t i = 0; i < alphabet.size();)
    {
        if (alphabet[i].symbols.size() == 0)
        {
            alphabet.erase(alphabet.begin() + i);
        }
        else
            i++;
    }
}

static
void
InflateDynamicConvertAssignedCodesToMapContainer(
    const std::vector<dynamic_alphabet>& alphabet,
    std::map<std::uint32_t, std::uint32_t>& key_values 
) {
    std::size_t kiter = 0;
    for (std::size_t i = 0; i < alphabet.size(); i++)
    {
        for (std::size_t j = 0; j < alphabet[i].symbols.size(); j++)
        {
            key_values[alphabet[i].symbol_codes[j]] = alphabet[i].symbols[j];
        }
    }
}

static
void
InflateDynamicCompleteExchange(
    std::vector<dynamic_alphabet>& alphabet,
    huffman_alphabet&              lookup
) {
    InflateDynamicSortByBitLength(alphabet);
    InflateDynamicAlphabetAssignCodes(alphabet);
    InflateDynamicSortSymbolsByBitLength(alphabet);
    std::size_t maximum = 0;
    for (std::size_t i = 0; i < alphabet.size(); i++)
    {
        if (alphabet[i].symbols.size() != 0)
            maximum = i;
    }
    lookup.maximum_blen = static_cast<std::size_t>(std::log2(alphabet[maximum].symbol_codes.back())) + 1;
    InflateDynamicConvertAssignedCodesToMapContainer(alphabet, lookup.alphabet);
    InflateDeallocateVector(alphabet);
    lookup.minimum_blen = 1;
}

static
std::int32_t
InflateDynamicCodeLengthDecodeForSymbol(
    datastream& dstream,
    const huffman_alphabet& code_length_alphabet,
    std::uint32_t& previous_length,
    std::vector<std::uint32_t>& lengths
) {
    std::int32_t result = INFLATE_OK;
    std::uint32_t code = 1;
    std::uint32_t current_length = static_cast<std::uint32_t>(~0);
    for (std::size_t i = 0; i < code_length_alphabet.maximum_blen; i++)
    {
        std::uint8_t bit;
        result = InflateGetDeflatedDatastreamIndex(dstream, bit);
        if (result != INFLATE_OK) return result;
        code = (code << 1) | bit;
        auto iterator = code_length_alphabet.alphabet.find(code);
        if (iterator != code_length_alphabet.alphabet.end())
        {
            current_length = iterator->second;
            break;
        }
    }

    if (current_length == static_cast<std::uint32_t>(~0))
    {
        return INFLATE_SYMBOL_NOT_FOUND;
    }

    if (current_length >= 0 && current_length <= 15)
    {
        lengths.push_back(current_length);
        previous_length = current_length;
    }
    else if (current_length == 16)
    {
        if (previous_length == static_cast<std::uint32_t>(~0))
        {
            result = INFLATE_INVALID_REPEAT_CODE;
            return result;
        }
        else
        {
            std::array<std::uint8_t, 2> bits{};
            result = InflateReadDatastreamBits(dstream, bits.data(), bits.size());
            if (result != INFLATE_OK) return result;
            current_length = previous_length * (3 + InflateConcatenateNonHuffmanBits(bits.data(), bits.size()));
            lengths.push_back(current_length);
        }
    }
    else if (current_length == 17)
    {
        std::array<std::uint8_t, 3> bits;
        std::uint32_t bit_value;
        result = InflateGetNonHuffmanIntegerValue(dstream, bits.data(), bits.size(), bit_value);
        if (result != INFLATE_OK) return result;
        if (bit_value == 8)
        {
            return INFLATE_INVALID_CODE_LENGTH;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(3) + bit_value; i++)
        {
            lengths.push_back(0);
            previous_length = 0;
        }
    }
    else if (current_length == 18)
    {
        std::array<std::uint8_t, 7> bits;
        std::uint32_t bit_value;
        result = InflateGetNonHuffmanIntegerValue(dstream, bits.data(), bits.size(), bit_value);
        if (result != INFLATE_OK) return result;
        for (std::size_t i = 0; i < static_cast<std::size_t>(11) + bit_value; i++)
        {
            lengths.push_back(0);
        }
    }
    else
    {
        return INFLATE_SYMBOL_NOT_FOUND;
    }

    return INFLATE_OK;
}

static
std::int32_t
InflateDynamicAssignSymbolsHuffmanCodes(
    datastream& dstream,
    const std::uint32_t                           hlit,
    const std::uint32_t                           hdist,
    const huffman_alphabet& code_length_alphabet,
    huffman_codes& codes
) {
    std::int32_t result = INFLATE_OK;
    std::uint32_t previous_code_length{ static_cast<std::uint32_t>(~0) };
    std::vector<std::uint32_t> bit_lengths;
    for (std::uint32_t i = 0; i < hlit + hdist;)
    {
        result = InflateDynamicCodeLengthDecodeForSymbol(
            dstream,
            code_length_alphabet,
            previous_code_length,
            bit_lengths
        );
        if (result != INFLATE_OK) return result;
        i = static_cast<std::uint32_t>(bit_lengths.size());
    }
    std::vector<dynamic_alphabet> pliteral_length_alphabet;

    for (std::size_t i = 0; i < hlit; i++)
    {
        std::uint32_t blen = bit_lengths[i];
        if (blen != 0)
        {
            bool found = false;
            for (std::size_t j = 0;j < pliteral_length_alphabet.size(); j++)
            {
                if (blen == pliteral_length_alphabet[j].blen)
                {
                    found = true;
                    pliteral_length_alphabet[j].symbols.push_back(static_cast<std::uint32_t>(i));
                }
            }
            if (found == false)
            {
                dynamic_alphabet dummy;
                dummy.blen = blen;
                dummy.symbols.push_back(static_cast<std::uint32_t>(i));
                pliteral_length_alphabet.push_back(dummy);
            }
        }
    }
    std::vector<dynamic_alphabet> pdistance_alphabet;
    for (std::size_t i = 0; i < hdist; i++)
    {
        std::uint32_t blen = bit_lengths[i + hlit];
        if (blen != 0)
        {
            bool found = false;
            for (std::size_t j = 0; j < pdistance_alphabet.size(); j++)
            {
                if (blen == pdistance_alphabet[j].blen)
                {
                    found = true;
                    pdistance_alphabet[j].symbols.push_back(static_cast<std::uint32_t>(i));
                }
            }
            if (found == false)
            {
                dynamic_alphabet dummy;
                dummy.blen = blen;
                dummy.symbols.push_back(static_cast<std::uint32_t>(i));
                pdistance_alphabet.push_back(dummy);
            }
        }
    }
    InflateDeallocateVector(bit_lengths);
    InflateDynamicAddDummyBitLengths(pliteral_length_alphabet);
    InflateDynamicAddDummyBitLengths(pdistance_alphabet);
    InflateDynamicCompleteExchange(pliteral_length_alphabet, codes.literal_length);
    InflateDynamicCompleteExchange(pdistance_alphabet, codes.distance);
    return result;
}

static
std::uint32_t
InflateDynamicDecodeAlphabetCodeLengthFromCodeLengthAlphabet(
    const std::vector<dynamic_alphabet>& code_length_alphabet,
    const std::size_t                       index,
    const std::uint32_t                     value
) {
    const dynamic_alphabet* vector = &code_length_alphabet[index];
    for (std::size_t i = 0; i < vector->symbol_codes.size(); i++)
    {
        if (value == vector->symbol_codes[i])
        {
            return vector->symbols[i];
        }
    }
    return static_cast<std::uint32_t>(~0);
}


static
std::int32_t
InflateDynamicHeader(
    datastream&                             dstream,
    huffman_codes&                          codes
) {
    std::int32_t result = INFLATE_OK;
    std::array<std::uint8_t, 5> bits;
    std::uint32_t hlit;
    std::uint32_t hdist;
    std::uint32_t hclen;
    result = InflateGetNonHuffmanIntegerValue(dstream, bits.data(), bits.size(), hlit);
    if (result != INFLATE_OK) return result;
    result = InflateGetNonHuffmanIntegerValue(dstream, bits.data(), bits.size(), hdist);
    if (result != INFLATE_OK) return result;
    result = InflateGetNonHuffmanIntegerValue(dstream, bits.data(), bits.size() - 1, hclen);
    if (result != INFLATE_OK) return result;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    const std::array<const std::uint32_t, 19> alphabet =
    {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    std::vector<dynamic_alphabet> symbols;    
    for (std::size_t i = 0; i <= 7; i++)
    {
        InflateDynamicAddBitLengthToDynamicAlphabet(i,symbols);
    }
    for (std::size_t i = 0; i < hclen; i++)
    {
        std::uint32_t blen;
        result = InflateGetNonHuffmanIntegerValue(dstream, bits.data(), bits.size() - 2, blen);
        if (result != INFLATE_OK) return result;
        if (blen != 0)
            symbols[blen].symbols.push_back(alphabet[i]);
    }
    huffman_alphabet code_length_alphabet;
    InflateDynamicCompleteExchange(symbols, code_length_alphabet);
    if (result != INFLATE_OK) return result;
    result = InflateDynamicAssignSymbolsHuffmanCodes(
        dstream,
        hlit,
        hdist,
        code_length_alphabet,
        codes
    );
    return result;
}


static
std::int32_t
InflateGetSymbol(
    datastream&                                   dstream,
    const std::map<std::uint32_t, std::uint32_t>& alphabet,
    const std::size_t                             min_blen,
    const std::size_t                             max_blen,
    std::uint32_t&                                symbol
) {
    std::int32_t result = INFLATE_OK;
    symbol = 1;
    for (std::size_t i = 0; i < min_blen; i++)
    {
        std::uint8_t bit;
        result = InflateGetDeflatedDatastreamIndex(dstream, bit);
        if (result != INFLATE_OK) return result;
        symbol = (symbol << 1) | bit;
    }
    auto iterator = alphabet.find(symbol);
    if (iterator != alphabet.end())
    {
        symbol = iterator->second;
        return result;
    }
    for (std::size_t i = min_blen; i < max_blen; i++)
    {
        std::uint8_t bit;
        result = InflateGetDeflatedDatastreamIndex(dstream, bit);
        if (result != INFLATE_OK) return result;
        symbol = (symbol << 1) | bit;
        
        auto iterator = alphabet.find(symbol);
        if (iterator != alphabet.end())
        {
            symbol = iterator->second;
            return result;
        }
    }
    result = INFLATE_SYMBOL_NOT_FOUND;
    return result;
}


static
std::int32_t
InflateGetHuffmanCodesFromBlock(
    const std::uint8_t         block_type,
    datastream&                dstream,
    huffman_codes&             codes,
    std::vector<std::uint8_t>& inflated_stream
) {
    std::int32_t result = INFLATE_OK;
    switch (block_type)
    {
    case INFLATE_NO_COMPRESSION_BLOCK_ID:
        result = InflateGetUncompressedData(dstream, inflated_stream);
        if (result != INFLATE_OK) return result;
        result = INFLATE_FALSE;
        return result;
        break;
    case INFLATE_FIXED_COMPRESSION_BLOCK_ID:
        InflateFixedHuffmanGetSymbols(codes);
        return result;
        break;
    case INFLATE_DYNAMIC_COMPRESSION_BLOCK_ID:
        result = InflateDynamicHeader(dstream, codes);
        return result;
        break;
    case INFLATE_RESERVED_COMPRESSION_BLOCK_ID:
        result = INFLATE_RESERVED_BLOCK;
        return result;
        break;
    default:
        result = INFLATE_RESERVED_BLOCK;
        return result;
        break;
    }
}

static
std::int32_t
InflateCompleteSlidingWindowLookupAndExchange(
    sliding_window&                          window,
    std::vector<std::uint8_t>&               istream,
    const std::uint32_t                      length,
    const std::uint32_t                      distance
) {
    std::vector<std::uint8_t> duplicated_string;
    std::int32_t result = INFLATE_OK;

    result = InflateGetDuplicatedStringFromSlidingWindow(
        window,
        length,
        distance,
        duplicated_string
    );
    if (result != INFLATE_OK) return result;

    istream.insert(istream.end(), duplicated_string.begin(), duplicated_string.end());

    InflateAppendDuplicatedStringToSlidingWindow(
        window,
        duplicated_string
    );
    return result;
}

static
std::int32_t
InflateAdler32ChecksumCompare(
    datastream&                      dstream,
    const std::vector<std::uint8_t>& inflated_datastream
) {
    std::int32_t result = INFLATE_OK;
    result = InflateAlignPointer(dstream);
    if (result != INFLATE_OK) return result;
    if (dstream.size - dstream.ptr == 4)
    {
        const std::uint32_t* adler32_address = reinterpret_cast<const std::uint32_t*>(dstream.cstream + dstream.ptr);
        std::uint32_t checksum = InflateReverseEndian(*adler32_address);
        std::uint32_t computed_checksum = InflateAdler32ComputeChecksum(
            inflated_datastream.data(),
            inflated_datastream.size()
        );

        if (computed_checksum != checksum)
        {
            result = INFLATE_DATA_INTEGRITY_FAIL;
        }
        return result;
    }
    else
    {
        result = INFLATE_FINAL_BLOCK_MISPLACED;
        return result;
    }
}

static
std::int32_t
InflateVerifyWindowSize(
    const std::uint32_t window_size
) {
    switch (window_size)
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
    }
}

static
std::int32_t
InflateParseEncodedBlock(
    datastream&                dstream,
    const huffman_codes&       codes,
    sliding_window&            window,
    std::vector<std::uint8_t>& istream
) {
    std::uint32_t literal_length_symbol = 0;
    std::uint32_t distance_symbol = 0;
    std::uint32_t length = 0;
    std::uint32_t distance = 0;
    std::int32_t result = INFLATE_OK;
    do
    {
        result = InflateGetSymbol(dstream, codes.literal_length.alphabet, codes.literal_length.minimum_blen, codes.literal_length.maximum_blen, literal_length_symbol);
        if (result != INFLATE_OK) return result;
        result = InflateFixedParseLengthSymbol(dstream, literal_length_symbol, istream, window, length);
        if (result == INFLATE_OK)
        {
            result = InflateGetSymbol(dstream, codes.distance.alphabet, codes.distance.minimum_blen, codes.distance.maximum_blen, distance_symbol);
            if (result != INFLATE_OK) return result;
            result = InflateFixedParseDistanceSymbol(dstream, distance_symbol, distance);
            if (result != INFLATE_OK) return result;
            
            result = InflateCompleteSlidingWindowLookupAndExchange(
                window,
                istream,
                length,
                distance
            );
            if (result != INFLATE_OK) return result;
        }
        else if (result == INFLATE_FALSE)
        {
            continue;
        }
        else
        {
            return result;
        }

    } while (literal_length_symbol != 256);

    result = INFLATE_OK;
    return result;
}

std::int32_t
codec::DeflateInflate(
const std::uint8_t* deflated_datastream,
const std::size_t   deflated_size,
const std::uint8_t* dictionary_data,
const std::size_t   dictionary_size,
const std::uint32_t window_size,
std::vector<std::uint8_t>& inflated_datastream
) {
    std::uint8_t bfinal{ 0 };
    std::int32_t result = INFLATE_OK;
    result = InflateVerifyWindowSize(
        window_size
    );
    if (result != INFLATE_OK) return result;
    sliding_window window(new std::uint8_t[window_size], window_size);
    datastream dstream(deflated_datastream, deflated_size);
    result = InflateAddPresetDictionaryToSlidingWindowIfPresent(
        dictionary_data,
        dictionary_size,
        window
    );
    if (result != INFLATE_OK && result != INFLATE_FALSE) return result;
    std::vector<std::uint8_t> pinflate_datastream;
    do
    {
        std::uint8_t btype;
        result = InflateGetBlockType(dstream, bfinal, btype);
        if (result != INFLATE_OK) return result;
        huffman_codes alphabet;
        result = InflateGetHuffmanCodesFromBlock(btype, dstream, alphabet, pinflate_datastream);
        if (result != INFLATE_OK) return result;
        else if (result == INFLATE_FALSE)
            continue;
        else
        {
            result = InflateParseEncodedBlock(dstream, alphabet, window, pinflate_datastream);
            if (result != INFLATE_OK) return result;
        }
    } while (bfinal == 0);
    
    result = InflateAdler32ChecksumCompare(
        dstream,
        pinflate_datastream
    );
    if (result != INFLATE_OK) return result;
    inflated_datastream = pinflate_datastream;
    return result;
}
