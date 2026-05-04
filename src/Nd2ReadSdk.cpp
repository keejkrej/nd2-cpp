#include "Nd2ReadSdk.h"

#include <algorithm>
#include <iomanip>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>

namespace
{
static constexpr uint32_t ND2_CHUNK_MAGIC = 0x0ABECEDAu;
static constexpr uint32_t JP2_MAGIC = 0x0C000000u;
static const char ND2_FILE_SIGNATURE[] = "ND2 FILE SIGNATURE CHUNK NAME01!";
static const char ND2_FILEMAP_SIGNATURE[] = "ND2 FILEMAP SIGNATURE NAME 0001!";
static const char ND2_CHUNKMAP_SIGNATURE[] = "ND2 CHUNK MAP SIGNATURE 0000001!";
static constexpr uint64_t CHUNK_ALIGNMENT = 4096u;

struct ChunkInfo
{
    uint64_t offset;
    uint64_t size;
};

struct ChunkHeader
{
    uint32_t magic;
    uint32_t nameLength;
    uint64_t dataLength;
};

struct ClxValue
{
    enum class Kind
    {
        Null,
        Bool,
        Int,
        UInt,
        Double,
        String,
        Bytes,
        Object,
        Array
    };

    Kind kind = Kind::Null;
    bool boolValue = false;
    int64_t intValue = 0;
    uint64_t uintValue = 0;
    double doubleValue = 0.0;
    std::string stringValue;
    std::vector<uint8_t> bytesValue;
    std::map<std::string, ClxValue> objectValue;
    std::vector<ClxValue> arrayValue;
};

struct LoopInfo
{
    std::string type;
    std::size_t count = 0;
    std::size_t nestingLevel = 0;
    ClxValue parameters;
};

struct Attributes
{
    uint32_t bitsInMemory = 0;
    uint32_t bitsSignificant = 0;
    uint32_t componentCount = 0;
    uint32_t heightPx = 0;
    uint32_t widthPx = 0;
    uint32_t sequenceCount = 0;
    uint32_t widthBytes = 0;
    uint32_t channelCount = 0;
    uint32_t tileWidthPx = 0;
    uint32_t tileHeightPx = 0;
    int compressionType = -1; // -1 none, 0 lossless, 1 lossy
    double compressionLevel = 0.0;
    std::string pixelDataType;
    bool hasCompressionType = false;
    bool hasCompressionLevel = false;
    bool hasTileWidth = false;
    bool hasTileHeight = false;
    bool hasWidthBytes = false;
    bool parsed = false;
    bool parseFailed = false;
    std::string parseError;
};

struct FileHandle
{
    std::string path;
    std::ifstream stream;
    uint32_t versionMajor = 0;
    uint32_t versionMinor = 0;
    uint64_t fileSize = 0;
    bool initialized = false;
    std::unordered_map<std::string, ChunkInfo> chunkMap;

    ClxValue rawAttributes;
    bool attributesCached = false;
    Attributes attributes;

    ClxValue rawExperiment;
    bool experimentCached = false;

    ClxValue rawMetadata;
    bool metadataCached = false;

    std::vector<LoopInfo> loops;
    bool loopsCached = false;

    std::vector<std::string> coordAxes;
    std::vector<std::size_t> coordShape;
    bool coordCacheReady = false;
};

static FileHandle* asHandle(LIMFILEHANDLE hFile)
{
    return static_cast<FileHandle*>(hFile);
}

static std::string toUtf8(LIMCWSTR widePath)
{
    if (!widePath)
    {
        return {};
    }

    std::string out;
    out.reserve(std::char_traits<wchar_t>::length(widePath));

    for (const auto* cursor = widePath; *cursor; ++cursor)
    {
        const wchar_t wc = *cursor;
        if (wc <= 0x7f)
        {
            out.push_back(static_cast<char>(wc));
        }
        else
        {
            out.push_back('?');
        }
    }

    return out;
}

static LIMSTR dupJson(const std::string& payload)
{
    const auto size = payload.size() + 1u;
    auto* out = static_cast<LIMSTR>(std::malloc(size));
    if (!out)
    {
        return nullptr;
    }

    std::memcpy(out, payload.c_str(), size);
    return out;
}

static LIMSTR invalidJson(const std::string& what, const std::string& path)
{
    std::ostringstream oss;
    oss << "{\"status\":\"error\",\"where\":\"" << what << "\",\"path\":\"" << path << "\"}";
    return dupJson(oss.str());
}

static LIMSTR unsupported(const char* api, const std::string& path)
{
    std::ostringstream oss;
    oss << "{\"status\":\"not_implemented\",\"implementation\":\"nd2-cpp\",\"api\":\"" << api << "\",\"path\":\"" << path << "\"}";
    return dupJson(oss.str());
}

static LIMRESULT validateHandle(LIMFILEHANDLE hFile)
{
    if (!hFile)
    {
        return LIM_ERR_HANDLE;
    }

    return LIM_OK;
}

static bool readU32(std::ifstream& stream, uint32_t& value)
{
    char buffer[4];
    if (!stream.read(buffer, 4))
    {
        return false;
    }

    value = static_cast<uint32_t>(static_cast<uint8_t>(buffer[0]))
            | (static_cast<uint32_t>(static_cast<uint8_t>(buffer[1])) << 8)
            | (static_cast<uint32_t>(static_cast<uint8_t>(buffer[2])) << 16)
            | (static_cast<uint32_t>(static_cast<uint8_t>(buffer[3])) << 24);
    return true;
}

static bool readU64(std::ifstream& stream, uint64_t& value)
{
    char buffer[8];
    if (!stream.read(buffer, 8))
    {
        return false;
    }

    value = static_cast<uint64_t>(static_cast<uint8_t>(buffer[0]))
            | (static_cast<uint64_t>(static_cast<uint8_t>(buffer[1])) << 8)
            | (static_cast<uint64_t>(static_cast<uint8_t>(buffer[2])) << 16)
            | (static_cast<uint64_t>(static_cast<uint8_t>(buffer[3])) << 24)
            | (static_cast<uint64_t>(static_cast<uint8_t>(buffer[4])) << 32)
            | (static_cast<uint64_t>(static_cast<uint8_t>(buffer[5])) << 40)
            | (static_cast<uint64_t>(static_cast<uint8_t>(buffer[6])) << 48)
            | (static_cast<uint64_t>(static_cast<uint8_t>(buffer[7])) << 56);
    return true;
}

static std::string utf16LeToUtf8(const uint8_t* bytes, std::size_t bytesLen)
{
    std::string out;
    out.reserve(bytesLen / 2);
    for (std::size_t i = 0; i + 1 < bytesLen; i += 2)
    {
        const uint16_t ch = static_cast<uint16_t>(bytes[i]) | (static_cast<uint16_t>(bytes[i + 1]) << 8);
        if (ch == 0)
        {
            break;
        }
        if (ch < 0x80)
        {
            out.push_back(static_cast<char>(ch));
        }
        else
        {
            out.push_back('?');
        }
    }

    return out;
}

static void jsonEscape(std::ostringstream& out, const std::string& text)
{
    for (const char ch : text)
    {
        switch (ch)
        {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                out << "\\u" << std::hex << std::uppercase << std::setfill('0') << std::setw(4)
                    << static_cast<int>(static_cast<unsigned char>(ch)) << std::nouppercase << std::dec;
            }
            else
            {
                out << ch;
            }
            break;
        }
    }
}

static void toJson(std::ostringstream& out, const ClxValue& value);

static void toJsonObject(std::ostringstream& out, const ClxValue& value)
{
    out << '{';
    bool first = true;
    for (auto it = value.objectValue.begin(); it != value.objectValue.end(); ++it)
    {
        if (!first)
        {
            out << ',';
        }
        first = false;

        out << '"';
        jsonEscape(out, it->first);
        out << "\":";
        toJson(out, it->second);
    }

    out << '}';
}

static void toJsonArray(std::ostringstream& out, const ClxValue& value)
{
    out << '[';
    for (std::size_t i = 0; i < value.arrayValue.size(); ++i)
    {
        if (i)
        {
            out << ',';
        }
        toJson(out, value.arrayValue[i]);
    }
    out << ']';
}

static void toJsonBytes(std::ostringstream& out, const ClxValue& value)
{
    out << '[';
    for (std::size_t i = 0; i < value.bytesValue.size(); ++i)
    {
        if (i)
        {
            out << ',';
        }
        out << static_cast<int>(value.bytesValue[i]);
    }
    out << ']';
}

static void toJson(std::ostringstream& out, const ClxValue& value)
{
    switch (value.kind)
    {
    case ClxValue::Kind::Null:
        out << "null";
        break;
    case ClxValue::Kind::Bool:
        out << (value.boolValue ? "true" : "false");
        break;
    case ClxValue::Kind::Int:
        out << value.intValue;
        break;
    case ClxValue::Kind::UInt:
        out << value.uintValue;
        break;
    case ClxValue::Kind::Double:
        {
            std::ostringstream num;
            num << std::fixed << value.doubleValue;
            out << num.str();
        }
        break;
    case ClxValue::Kind::String:
        out << '"';
        jsonEscape(out, value.stringValue);
        out << '"';
        break;
    case ClxValue::Kind::Bytes:
        toJsonBytes(out, value);
        break;
    case ClxValue::Kind::Object:
        toJsonObject(out, value);
        break;
    case ClxValue::Kind::Array:
        toJsonArray(out, value);
        break;
    }
}

static bool looksLikeClxLite(const std::vector<uint8_t>& data)
{
    if (data.size() < 2)
    {
        return false;
    }

    const uint8_t dtype = data[0];
    const uint8_t nameLength = data[1];

    if (dtype == 76)
    {
        return true;
    }

    if (dtype < 1 || dtype > 11)
    {
        return false;
    }

    if (nameLength <= 1)
    {
        return false;
    }

    const std::size_t nameBytes = static_cast<std::size_t>(nameLength) * 2u;
    if (data.size() < 2 + nameBytes)
    {
        return false;
    }

    if (data[nameBytes] != 0 || data[nameBytes + 1] != 0)
    {
        return false;
    }

    return true;
}

static bool inflateAll(const uint8_t* in, std::size_t inLen, std::vector<uint8_t>& out)
{
    if (inLen == 0)
    {
        out.clear();
        return true;
    }

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
    {
        return false;
    }

    std::vector<uint8_t> buffer(16384);
    out.clear();

    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in));
    strm.avail_in = static_cast<uInt>(inLen);

    int ret;
    do
    {
        strm.next_out = buffer.data();
        strm.avail_out = static_cast<uInt>(buffer.size());

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_STREAM_END && ret != Z_OK)
        {
            inflateEnd(&strm);
            return false;
        }

        const std::size_t produced = buffer.size() - static_cast<std::size_t>(strm.avail_out);
        out.insert(out.end(), buffer.data(), buffer.data() + produced);
    }
    while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    return true;
}

class ClxLiteParser
{
public:
    static constexpr uint8_t TYPE_BOOL = 1;
    static constexpr uint8_t TYPE_INT32 = 2;
    static constexpr uint8_t TYPE_UINT32 = 3;
    static constexpr uint8_t TYPE_INT64 = 4;
    static constexpr uint8_t TYPE_UINT64 = 5;
    static constexpr uint8_t TYPE_DOUBLE = 6;
    static constexpr uint8_t TYPE_VOID_PTR = 7;
    static constexpr uint8_t TYPE_STRING = 8;
    static constexpr uint8_t TYPE_BYTE_ARRAY = 9;
    static constexpr uint8_t TYPE_DEPRECATED = 10;
    static constexpr uint8_t TYPE_LEVEL = 11;
    static constexpr uint8_t TYPE_COMPRESS = 76;

    static ClxValue parse(const std::vector<uint8_t>& data)
    {
        ClxValue out;
        out.kind = ClxValue::Kind::Object;
        std::size_t pos = 0;
        parseWithCount(data, pos, 1, out);
        return out;
    }

private:
    static bool parseWithCount(const std::vector<uint8_t>& data, std::size_t& pos, std::size_t count, ClxValue& target)
    {
        for (std::size_t item = 0; item < count; ++item)
        {
            if (pos >= data.size())
            {
                break;
            }

            const std::size_t entryStart = pos;
            const uint8_t dtype = data[pos++];
            if (pos >= data.size())
            {
                return false;
            }

            const uint8_t nameLen = data[pos++];
            if (dtype == TYPE_DEPRECATED)
            {
                return false;
            }

            std::string name;
            if (dtype != TYPE_COMPRESS)
            {
                const std::size_t nameBytes = static_cast<std::size_t>(nameLen) * 2u;
                if (pos + nameBytes > data.size())
                {
                    return false;
                }
                name = utf16LeToUtf8(&data[pos], nameBytes);
                pos += nameBytes;
                if (!name.empty() && name.back() == '\0')
                {
                    name.pop_back();
                }
            }

            ClxValue value;
            if (dtype == TYPE_COMPRESS)
            {
                if (pos + 10 > data.size())
                {
                    return false;
                }
                pos += 10;
                std::vector<uint8_t> compressed(data.begin() + static_cast<long>(pos), data.end());
                std::vector<uint8_t> decompressed;
                if (inflateAll(compressed.data(), compressed.size(), decompressed) && !decompressed.empty())
                {
                    std::size_t npos = 0;
                    parseWithCount(decompressed, npos, 1, target);
                }
                return true;
            }

            switch (dtype)
            {
            case TYPE_BOOL:
                if (pos + 1 > data.size())
                {
                    return false;
                }
                value.kind = ClxValue::Kind::Bool;
                value.boolValue = data[pos] != 0;
                ++pos;
                break;
            case TYPE_INT32:
                if (pos + 4 > data.size())
                {
                    return false;
                }
                value.kind = ClxValue::Kind::Int;
                value.intValue = static_cast<int64_t>(static_cast<int32_t>(
                    static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8)
                    | (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24)));
                pos += 4;
                break;
            case TYPE_UINT32:
                if (pos + 4 > data.size())
                {
                    return false;
                }
                value.kind = ClxValue::Kind::UInt;
                value.uintValue = static_cast<uint32_t>(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
                pos += 4;
                break;
            case TYPE_INT64:
                if (pos + 8 > data.size())
                {
                    return false;
                }
                value.kind = ClxValue::Kind::Int;
                value.intValue = 0;
                for (std::size_t i = 0; i < 8; ++i)
                {
                    value.intValue |= static_cast<int64_t>(static_cast<uint64_t>(data[pos + i]) << (8 * i));
                }
                pos += 8;
                break;
            case TYPE_UINT64:
            case TYPE_VOID_PTR:
                if (pos + 8 > data.size())
                {
                    return false;
                }
                value.kind = ClxValue::Kind::UInt;
                value.uintValue = 0;
                for (std::size_t i = 0; i < 8; ++i)
                {
                    value.uintValue |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
                }
                pos += 8;
                break;
            case TYPE_DOUBLE:
                if (pos + 8 > data.size())
                {
                    return false;
                }
                value.kind = ClxValue::Kind::Double;
                {
                    std::uint64_t raw = 0;
                    for (std::size_t i = 0; i < 8; ++i)
                    {
                        raw |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
                    }
                    std::memcpy(&value.doubleValue, &raw, sizeof(double));
                }
                pos += 8;
                break;
            case TYPE_STRING:
                {
                    std::vector<uint8_t> bytes;
                    while (pos + 1 < data.size())
                    {
                        uint8_t b1 = data[pos++];
                        uint8_t b2 = data[pos++];
                        if (b1 == 0 && b2 == 0)
                        {
                            break;
                        }
                        bytes.push_back(b1);
                        bytes.push_back(b2);
                    }
                    value.kind = ClxValue::Kind::String;
                    value.stringValue = utf16LeToUtf8(bytes.data(), bytes.size());
                }
                break;
            case TYPE_BYTE_ARRAY:
                if (pos + 8 > data.size())
                {
                    return false;
                }
                {
                    uint64_t byteSize = 0;
                    for (std::size_t i = 0; i < 8; ++i)
                    {
                        byteSize |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
                    }
                    pos += 8;

                    if (pos + byteSize > data.size())
                    {
                        return false;
                    }

                    value.kind = ClxValue::Kind::Bytes;
                    value.bytesValue.assign(data.begin() + static_cast<long>(pos), data.begin() + static_cast<long>(pos + byteSize));
                    pos += static_cast<std::size_t>(byteSize);

                    if (looksLikeClxLite(value.bytesValue))
                    {
                        ClxValue nested = parse(value.bytesValue);
                        value = nested;
                    }
                }
                break;
            case TYPE_LEVEL:
                if (pos + 12 > data.size())
                {
                    return false;
                }
                {
                    const std::size_t levelStart = entryStart;
                    const uint32_t itemCount = static_cast<uint32_t>(data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
                    const uint64_t length =
                        static_cast<uint64_t>(data[pos + 4])
                        | (static_cast<uint64_t>(data[pos + 5]) << 8)
                        | (static_cast<uint64_t>(data[pos + 6]) << 16)
                        | (static_cast<uint64_t>(data[pos + 7]) << 24)
                        | (static_cast<uint64_t>(data[pos + 8]) << 32)
                        | (static_cast<uint64_t>(data[pos + 9]) << 40)
                        | (static_cast<uint64_t>(data[pos + 10]) << 48)
                        | (static_cast<uint64_t>(data[pos + 11]) << 56);
                    pos += 12;

                    const std::size_t listStart = pos;
                    const std::size_t nestedStart = listStart - levelStart;
                    if (length < nestedStart)
                    {
                        return false;
                    }

                    const std::size_t nestedLength = static_cast<std::size_t>(length - nestedStart);
                    if (listStart > data.size() || data.size() - listStart < nestedLength)
                    {
                        return false;
                    }

                    std::size_t nestedPos = 0;
                    std::vector<uint8_t> nestedPayload(data.begin() + static_cast<long>(listStart), data.begin() + static_cast<long>(listStart + nestedLength));
                    value.kind = ClxValue::Kind::Object;
                    parseWithCount(nestedPayload, nestedPos, itemCount, value);

                    pos = listStart + nestedLength;
                    const std::size_t skip = static_cast<std::size_t>(itemCount) * 8u;
                    if (pos + skip > data.size())
                    {
                        return false;
                    }
                    pos += skip;

                    if (value.objectValue.size() == 1)
                    {
                        auto it = value.objectValue.find("");
                        if (it != value.objectValue.end())
                        {
                            if (it->second.kind == ClxValue::Kind::Array)
                            {
                                // keep as array as-is
                            }
                            else
                            {
                                ClxValue tmpVal;
                                tmpVal.kind = ClxValue::Kind::Array;
                                tmpVal.arrayValue.push_back(it->second);
                                value.objectValue.clear();
                                value.objectValue[""] = std::move(tmpVal);
                            }
                        }
                    }
                }
                break;
            default:
                return false;
            }

            if (!name.empty())
            {
                target.objectValue[name] = value;
            }
            else
            {
                auto& current = target.objectValue[""];
                if (current.kind == ClxValue::Kind::Null)
                {
                    current = value;
                }
                else if (current.kind != ClxValue::Kind::Array)
                {
                    ClxValue arr;
                    arr.kind = ClxValue::Kind::Array;
                    arr.arrayValue.push_back(current);
                    arr.arrayValue.push_back(value);
                    current = std::move(arr);
                }
                else
                {
                    current.arrayValue.push_back(value);
                }
            }

        }

        return true;
    }
};

static bool readChunkHeader(std::ifstream& stream, ChunkHeader& header)
{
    return readU32(stream, header.magic) && readU32(stream, header.nameLength) && readU64(stream, header.dataLength);
}

static std::string makePathKeyFromStream(const char* ptr, std::size_t len)
{
    return std::string(ptr, ptr + static_cast<long>(len));
}

static bool readChunkMapAtOffset(FileHandle& handle, uint64_t chunkMapOffset)
{
    handle.chunkMap.clear();
    if (chunkMapOffset + 16 > handle.fileSize)
    {
        return false;
    }

    auto& stream = handle.stream;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(chunkMapOffset), std::ios::beg);
    ChunkHeader header;
    if (!readChunkHeader(stream, header) || header.magic != ND2_CHUNK_MAGIC)
    {
        return false;
    }

    if (header.nameLength == 0 || static_cast<std::size_t>(header.nameLength) > handle.fileSize)
    {
        return false;
    }

    std::vector<char> signatureBuf(header.nameLength, '\0');
    if (!stream.read(signatureBuf.data(), header.nameLength))
    {
        return false;
    }

    if (std::memcmp(signatureBuf.data(), ND2_FILEMAP_SIGNATURE, std::min<std::size_t>(std::strlen(ND2_FILEMAP_SIGNATURE), static_cast<std::size_t>(header.nameLength))) != 0)
    {
        if (header.nameLength < std::strlen(ND2_FILEMAP_SIGNATURE))
        {
            return false;
        }
    }

    if (header.nameLength > std::strlen(ND2_FILEMAP_SIGNATURE))
    {
        for (std::size_t i = std::strlen(ND2_FILEMAP_SIGNATURE); i < static_cast<std::size_t>(header.nameLength); ++i)
        {
            if (signatureBuf[i] != '\0')
            {
                return false;
            }
        }
    }

    if (header.dataLength == 0)
    {
        return false;
    }

    if (header.dataLength > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }

    const std::size_t mapBytes = static_cast<std::size_t>(header.dataLength);
    std::vector<uint8_t> mapData(mapBytes);
    if (!stream.read(reinterpret_cast<char*>(mapData.data()), static_cast<std::streamsize>(mapBytes)))
    {
        return false;
    }

    std::size_t pos = 0;
    while (pos < mapData.size())
    {
        std::size_t nameEnd = pos;
        while (nameEnd < mapData.size() && mapData[nameEnd] != '!')
        {
            ++nameEnd;
        }
        if (nameEnd >= mapData.size())
        {
            break;
        }

        const std::size_t nameLen = nameEnd - pos + 1u;
        std::string chunkName(reinterpret_cast<const char*>(mapData.data() + pos), nameLen);

        if (chunkName == ND2_CHUNKMAP_SIGNATURE)
        {
            break;
        }

        const std::size_t valuePos0 = nameEnd + 1u;
        if (valuePos0 + 16 > mapData.size())
        {
            break;
        }

        ChunkInfo bestEntry{};
        bool found = false;
        int bestScore = -1;

        for (std::size_t candidate = 0; candidate <= 1; ++candidate)
        {
            const std::size_t valuePos = valuePos0 + candidate;
            if (valuePos + 16 > mapData.size())
            {
                continue;
            }

            uint64_t off = 0;
            for (std::size_t i = 0; i < 8; ++i)
            {
                off |= static_cast<uint64_t>(mapData[valuePos + i]) << (8 * i);
            }

            uint64_t sz = 0;
            for (std::size_t i = 0; i < 8; ++i)
            {
                sz |= static_cast<uint64_t>(mapData[valuePos + 8 + i]) << (8 * i);
            }

            int score = 0;
            if (off <= handle.fileSize)
            {
                score = 1;
                if (off + sz <= handle.fileSize)
                {
                    score = 2;
                }
            }

            if (score > bestScore)
            {
                bestScore = score;
                bestEntry = {off, sz};
                found = true;
                if (score == 2)
                {
                    break;
                }
            }
        }

        if (!found)
        {
            return false;
        }

        handle.chunkMap[chunkName] = bestEntry;
        pos = valuePos0 + 16;
    }

    return !handle.chunkMap.empty();
}

static bool readChunkByName(FileHandle& handle, const std::string& chunkName, std::vector<uint8_t>& payload)
{
    const auto it = handle.chunkMap.find(chunkName);
    if (it == handle.chunkMap.end())
    {
        return false;
    }

    auto& stream = handle.stream;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(it->second.offset), std::ios::beg);

    ChunkHeader header;
    if (!readChunkHeader(stream, header) || header.magic != ND2_CHUNK_MAGIC)
    {
        return false;
    }

    if (header.dataLength == 0)
    {
        payload.clear();
        return true;
    }

    stream.seekg(static_cast<std::streamoff>(header.nameLength), std::ios::cur);

    if (header.dataLength > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }

    payload.resize(static_cast<std::size_t>(header.dataLength));
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size())));
}

static bool readVersionAndInit(FileHandle& handle)
{
    auto& stream = handle.stream;
    if (!stream)
    {
        return false;
    }

    stream.seekg(0, std::ios::end);
    handle.fileSize = static_cast<uint64_t>(stream.tellg());
    if (handle.fileSize < 112)
    {
        return false;
    }

    char header[112];
    stream.seekg(0, std::ios::beg);
    if (!stream.read(header, sizeof(header)))
    {
        return false;
    }

    auto magic = static_cast<uint32_t>(static_cast<uint8_t>(header[0]))
               | (static_cast<uint32_t>(static_cast<uint8_t>(header[1])) << 8)
               | (static_cast<uint32_t>(static_cast<uint8_t>(header[2])) << 16)
               | (static_cast<uint32_t>(static_cast<uint8_t>(header[3])) << 24);

    if (magic == JP2_MAGIC)
    {
        return false;
    }

    if (magic != ND2_CHUNK_MAGIC)
    {
        return false;
    }

    const uint32_t nameLen = static_cast<uint32_t>(static_cast<uint8_t>(header[4]))
                           | (static_cast<uint32_t>(static_cast<uint8_t>(header[5])) << 8)
                           | (static_cast<uint32_t>(static_cast<uint8_t>(header[6])) << 16)
                           | (static_cast<uint32_t>(static_cast<uint8_t>(header[7])) << 24);

    const uint64_t dataLen = static_cast<uint64_t>(static_cast<uint8_t>(header[8]))
                           | (static_cast<uint64_t>(static_cast<uint8_t>(header[9])) << 8)
                           | (static_cast<uint64_t>(static_cast<uint8_t>(header[10])) << 16)
                           | (static_cast<uint64_t>(static_cast<uint8_t>(header[11])) << 24)
                           | (static_cast<uint64_t>(static_cast<uint8_t>(header[12])) << 32)
                           | (static_cast<uint64_t>(static_cast<uint8_t>(header[13])) << 40)
                           | (static_cast<uint64_t>(static_cast<uint8_t>(header[14])) << 48)
                           | (static_cast<uint64_t>(static_cast<uint8_t>(header[15])) << 56);

    if (nameLen != 32u || dataLen != 64u)
    {
        return false;
    }

    if (std::memcmp(header + 16, ND2_FILE_SIGNATURE, std::strlen(ND2_FILE_SIGNATURE)) != 0)
    {
        return false;
    }

    const char* ver = header + 48;
    handle.versionMajor = static_cast<uint32_t>((ver[3] - '0'));
    handle.versionMinor = static_cast<uint32_t>((ver[5] - '0'));

    stream.seekg(-40, std::ios::end);
    char sig[32];
    uint64_t chunkMapOffset = 0;
    if (!stream.read(sig, 32) || !stream.read(reinterpret_cast<char*>(&chunkMapOffset), 8))
    {
        return false;
    }

    if (std::memcmp(sig, ND2_CHUNKMAP_SIGNATURE, std::strlen(ND2_CHUNKMAP_SIGNATURE)) != 0)
    {
        return false;
    }

        const uint64_t chunkMapOffsetLE =
            static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[0])
            | (static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[1]) << 8)
            | (static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[2]) << 16)
            | (static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[3]) << 24)
            | (static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[4]) << 32)
            | (static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[5]) << 40)
            | (static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[6]) << 48)
            | (static_cast<uint64_t>(reinterpret_cast<unsigned char*>(&chunkMapOffset)[7]) << 56);

    if (chunkMapOffsetLE > handle.fileSize)
    {
        return false;
    }

    if (!readChunkMapAtOffset(handle, chunkMapOffsetLE))
    {
        return false;
    }

    handle.initialized = true;
    return true;
}

static bool hasClxData(const std::vector<uint8_t>& bytes)
{
    return !bytes.empty() && looksLikeClxLite(bytes);
}

static bool parseNumeric(const ClxValue& value, uint64_t& out)
{
    if (value.kind == ClxValue::Kind::UInt)
    {
        out = value.uintValue;
        return true;
    }

    if (value.kind == ClxValue::Kind::Int)
    {
        if (value.intValue < 0)
        {
            return false;
        }
        out = static_cast<uint64_t>(value.intValue);
        return true;
    }

    if (value.kind == ClxValue::Kind::Double)
    {
        if (value.doubleValue < 0.0)
        {
            return false;
        }
        out = static_cast<uint64_t>(value.doubleValue);
        return true;
    }

    if (value.kind == ClxValue::Kind::String)
    {
        try
        {
            std::size_t idx = 0;
            out = std::stoull(value.stringValue, &idx);
            return idx == value.stringValue.size();
        }
        catch (...)
        {
            return false;
        }
    }

    return false;
}

static bool valueFromKey(const ClxValue& object, const char* key, ClxValue& out)
{
    if (object.kind != ClxValue::Kind::Object)
    {
        return false;
    }

    const std::string base(key);
    const auto direct = object.objectValue.find(base);
    if (direct != object.objectValue.end())
    {
        out = direct->second;
        return true;
    }

    const auto withU32 = object.objectValue.find(base + "_u32");
    if (withU32 != object.objectValue.end())
    {
        out = withU32->second;
        return true;
    }

    const auto withI32 = object.objectValue.find(base + "_i32");
    if (withI32 != object.objectValue.end())
    {
        out = withI32->second;
        return true;
    }

    return false;
}

static const ClxValue* objectForNameOrSingleKey(const ClxValue& root, const char* names[])
{
    const ClxValue* node = &root;
    bool progressed = false;
    while (node->kind == ClxValue::Kind::Object)
    {
        progressed = false;
        if (names)
        {
            for (std::size_t i = 0; names[i] != nullptr; ++i)
            {
                auto it = node->objectValue.find(names[i]);
                if (it != node->objectValue.end())
                {
                    node = &it->second;
                    progressed = true;
                    break;
                }
            }
        }

        if (progressed)
        {
            continue;
        }

        if (node->objectValue.size() == 1)
        {
            const auto& firstPair = *node->objectValue.begin();
            if (firstPair.first == "" || firstPair.first.find("i0000000000") == 0 || firstPair.first == "SLxExperiment")
            {
                node = &firstPair.second;
                continue;
            }
        }

        break;
    }

    return node;
}

static bool unwrapObjectByKey(const ClxValue& source, const char* key, ClxValue& out)
{
    if (source.kind != ClxValue::Kind::Object)
    {
        return false;
    }

    const auto it = source.objectValue.find(key);
    if (it == source.objectValue.end())
    {
        return false;
    }

    out = it->second;
    return true;
}

static bool parseAttributeCore(const ClxValue& rawAttrObj, Attributes& attrs)
{
    if (rawAttrObj.kind != ClxValue::Kind::Object)
    {
        attrs.parseFailed = true;
        attrs.parseError = "attributes chunk root is not object";
        return false;
    }

    const char* attrKeys[] = {"SLxImageAttributes", nullptr};
    const ClxValue* candidate = objectForNameOrSingleKey(rawAttrObj, attrKeys);
    if (!candidate || candidate->kind != ClxValue::Kind::Object)
    {
        candidate = &rawAttrObj;
    }

    if (candidate->kind != ClxValue::Kind::Object)
    {
        attrs.parseFailed = true;
        attrs.parseError = "attributes object missing";
        return false;
    }

    ClxValue value;

    auto require = [&](const char* key, uint64_t& out) -> bool {
        return valueFromKey(*candidate, key, value) && parseNumeric(value, out);
    };

    auto optional = [&](const char* key, uint64_t& out) -> bool {
        if (!valueFromKey(*candidate, key, value))
        {
            return false;
        }
        return parseNumeric(value, out);
    };

    uint64_t bitsInMemory = 0;
    uint64_t bitsSignificant = 0;
    uint64_t componentCount = 0;
    uint64_t height = 0;
    uint64_t seqCount = 0;
    uint64_t width = 0;

    if (!require("uiBpcInMemory", bitsInMemory)
        || !require("uiBpcSignificant", bitsSignificant)
        || !require("uiComp", componentCount)
        || !require("uiHeight", height)
        || !require("uiWidth", width)
        || !require("uiSequenceCount", seqCount))
    {
        attrs.parseFailed = true;
        attrs.parseError = "required attributes missing";
        return false;
    }

    attrs.bitsInMemory = static_cast<uint32_t>(bitsInMemory);
    attrs.bitsSignificant = static_cast<uint32_t>(bitsSignificant);
    attrs.componentCount = static_cast<uint32_t>(componentCount);
    attrs.heightPx = static_cast<uint32_t>(height);
    attrs.widthPx = static_cast<uint32_t>(width);
    attrs.sequenceCount = static_cast<uint32_t>(seqCount);

    uint64_t widthBytes = 0;
    if (optional("uiWidthBytes", widthBytes))
    {
        attrs.widthBytes = static_cast<uint32_t>(widthBytes);
        attrs.hasWidthBytes = true;
    }

    uint64_t channelCount = attrs.componentCount;
    if (optional("uiChannelCount", channelCount))
    {
        attrs.channelCount = static_cast<uint32_t>(channelCount);
    }
    else
    {
        attrs.channelCount = attrs.componentCount;
    }

    uint64_t tileW = 0;
    if (optional("uiTileWidth", tileW) && tileW > 0)
    {
        attrs.tileWidthPx = static_cast<uint32_t>(tileW);
        attrs.hasTileWidth = true;
    }

    uint64_t tileH = 0;
    if (optional("uiTileHeight", tileH) && tileH > 0)
    {
        attrs.tileHeightPx = static_cast<uint32_t>(tileH);
        attrs.hasTileHeight = true;
    }

    if (valueFromKey(*candidate, "eCompression", value))
    {
        if (value.kind == ClxValue::Kind::String)
        {
            if (value.stringValue == "lossless")
            {
                attrs.compressionType = 0;
                attrs.hasCompressionType = true;
            }
            else if (value.stringValue == "lossy")
            {
                attrs.compressionType = 1;
                attrs.hasCompressionType = true;
            }
        }
        else if (value.kind == ClxValue::Kind::UInt || value.kind == ClxValue::Kind::Int)
        {
            uint64_t c = 0;
            if (parseNumeric(value, c) && c < 2)
            {
                attrs.compressionType = static_cast<int>(c);
                attrs.hasCompressionType = true;
            }
        }
    }

    if (valueFromKey(*candidate, "dCompressionParam", value) && (value.kind == ClxValue::Kind::Double || value.kind == ClxValue::Kind::UInt || value.kind == ClxValue::Kind::Int))
    {
        if (value.kind == ClxValue::Kind::Double)
        {
            attrs.compressionLevel = value.doubleValue;
        }
        else if (value.kind == ClxValue::Kind::UInt)
        {
            attrs.compressionLevel = static_cast<double>(value.uintValue);
        }
        else
        {
            attrs.compressionLevel = static_cast<double>(value.intValue);
        }
        attrs.hasCompressionLevel = true;
    }

    if (valueFromKey(*candidate, "uiCompBPC", value) && value.kind == ClxValue::Kind::UInt)
    {
        attrs.pixelDataType = (value.uintValue == 3) ? "float" : "unsigned";
    }
    else
    {
        attrs.pixelDataType = "unsigned";
    }

    attrs.parsed = true;
    return true;
}

static bool ensureAttributeCache(FileHandle& handle)
{
    if (handle.attributesCached)
    {
        return handle.attributes.parsed;
    }

    std::string name = (handle.versionMajor >= 3) ? "ImageAttributesLV!" : "ImageAttributes!";
    std::vector<uint8_t> payload;
    if (!readChunkByName(handle, name, payload))
    {
        handle.attributesCached = true;
        handle.attributes.parseFailed = true;
        handle.attributes.parseError = "attributes chunk missing";
        return false;
    }

    if (payload.empty())
    {
        handle.attributesCached = true;
        handle.attributes.parseFailed = true;
        handle.attributes.parseError = "empty attributes chunk";
        return false;
    }

    if (looksLikeClxLite(payload))
    {
        try
        {
            handle.rawAttributes = ClxLiteParser::parse(payload);
            handle.attributesCached = true;
            return parseAttributeCore(handle.rawAttributes, handle.attributes);
        }
        catch (...)
        {
            handle.attributesCached = true;
            handle.attributes.parseFailed = true;
            handle.attributes.parseError = "failed to parse attributes clx";
            return false;
        }
    }

    handle.attributesCached = true;
    handle.attributes.parseFailed = true;
    handle.attributes.parseError = "attributes payload not clx";
    return false;
}

static const ClxValue& toJsonValueOrEmpty(const ClxValue& value)
{
    return value;
}

static bool ensureExperimentCache(FileHandle& handle)
{
    if (handle.experimentCached)
    {
        return true;
    }

    const bool isV3 = handle.versionMajor >= 3;
    std::string name = isV3 ? "ImageMetadataLV!" : "ImageMetadata!";
    std::vector<uint8_t> payload;
    if (readChunkByName(handle, name, payload) && !payload.empty() && looksLikeClxLite(payload))
    {
        try
        {
            ClxValue parsed = ClxLiteParser::parse(payload);
            if (isV3 && unwrapObjectByKey(parsed, "SLxExperiment", handle.rawExperiment))
            {
                // unwrapped
            }
            else
            {
                handle.rawExperiment = std::move(parsed);
            }
        }
        catch (...)
        {
            // leave empty object as-is
        }
    }

    if (isV3 && (handle.rawExperiment.kind != ClxValue::Kind::Object || handle.rawExperiment.objectValue.empty()))
    {
        if (readChunkByName(handle, "ImageMetadata!", payload) && !payload.empty() && looksLikeClxLite(payload))
        {
            try
            {
                ClxValue parsed = ClxLiteParser::parse(payload);
                if (unwrapObjectByKey(parsed, "SLxExperiment", handle.rawExperiment))
                {
                    // unwrapped
                }
                else
                {
                    handle.rawExperiment = std::move(parsed);
                }
            }
            catch (...)
            {
                // leave empty object as-is
            }
        }
    }

    handle.experimentCached = true;
    return true;
}

static bool parseLoop(const ClxValue& loopObj, LoopInfo& out, std::size_t nestingLevel)
{
    if (loopObj.kind != ClxValue::Kind::Object)
    {
        return false;
    }

    ClxValue typeValue;
    if (!valueFromKey(loopObj, "uiLoopType", typeValue) && !valueFromKey(loopObj, "eType", typeValue))
    {
        return false;
    }

    uint64_t typeRaw = 0;
    if (!parseNumeric(typeValue, typeRaw))
    {
        return false;
    }

    ClxValue countValue;
    ClxValue paramsCopy;
    const ClxValue* params = &loopObj;
    if (valueFromKey(loopObj, "uLoopPars", countValue) && countValue.kind == ClxValue::Kind::Object)
    {
        params = &countValue;
        if (params->objectValue.size() == 1)
        {
            const auto& p = *params->objectValue.begin();
            if (p.first == "i0000000000" && p.second.kind == ClxValue::Kind::Object)
            {
                params = &p.second;
            }
        }
    }

    uint64_t countRaw = 0;
    if (valueFromKey(*params, "uiCount", countValue))
    {
        if (!parseNumeric(countValue, countRaw))
        {
            return false;
        }
    }
    else if (valueFromKey(loopObj, "uiCount", countValue))
    {
        if (!parseNumeric(countValue, countRaw))
        {
            return false;
        }
    }
    else if (typeRaw == 6 && valueFromKey(*params, "pPlanes", countValue) && countValue.kind == ClxValue::Kind::Object)
    {
        if (!valueFromKey(countValue, "uiCount", countValue) || !parseNumeric(countValue, countRaw))
        {
            return false;
        }
    }

    if (countRaw == 0)
    {
        return false;
    }

    switch (typeRaw)
    {
    case 1:
        out.type = "TimeLoop";
        break;
    case 2:
        out.type = "XYPosLoop";
        break;
    case 4:
        out.type = "ZStackLoop";
        break;
    case 6:
        out.type = "SpectLoop";
        break;
    case 7:
        out.type = "CustomLoop";
        break;
    case 8:
        out.type = "NETimeLoop";
        break;
    default:
        out.type = "CustomLoop";
        break;
    }

    out.count = static_cast<std::size_t>(countRaw);
    {
        uint64_t nestingRaw = 0;
        if (valueFromKey(loopObj, "uiNestingLevel", countValue) && parseNumeric(countValue, nestingRaw))
        {
            out.nestingLevel = static_cast<std::size_t>(nestingRaw);
        }
        else
        {
            out.nestingLevel = nestingLevel;
        }
    }

    if (params && params->kind == ClxValue::Kind::Object)
    {
        paramsCopy = *params;
    }
    out.parameters = paramsCopy;
    return true;
}

static void collectLoopTree(const ClxValue& node, std::vector<LoopInfo>& out, std::size_t nestingLevel)
{
    const ClxValue* current = objectForNameOrSingleKey(node, nullptr);
    if (!current || current->kind != ClxValue::Kind::Object)
    {
        return;
    }

    LoopInfo info;
    if (parseLoop(*current, info, nestingLevel))
    {
        out.push_back(info);
    }

    auto it = current->objectValue.find("ppNextLevelEx");
    if (it == current->objectValue.end())
    {
        return;
    }

    const ClxValue& child = it->second;
    if (child.kind == ClxValue::Kind::Array)
    {
        for (const auto& item : child.arrayValue)
        {
            collectLoopTree(item, out, nestingLevel + 1);
        }
    }
    else if (child.kind == ClxValue::Kind::Object)
    {
        if (child.objectValue.find("uiLoopType") != child.objectValue.end() || child.objectValue.find("eType") != child.objectValue.end())
        {
            collectLoopTree(child, out, nestingLevel + 1);
        }
        else
        {
            for (const auto& kv : child.objectValue)
            {
                collectLoopTree(kv.second, out, nestingLevel + 1);
            }
        }
    }
}

static void ensureCoordCache(FileHandle& handle)
{
    if (handle.coordCacheReady)
    {
        return;
    }

    ensureAttributeCache(handle);
    if (!handle.attributes.parsed)
    {
        handle.coordAxes.clear();
        handle.coordShape.clear();
        handle.coordCacheReady = true;
        return;
    }

    ensureExperimentCache(handle);

    std::vector<LoopInfo> loops;
    collectLoopTree(handle.rawExperiment, loops, 0);
    handle.loops = loops;

    if (loops.empty())
    {
        handle.coordAxes.clear();
        handle.coordShape.clear();
        handle.coordCacheReady = true;
        return;
    }

    for (const auto& loop : loops)
    {
        if (loop.type == "TimeLoop" || loop.type == "NETimeLoop")
        {
            handle.coordAxes.push_back("T");
            handle.coordShape.push_back(loop.count);
        }
        else if (loop.type == "XYPosLoop")
        {
            handle.coordAxes.push_back("P");
            handle.coordShape.push_back(loop.count);
        }
        else if (loop.type == "ZStackLoop")
        {
            handle.coordAxes.push_back("Z");
            handle.coordShape.push_back(loop.count);
        }
        else
        {
            handle.coordAxes.push_back("U");
            handle.coordShape.push_back(loop.count);
        }
    }

    handle.coordCacheReady = true;
}

static LIMSIZE coordsSize(const FileHandle& handle)
{
    if (handle.coordAxes.empty())
    {
        return 0;
    }
    return static_cast<LIMSIZE>(handle.coordAxes.size());
}

static ClxValue mergeMetadata(const ClxValue& base, const ClxValue& patch)
{
    if (base.kind != ClxValue::Kind::Object || patch.kind != ClxValue::Kind::Object)
    {
        return patch;
    }

    ClxValue out = base;
    for (const auto& item : patch.objectValue)
    {
        const auto it = out.objectValue.find(item.first);
        if (it != out.objectValue.end())
        {
            it->second = mergeMetadata(it->second, item.second);
        }
        else
        {
            out.objectValue.insert(item);
        }
    }

    return out;
}

static ClxValue buildExperimentArray(const FileHandle& handle)
{
    ClxValue out;
    out.kind = ClxValue::Kind::Array;
    if (handle.loops.empty())
    {
        return out;
    }

    for (const auto& loop : handle.loops)
    {
        ClxValue item;
        item.kind = ClxValue::Kind::Object;
        item.objectValue["type"].kind = ClxValue::Kind::String;
        item.objectValue["type"].stringValue = loop.type;
        item.objectValue["count"].kind = ClxValue::Kind::UInt;
        item.objectValue["count"].uintValue = loop.count;
        item.objectValue["nestingLevel"].kind = ClxValue::Kind::UInt;
        item.objectValue["nestingLevel"].uintValue = loop.nestingLevel;
        item.objectValue["parameters"].kind = ClxValue::Kind::Object;
        if (loop.parameters.kind == ClxValue::Kind::Object || loop.parameters.kind == ClxValue::Kind::Array)
        {
            item.objectValue["parameters"] = loop.parameters;
        }

        out.arrayValue.push_back(item);
    }

    return out;
}

static LIMSTR toJsonString(const ClxValue& value)
{
    std::ostringstream out;
    toJson(out, value);
    return dupJson(out.str());
}

static LIMSTR toAttributesJsonString(const Attributes& attrs)
{
    ClxValue value;
    value.kind = ClxValue::Kind::Object;
    value.objectValue["bitsPerComponentInMemory"].kind = ClxValue::Kind::UInt;
    value.objectValue["bitsPerComponentInMemory"].uintValue = attrs.bitsInMemory;

    value.objectValue["bitsPerComponentSignificant"].kind = ClxValue::Kind::UInt;
    value.objectValue["bitsPerComponentSignificant"].uintValue = attrs.bitsSignificant;

    value.objectValue["componentCount"].kind = ClxValue::Kind::UInt;
    value.objectValue["componentCount"].uintValue = attrs.componentCount;

    value.objectValue["heightPx"].kind = ClxValue::Kind::UInt;
    value.objectValue["heightPx"].uintValue = attrs.heightPx;

    value.objectValue["pixelDataType"].kind = ClxValue::Kind::String;
    value.objectValue["pixelDataType"].stringValue = attrs.pixelDataType.empty() ? "unsigned" : attrs.pixelDataType;

    value.objectValue["sequenceCount"].kind = ClxValue::Kind::UInt;
    value.objectValue["sequenceCount"].uintValue = attrs.sequenceCount;

    value.objectValue["widthPx"].kind = ClxValue::Kind::UInt;
    value.objectValue["widthPx"].uintValue = attrs.widthPx;

    if (attrs.hasWidthBytes)
    {
        value.objectValue["widthBytes"].kind = ClxValue::Kind::UInt;
        value.objectValue["widthBytes"].uintValue = attrs.widthBytes;
    }

    if (attrs.hasCompressionType)
    {
        value.objectValue["compressionType"].kind = ClxValue::Kind::String;
        value.objectValue["compressionType"].stringValue = (attrs.compressionType == 0) ? "lossless" : "lossy";
    }

    if (attrs.hasCompressionLevel)
    {
        value.objectValue["compressionLevel"].kind = ClxValue::Kind::Double;
        value.objectValue["compressionLevel"].doubleValue = attrs.compressionLevel;
    }

    if (attrs.channelCount > 0)
    {
        value.objectValue["channelCount"].kind = ClxValue::Kind::UInt;
        value.objectValue["channelCount"].uintValue = attrs.channelCount;
    }

    if (attrs.hasTileWidth && attrs.tileWidthPx != attrs.widthPx)
    {
        value.objectValue["tileWidthPx"].kind = ClxValue::Kind::UInt;
        value.objectValue["tileWidthPx"].uintValue = attrs.tileWidthPx;
    }

    if (attrs.hasTileHeight)
    {
        value.objectValue["tileHeightPx"].kind = ClxValue::Kind::UInt;
        value.objectValue["tileHeightPx"].uintValue = attrs.tileHeightPx;
    }

    return toJsonString(value);
}

static bool readFrameChunkPayloadOffset(FileHandle& handle, const std::string& name, uint64_t& offset)
{
    const auto it = handle.chunkMap.find(name);
    if (it == handle.chunkMap.end())
    {
        return false;
    }

    auto& stream = handle.stream;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(it->second.offset), std::ios::beg);

    ChunkHeader header;
    if (!readChunkHeader(stream, header) || header.magic != ND2_CHUNK_MAGIC)
    {
        return false;
    }

    offset = static_cast<uint64_t>(it->second.offset) + 16ULL + static_cast<uint64_t>(header.nameLength);
    return offset <= handle.fileSize;
}

static bool ensureImageUncompressedPayload(FileHandle& handle, const std::string& name, std::vector<uint8_t>& out, std::size_t expected)
{
    auto it = handle.chunkMap.find(name);
    if (it == handle.chunkMap.end())
    {
        return false;
    }

    uint64_t payloadOffset = 0;
    if (!readFrameChunkPayloadOffset(handle, name, payloadOffset))
    {
        payloadOffset = it->second.offset + CHUNK_ALIGNMENT;
    }

    if (payloadOffset > std::numeric_limits<uint64_t>::max() - 8ULL)
    {
        return false;
    }
    payloadOffset += 8ULL;

    if (payloadOffset + expected > handle.fileSize)
    {
        return false;
    }

    auto& stream = handle.stream;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(payloadOffset), std::ios::beg);
    out.resize(expected);
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(expected)));
}
} // namespace

LIMFILEAPI LIMFILEHANDLE Lim_FileOpenForReadUtf8(LIMCSTR szFileNameUtf8)
{
    if (!szFileNameUtf8)
    {
        return nullptr;
    }

    auto* handle = new FileHandle();
    handle->path = szFileNameUtf8;
    handle->stream.open(handle->path, std::ios::binary);

    if (!handle->stream || !readVersionAndInit(*handle))
    {
        delete handle;
        return nullptr;
    }

    return handle;
}

LIMFILEAPI LIMFILEHANDLE Lim_FileOpenForRead(LIMCWSTR wszFileName)
{
    const std::string path = toUtf8(wszFileName);
    return Lim_FileOpenForReadUtf8(path.c_str());
}

LIMFILEAPI void Lim_FileClose(LIMFILEHANDLE hFile)
{
    if (!hFile)
    {
        return;
    }

    delete asHandle(hFile);
}

LIMFILEAPI LIMSIZE Lim_FileGetCoordSize(LIMFILEHANDLE hFile)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return 0;
    }

    auto* handle = asHandle(hFile);
    ensureAttributeCache(*handle);
    ensureCoordCache(*handle);

    return coordsSize(*handle);
}

LIMFILEAPI LIMUINT Lim_FileGetCoordInfo(LIMFILEHANDLE hFile, LIMUINT coord, LIMSTR type, LIMSIZE maxTypeSize)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return 0;
    }

    auto* handle = asHandle(hFile);
    ensureAttributeCache(*handle);
    ensureCoordCache(*handle);

    if (coord >= static_cast<LIMUINT>(coordsSize(*handle)))
    {
        if (type && maxTypeSize > 0)
        {
            type[0] = '\0';
        }
        return 0;
    }

    const auto& axis = handle->coordAxes[coord];
    const char* label = "Unknown";
    if (axis == "T")
    {
        label = "TimeLoop";
    }
    else if (axis == "P")
    {
        label = "XYPosLoop";
    }
    else if (axis == "Z")
    {
        label = "ZStackLoop";
    }
    else if (axis == "C")
    {
        label = "Unknown";
    }

    if (type && maxTypeSize > 0)
    {
        const std::size_t len = std::char_traits<LIMCHAR>::length(label);
        if (len + 1 > static_cast<std::size_t>(maxTypeSize))
        {
            return 0;
        }
        std::memcpy(type, label, len + 1);
        return coord + 1u;
    }

    return coord + 1u;
}

LIMFILEAPI LIMUINT Lim_FileGetSeqCount(LIMFILEHANDLE hFile)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return 0;
    }

    auto* handle = asHandle(hFile);
    if (!ensureAttributeCache(*handle) || !handle->attributes.parsed)
    {
        return 0;
    }

    return handle->attributes.sequenceCount;
}

LIMFILEAPI LIMBOOL Lim_FileGetSeqIndexFromCoords(LIMFILEHANDLE hFile, const LIMUINT* coords, LIMSIZE coordCount, LIMUINT* seqIdx)
{
    if (validateHandle(hFile) != LIM_OK || !coords || !seqIdx)
    {
        return 0;
    }

    auto* handle = asHandle(hFile);
    ensureAttributeCache(*handle);
    ensureCoordCache(*handle);

    if (coordCount != coordsSize(*handle))
    {
        return 0;
    }

    if (coordCount == 0)
    {
        *seqIdx = 0;
        return 1;
    }

    std::size_t seq = 0;
    std::size_t stride = 1;
    for (std::size_t i = 0; i < coordCount; ++i)
    {
        const std::size_t shape = handle->coordShape[i];
        const std::size_t value = coords[i];
        if (value >= shape)
        {
            return 0;
        }

        seq += value * stride;
        stride *= shape;
    }

    *seqIdx = static_cast<LIMUINT>(seq);
    return 1;
}

LIMFILEAPI LIMSIZE Lim_FileGetCoordsFromSeqIndex(LIMFILEHANDLE hFile, LIMUINT seqIdx, LIMUINT* coords, LIMSIZE maxCoordCount)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return 0;
    }

    auto* handle = asHandle(hFile);
    ensureAttributeCache(*handle);
    ensureCoordCache(*handle);

    const std::size_t dim = coordsSize(*handle);
    if (!coords)
    {
        return dim;
    }

    if (maxCoordCount < dim)
    {
        return 0;
    }

    std::size_t total = 1;
    for (std::size_t shape : handle->coordShape)
    {
        total *= shape;
    }

    if (seqIdx >= total)
    {
        return 0;
    }

    std::size_t remaining = seqIdx;
    for (std::size_t i = 0; i < dim; ++i)
    {
        const std::size_t shape = handle->coordShape[i];
        coords[i] = static_cast<LIMUINT>(remaining % shape);
        remaining /= shape;
    }

    return dim;
}

LIMFILEAPI LIMSTR Lim_FileGetAttributes(LIMFILEHANDLE hFile)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return nullptr;
    }

    auto* handle = asHandle(hFile);
    if (!ensureAttributeCache(*handle) || !handle->attributes.parsed)
    {
        return invalidJson("attributes", handle->path);
    }

    return toAttributesJsonString(handle->attributes);
}

static bool ensureMetadataChunkCache(FileHandle& handle, ClxValue& cache, const std::string& name, const char* unwrapKey = nullptr)
{
    if (&cache == &handle.rawMetadata && handle.experimentCached)
    {
        if (cache.kind != ClxValue::Kind::Null)
        {
            return true;
        }
    }

    if (!cache.objectValue.empty() || cache.kind != ClxValue::Kind::Null)
    {
        return true;
    }

    std::vector<uint8_t> payload;
    if (!readChunkByName(handle, name, payload) || payload.empty() || !looksLikeClxLite(payload))
    {
        return false;
    }

    try
    {
        ClxValue parsed = ClxLiteParser::parse(payload);
        if (unwrapKey && unwrapObjectByKey(parsed, unwrapKey, cache))
        {
            return true;
        }

        cache = std::move(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

LIMFILEAPI LIMSTR Lim_FileGetMetadata(LIMFILEHANDLE hFile)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return nullptr;
    }

    auto* handle = asHandle(hFile);
    if (!handle->metadataCached)
    {
        std::vector<uint8_t> payload;
        const bool isV3 = handle->versionMajor >= 3;
        const char* seq0Name = isV3 ? "ImageMetadataSeqLV|0!" : "ImageMetadataSeq|0!";
        if (readChunkByName(*handle, seq0Name, payload) && !payload.empty() && looksLikeClxLite(payload))
        {
            try
            {
                ClxValue parsed = ClxLiteParser::parse(payload);
                if (isV3 && unwrapObjectByKey(parsed, "SLxPictureMetadata", handle->rawMetadata))
                {
                    // unwrapped
                }
                else
                {
                    handle->rawMetadata = std::move(parsed);
                }
            }
            catch (...)
            {
                handle->rawMetadata = ClxValue();
            }
        }
        else if (isV3
                 && readChunkByName(*handle, "ImageMetadataSeq|0!", payload)
                 && !payload.empty()
                 && looksLikeClxLite(payload))
        {
            try
            {
                ClxValue parsed = ClxLiteParser::parse(payload);
                handle->rawMetadata = std::move(parsed);
            }
            catch (...)
            {
                handle->rawMetadata = ClxValue();
            }
        }
        else if (isV3
                 ? (ensureMetadataChunkCache(*handle, handle->rawMetadata, "ImageMetadataLV!", "SLxPictureMetadata")
                    || ensureMetadataChunkCache(*handle, handle->rawMetadata, "ImageMetadata!", "SLxPictureMetadata"))
                 : (ensureMetadataChunkCache(*handle, handle->rawMetadata, "ImageMetadata!")
                    || ensureMetadataChunkCache(*handle, handle->rawMetadata, "ImageMetadataLV!")))
        {
            // okay
        }
        else
        {
            handle->rawMetadata = ClxValue();
        }

        handle->metadataCached = true;
    }

    return handle->rawMetadata.kind == ClxValue::Kind::Null ? invalidJson("metadata", handle->path)
                                                           : toJsonString(handle->rawMetadata);
}

LIMFILEAPI LIMSTR Lim_FileGetFrameMetadata(LIMFILEHANDLE hFile, LIMUINT uiSeqIndex)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return nullptr;
    }

    auto* handle = asHandle(hFile);
    if (!ensureAttributeCache(*handle) || !handle->attributes.parsed)
    {
        return invalidJson("frame-metadata", handle->path);
    }

    if (uiSeqIndex >= handle->attributes.sequenceCount)
    {
        return invalidJson("frame-metadata", handle->path);
    }

    if (!handle->metadataCached)
    {
        Lim_FileGetMetadata(hFile);
    }

    const bool isV3 = handle->versionMajor >= 3;
    const std::string frameName = std::string("ImageMetadataSeq") + (isV3 ? "LV|" : "|") + std::to_string(uiSeqIndex) + '!';
    std::vector<uint8_t> payload;
    if (readChunkByName(*handle, frameName, payload) && looksLikeClxLite(payload))
    {
        try
        {
            ClxValue frameValue = ClxLiteParser::parse(payload);
            if (isV3)
            {
                ClxValue unwrapped;
                if (unwrapObjectByKey(frameValue, "SLxPictureMetadata", unwrapped))
                {
                    frameValue = std::move(unwrapped);
                }
            }
            const ClxValue merged = mergeMetadata(handle->rawMetadata, frameValue);
            return toJsonString(merged);
        }
        catch (...)
        {
            return toJsonString(handle->rawMetadata);
        }
    }

    if (isV3 && readChunkByName(*handle, std::string("ImageMetadataSeq|") + std::to_string(uiSeqIndex) + "!", payload) && looksLikeClxLite(payload))
    {
        try
        {
            ClxValue frameValue = ClxLiteParser::parse(payload);
            ClxValue unwrapped;
            if (unwrapObjectByKey(frameValue, "SLxPictureMetadata", unwrapped))
            {
                frameValue = std::move(unwrapped);
            }
            const ClxValue merged = mergeMetadata(handle->rawMetadata, frameValue);
            return toJsonString(merged);
        }
        catch (...)
        {
            return toJsonString(handle->rawMetadata);
        }
    }

    return toJsonString(handle->rawMetadata);
}

LIMFILEAPI LIMSTR Lim_FileGetTextinfo(LIMFILEHANDLE hFile)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return nullptr;
    }

    auto* handle = asHandle(hFile);

    std::string textName = (handle->versionMajor >= 3) ? "ImageTextInfoLV!" : "ImageTextInfo!";
    std::vector<uint8_t> payload;
    if (!readChunkByName(*handle, textName, payload) || payload.empty() || !looksLikeClxLite(payload))
    {
        return invalidJson("textinfo", handle->path);
    }

    try
    {
        ClxValue parsed = ClxLiteParser::parse(payload);
        if (handle->versionMajor >= 3)
        {
            ClxValue unwrapped;
            if (unwrapObjectByKey(parsed, "SLxImageTextInfo", unwrapped))
            {
                parsed = std::move(unwrapped);
            }
        }

        return toJsonString(parsed);
    }
    catch (...)
    {
        return invalidJson("textinfo", handle->path);
    }
}

LIMFILEAPI LIMSTR Lim_FileGetExperiment(LIMFILEHANDLE hFile)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return nullptr;
    }

    auto* handle = asHandle(hFile);
    ensureCoordCache(*handle);
    ensureExperimentCache(*handle);

    return toJsonString(buildExperimentArray(*handle));
}

LIMFILEAPI LIMRESULT Lim_FileGetImageData(LIMFILEHANDLE hFile, LIMUINT uiSeqIndex, LIMPICTURE* pPicture)
{
    if (validateHandle(hFile) != LIM_OK)
    {
        return LIM_ERR_HANDLE;
    }

    if (!pPicture)
    {
        return LIM_ERR_INVALIDARG;
    }

    auto* handle = asHandle(hFile);
    if (!ensureAttributeCache(*handle) || !handle->attributes.parsed)
    {
        return LIM_ERR_NOTFOUND;
    }

    const auto& attrs = handle->attributes;
    if (attrs.sequenceCount == 0)
    {
        return LIM_ERR_OUTOFRANGE;
    }

    if (uiSeqIndex >= attrs.sequenceCount)
    {
        return LIM_ERR_OUTOFRANGE;
    }

    const std::size_t bytesPerComponent = (attrs.bitsInMemory + 7u) / 8u;
    const std::size_t expectedBytesPerLine = attrs.hasWidthBytes ? attrs.widthBytes
                                  : attrs.widthPx * std::max<std::size_t>(1u, attrs.componentCount) * bytesPerComponent;
    const std::size_t expectedSize = expectedBytesPerLine * attrs.heightPx;
    if (expectedSize == 0)
    {
        return LIM_ERR_INVALIDARG;
    }

    if ((pPicture->pImageData == nullptr) || (pPicture->uiSize == 0) || (pPicture->uiWidth == 0) || (pPicture->uiHeight == 0)
        || (pPicture->uiBitsPerComp == 0) || (pPicture->uiComponents == 0))
    {
        const LIMSIZE allocated = Lim_InitPicture(pPicture, attrs.widthPx, attrs.heightPx, attrs.bitsInMemory, attrs.componentCount);
        if (0 == allocated)
        {
            return LIM_ERR_OUTOFMEMORY;
        }
    }

    if (pPicture->uiSize != expectedSize)
    {
        return LIM_ERR_INVALIDARG;
    }

    if (pPicture->uiWidth != attrs.widthPx || pPicture->uiHeight != attrs.heightPx
        || pPicture->uiBitsPerComp != attrs.bitsInMemory || pPicture->uiComponents != attrs.componentCount)
    {
        return LIM_ERR_INVALIDARG;
    }

    const std::string seqName = std::string("ImageDataSeq|") + std::to_string(uiSeqIndex) + '!';
    std::vector<uint8_t> payload;

    bool gotFrame = false;
    if (attrs.hasCompressionType && (attrs.compressionType == 0 || attrs.compressionType == 1))
    {
        if (readChunkByName(*handle, seqName, payload) && payload.size() >= 8)
        {
            gotFrame = inflateAll(payload.data() + 8, payload.size() - 8, payload);
        }
    }

    if (!gotFrame)
    {
        if (!ensureImageUncompressedPayload(*handle, seqName, payload, expectedSize))
        {
            return LIM_ERR_NOTFOUND;
        }
        gotFrame = true;
    }

    if (!gotFrame || payload.size() < expectedSize)
    {
        return LIM_ERR_UNEXPECTED;
    }

    std::memcpy(pPicture->pImageData, payload.data(), expectedSize);

    pPicture->uiWidth = attrs.widthPx;
    pPicture->uiHeight = attrs.heightPx;
    pPicture->uiBitsPerComp = attrs.bitsInMemory;
    pPicture->uiComponents = attrs.componentCount;
    pPicture->uiWidthBytes = expectedBytesPerLine;
    pPicture->uiSize = expectedSize;

    return LIM_OK;
}

LIMFILEAPI void Lim_FileFreeString(LIMSTR str)
{
    std::free(str);
}

LIMFILEAPI LIMSIZE Lim_InitPicture(LIMPICTURE* pPicture, LIMUINT width, LIMUINT height, LIMUINT bpc, LIMUINT components)
{
    if (!pPicture || width == 0 || height == 0 || bpc == 0 || components == 0)
    {
        return 0;
    }

    if (pPicture->pImageData != nullptr)
    {
        std::free(pPicture->pImageData);
        pPicture->pImageData = nullptr;
    }

    const LIMSIZE bytesPerComponent = (bpc + 7u) / 8u;
    const LIMSIZE bytesPerPixel = components * bytesPerComponent;
    if (bytesPerPixel == 0)
    {
        return 0;
    }

    const LIMSIZE widthBytes = ((width * bytesPerPixel) + 3u) & ~3u;
    const LIMSIZE totalSize = widthBytes * height;

    void* buffer = std::malloc(totalSize);
    if (!buffer)
    {
        return 0;
    }

    std::memset(buffer, 0, totalSize);

    pPicture->uiWidth = width;
    pPicture->uiHeight = height;
    pPicture->uiBitsPerComp = bpc;
    pPicture->uiComponents = components;
    pPicture->uiWidthBytes = widthBytes;
    pPicture->uiSize = totalSize;
    pPicture->pImageData = buffer;

    return totalSize;
}

LIMFILEAPI void Lim_DestroyPicture(LIMPICTURE* pPicture)
{
    if (!pPicture)
    {
        return;
    }

    if (pPicture->pImageData)
    {
        std::free(pPicture->pImageData);
    }

    pPicture->uiWidth = 0;
    pPicture->uiHeight = 0;
    pPicture->uiBitsPerComp = 0;
    pPicture->uiComponents = 0;
    pPicture->uiWidthBytes = 0;
    pPicture->uiSize = 0;
    pPicture->pImageData = nullptr;
}
