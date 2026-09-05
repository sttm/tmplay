#include "SeratoMarkers2.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace {

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back((std::uint8_t)((value >> 24) & 0xff));
    bytes.push_back((std::uint8_t)((value >> 16) & 0xff));
    bytes.push_back((std::uint8_t)((value >> 8) & 0xff));
    bytes.push_back((std::uint8_t)(value & 0xff));
}

void appendString0(std::vector<std::uint8_t>& bytes, const std::string& value)
{
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0);
}

void appendEntry(std::vector<std::uint8_t>& payload,
                 const std::string& type,
                 const std::vector<std::uint8_t>& data)
{
    appendString0(payload, type);
    appendU32(payload, (std::uint32_t)data.size());
    payload.insert(payload.end(), data.begin(), data.end());
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk = (std::uint32_t)bytes[i] << 16;
        if (i + 1 < bytes.size()) {
            chunk |= (std::uint32_t)bytes[i + 1] << 8;
        }
        if (i + 2 < bytes.size()) {
            chunk |= bytes[i + 2];
        }
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[chunk & 0x3f] : '=');
    }
    return out;
}

int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

std::vector<std::uint8_t> base64Decode(const std::vector<std::uint8_t>& input)
{
    std::vector<std::uint8_t> out;
    int value = 0;
    int bits = -8;
    for (std::uint8_t byte : input) {
        char c = (char)byte;
        if (c == '=') {
            break;
        }
        int decoded = base64Value(c);
        if (decoded < 0) {
            continue;
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back((std::uint8_t)((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, size_t offset)
{
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return ((std::uint32_t)bytes[offset] << 24) |
           ((std::uint32_t)bytes[offset + 1] << 16) |
           ((std::uint32_t)bytes[offset + 2] << 8) |
           (std::uint32_t)bytes[offset + 3];
}

std::vector<SeratoCue> decodePayload(const std::vector<std::uint8_t>& payload)
{
    std::vector<SeratoCue> cues;
    if (payload.size() < 2) {
        return cues;
    }

    size_t pos = 2;
    while (pos < payload.size() && payload[pos] != 0) {
        size_t type_start = pos;
        while (pos < payload.size() && payload[pos] != 0) {
            pos++;
        }
        if (pos >= payload.size()) {
            break;
        }
        std::string type((const char*)payload.data() + type_start,
                         pos - type_start);
        pos++;
        if (pos + 4 > payload.size()) {
            break;
        }
        std::uint32_t size = readU32(payload, pos);
        pos += 4;
        if (pos + size > payload.size()) {
            break;
        }
        if (type == "CUE" && size >= 13) {
            const auto* data = payload.data() + pos;
            int index = std::clamp((int)data[1], 0, 7);
            std::uint32_t ms =
                ((std::uint32_t)data[2] << 24) |
                ((std::uint32_t)data[3] << 16) |
                ((std::uint32_t)data[4] << 8) |
                (std::uint32_t)data[5];
            std::uint32_t color =
                ((std::uint32_t)data[7] << 16) |
                ((std::uint32_t)data[8] << 8) |
                (std::uint32_t)data[9];
            cues.push_back({
                index,
                "CUE " + std::to_string(index + 1),
                (double)ms / 1000.0,
                color,
            });
        }
        pos += size;
    }
    return cues;
}

std::vector<std::uint8_t> lineWrappedBase64(const std::vector<std::uint8_t>& bytes,
                                            bool replacePadding)
{
    std::string encoded = base64Encode(bytes);
    if (replacePadding) {
        std::replace(encoded.begin(), encoded.end(), '=', 'A');
    }

    std::vector<std::uint8_t> out;
    int line_count = 0;
    for (char c : encoded) {
        out.push_back((std::uint8_t)c);
        line_count++;
        if (line_count == 72) {
            out.push_back('\n');
            line_count = 0;
        }
    }
    return out;
}

std::vector<std::uint8_t> mp4Envelope(const std::string& name,
                                      const std::vector<std::uint8_t>& data)
{
    std::vector<std::uint8_t> envelope;
    const std::string mime = "application/octet-stream";
    envelope.insert(envelope.end(), mime.begin(), mime.end());
    envelope.push_back(0);
    envelope.push_back(0);
    envelope.insert(envelope.end(), name.begin(), name.end());
    envelope.push_back(0);
    envelope.insert(envelope.end(), data.begin(), data.end());
    auto wrapped = lineWrappedBase64(envelope, false);
    wrapped.push_back('\n');
    return wrapped;
}

std::vector<std::uint8_t> cueData(const SeratoCue& cue)
{
    std::vector<std::uint8_t> data;
    data.push_back(0);
    data.push_back((std::uint8_t)std::clamp(cue.index, 0, 7));
    appendU32(data, (std::uint32_t)std::llround(std::max(0.0, cue.seconds) * 1000.0));
    data.push_back(0);
    data.push_back((std::uint8_t)((cue.colorRgb >> 16) & 0xff));
    data.push_back((std::uint8_t)((cue.colorRgb >> 8) & 0xff));
    data.push_back((std::uint8_t)(cue.colorRgb & 0xff));
    data.push_back(0);
    data.push_back(0);
    data.push_back(0);
    return data;
}

std::vector<std::uint8_t> decodedMarkers2Payload(const std::vector<SeratoCue>& cues)
{
    std::vector<std::uint8_t> decoded = {0x01, 0x01};

    std::vector<std::uint8_t> color = {0, 0xff, 0xff, 0xff};
    appendEntry(decoded, "COLOR", color);
    for (const auto& cue : cues) {
        appendEntry(decoded, "CUE", cueData(cue));
    }
    std::vector<std::uint8_t> bpm_lock = {0};
    appendEntry(decoded, "BPMLOCK", bpm_lock);
    decoded.push_back(0);
    return decoded;
}

}  // namespace

std::vector<std::uint8_t> encodeSeratoMarkers2(const std::vector<SeratoCue>& cues)
{
    std::string encoded = base64Encode(decodedMarkers2Payload(cues));
    std::vector<std::uint8_t> markers = {0x01, 0x01};
    int line_count = 0;
    for (char c : encoded) {
        markers.push_back((std::uint8_t)c);
        line_count++;
        if (line_count == 72) {
            markers.push_back('\n');
            line_count = 0;
        }
    }
    if (markers.size() < 470) {
        markers.resize(470, 0);
    }
    return markers;
}

std::vector<std::uint8_t> encodeSeratoMarkers2Mp4(const std::vector<SeratoCue>& cues)
{
    std::vector<std::uint8_t> markers = {0x01, 0x01};
    auto inner = lineWrappedBase64(decodedMarkers2Payload(cues), true);
    markers.insert(markers.end(), inner.begin(), inner.end());
    markers.resize(470, 0);
    return mp4Envelope("Serato Markers2", markers);
}

std::vector<std::uint8_t> encodeSeratoMarkers2Ogg(const std::vector<SeratoCue>& cues)
{
    return lineWrappedBase64(decodedMarkers2Payload(cues), true);
}

std::vector<std::uint8_t> encodeSeratoMarkersLegacy(const std::vector<SeratoCue>& cues)
{
    std::vector<std::uint8_t> markers = {0x02, 0x05};
    appendU32(markers, 14);

    auto appendUnset = [&](std::uint8_t type) {
        markers.insert(markers.end(), {0xff, 0xff, 0xff, 0xff});
        markers.insert(markers.end(), {0xff, 0xff, 0xff, 0xff});
        markers.push_back(0x00);
        markers.insert(markers.end(), {0xff, 0xff, 0xff, 0xff});
        markers.insert(markers.end(), {0x00, 0x00, 0x00, 0x00});
        markers.push_back(type);
        markers.push_back(0x00);
    };

    for (int index = 0; index < 5; ++index) {
        auto cue = std::find_if(cues.begin(), cues.end(), [&](const SeratoCue& item) {
            return item.index == index;
        });
        if (cue == cues.end()) {
            appendUnset(0x00);
            continue;
        }

        std::uint32_t ms =
            (std::uint32_t)std::llround(std::max(0.0, cue->seconds) * 1000.0);
        appendU32(markers, ms);
        markers.insert(markers.end(), {0xff, 0xff, 0xff, 0xff});
        markers.push_back(0x00);
        markers.insert(markers.end(), {0xff, 0xff, 0xff, 0xff});
        markers.push_back(0x00);
        markers.push_back((std::uint8_t)((cue->colorRgb >> 16) & 0xff));
        markers.push_back((std::uint8_t)((cue->colorRgb >> 8) & 0xff));
        markers.push_back((std::uint8_t)(cue->colorRgb & 0xff));
        markers.push_back(0x01);
        markers.push_back(0x00);
    }

    for (int index = 5; index < 14; ++index) {
        appendUnset(0x03);
    }

    markers.insert(markers.end(), {0x00, 0xff, 0xff, 0xff});
    return markers;
}

std::vector<std::uint8_t> encodeSeratoMarkersLegacyMp4(const std::vector<SeratoCue>& cues)
{
    return mp4Envelope("Serato Markers_", encodeSeratoMarkersLegacy(cues));
}

std::vector<SeratoCue> decodeSeratoMarkers2(const std::vector<std::uint8_t>& markers)
{
    if (markers.size() < 3 || markers[0] != 0x01 || markers[1] != 0x01) {
        return {};
    }
    std::vector<std::uint8_t> encoded(markers.begin() + 2, markers.end());
    auto payload = base64Decode(encoded);
    return decodePayload(payload);
}

std::vector<SeratoCue> decodeSeratoMarkers2Mp4(const std::vector<std::uint8_t>& markers)
{
    auto envelope = base64Decode(markers);
    const std::string name = "Serato Markers2";
    auto found = std::search(envelope.begin(),
                             envelope.end(),
                             name.begin(),
                             name.end());
    if (found == envelope.end()) {
        return {};
    }
    size_t start = (size_t)std::distance(envelope.begin(), found) + name.size();
    if (start < envelope.size() && envelope[start] == 0) {
        start++;
    }
    if (start >= envelope.size()) {
        return {};
    }
    std::vector<std::uint8_t> direct(envelope.begin() + (long)start,
                                     envelope.end());
    return decodeSeratoMarkers2(direct);
}

std::vector<SeratoCue> decodeSeratoMarkers2Ogg(const std::vector<std::uint8_t>& markers)
{
    return decodePayload(base64Decode(markers));
}
