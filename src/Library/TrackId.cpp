#include "TrackId.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kSampleBytes = 1024 * 1024;

std::int64_t fileMtimeSeconds(const fs::path& path)
{
    std::error_code ec;
    auto time = fs::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(
        system_time.time_since_epoch()).count();
}

std::string hexBytes(const unsigned char* data, std::size_t size)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        stream << std::setw(2) << (int)data[i];
    }
    return stream.str();
}

std::string fallbackHash(const std::string& data)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

std::string sha256Hex(const std::string& data)
{
#if defined(__APPLE__)
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(data.data(), (CC_LONG)data.size(), digest.data());
    return hexBytes(digest.data(), digest.size());
#else
    return fallbackHash(data);
#endif
}

void appendFileSample(std::string& data,
                      std::ifstream& stream,
                      std::uintmax_t offset,
                      std::size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    std::vector<char> buffer(bytes);
    stream.seekg((std::streamoff)offset, std::ios::beg);
    stream.read(buffer.data(), (std::streamsize)buffer.size());
    data.append(buffer.data(), (std::size_t)std::max<std::streamsize>(0, stream.gcount()));
}

}  // namespace

TrackIdentity generateTrackIdentity(const fs::path& path, double /*durationSeconds*/)
{
    TrackIdentity identity;

    std::error_code ec;
    identity.fileSize = fs::file_size(path, ec);
    if (ec) {
        identity.fileSize = 0;
    }
    identity.fileMtime = fileMtimeSeconds(path);

    std::string hash_input;
    hash_input.reserve(128 + std::min<std::uintmax_t>(identity.fileSize, kSampleBytes * 2));
    hash_input += std::to_string(identity.fileSize);
    hash_input += "|";

    std::ifstream stream(path, std::ios::binary);
    if (stream) {
        std::size_t first_bytes =
            (std::size_t)std::min<std::uintmax_t>(identity.fileSize, kSampleBytes);
        appendFileSample(hash_input, stream, 0, first_bytes);
        if (identity.fileSize > kSampleBytes) {
            std::uintmax_t offset = identity.fileSize >
                    (std::uintmax_t)kSampleBytes
                ? identity.fileSize - kSampleBytes
                : 0;
            appendFileSample(hash_input, stream, offset, kSampleBytes);
        }
    } else {
        hash_input += path.filename().string();
    }

    identity.contentHash = sha256Hex(hash_input);
    identity.id = identity.contentHash;
    return identity;
}
