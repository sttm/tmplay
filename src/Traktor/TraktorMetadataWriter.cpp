#include "TraktorMetadataWriter.h"
#include "TraktorTemplateBlob.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>

#include <taglib/mpegfile.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/privateframe.h>
#include <taglib/tbytevector.h>
#include <taglib/wavfile.h>
#include <taglib/xiphcomment.h>

namespace fs = std::filesystem;

namespace {

std::string lowerExtension(const fs::path& file)
{
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

bool isMp3(const fs::path& file)
{
    return lowerExtension(file) == ".mp3";
}

bool isWav(const fs::path& file)
{
    return lowerExtension(file) == ".wav";
}

bool isFlac(const fs::path& file)
{
    return lowerExtension(file) == ".flac";
}

bool isM4a(const fs::path& file)
{
    auto ext = lowerExtension(file);
    return ext == ".m4a" || ext == ".aac" || ext == ".alac";
}

bool supportsId3v2TraktorTag(const fs::path& file)
{
    return isMp3(file);
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::uint32_t readU32Le(const TagLib::ByteVector& data, int offset)
{
    if (offset + 4 > data.size()) {
        return 0;
    }
    return ((std::uint32_t)(unsigned char)data[offset]) |
        ((std::uint32_t)(unsigned char)data[offset + 1] << 8) |
        ((std::uint32_t)(unsigned char)data[offset + 2] << 16) |
        ((std::uint32_t)(unsigned char)data[offset + 3] << 24);
}

std::uint32_t readU32Be(const std::vector<unsigned char>& data, std::size_t offset)
{
    if (offset + 4 > data.size()) {
        return 0;
    }
    return ((std::uint32_t)data[offset] << 24) |
        ((std::uint32_t)data[offset + 1] << 16) |
        ((std::uint32_t)data[offset + 2] << 8) |
        (std::uint32_t)data[offset + 3];
}

void writeU32Be(std::vector<unsigned char>& data,
                std::size_t offset,
                std::uint32_t value)
{
    if (offset + 4 > data.size()) {
        return;
    }
    data[offset] = (unsigned char)((value >> 24) & 0xffu);
    data[offset + 1] = (unsigned char)((value >> 16) & 0xffu);
    data[offset + 2] = (unsigned char)((value >> 8) & 0xffu);
    data[offset + 3] = (unsigned char)(value & 0xffu);
}

std::int32_t readI32Le(const TagLib::ByteVector& data, int offset)
{
    return (std::int32_t)readU32Le(data, offset);
}

double readDoubleLe(const TagLib::ByteVector& data, int offset)
{
    if (offset + 8 > data.size()) {
        return 0.0;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits |= ((std::uint64_t)(unsigned char)data[offset + i]) << (8 * i);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void writeU32Le(TagLib::ByteVector& data, int offset, std::uint32_t value)
{
    if (offset + 4 > data.size()) {
        return;
    }
    data[offset] = (char)(value & 0xffu);
    data[offset + 1] = (char)((value >> 8) & 0xffu);
    data[offset + 2] = (char)((value >> 16) & 0xffu);
    data[offset + 3] = (char)((value >> 24) & 0xffu);
}

void writeDoubleLe(TagLib::ByteVector& data, int offset, double value)
{
    if (offset + 8 > data.size()) {
        return;
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        data[offset + i] = (char)((bits >> (8 * i)) & 0xffu);
    }
}

void appendU32Le(TagLib::ByteVector& data, std::uint32_t value)
{
    data.append((char)(value & 0xffu));
    data.append((char)((value >> 8) & 0xffu));
    data.append((char)((value >> 16) & 0xffu));
    data.append((char)((value >> 24) & 0xffu));
}

void appendI32Le(TagLib::ByteVector& data, std::int32_t value)
{
    appendU32Le(data, (std::uint32_t)value);
}

void appendDoubleLe(TagLib::ByteVector& data, double value)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        data.append((char)((bits >> (8 * i)) & 0xffu));
    }
}

void appendTraktorString(TagLib::ByteVector& data, const std::string& value)
{
    appendU32Le(data, (std::uint32_t)value.size());
    for (char c : value) {
        data.append(c);
        data.append('\0');
    }
}

TagLib::ByteVector frameIdBytes(const std::string& id)
{
    TagLib::ByteVector bytes;
    for (auto it = id.rbegin(); it != id.rend(); ++it) {
        bytes.append(*it);
    }
    return bytes;
}

TagLib::ByteVector buildFrame(const std::string& id,
                              const TagLib::ByteVector& payload,
                              std::uint32_t children = 0)
{
    TagLib::ByteVector frame = frameIdBytes(id);
    appendU32Le(frame, (std::uint32_t)payload.size());
    appendU32Le(frame, children);
    frame.append(payload);
    return frame;
}

std::string normalizedFrameId(const TagLib::ByteVector& data, int offset)
{
    if (offset + 4 > data.size()) {
        return {};
    }
    std::string id;
    id.reserve(4);
    for (int i = 3; i >= 0; --i) {
        char c = data[offset + i];
        if (c != ' ') {
            id += c;
        }
    }
    return id;
}

std::optional<TagLib::ByteVector> traktor4PrivData(TagLib::ID3v2::Tag* tag)
{
    if (tag == nullptr) {
        return std::nullopt;
    }
    auto frames = tag->frameList("PRIV");
    for (auto* frame : frames) {
        auto* priv = dynamic_cast<TagLib::ID3v2::PrivateFrame*>(frame);
        if (priv != nullptr && priv->owner() == "TRAKTOR4") {
            return priv->data();
        }
    }
    return std::nullopt;
}

std::vector<unsigned char> readBinaryFile(const fs::path& file, std::string& error);

bool writeBinaryFile(const fs::path& file,
                     const std::vector<unsigned char>& data,
                     std::string& error);

TagLib::ByteVector unescapeXiphBinary(const std::string& text)
{
    TagLib::ByteVector data;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i + 3 < text.size() && text[i] == '\\' && text[i + 1] == 'x') {
            int hi = hexValue(text[i + 2]);
            int lo = hexValue(text[i + 3]);
            if (hi >= 0 && lo >= 0) {
                data.append((char)((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        data.append(text[i]);
    }
    return data;
}

std::string escapeXiphBinary(const TagLib::ByteVector& data)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve((std::size_t)data.size() * 4);
    for (int i = 0; i < data.size(); ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 0x20 && c <= 0x7e && c != '\\') {
            text.push_back((char)c);
        } else {
            text += "\\x";
            text.push_back(hex[c >> 4]);
            text.push_back(hex[c & 0x0f]);
        }
    }
    return text;
}

constexpr std::string_view kBase91Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    "!#$%&()*+,./:;<=>?@[]^_`{|}~\"";

std::optional<TagLib::ByteVector> base91Decode(const std::string& text,
                                               std::string& error)
{
    int decode_table[256];
    std::fill(std::begin(decode_table), std::end(decode_table), -1);
    for (std::size_t i = 0; i < kBase91Alphabet.size(); ++i) {
        decode_table[(unsigned char)kBase91Alphabet[i]] = (int)i;
    }

    int value = -1;
    unsigned int bit_buffer = 0;
    int bit_count = 0;
    TagLib::ByteVector output;
    for (unsigned char ch : text) {
        int decoded = decode_table[ch];
        if (decoded < 0) {
            error = "FLAC TRAKTOR4 field has invalid base91 character";
            return std::nullopt;
        }
        if (value < 0) {
            value = decoded;
            continue;
        }
        value += decoded * 91;
        bit_buffer |= (unsigned int)value << bit_count;
        bit_count += (value & 8191) > 88 ? 13 : 14;
        while (bit_count > 7) {
            output.append((char)(bit_buffer & 0xffu));
            bit_buffer >>= 8;
            bit_count -= 8;
        }
        value = -1;
    }
    if (value >= 0) {
        output.append((char)((bit_buffer | ((unsigned int)value << bit_count)) & 0xffu));
    }
    error.clear();
    return output;
}

std::string base91Encode(const TagLib::ByteVector& data)
{
    unsigned int bit_buffer = 0;
    int bit_count = 0;
    std::string output;
    output.reserve((std::size_t)data.size() * 123 / 100 + 8);

    for (int i = 0; i < data.size(); ++i) {
        bit_buffer |= (unsigned int)(unsigned char)data[i] << bit_count;
        bit_count += 8;
        if (bit_count > 13) {
            unsigned int value = bit_buffer & 8191u;
            if (value > 88) {
                bit_buffer >>= 13;
                bit_count -= 13;
            } else {
                value = bit_buffer & 16383u;
                bit_buffer >>= 14;
                bit_count -= 14;
            }
            output.push_back(kBase91Alphabet[value % 91]);
            output.push_back(kBase91Alphabet[value / 91]);
        }
    }
    if (bit_count > 0) {
        output.push_back(kBase91Alphabet[bit_buffer % 91]);
        if (bit_count > 7 || bit_buffer > 90) {
            output.push_back(kBase91Alphabet[bit_buffer / 91]);
        }
    }
    return output;
}

std::optional<TagLib::ByteVector> flacTraktorDataFromFile(const fs::path& file,
                                                          std::string& error)
{
    TagLib::FLAC::File flac(file.c_str(), false);
    if (!flac.isValid()) {
        error = "Could not open FLAC with TagLib";
        return std::nullopt;
    }
    auto* xiph = flac.xiphComment(false);
    if (xiph == nullptr) {
        error = "FLAC has no Xiph comment";
        return std::nullopt;
    }
    auto fields = xiph->fieldListMap();
    auto it = fields.find("ID3V2_PRIV.TRAKTOR4");
    if (it != fields.end() && !it->second.isEmpty()) {
        error.clear();
        return unescapeXiphBinary(it->second.front().to8Bit(true));
    }

    it = fields.find("TRAKTOR4");
    if (it != fields.end() && !it->second.isEmpty()) {
        auto decoded = base91Decode(it->second.front().to8Bit(true), error);
        if (decoded && decoded->size() >= 4 &&
            decoded->mid(0, 4) == TagLib::ByteVector("DMRT")) {
            error.clear();
            return decoded;
        }
        if (error.empty()) {
            error = "FLAC TRAKTOR4 Xiph field is not a binary TRAKTOR4 blob";
        }
        return std::nullopt;
    }

    error = "FLAC has no TRAKTOR4 Xiph field";
    return std::nullopt;
}

bool writeFlacTraktorData(const fs::path& file,
                          const TagLib::ByteVector& blob,
                          std::string& error)
{
    TagLib::FLAC::File flac(file.c_str());
    if (!flac.isValid()) {
        error = "Could not open FLAC with TagLib";
        return false;
    }
    auto* xiph = flac.xiphComment(true);
    if (xiph == nullptr) {
        error = "Could not create FLAC Xiph comment";
        return false;
    }
    xiph->removeFields("ID3V2_PRIV.TRAKTOR4");
    xiph->addField("TRAKTOR4",
                   TagLib::String(base91Encode(blob), TagLib::String::UTF8),
                   true);
    if (!flac.save()) {
        error = "Could not save FLAC";
        return false;
    }
    error.clear();
    return true;
}

TagLib::ByteVector wavBlobFromNitrPayload(const std::vector<unsigned char>& payload)
{
    if (payload.size() >= 16 &&
        std::string(reinterpret_cast<const char*>(payload.data()), 4) == "NTKB" &&
        std::string(reinterpret_cast<const char*>(payload.data() + 8), 4) == "data") {
        std::uint32_t data_size =
            (std::uint32_t)payload[12] |
            ((std::uint32_t)payload[13] << 8) |
            ((std::uint32_t)payload[14] << 16) |
            ((std::uint32_t)payload[15] << 24);
        std::size_t data_start = 16;
        std::size_t data_end = std::min<std::size_t>(payload.size(), data_start + data_size);
        TagLib::ByteVector blob;
        for (std::size_t i = data_start; i < data_end; ++i) {
            blob.append((char)payload[i]);
        }
        return blob;
    }
    return {};
}

std::vector<unsigned char> wavNitrPayloadForBlob(const TagLib::ByteVector& blob)
{
    std::vector<unsigned char> payload;
    payload.insert(payload.end(), {'N', 'T', 'K', 'B'});
    std::uint32_t ntkb_size = 8u + (std::uint32_t)blob.size();
    for (int i = 0; i < 4; ++i) {
        payload.push_back((unsigned char)((ntkb_size >> (8 * i)) & 0xffu));
    }
    payload.insert(payload.end(), {'d', 'a', 't', 'a'});
    for (int i = 0; i < 4; ++i) {
        payload.push_back((unsigned char)(((std::uint32_t)blob.size() >> (8 * i)) & 0xffu));
    }
    for (int i = 0; i < blob.size(); ++i) {
        payload.push_back((unsigned char)blob[i]);
    }
    return payload;
}

struct WavNitrRange {
    std::size_t riffSizeOffset = 4;
    std::size_t listSizeOffset = 0;
    std::size_t infoPayloadStart = 0;
    std::size_t infoPayloadEnd = 0;
    std::size_t nitrChunkStart = 0;
    std::size_t nitrPayloadStart = 0;
    std::size_t nitrPayloadEnd = 0;
    bool hasInfo = false;
    bool hasNitr = false;
};

std::uint32_t readU32LeRaw(const std::vector<unsigned char>& data, std::size_t offset)
{
    if (offset + 4 > data.size()) {
        return 0;
    }
    return (std::uint32_t)data[offset] |
        ((std::uint32_t)data[offset + 1] << 8) |
        ((std::uint32_t)data[offset + 2] << 16) |
        ((std::uint32_t)data[offset + 3] << 24);
}

void writeU32LeRaw(std::vector<unsigned char>& data,
                   std::size_t offset,
                   std::uint32_t value)
{
    if (offset + 4 > data.size()) {
        return;
    }
    data[offset] = (unsigned char)(value & 0xffu);
    data[offset + 1] = (unsigned char)((value >> 8) & 0xffu);
    data[offset + 2] = (unsigned char)((value >> 16) & 0xffu);
    data[offset + 3] = (unsigned char)((value >> 24) & 0xffu);
}

bool findWavNitrRange(const std::vector<unsigned char>& data, WavNitrRange& range)
{
    if (data.size() < 12 ||
        std::string(reinterpret_cast<const char*>(data.data()), 4) != "RIFF" ||
        std::string(reinterpret_cast<const char*>(data.data() + 8), 4) != "WAVE") {
        return false;
    }
    std::size_t offset = 12;
    while (offset + 8 <= data.size()) {
        std::string id(reinterpret_cast<const char*>(data.data() + offset), 4);
        std::uint32_t size = readU32LeRaw(data, offset + 4);
        std::size_t payload = offset + 8;
        std::size_t end = payload + size;
        if (end > data.size()) {
            return false;
        }
        if (id == "LIST" && size >= 4 &&
            std::string(reinterpret_cast<const char*>(data.data() + payload), 4) == "INFO") {
            range.hasInfo = true;
            range.listSizeOffset = offset + 4;
            range.infoPayloadStart = payload + 4;
            range.infoPayloadEnd = end;
            std::size_t sub = payload + 4;
            while (sub + 8 <= end) {
                std::string sid(reinterpret_cast<const char*>(data.data() + sub), 4);
                std::uint32_t ssize = readU32LeRaw(data, sub + 4);
                std::size_t spayload = sub + 8;
                std::size_t send = spayload + ssize;
                if (send > end) {
                    break;
                }
                if (sid == "NITR") {
                    range.hasNitr = true;
                    range.nitrChunkStart = sub;
                    range.nitrPayloadStart = spayload;
                    range.nitrPayloadEnd = send;
                    return true;
                }
                sub = send + (ssize % 2);
            }
            return true;
        }
        offset = end + (size % 2);
    }
    return true;
}

std::optional<TagLib::ByteVector> wavTraktorDataFromFile(const fs::path& file,
                                                         std::string& error)
{
    auto bytes = readBinaryFile(file, error);
    if (!error.empty()) {
        return std::nullopt;
    }
    WavNitrRange range;
    if (!findWavNitrRange(bytes, range) || !range.hasNitr) {
        error = "WAV has no Traktor NITR INFO chunk";
        return std::nullopt;
    }
    std::vector<unsigned char> payload(
        bytes.begin() + range.nitrPayloadStart,
        bytes.begin() + range.nitrPayloadEnd);
    auto blob = wavBlobFromNitrPayload(payload);
    if (blob.isEmpty()) {
        error = "WAV NITR chunk does not contain NTKB/data";
        return std::nullopt;
    }
    error.clear();
    return blob;
}

bool writeWavTraktorData(const fs::path& file,
                         const TagLib::ByteVector& blob,
                         std::string& error)
{
    auto bytes = readBinaryFile(file, error);
    if (!error.empty()) {
        return false;
    }
    WavNitrRange range;
    if (!findWavNitrRange(bytes, range)) {
        error = "Invalid WAV file";
        return false;
    }
    auto payload = wavNitrPayloadForBlob(blob);
    std::vector<unsigned char> nitr_chunk;
    nitr_chunk.insert(nitr_chunk.end(), {'N', 'I', 'T', 'R'});
    for (int i = 0; i < 4; ++i) {
        nitr_chunk.push_back((unsigned char)(((std::uint32_t)payload.size() >> (8 * i)) & 0xffu));
    }
    nitr_chunk.insert(nitr_chunk.end(), payload.begin(), payload.end());
    if (payload.size() % 2) {
        nitr_chunk.push_back(0);
    }

    std::vector<unsigned char> rebuilt;
    long delta = 0;
    if (range.hasNitr) {
        std::uint32_t old_size = readU32LeRaw(bytes, range.nitrChunkStart + 4);
        std::size_t old_end = range.nitrPayloadEnd + (old_size % 2);
        delta = (long)nitr_chunk.size() - (long)(old_end - range.nitrChunkStart);
        rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.begin() + range.nitrChunkStart);
        rebuilt.insert(rebuilt.end(), nitr_chunk.begin(), nitr_chunk.end());
        rebuilt.insert(rebuilt.end(), bytes.begin() + old_end, bytes.end());
    } else if (range.hasInfo) {
        delta = (long)nitr_chunk.size();
        rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.begin() + range.infoPayloadEnd);
        rebuilt.insert(rebuilt.end(), nitr_chunk.begin(), nitr_chunk.end());
        rebuilt.insert(rebuilt.end(), bytes.begin() + range.infoPayloadEnd, bytes.end());
    } else {
        std::vector<unsigned char> list_chunk;
        list_chunk.insert(list_chunk.end(), {'L', 'I', 'S', 'T'});
        std::uint32_t list_size = 4u + (std::uint32_t)nitr_chunk.size();
        for (int i = 0; i < 4; ++i) {
            list_chunk.push_back((unsigned char)((list_size >> (8 * i)) & 0xffu));
        }
        list_chunk.insert(list_chunk.end(), {'I', 'N', 'F', 'O'});
        list_chunk.insert(list_chunk.end(), nitr_chunk.begin(), nitr_chunk.end());
        delta = (long)list_chunk.size();
        rebuilt = bytes;
        rebuilt.insert(rebuilt.end(), list_chunk.begin(), list_chunk.end());
    }

    if (rebuilt.empty()) {
        rebuilt = bytes;
    }
    if (range.hasInfo) {
        writeU32LeRaw(rebuilt, range.listSizeOffset,
                      readU32LeRaw(rebuilt, range.listSizeOffset) + (std::uint32_t)delta);
    }
    writeU32LeRaw(rebuilt, range.riffSizeOffset,
                  readU32LeRaw(rebuilt, range.riffSizeOffset) + (std::uint32_t)delta);
    return writeBinaryFile(file, rebuilt, error);
}

struct Mp4TraktorDataRange {
    std::size_t dataPayloadStart = 0;
    std::size_t dataPayloadEnd = 0;
    std::vector<std::size_t> sizeOffsets;
};

struct Mp4ContainerRange {
    std::size_t frameStart = 0;
    std::size_t frameEnd = 0;
    std::vector<std::size_t> sizeOffsets;
};

std::vector<unsigned char> readBinaryFile(const fs::path& file, std::string& error)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "Could not open file for reading";
        return {};
    }
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

bool writeBinaryFile(const fs::path& file,
                     const std::vector<unsigned char>& data,
                     std::string& error)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "Could not open file for writing";
        return false;
    }
    stream.write(reinterpret_cast<const char*>(data.data()),
                 (std::streamsize)data.size());
    if (!stream) {
        error = "Could not write file";
        return false;
    }
    error.clear();
    return true;
}

bool findMp4NitrDataRange(const std::vector<unsigned char>& data,
                          std::size_t start,
                          std::size_t end,
                          std::vector<std::size_t>& stack,
                          Mp4TraktorDataRange& range)
{
    std::size_t offset = start;
    while (offset + 8 <= end) {
        std::uint32_t size = readU32Be(data, offset);
        if (size < 8 || offset + size > end) {
            return false;
        }
        std::string type(reinterpret_cast<const char*>(data.data() + offset + 4), 4);
        std::size_t payload = offset + 8;
        if (type == "meta") {
            payload += 4; // version/flags
        }
        if (type == "data" &&
            std::find_if(stack.begin(), stack.end(), [&](std::size_t size_offset) {
                return std::string(
                    reinterpret_cast<const char*>(data.data() + size_offset + 4), 4) == "NITR";
            }) != stack.end()) {
            stack.push_back(offset);
            range.dataPayloadStart = payload;
            range.dataPayloadEnd = offset + size;
            range.sizeOffsets = stack;
            stack.pop_back();
            return true;
        }
        if (type == "moov" || type == "udta" || type == "meta" ||
            type == "ilst" || type == "NITR" || type == "NTKB") {
            stack.push_back(offset);
            if (findMp4NitrDataRange(data, payload, offset + size, stack, range)) {
                return true;
            }
            stack.pop_back();
        }
        offset += size;
    }
    return false;
}

bool findMp4ContainerRange(const std::vector<unsigned char>& data,
                           std::size_t start,
                           std::size_t end,
                           const std::string& wanted,
                           std::vector<std::size_t>& stack,
                           Mp4ContainerRange& range)
{
    std::size_t offset = start;
    while (offset + 8 <= end) {
        std::uint32_t size = readU32Be(data, offset);
        if (size < 8 || offset + size > end) {
            return false;
        }
        std::string type(reinterpret_cast<const char*>(data.data() + offset + 4), 4);
        std::size_t payload = offset + 8;
        if (type == "meta") {
            payload += 4;
        }
        if (type == wanted) {
            stack.push_back(offset);
            range.frameStart = offset;
            range.frameEnd = offset + size;
            range.sizeOffsets = stack;
            stack.pop_back();
            return true;
        }
        if (type == "moov" || type == "udta" || type == "meta" || type == "ilst") {
            stack.push_back(offset);
            if (findMp4ContainerRange(data, payload, offset + size,
                                      wanted, stack, range)) {
                return true;
            }
            stack.pop_back();
        }
        offset += size;
    }
    return false;
}

std::vector<unsigned char> beAtom(const std::string& type,
                                  const std::vector<unsigned char>& payload)
{
    std::vector<unsigned char> atom(8 + payload.size());
    writeU32Be(atom, 0, (std::uint32_t)atom.size());
    for (int i = 0; i < 4; ++i) {
        atom[(std::size_t)4 + i] = (unsigned char)type[(std::size_t)i];
    }
    std::copy(payload.begin(), payload.end(), atom.begin() + 8);
    return atom;
}

std::vector<unsigned char> nitrAtomForBlob(const TagLib::ByteVector& blob)
{
    std::vector<unsigned char> blob_bytes;
    blob_bytes.reserve((std::size_t)blob.size());
    for (int i = 0; i < blob.size(); ++i) {
        blob_bytes.push_back((unsigned char)blob[i]);
    }
    auto data_atom = beAtom("data", blob_bytes);
    auto ntkb_atom = beAtom("NTKB", data_atom);
    return beAtom("NITR", ntkb_atom);
}

std::optional<TagLib::ByteVector> m4aTraktorDataFromFile(const fs::path& file,
                                                         std::string& error)
{
    auto bytes = readBinaryFile(file, error);
    if (!error.empty()) {
        return std::nullopt;
    }
    Mp4TraktorDataRange range;
    std::vector<std::size_t> stack;
    if (!findMp4NitrDataRange(bytes, 0, bytes.size(), stack, range)) {
        error = "M4A has no Traktor NITR data atom";
        return std::nullopt;
    }
    TagLib::ByteVector blob;
    for (std::size_t i = range.dataPayloadStart; i < range.dataPayloadEnd; ++i) {
        blob.append((char)bytes[i]);
    }
    error.clear();
    return blob;
}

bool writeM4aTraktorData(const fs::path& file,
                         const TagLib::ByteVector& blob,
                         std::string& error)
{
    auto bytes = readBinaryFile(file, error);
    if (!error.empty()) {
        return false;
    }
    Mp4TraktorDataRange range;
    std::vector<std::size_t> stack;
    if (!findMp4NitrDataRange(bytes, 0, bytes.size(), stack, range)) {
        Mp4ContainerRange ilst;
        std::vector<std::size_t> ilst_stack;
        if (!findMp4ContainerRange(bytes, 0, bytes.size(), "ilst",
                                   ilst_stack, ilst)) {
            error = "M4A has no ilst atom for Traktor NITR insertion";
            return false;
        }
        auto nitr = nitrAtomForBlob(blob);
        std::vector<unsigned char> rebuilt;
        rebuilt.reserve(bytes.size() + nitr.size());
        rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.begin() + ilst.frameEnd);
        rebuilt.insert(rebuilt.end(), nitr.begin(), nitr.end());
        rebuilt.insert(rebuilt.end(), bytes.begin() + ilst.frameEnd, bytes.end());
        for (auto size_offset : ilst.sizeOffsets) {
            std::uint32_t old_size = readU32Be(rebuilt, size_offset);
            writeU32Be(rebuilt, size_offset,
                       old_size + (std::uint32_t)nitr.size());
        }
        return writeBinaryFile(file, rebuilt, error);
    }
    std::vector<unsigned char> replacement;
    replacement.reserve((std::size_t)blob.size());
    for (int i = 0; i < blob.size(); ++i) {
        replacement.push_back((unsigned char)blob[i]);
    }

    std::vector<unsigned char> rebuilt;
    rebuilt.reserve(bytes.size() - (range.dataPayloadEnd - range.dataPayloadStart) +
                    replacement.size());
    rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.begin() + range.dataPayloadStart);
    rebuilt.insert(rebuilt.end(), replacement.begin(), replacement.end());
    rebuilt.insert(rebuilt.end(), bytes.begin() + range.dataPayloadEnd, bytes.end());

    long delta = (long)replacement.size() -
        (long)(range.dataPayloadEnd - range.dataPayloadStart);
    for (auto size_offset : range.sizeOffsets) {
        std::uint32_t old_size = readU32Be(rebuilt, size_offset);
        writeU32Be(rebuilt, size_offset, (std::uint32_t)((long)old_size + delta));
    }
    return writeBinaryFile(file, rebuilt, error);
}

struct TraktorFrameRange {
    int frameStart = 0;
    int dataStart = 0;
    int dataEnd = 0;
    int frameEnd = 0;
};

std::string readTraktorString(const TagLib::ByteVector& data, int& offset, int end)
{
    if (offset + 4 > end) {
        offset = end;
        return {};
    }
    int length = (int)readU32Le(data, offset);
    offset += 4;
    std::string value;
    value.reserve((std::size_t)length);
    for (int i = 0; i < length && offset + 1 < end; ++i) {
        value += data[offset];
        offset += 2;
    }
    return value;
}

bool findCuepDataRange(const TagLib::ByteVector& data,
                       int offset,
                       int end,
                       int& cuep_data_start,
                       int& cuep_data_end)
{
    while (offset + 12 <= end) {
        std::string id = normalizedFrameId(data, offset);
        int length = (int)readU32Le(data, offset + 4);
        int children = (int)readU32Le(data, offset + 8);
        int data_start = offset + 12;
        int data_end = data_start + length;
        if (length < 0 || data_end > end) {
            return false;
        }
        if (id == "CUEP") {
            cuep_data_start = data_start;
            cuep_data_end = data_end;
            return true;
        }
        if (children > 0 &&
            findCuepDataRange(data, data_start, data_end,
                              cuep_data_start, cuep_data_end)) {
            return true;
        }
        offset = data_end;
    }
    return false;
}

bool findFrameRange(const TagLib::ByteVector& data,
                    int offset,
                    int end,
                    const std::string& wanted,
                    TraktorFrameRange& range)
{
    while (offset + 12 <= end) {
        std::string id = normalizedFrameId(data, offset);
        int length = (int)readU32Le(data, offset + 4);
        int children = (int)readU32Le(data, offset + 8);
        int data_start = offset + 12;
        int data_end = data_start + length;
        if (length < 0 || data_end > end) {
            return false;
        }
        if (id == wanted) {
            range = {offset, data_start, data_end, data_end};
            return true;
        }
        if (children > 0 &&
            findFrameRange(data, data_start, data_end, wanted, range)) {
            return true;
        }
        offset = data_end;
    }
    return false;
}

std::uint32_t traktorChecksumSalt(const TagLib::ByteVector& data,
                                  std::string& error)
{
    int offset = 0;
    const int end = data.size();
    while (offset + 12 <= end) {
        std::string id = normalizedFrameId(data, offset);
        int length = (int)readU32Le(data, offset + 4);
        int data_start = offset + 12;
        int data_end = data_start + length;
        if (length < 0 || data_end > end) {
            error = "TRAKTOR4 frame tree is truncated before CHKS";
            return 0x186u;
        }
        if (id == "CHKS") {
            std::uint32_t after_sum = 0;
            for (int i = data_end; i < end; ++i) {
                after_sum += (unsigned char)data[i];
            }
            error.clear();
            return readU32Le(data, data_start) - after_sum;
        }
        if (id == "TRMD" || id == "HDR") {
            offset = data_start;
            continue;
        }
        offset = data_end;
    }
    error = "TRAKTOR4 blob has no CHKS frame";
    return 0x186u;
}

bool updateTraktorChecksum(TagLib::ByteVector& data,
                           std::uint32_t salt,
                           std::string& error)
{
    int offset = 0;
    const int end = data.size();
    while (offset + 12 <= end) {
        std::string id = normalizedFrameId(data, offset);
        int length = (int)readU32Le(data, offset + 4);
        int data_start = offset + 12;
        int data_end = data_start + length;
        if (length < 0 || data_end > end) {
            error = "TRAKTOR4 frame tree is truncated before CHKS";
            return false;
        }
        if (id == "CHKS") {
            std::uint32_t checksum = salt;
            for (int i = data_end; i < end; ++i) {
                checksum += (unsigned char)data[i];
            }
            writeU32Le(data, data_start, checksum);
            error.clear();
            return true;
        }
        if (id == "TRMD" || id == "HDR") {
            offset = data_start;
            continue;
        }
        offset = data_end;
    }
    error = "TRAKTOR4 blob has no CHKS frame";
    return false;
}

TagLib::ByteVector buildCuepPayload(const std::vector<SeratoCue>& cues)
{
    std::vector<SeratoCue> sorted = cues;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.index != b.index) {
            return a.index < b.index;
        }
        return a.seconds < b.seconds;
    });

    TagLib::ByteVector payload;
    appendU32Le(payload, (std::uint32_t)sorted.size());
    for (const auto& cue : sorted) {
        appendU32Le(payload, 1);
        appendTraktorString(payload, cue.name.empty() ? std::string("n.n.") : cue.name);
        appendU32Le(payload, 0);
        appendU32Le(payload, 0);
        appendDoubleLe(payload, cue.seconds * 1000.0);
        appendDoubleLe(payload, 0.0);
        appendI32Le(payload, -1);
        appendI32Le(payload, cue.index);
    }
    return payload;
}

bool rebuildCuepFrame(TagLib::ByteVector& data,
                      const std::vector<SeratoCue>& cues,
                      std::string& error)
{
    std::uint32_t checksum_salt = traktorChecksumSalt(data, error);
    if (!error.empty()) {
        return false;
    }

    TraktorFrameRange cuep;
    TraktorFrameRange data_frame;
    TraktorFrameRange trmd;
    if (!findFrameRange(data, 0, data.size(), "CUEP", cuep) ||
        !findFrameRange(data, 0, data.size(), "DATA", data_frame) ||
        !findFrameRange(data, 0, data.size(), "TRMD", trmd)) {
        error = "Template TRAKTOR4 blob is missing TRMD/DATA/CUEP frames";
        return false;
    }

    TagLib::ByteVector new_cuep = buildFrame("CUEP", buildCuepPayload(cues));
    int old_cuep_size = cuep.frameEnd - cuep.frameStart;
    int delta = new_cuep.size() - old_cuep_size;

    TagLib::ByteVector rebuilt;
    rebuilt.append(data.mid(0, (unsigned int)cuep.frameStart));
    rebuilt.append(new_cuep);
    rebuilt.append(data.mid((unsigned int)cuep.frameEnd));
    data = rebuilt;

    writeU32Le(data, data_frame.frameStart + 4,
               readU32Le(data, data_frame.frameStart + 4) + delta);
    writeU32Le(data, trmd.frameStart + 4,
               readU32Le(data, trmd.frameStart + 4) + delta);
    return updateTraktorChecksum(data, checksum_salt, error);
}

std::vector<SeratoCue> parseCuepData(const TagLib::ByteVector& data,
                                     int offset,
                                     int end)
{
    std::vector<SeratoCue> cues;
    if (offset + 4 > end) {
        return cues;
    }
    int count = (int)readU32Le(data, offset);
    offset += 4;
    cues.reserve((std::size_t)std::max(0, count));
    for (int i = 0; i < count && offset < end; ++i) {
        if (offset + 8 > end) {
            break;
        }
        offset += 4; // Unknown, usually 1.
        std::string name = readTraktorString(data, offset, end);
        if (offset + 28 > end) {
            break;
        }
        int display_order = (int)readU32Le(data, offset);
        offset += 4;
        int type = (int)readU32Le(data, offset);
        offset += 4;
        double start_ms = readDoubleLe(data, offset);
        offset += 8;
        offset += 8; // length
        offset += 4; // repeats
        int hotcue = readI32Le(data, offset);
        offset += 4;

        if (hotcue >= 0 && (type == 0 || type == 3 || type == 5)) {
            int index = hotcue;
            cues.push_back({
                index,
                name.empty() ? std::string("n.n.") : name,
                start_ms / 1000.0,
                0xffffffu,
            });
        }
    }
    std::sort(cues.begin(), cues.end(), [](const auto& a, const auto& b) {
        if (a.index != b.index) {
            return a.index < b.index;
        }
        return a.seconds < b.seconds;
    });
    return cues;
}

bool findCuepFrame(const TagLib::ByteVector& data,
                   int offset,
                   int end,
                   std::vector<SeratoCue>& cues)
{
    while (offset + 12 <= end) {
        std::string id = normalizedFrameId(data, offset);
        int length = (int)readU32Le(data, offset + 4);
        int children = (int)readU32Le(data, offset + 8);
        int data_start = offset + 12;
        int data_end = data_start + length;
        if (length < 0 || data_end > end) {
            return false;
        }
        if (id == "CUEP") {
            cues = parseCuepData(data, data_start, data_end);
            return true;
        }
        if (children > 0 && findCuepFrame(data, data_start, data_end, cues)) {
            return true;
        }
        offset = data_end;
    }
    return false;
}

TraktorEmbeddedMetadataStatus inspectId3v2(TagLib::ID3v2::Tag* tag)
{
    TraktorEmbeddedMetadataStatus status;
    status.supportedContainer = true;
    if (tag == nullptr) {
        status.detail = "No ID3v2 tag";
        return status;
    }

    if (auto data = traktor4PrivData(tag)) {
        status.hasTraktor4Tag = true;
        status.tagSize = (std::size_t)data->size();
        status.detail = "TRAKTOR4 PRIV tag found";
        return status;
    }

    status.detail = "TRAKTOR4 PRIV tag not found";
    return status;
}

std::vector<SeratoCue> readId3v2TraktorCues(TagLib::ID3v2::Tag* tag)
{
    auto data = traktor4PrivData(tag);
    if (!data || data->size() < 12) {
        return {};
    }
    std::vector<SeratoCue> cues;
    findCuepFrame(*data, 0, data->size(), cues);
    return cues;
}

TagLib::ID3v2::Tag* id3v2TagForRead(TagLib::MPEG::File& file)
{
    return file.ID3v2Tag(false);
}

TagLib::ID3v2::Tag* id3v2TagForRead(TagLib::RIFF::WAV::File& file)
{
    return file.hasID3v2Tag() ? file.ID3v2Tag() : nullptr;
}

TagLib::ID3v2::Tag* id3v2TagForRead(TagLib::FLAC::File& file)
{
    return file.ID3v2Tag(false);
}

TagLib::ByteVector embeddedTraktorTemplateData()
{
    TagLib::ByteVector data;
    for (std::size_t i = 0; i < TraktorTemplateBlob::kSize; ++i) {
        data.append((char)TraktorTemplateBlob::kData[i]);
    }
    return data;
}

std::optional<TagLib::ByteVector> traktor4PrivDataFromFile(const fs::path& file,
                                                           std::string& error)
{
    if (!supportsId3v2TraktorTag(file)) {
        error = "Embedded TRAKTOR4 template currently supports MP3/WAV/FLAC ID3v2 only";
        return std::nullopt;
    }
    TagLib::ID3v2::Tag* tag = nullptr;
    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str(), false);
        if (!mp3.isValid()) {
            error = "Could not open MP3 with TagLib";
            return std::nullopt;
        }
        tag = id3v2TagForRead(mp3);
        auto data = traktor4PrivData(tag);
        if (!data) {
            error = "MP3 has no TRAKTOR4 PRIV tag";
            return std::nullopt;
        }
        error.clear();
        return data;
    }
    if (isWav(file)) {
        TagLib::RIFF::WAV::File wav(file.c_str(), false);
        if (!wav.isValid()) {
            error = "Could not open WAV with TagLib";
            return std::nullopt;
        }
        tag = id3v2TagForRead(wav);
    } else if (isFlac(file)) {
        TagLib::FLAC::File flac(file.c_str(), false);
        if (!flac.isValid()) {
            error = "Could not open FLAC with TagLib";
            return std::nullopt;
        }
        tag = id3v2TagForRead(flac);
    }
    auto data = traktor4PrivData(tag);
    if (!data) {
        error = "File has no TRAKTOR4 PRIV tag";
        return std::nullopt;
    }
    error.clear();
    return data;
}

std::vector<SeratoCue> traktorCuesForTrack(const LibraryTrack& track)
{
    std::vector<SeratoCue> cues;
    cues.reserve(track.cues.size());
    for (const auto& cue : track.cues) {
        if (cue.index < 0 || cue.index > 7 || cue.positionSeconds < 0.0) {
            continue;
        }
        cues.push_back({
            cue.index,
            cue.name.empty() ? std::string("n.n.") : cue.name,
            cue.positionSeconds,
            0xffffffu,
        });
    }
    return cues;
}

bool writeTraktor4PrivFrame(const fs::path& file,
                            const TagLib::ByteVector& data,
                            std::string& error)
{
    if (!supportsId3v2TraktorTag(file)) {
        error = "Embedded TRAKTOR4 writing currently supports MP3/WAV/FLAC ID3v2 only";
        return false;
    }

    auto write_frame = [&](TagLib::ID3v2::Tag* tag) -> bool {
        if (tag == nullptr) {
            error = "Could not create ID3v2 tag";
            return false;
        }

        auto frames = tag->frameList("PRIV");
        for (auto* frame : frames) {
            auto* priv = dynamic_cast<TagLib::ID3v2::PrivateFrame*>(frame);
            if (priv != nullptr && priv->owner() == "TRAKTOR4") {
                tag->removeFrame(frame);
            }
        }

        auto* frame = new TagLib::ID3v2::PrivateFrame();
        frame->setOwner("TRAKTOR4");
        frame->setData(data);
        tag->addFrame(frame);
        return true;
    };

    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str());
        if (!mp3.isValid() || !write_frame(mp3.ID3v2Tag(true))) {
            if (error.empty()) {
                error = "Could not open output MP3 with TagLib";
            }
            return false;
        }
        if (!mp3.save()) {
            error = "Could not save output MP3";
            return false;
        }
    } else if (isWav(file)) {
        TagLib::RIFF::WAV::File wav(file.c_str());
        if (!wav.isValid() || !write_frame(wav.ID3v2Tag())) {
            if (error.empty()) {
                error = "Could not open output WAV with TagLib";
            }
            return false;
        }
        if (!wav.save(TagLib::RIFF::WAV::File::ID3v2,
                      TagLib::File::StripNone)) {
            error = "Could not save output WAV";
            return false;
        }
    } else if (isFlac(file)) {
        TagLib::FLAC::File flac(file.c_str());
        if (!flac.isValid() || !write_frame(flac.ID3v2Tag(true))) {
            if (error.empty()) {
                error = "Could not open output FLAC with TagLib";
            }
            return false;
        }
        if (!flac.save()) {
            error = "Could not save output FLAC";
            return false;
        }
    }
    error.clear();
    return true;
}

}  // namespace

TraktorEmbeddedMetadataStatus
TraktorMetadataWriter::inspect(const fs::path& file, std::string& error) const
{
    if (isM4a(file)) {
        auto data = m4aTraktorDataFromFile(file, error);
        if (!data) {
            error.clear();
            return {true, false, 0, "M4A Traktor NITR data atom not found"};
        }
        return {true, true, (std::size_t)data->size(), "M4A Traktor NITR data atom found"};
    }
    if (isFlac(file)) {
        auto data = flacTraktorDataFromFile(file, error);
        if (!data) {
            error.clear();
            return {true, false, 0, "FLAC Traktor Xiph field not found"};
        }
        return {true, true, (std::size_t)data->size(), "FLAC Traktor Xiph field found"};
    }
    if (isWav(file)) {
        auto data = wavTraktorDataFromFile(file, error);
        if (!data) {
            error.clear();
            return {true, false, 0, "WAV Traktor NITR INFO chunk not found"};
        }
        return {true, true, (std::size_t)data->size(), "WAV Traktor NITR INFO chunk found"};
    }
    if (!supportsId3v2TraktorTag(file)) {
        return {
            false,
            false,
            0,
            "Embedded TRAKTOR4 detection currently supports MP3/WAV/FLAC ID3v2 only",
        };
    }

    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str(), false);
        if (!mp3.isValid()) {
            error = "Could not open MP3 with TagLib";
            return {};
        }
        return inspectId3v2(id3v2TagForRead(mp3));
    }
    if (isWav(file)) {
        TagLib::RIFF::WAV::File wav(file.c_str(), false);
        if (!wav.isValid()) {
            error = "Could not open WAV with TagLib";
            return {};
        }
        return inspectId3v2(id3v2TagForRead(wav));
    }
    TagLib::FLAC::File flac(file.c_str(), false);
    if (!flac.isValid()) {
        error = "Could not open FLAC with TagLib";
        return {};
    }
    return inspectId3v2(id3v2TagForRead(flac));
}

std::vector<SeratoCue>
TraktorMetadataWriter::readCues(const fs::path& file, std::string& error) const
{
    if (isM4a(file)) {
        auto data = m4aTraktorDataFromFile(file, error);
        if (!data) {
            return {};
        }
        std::vector<SeratoCue> cues;
        findCuepFrame(*data, 0, data->size(), cues);
        error.clear();
        return cues;
    }
    if (isFlac(file)) {
        auto data = flacTraktorDataFromFile(file, error);
        if (!data) {
            return {};
        }
        std::vector<SeratoCue> cues;
        findCuepFrame(*data, 0, data->size(), cues);
        error.clear();
        return cues;
    }
    if (isWav(file)) {
        auto data = wavTraktorDataFromFile(file, error);
        if (!data) {
            return {};
        }
        std::vector<SeratoCue> cues;
        findCuepFrame(*data, 0, data->size(), cues);
        error.clear();
        return cues;
    }
    if (!supportsId3v2TraktorTag(file)) {
        error = "Embedded TRAKTOR4 cue read currently supports MP3/WAV/FLAC ID3v2 and M4A NITR";
        return {};
    }

    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str(), false);
        if (!mp3.isValid()) {
            error = "Could not open MP3 with TagLib";
            return {};
        }
        error.clear();
        return readId3v2TraktorCues(id3v2TagForRead(mp3));
    }
    if (isWav(file)) {
        TagLib::RIFF::WAV::File wav(file.c_str(), false);
        if (!wav.isValid()) {
            error = "Could not open WAV with TagLib";
            return {};
        }
        error.clear();
        return readId3v2TraktorCues(id3v2TagForRead(wav));
    }
    TagLib::FLAC::File flac(file.c_str(), false);
    if (!flac.isValid()) {
        error = "Could not open FLAC with TagLib";
        return {};
    }
    error.clear();
    return readId3v2TraktorCues(id3v2TagForRead(flac));
}

bool TraktorMetadataWriter::writeCues(const LibraryTrack& track,
                                      std::string& error,
                                      TraktorEmbeddedMetadataStatus* status) const
{
    auto inspected = inspect(track.path, error);
    if (status != nullptr) {
        *status = inspected;
    }
    if (!error.empty()) {
        return false;
    }
    if (!inspected.supportedContainer) {
        error.clear();
        return true;
    }

    auto cues = traktorCuesForTrack(track);
    if (cues.empty() && !inspected.hasTraktor4Tag) {
        error.clear();
        return true;
    }

    std::optional<TagLib::ByteVector> data;
    if (isM4a(track.path)) {
        if (!inspected.hasTraktor4Tag) {
            data = embeddedTraktorTemplateData();
        } else {
            data = m4aTraktorDataFromFile(track.path, error);
        }
    } else if (isFlac(track.path)) {
        if (inspected.hasTraktor4Tag) {
            data = flacTraktorDataFromFile(track.path, error);
        } else {
            data = embeddedTraktorTemplateData();
        }
    } else if (isWav(track.path)) {
        if (inspected.hasTraktor4Tag) {
            data = wavTraktorDataFromFile(track.path, error);
        } else {
            data = embeddedTraktorTemplateData();
        }
    } else if (inspected.hasTraktor4Tag) {
        data = traktor4PrivDataFromFile(track.path, error);
    } else {
        data = embeddedTraktorTemplateData();
    }
    if (!data) {
        return false;
    }

    TagLib::ByteVector modified = *data;
    if (!rebuildCuepFrame(modified, cues, error)) {
        return false;
    }
    if (isM4a(track.path)) {
        return writeM4aTraktorData(track.path, modified, error);
    }
    if (isFlac(track.path)) {
        return writeFlacTraktorData(track.path, modified, error);
    }
    if (isWav(track.path)) {
        return writeWavTraktorData(track.path, modified, error);
    }
    return writeTraktor4PrivFrame(track.path, modified, error);
}

bool TraktorMetadataWriter::makeCueTemplateTestFile(
    const fs::path& cleanInput,
    const fs::path& templateFile,
    const fs::path& output,
    const std::vector<SeratoCue>& cues,
    std::string& error) const
{
    if ((!supportsId3v2TraktorTag(cleanInput) && !isM4a(cleanInput) &&
         !isFlac(cleanInput) && !isWav(cleanInput)) ||
        (!supportsId3v2TraktorTag(output) && !isM4a(output) &&
         !isFlac(output) && !isWav(output)) ||
        (!templateFile.empty() && !isMp3(templateFile))) {
        error = "Traktor cue template test currently supports MP3/M4A/WAV/FLAC files";
        return false;
    }
    if (!fs::is_regular_file(cleanInput)) {
        error = "Clean input file does not exist";
        return false;
    }
    if (!templateFile.empty() && !fs::is_regular_file(templateFile)) {
        error = "Template file does not exist";
        return false;
    }
    if (cues.empty()) {
        error = "No cues requested";
        return false;
    }

    TagLib::ByteVector modified;
    if (isM4a(cleanInput)) {
        auto data = m4aTraktorDataFromFile(cleanInput, error);
        modified = data ? *data : embeddedTraktorTemplateData();
        error.clear();
    } else if (isFlac(cleanInput)) {
        auto data = flacTraktorDataFromFile(cleanInput, error);
        modified = data ? *data : embeddedTraktorTemplateData();
        error.clear();
    } else if (isWav(cleanInput)) {
        auto data = wavTraktorDataFromFile(cleanInput, error);
        modified = data ? *data : embeddedTraktorTemplateData();
        error.clear();
    } else if (templateFile.empty()) {
        modified = embeddedTraktorTemplateData();
    } else {
        auto data = traktor4PrivDataFromFile(templateFile, error);
        if (!data) {
            return false;
        }
        modified = *data;
    }
    if (!rebuildCuepFrame(modified, cues, error)) {
        return false;
    }

    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    if (ec) {
        error = "Could not create output directory: " + ec.message();
        return false;
    }
    fs::copy_file(cleanInput, output, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Could not copy clean MP3 to output: " + ec.message();
        return false;
    }

    if (isM4a(output)) {
        return writeM4aTraktorData(output, modified, error);
    }
    if (isFlac(output)) {
        return writeFlacTraktorData(output, modified, error);
    }
    if (isWav(output)) {
        return writeWavTraktorData(output, modified, error);
    }
    return writeTraktor4PrivFrame(output, modified, error);
}
