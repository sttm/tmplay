#include "MetadataWriter.hpp"

#include "ProcessRunner.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <vector>

#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4tag.h>
#include <taglib/tstringlist.h>

namespace fs = std::filesystem;

namespace {

std::string lowercaseExtension(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return extension;
}

bool writeMp4Metadata(const fs::path& path,
                      const AudioMetadata& metadata,
                      std::string& error)
{
    TagLib::MP4::File file(path.c_str(), false);
    if (!file.isValid()) {
        error = "Unable to open MP4/M4A tags";
        return false;
    }

    TagLib::MP4::Tag* tag = file.tag();
    if (tag == nullptr) {
        error = "Unable to create MP4/M4A tags";
        return false;
    }

    if (metadata.bpm > 0.0) {
        tag->setItem("tmpo", TagLib::MP4::Item((unsigned int)std::lround(metadata.bpm)));
    }
    if (!metadata.key.empty()) {
        TagLib::StringList key;
        key.append(metadata.key);
        tag->setItem("----:com.apple.iTunes:initialkey", TagLib::MP4::Item(key));
        tag->setItem("----:com.apple.iTunes:KEY", TagLib::MP4::Item(key));
    }
    if (!metadata.genre.empty()) {
        tag->setGenre(TagLib::String(metadata.genre, TagLib::String::UTF8));
    }

    if (!file.save()) {
        error = "Unable to save MP4/M4A tags";
        return false;
    }
    return true;
}

std::string ffmpegMuxerForExtension(const std::string& extension)
{
    if (extension == ".alac") {
        return "ipod";
    }
    return {};
}

}  // namespace

bool MetadataWriter::write(const std::string& path,
                           const AudioMetadata& metadata,
                           std::string& error) const
{
    auto ffmpeg = ProcessRunner::findExecutable("ffmpeg");
    if (!ffmpeg) {
        error = "ffmpeg not found";
        return false;
    }
    if (metadata.bpm <= 0.0 && metadata.key.empty() && metadata.genre.empty()) {
        error = "No metadata to write";
        return false;
    }

    fs::path original(path);
    std::string extension = lowercaseExtension(original);
    if (extension == ".m4a" || extension == ".mp4" || extension == ".mov" ||
        extension == ".aac" || extension == ".alac") {
        return writeMp4Metadata(original, metadata, error);
    }

    fs::path temp = original.parent_path() /
        (original.stem().string() + ".tmplay_tmp" + original.extension().string());

    std::vector<std::string> args = {
        *ffmpeg, "-nostdin", "-loglevel", "error", "-y",
        "-i", original.string(),
        "-map", "0", "-map_metadata", "0",
    };
    if (metadata.bpm > 0.0) {
        std::string bpm = std::to_string((int)std::lround(metadata.bpm));
        args.push_back("-metadata");
        args.push_back("BPM=" + bpm);
        args.push_back("-metadata");
        args.push_back("TBPM=" + bpm);
    }
    if (!metadata.key.empty()) {
        args.push_back("-metadata");
        args.push_back("INITIALKEY=" + metadata.key);
        args.push_back("-metadata");
        args.push_back("KEY=" + metadata.key);
    }
    if (!metadata.genre.empty()) {
        args.push_back("-metadata");
        args.push_back("genre=" + metadata.genre);
    }
    args.insert(args.end(), {"-c", "copy"});
    std::string muxer = ffmpegMuxerForExtension(extension);
    if (!muxer.empty()) {
        args.insert(args.end(), {"-f", muxer});
    }
    args.push_back(temp.string());

    if (ProcessRunner::run(args) != 0 || !fs::exists(temp)) {
        std::error_code ec;
        fs::remove(temp, ec);
        error = "Unable to write audio tags";
        return false;
    }

    std::error_code ec;
    fs::rename(temp, original, ec);
    if (ec) {
        fs::remove(temp, ec);
        error = "Unable to replace tagged audio file";
        return false;
    }
    return true;
}
