#include "SeratoCueWriter.h"

#include "SeratoMarkers2.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <taglib/flacfile.h>
#include <taglib/generalencapsulatedobjectframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tbytevector.h>
#include <taglib/tstringlist.h>
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

bool isFlac(const fs::path& file)
{
    return lowerExtension(file) == ".flac";
}

bool isSupportedSeratoContainer(const fs::path& file)
{
    return isMp3(file) || isFlac(file);
}

TagLib::ByteVector toByteVector(const std::vector<std::uint8_t>& bytes)
{
    return TagLib::ByteVector(reinterpret_cast<const char*>(bytes.data()),
                              (unsigned int)bytes.size());
}

TagLib::String toLatin1String(const std::vector<std::uint8_t>& bytes)
{
    return TagLib::String(std::string(reinterpret_cast<const char*>(bytes.data()),
                                      bytes.size()),
                          TagLib::String::Latin1);
}

std::vector<std::uint8_t> fromByteVector(const TagLib::ByteVector& value)
{
    std::vector<std::uint8_t> out((size_t)value.size());
    for (int i = 0; i < value.size(); ++i) {
        out[(size_t)i] = (std::uint8_t)value[i];
    }
    return out;
}

std::vector<std::uint8_t> fromTagLibString(const TagLib::String& value)
{
    std::string text = value.to8Bit(true);
    return {text.begin(), text.end()};
}

std::vector<SeratoCue> readId3Markers2(TagLib::ID3v2::Tag* tag)
{
    if (tag == nullptr) {
        return {};
    }
    auto frames = tag->frameList("GEOB");
    for (auto* frame : frames) {
        auto* geob = dynamic_cast<TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
        if (geob != nullptr && geob->description() == "Serato Markers2") {
            return decodeSeratoMarkers2(fromByteVector(geob->object()));
        }
    }
    return {};
}

bool isSeratoMarkerDescription(const TagLib::String& description)
{
    std::string value = description.to8Bit(true);
    return value == "Serato Markers2" ||
        value == "Serato Markers_" ||
        value == "Serato Markers";
}

bool writeId3Markers2(TagLib::ID3v2::Tag* tag,
                      const std::vector<SeratoCue>& cues,
                      bool overwriteExistingCues,
                      std::string& error)
{
    if (tag == nullptr) {
        error = "Could not create ID3v2 tag";
        return false;
    }

    auto frames = tag->frameList("GEOB");
    for (auto* frame : frames) {
        auto* geob = dynamic_cast<TagLib::ID3v2::GeneralEncapsulatedObjectFrame*>(frame);
        if (geob != nullptr && isSeratoMarkerDescription(geob->description())) {
            if (!overwriteExistingCues) {
                return true;
            }
            tag->removeFrame(frame, true);
        }
    }

    if (cues.empty()) {
        return true;
    }

    auto* markers = new TagLib::ID3v2::GeneralEncapsulatedObjectFrame;
    markers->setTextEncoding(TagLib::String::Latin1);
    markers->setMimeType("application/octet-stream");
    markers->setFileName("");
    markers->setDescription("Serato Markers2");
    markers->setObject(toByteVector(encodeSeratoMarkers2(cues)));
    tag->addFrame(markers);
    return true;
}

std::vector<std::uint8_t> readXiphField(TagLib::Ogg::XiphComment* tag,
                                        const TagLib::StringList& keys)
{
    if (tag == nullptr) {
        return {};
    }
    const auto& fields = tag->fieldListMap();
    for (const auto& key : keys) {
        if (!fields.contains(key)) {
            continue;
        }
        const auto values = fields[key];
        if (!values.isEmpty()) {
            return fromTagLibString(values.front());
        }
    }
    return {};
}

bool xiphHasAny(TagLib::Ogg::XiphComment* tag, const TagLib::StringList& keys)
{
    if (tag == nullptr) {
        return false;
    }
    const auto& fields = tag->fieldListMap();
    for (const auto& key : keys) {
        if (fields.contains(key)) {
            return true;
        }
    }
    return false;
}

void removeXiphFields(TagLib::Ogg::XiphComment* tag,
                      const TagLib::StringList& keys)
{
    if (tag == nullptr) {
        return;
    }
    for (const auto& key : keys) {
        tag->removeFields(key);
    }
}

void replaceXiphField(TagLib::Ogg::XiphComment* tag,
                      const TagLib::StringList& keys,
                      const TagLib::String& writeKey,
                      const std::vector<std::uint8_t>& bytes)
{
    removeXiphFields(tag, keys);
    tag->addField(writeKey, toLatin1String(bytes), true);
}

bool createBackup(const fs::path& file, std::string& error)
{
    fs::path backup = file;
    backup += ".autocue.bak";
    std::error_code ec;
    if (!fs::exists(backup, ec)) {
        fs::copy_file(file, backup, fs::copy_options::none, ec);
        if (ec) {
            error = "Could not create backup: " + ec.message();
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<SeratoCue> SeratoCueWriter::readCues(const fs::path& file,
                                                 std::string& error) const
{
    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str(), false);
        if (!mp3.isValid()) {
            error = "Could not open MP3 with TagLib";
            return {};
        }
        error.clear();
        return readId3Markers2(mp3.ID3v2Tag(false));
    }

    if (isFlac(file)) {
        TagLib::FLAC::File flac(file.c_str(), false);
        if (!flac.isValid()) {
            error = "Could not open FLAC with TagLib";
            return {};
        }
    TagLib::StringList keys;
    keys.append("SERATO_MARKERS_V2");
    keys.append("serato_markers_v2");
    keys.append("SERATO_MARKERS2");
    keys.append("serato_markers2");
    keys.append("SERATO_MARKERS");
    keys.append("serato_markers");
    error.clear();
    return decodeSeratoMarkers2Mp4(readXiphField(flac.xiphComment(false), keys));
    }

    error = "Serato cues are stored only in MP3 or FLAC";
    return {};
}

bool SeratoCueWriter::writeCues(const fs::path& file,
                                const std::vector<SeratoCue>& cues,
                                std::string& error)
{
    return writeCues(file, cues, error, true, true);
}

bool SeratoCueWriter::writeCues(const fs::path& file,
                                const std::vector<SeratoCue>& cues,
                                std::string& error,
                                bool backupBeforeWrite,
                                bool overwriteExistingCues)
{
    if (!isSupportedSeratoContainer(file)) {
        error = "Serato cues can be written only to MP3 or FLAC";
        return false;
    }

    if (backupBeforeWrite && !createBackup(file, error)) {
        return false;
    }

    if (isMp3(file)) {
        TagLib::MPEG::File mp3(file.c_str(), false);
        if (!mp3.isValid()) {
            error = "Could not open MP3 with TagLib";
            return false;
        }
        if (!writeId3Markers2(mp3.ID3v2Tag(true), cues, overwriteExistingCues, error)) {
            return false;
        }
        if (!mp3.save(TagLib::MPEG::File::ID3v2,
                      TagLib::MPEG::File::StripOthers,
                      TagLib::ID3v2::v4)) {
            error = "TagLib failed to save MP3 ID3v2 tag";
            return false;
        }
        error.clear();
        return true;
    }

    TagLib::FLAC::File flac(file.c_str(), false);
    if (!flac.isValid()) {
        error = "Could not open FLAC with TagLib";
        return false;
    }
    TagLib::Ogg::XiphComment* tag = flac.xiphComment(true);
    TagLib::StringList keys;
    keys.append("SERATO_MARKERS_V2");
    keys.append("serato_markers_v2");
    keys.append("SERATO_MARKERS2");
    keys.append("serato_markers2");
    keys.append("SERATO_MARKERS");
    keys.append("serato_markers");
    if (!overwriteExistingCues && xiphHasAny(tag, keys)) {
        error.clear();
        return true;
    }
    if (cues.empty()) {
        removeXiphFields(tag, keys);
    } else {
        replaceXiphField(tag, keys, "SERATO_MARKERS_V2", encodeSeratoMarkers2Mp4(cues));
    }
    if (!flac.save()) {
        error = "TagLib failed to save FLAC Vorbis comment";
        return false;
    }
    error.clear();
    return true;
}
