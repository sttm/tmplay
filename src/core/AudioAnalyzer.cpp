#include "AudioAnalyzer.hpp"

#include "ProcessRunner.hpp"

#include <algorithmfactory.h>
#include <essentia.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4tag.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <vector>

#if defined(TPLAY_WITH_ONNXRUNTIME)
#include <onnxruntime_cxx_api.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

double parseNumber(const std::string& value)
{
    try {
        return std::stod(value);
    } catch (...) {
        return 0.0;
    }
}

std::string compactTagName(std::string value)
{
    value = lowercase(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](char c) {
                    return c == ' ' || c == '_' || c == '-';
                }),
                value.end());
    return value;
}

bool containsText(const std::string& value, const std::string& needle)
{
    return value.find(needle) != std::string::npos;
}

std::string lowercaseExtension(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return extension;
}

void readMp4TagMetadata(const std::string& path, AudioMetadata& metadata)
{
    TagLib::MP4::File file(path.c_str(), true);
    if (!file.isValid() || file.tag() == nullptr) {
        return;
    }

    auto* tag = file.tag();
    auto readString = [&](const char* name) {
        if (!tag->contains(name)) {
            return std::string{};
        }
        auto values = tag->item(name).toStringList();
        if (values.isEmpty()) {
            return std::string{};
        }
        return values.front().to8Bit(true);
    };

    if (metadata.bpm <= 0.0 && tag->contains("tmpo")) {
        auto values = tag->item("tmpo").toIntPair();
        if (values.first > 0) {
            metadata.bpm = values.first;
        }
    }
    if (metadata.key.empty()) {
        metadata.key = readString("----:com.apple.iTunes:initialkey");
    }
    if (metadata.key.empty()) {
        metadata.key = readString("----:com.apple.iTunes:KEY");
    }
    if (metadata.key.empty()) {
        metadata.key = readString("©key");
    }
}

void ensureEssentiaInitialized()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        essentia::init();
    });
}

void parseKeyValueOutput(const std::string& output, AudioMetadata& metadata)
{
    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        std::string name = lowercase(ProcessRunner::trim(line.substr(0, separator)));
        std::string value = ProcessRunner::trim(line.substr(separator + 1));
        std::string compact_name = compactTagName(name);
        bool is_tag = compact_name.starts_with("tag:");
        if (name == "title" || name == "tag:title") {
            metadata.title = value;
        } else if (name == "artist" || name == "tag:artist" ||
                   name == "tag:album_artist") {
            if (metadata.artist.empty()) {
                metadata.artist = value;
            }
        } else if (name == "album" || name == "tag:album") {
            metadata.album = value;
        } else if (name == "duration" || name == "format.duration") {
            metadata.duration = parseNumber(value);
        } else if (name == "bit_rate" || name == "format.bit_rate") {
            metadata.bitrateKbps = parseNumber(value) / 1000.0;
        } else if (name == "sample_rate" ||
                   name.ends_with(".sample_rate")) {
            metadata.sampleRateHz = parseNumber(value);
        } else if (name == "size" || name == "format.size") {
            metadata.sizeBytes = (std::uintmax_t)parseNumber(value);
        } else if (name == "bpm" || name == "tag:bpm" ||
                   name == "tag:tbpm" || name == "tag:tmpo" ||
                   name == "tag:tempo" ||
                   (is_tag && (containsText(compact_name, "bpm") ||
                               containsText(compact_name, "tbpm") ||
                               containsText(compact_name, "tmpo") ||
                               compact_name.ends_with(":tempo")))) {
            metadata.bpm = parseNumber(value);
        } else if (name == "key" || name == "tag:initialkey" ||
                   name == "tag:initial key" || name == "tag:initial_key" ||
                   name == "tag:key" ||
                   (is_tag && (containsText(compact_name, "initialkey") ||
                               compact_name.ends_with(":key") ||
                               compact_name.ends_with(":tkey")))) {
            metadata.key = value;
        } else if (name == "genre" || name == "tag:genre") {
            metadata.genre = value;
        }
    }
}

std::array<float, 12> chromaFromSpectrum(const std::vector<essentia::Real>& spectrum,
                                         double sampleRate,
                                         int frameSize)
{
    std::array<float, 12> chroma{};
    if (spectrum.empty() || frameSize <= 0) {
        return chroma;
    }

    for (size_t i = 1; i < spectrum.size(); ++i) {
        double frequency = (double)i * sampleRate / (double)frameSize;
        if (frequency < 40.0 || frequency > 5000.0) {
            continue;
        }
        double midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
        int pitchClass = ((int)std::llround(midi) % 12 + 12) % 12;
        chroma[(size_t)pitchClass] += (float)spectrum[i];
    }

    float sum = std::accumulate(chroma.begin(), chroma.end(), 0.0f);
    if (sum > 0.0f) {
        for (float& value : chroma) {
            value /= sum;
        }
    }
    return chroma;
}

float spectralCentroidFromSpectrum(const std::vector<essentia::Real>& spectrum,
                                   double sampleRate,
                                   int frameSize)
{
    double weighted = 0.0;
    double total = 0.0;
    for (size_t i = 1; i < spectrum.size(); ++i) {
        double frequency = (double)i * sampleRate / (double)frameSize;
        double magnitude = std::max(0.0f, spectrum[i]);
        weighted += frequency * magnitude;
        total += magnitude;
    }
    if (total <= 0.0) {
        return 0.0f;
    }
    return (float)(weighted / total / (sampleRate * 0.5));
}

#if defined(TPLAY_WITH_ONNXRUNTIME)

constexpr int kGenreSampleRate = 16000;
constexpr int kGenreFrameSize = 512;
constexpr int kGenreHopSize = 256;
constexpr int kGenrePatchFrames = 128;
constexpr int kGenreBatchSize = 64;
constexpr int kGenreMelBands = 96;
constexpr int kGenreClassCount = 400;
constexpr int kGenreEmbeddingSize = 1280;

struct DiscogsGenreModel {
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> labels;
    std::vector<float> classifier;
    std::string error;
};


std::optional<fs::path> genreModelDirectory()
{
    const fs::path executable_dir = ProcessRunner::executableDirectory();
    const std::array<fs::path, 3> candidates = {
        executable_dir / "models" / "genre_discogs400",
        executable_dir.parent_path() / "models" / "genre_discogs400",
        fs::current_path() / "models" / "genre_discogs400",
    };
    for (const auto& path : candidates) {
        if (fs::is_regular_file(path / "discogs-effnet-bsdynamic-1.onnx") &&
            fs::is_regular_file(path / "genre_discogs400-discogs-effnet-1.json")) {
            return path;
        }
    }
    return std::nullopt;
}

std::vector<std::string> readDiscogsLabels(const fs::path& path)
{
    std::ifstream file(path);
    std::string json((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    const size_t classes = json.find("\"classes\"");
    const size_t start = json.find('[', classes);
    if (classes == std::string::npos || start == std::string::npos) {
        return {};
    }

    std::vector<std::string> labels;
    size_t cursor = start + 1;
    while (cursor < json.size() && labels.size() < kGenreClassCount) {
        const size_t quote = json.find('"', cursor);
        if (quote == std::string::npos || json.find(']', cursor) < quote) {
            break;
        }
        std::string label;
        bool escaped = false;
        for (size_t index = quote + 1; index < json.size(); ++index) {
            const char character = json[index];
            if (escaped) {
                // Genre names are primarily UTF-8.  Preserve simple escaped
                // characters; unicode escapes remain readable enough as text.
                label.push_back(character == 'n' ? ' ' : character);
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                cursor = index + 1;
                break;
            } else {
                label.push_back(character);
            }
        }
        if (!label.empty()) {
            labels.push_back(std::move(label));
        }
    }
    return labels;
}

std::vector<float> readDiscogsClassifier(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    constexpr size_t count = 2 +
        (size_t)kGenreEmbeddingSize * kGenreClassCount + kGenreClassCount;
    std::vector<float> raw(count - 2);
    std::array<std::int32_t, 2> shape{};
    file.read(reinterpret_cast<char*>(shape.data()), sizeof(shape));
    file.read(reinterpret_cast<char*>(raw.data()),
              (std::streamsize)(raw.size() * sizeof(float)));
    if (!file || shape[0] != -1 || shape[1] != kGenreEmbeddingSize) {
        return {};
    }
    return raw;
}

DiscogsGenreModel& discogsGenreModel()
{
    static DiscogsGenreModel model;
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        const auto directory = genreModelDirectory();
        if (!directory) {
            model.error = "Discogs400 model files are missing";
            return;
        }

        model.labels = readDiscogsLabels(
            *directory / "genre_discogs400-discogs-effnet-1.json");
        if (model.labels.size() != kGenreClassCount) {
            model.error = "Discogs400 class metadata is invalid";
            return;
        }
        model.classifier = readDiscogsClassifier(
            *directory / "genre_discogs400-discogs-effnet-head.bin");

        try {
            static Ort::Env environment(ORT_LOGGING_LEVEL_WARNING,
                                        "tplay-genre");
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(2);
            model.session = std::make_unique<Ort::Session>(
                environment,
                (*directory / "discogs-effnet-bsdynamic-1.onnx").c_str(),
                options);
        } catch (const Ort::Exception& exception) {
            model.error = std::string("Unable to load Discogs400 model: ") +
                          exception.what();
        }
    });
    return model;
}

std::string genreLabel(const std::string& label)
{
    std::string result = label;
    size_t delimiter = 0;
    while ((delimiter = result.find("---", delimiter)) != std::string::npos) {
        result.replace(delimiter, 3, " / ");
        delimiter += 3;
    }
    return result;
}

std::optional<std::string> inferDiscogsGenre(const std::string& path)
{
    DiscogsGenreModel& model = discogsGenreModel();
    if (!model.session || model.labels.size() != kGenreClassCount) {
        return std::nullopt;
    }

    try {
        auto& factory = essentia::standard::AlgorithmFactory::instance();
        std::vector<essentia::Real> audio;
        std::unique_ptr<essentia::standard::Algorithm> loader(
            factory.create("MonoLoader",
                           "filename", path,
                           "sampleRate", (essentia::Real)kGenreSampleRate,
                           "resampleQuality", 4));
        loader->output("audio").set(audio);
        loader->compute();
        if (audio.size() < kGenreFrameSize) {
            return std::nullopt;
        }

        const size_t frame_count =
            1 + (audio.size() - kGenreFrameSize) / kGenreHopSize;
        const size_t max_start = frame_count > kGenrePatchFrames
            ? frame_count - kGenrePatchFrames : 0;
        std::vector<float> mel_batch(
            (size_t)kGenreBatchSize * kGenrePatchFrames * kGenreMelBands);
        std::unique_ptr<essentia::standard::Algorithm> mel(
            factory.create("TensorflowInputMusiCNN"));

        for (int patch = 0; patch < kGenreBatchSize; ++patch) {
            const size_t patch_start = (size_t)std::llround(
                (double)patch * (double)max_start / (kGenreBatchSize - 1));
            for (int frame_index = 0; frame_index < kGenrePatchFrames;
                 ++frame_index) {
                const size_t source_frame = std::min(
                    patch_start + (size_t)frame_index, frame_count - 1);
                const size_t sample = source_frame * kGenreHopSize;
                std::vector<essentia::Real> frame(
                    audio.begin() + (std::ptrdiff_t)sample,
                    audio.begin() + (std::ptrdiff_t)(sample + kGenreFrameSize));
                std::vector<essentia::Real> bands;
                mel->input("frame").set(frame);
                mel->output("bands").set(bands);
                mel->compute();
                if (bands.size() != kGenreMelBands) {
                    return std::nullopt;
                }
                const size_t destination =
                    ((size_t)patch * kGenrePatchFrames + frame_index) *
                    kGenreMelBands;
                std::copy(bands.begin(), bands.end(),
                          mel_batch.begin() + (std::ptrdiff_t)destination);
            }
        }

        Ort::AllocatorWithDefaultOptions allocator;
        auto input_name = model.session->GetInputNameAllocated(0, allocator);
        const char* input_names[] = {input_name.get()};
        const std::array<int64_t, 3> input_shape = {
            kGenreBatchSize, kGenrePatchFrames, kGenreMelBands};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                  OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory, mel_batch.data(), mel_batch.size(), input_shape.data(),
            input_shape.size());

        const size_t output_count = model.session->GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> output_names_storage;
        std::vector<const char*> output_names;
        output_names_storage.reserve(output_count);
        output_names.reserve(output_count);
        for (size_t index = 0; index < output_count; ++index) {
            output_names_storage.push_back(
                model.session->GetOutputNameAllocated(index, allocator));
            output_names.push_back(output_names_storage.back().get());
        }
        auto outputs = model.session->Run(Ort::RunOptions{nullptr}, input_names,
                                          &input, 1, output_names.data(),
                                          output_names.size());

        const float* direct_scores = nullptr;
        size_t direct_count = 0;
        const float* embeddings = nullptr;
        size_t embedding_count = 0;
        for (const auto& output : outputs) {
            if (!output.IsTensor()) {
                continue;
            }
            const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.empty()) {
                continue;
            }
            const size_t width = (size_t)shape.back();
            const float* values = output.GetTensorData<float>();
            const size_t count = output.GetTensorTypeAndShapeInfo().GetElementCount();
            if (width == kGenreClassCount && count >= kGenreClassCount) {
                direct_scores = values;
                direct_count = count;
            } else if (width == kGenreEmbeddingSize &&
                       count >= kGenreEmbeddingSize) {
                embeddings = values;
                embedding_count = count;
            }
        }

        std::vector<double> scores(kGenreClassCount, 0.0);
        if (direct_scores != nullptr) {
            const size_t rows = direct_count / kGenreClassCount;
            for (size_t row = 0; row < rows; ++row) {
                for (int genre = 0; genre < kGenreClassCount; ++genre) {
                    scores[(size_t)genre] +=
                        direct_scores[row * kGenreClassCount + genre];
                }
            }
            for (double& score : scores) {
                score /= (double)rows;
            }
        } else if (embeddings != nullptr &&
                   model.classifier.size() ==
                       (size_t)kGenreEmbeddingSize * kGenreClassCount +
                       kGenreClassCount) {
            const size_t rows = embedding_count / kGenreEmbeddingSize;
            const float* weights = model.classifier.data();
            const float* bias = weights +
                (size_t)kGenreEmbeddingSize * kGenreClassCount;
            for (size_t row = 0; row < rows; ++row) {
                for (int genre = 0; genre < kGenreClassCount; ++genre) {
                    double value = bias[genre];
                    for (int dimension = 0; dimension < kGenreEmbeddingSize;
                         ++dimension) {
                        value += embeddings[row * kGenreEmbeddingSize + dimension] *
                            weights[(size_t)dimension * kGenreClassCount + genre];
                    }
                    scores[(size_t)genre] += 1.0 / (1.0 + std::exp(-value));
                }
            }
            for (double& score : scores) {
                score /= (double)rows;
            }
        } else {
            return std::nullopt;
        }

        const auto best = std::max_element(scores.begin(), scores.end());
        if (best == scores.end()) {
            return std::nullopt;
        }
        return genreLabel(model.labels[(size_t)(best - scores.begin())]);
    } catch (const Ort::Exception&) {
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

#endif

}  // namespace

AudioMetadata AudioAnalyzer::readEmbeddedMetadata(const std::string& path) const
{
    AudioMetadata result;
    auto ffprobe = ProcessRunner::findExecutable("ffprobe");
    if (!ffprobe) {
        result.error = "ffprobe not found";
        return result;
    }

    std::string output;
    int exit_code = ProcessRunner::run({
        *ffprobe,
        "-v", "error",
        "-show_entries", "format=duration,bit_rate,size:stream=sample_rate:format_tags:stream_tags",
        "-of", "default=noprint_wrappers=1",
        path,
    }, &output);
    if (exit_code != 0) {
        result.error = "Unable to read audio metadata";
        return result;
    }

    parseKeyValueOutput(output, result);
    std::string extension = lowercaseExtension(path);
    if (extension == ".m4a" || extension == ".mp4" || extension == ".mov" ||
        extension == ".aac" || extension == ".alac") {
        readMp4TagMetadata(path, result);
    }
    if (result.sizeBytes == 0) {
        std::error_code ec;
        result.sizeBytes = fs::file_size(path, ec);
        if (ec) {
            result.sizeBytes = 0;
        }
    }
    result.succeeded = true;
    return result;
}

AudioMetadata AudioAnalyzer::analyzeWithEssentia(const std::string& path) const
{
    AudioMetadata result;

    try {
        ensureEssentiaInitialized();

        constexpr double sample_rate = 44100.0;
        auto& factory = essentia::standard::AlgorithmFactory::instance();

        std::vector<essentia::Real> audio;
        std::unique_ptr<essentia::standard::Algorithm> loader(
            factory.create("MonoLoader",
                           "filename", path,
                           "sampleRate", sample_rate,
                           "resampleQuality", 1));
        loader->output("audio").set(audio);
        loader->compute();

        if (audio.empty()) {
            result.error = "Essentia loaded no audio";
            return result;
        }

        result.duration = (double)audio.size() / sample_rate;

        essentia::Real bpm = 0.0;
        essentia::Real confidence = 0.0;
        std::vector<essentia::Real> ticks;
        std::vector<essentia::Real> estimates;
        std::vector<essentia::Real> bpm_intervals;
        std::unique_ptr<essentia::standard::Algorithm> rhythm(
            factory.create("RhythmExtractor2013", "method", "multifeature"));
        rhythm->input("signal").set(audio);
        rhythm->output("bpm").set(bpm);
        rhythm->output("ticks").set(ticks);
        rhythm->output("confidence").set(confidence);
        rhythm->output("estimates").set(estimates);
        rhythm->output("bpmIntervals").set(bpm_intervals);
        rhythm->compute();
        result.bpm = bpm;

        std::string key;
        std::string scale;
        essentia::Real strength = 0.0;
        std::unique_ptr<essentia::standard::Algorithm> key_extractor(
            factory.create("KeyExtractor",
                           "profileType", "edma",
                           "sampleRate", sample_rate,
                           // A higher resolution HPCP lets Essentia apply its
                           // detuning correction instead of assuming A=440
                           // exactly.  It noticeably improves key estimates
                           // for mastered and vinyl-derived material.
                           "hpcpSize", 36,
                           "averageDetuningCorrection", true,
                           "frameSize", 4096,
                           "hopSize", 2048));
        key_extractor->input("audio").set(audio);
        key_extractor->output("key").set(key);
        key_extractor->output("scale").set(scale);
        key_extractor->output("strength").set(strength);
        key_extractor->compute();

        if (!key.empty()) {
            result.key = key + (scale == "minor" ? "m" : "");
        }

#if defined(TPLAY_WITH_ONNXRUNTIME)
        // Discogs-EffNet takes 16 kHz MusiCNN mel patches. It is deliberately
        // run here, in the metadata worker, never from the FTXUI event loop.
        if (const auto genre = inferDiscogsGenre(path)) {
            result.genre = *genre;
        }
#endif

        result.succeeded = result.duration > 0.0 ||
                           result.bpm > 0.0 ||
                           !result.key.empty() || !result.genre.empty();
        if (!result.succeeded) {
            result.error = "Essentia returned no metadata";
        }
        return result;
    } catch (const std::exception& e) {
        result.error = std::string("Essentia analysis failed: ") + e.what();
    } catch (...) {
        result.error = "Essentia analysis failed";
    }

    return result;
}

AutoCueFeatures AudioAnalyzer::extractAutoCueFeatures(const fs::path& file) const
{
    ensureEssentiaInitialized();

    constexpr double sample_rate = 44100.0;
    constexpr int frame_size = 2048;
    constexpr int hop_size = 512;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::vector<essentia::Real> audio;
    std::unique_ptr<essentia::standard::Algorithm> loader(
        factory.create("MonoLoader",
                       "filename", file.string(),
                       "sampleRate", sample_rate,
                       "resampleQuality", 1));
    loader->output("audio").set(audio);
    loader->compute();
    if (audio.empty()) {
        throw std::runtime_error("Essentia loaded no audio");
    }

    AutoCueFeatures features;
    features.duration = (double)audio.size() / sample_rate;
    features.hopSeconds = (double)hop_size / sample_rate;

    essentia::Real bpm = 0.0;
    essentia::Real confidence = 0.0;
    std::vector<essentia::Real> ticks;
    std::vector<essentia::Real> estimates;
    std::vector<essentia::Real> bpm_intervals;
    std::unique_ptr<essentia::standard::Algorithm> rhythm(
        factory.create("RhythmExtractor2013", "method", "multifeature"));
    rhythm->input("signal").set(audio);
    rhythm->output("bpm").set(bpm);
    rhythm->output("ticks").set(ticks);
    rhythm->output("confidence").set(confidence);
    rhythm->output("estimates").set(estimates);
    rhythm->output("bpmIntervals").set(bpm_intervals);
    rhythm->compute();
    features.beats.reserve(ticks.size());
    for (essentia::Real tick : ticks) {
        features.beats.push_back((float)tick);
    }

    std::vector<essentia::Real> frame;
    std::vector<essentia::Real> windowed;
    std::vector<essentia::Real> spectrum;
    std::unique_ptr<essentia::standard::Algorithm> windowing(
        factory.create("Windowing", "type", "hann"));
    std::unique_ptr<essentia::standard::Algorithm> spectrum_alg(
        factory.create("Spectrum"));
    windowing->input("frame").set(frame);
    windowing->output("frame").set(windowed);
    spectrum_alg->input("frame").set(windowed);
    spectrum_alg->output("spectrum").set(spectrum);

    std::vector<essentia::Real> previous_spectrum;
    float previous_energy = 0.0f;
    for (size_t start = 0; start < audio.size(); start += hop_size) {
        frame.assign((size_t)frame_size, 0.0f);
        size_t available = std::min((size_t)frame_size, audio.size() - start);
        std::copy(audio.begin() + (long)start,
                  audio.begin() + (long)(start + available),
                  frame.begin());

        double sum_squares = 0.0;
        for (essentia::Real sample : frame) {
            sum_squares += (double)sample * (double)sample;
        }
        float energy = (float)std::sqrt(sum_squares / (double)frame.size());
        features.energyCurve.push_back(energy);
        features.onsetCurve.push_back(std::max(0.0f, energy - previous_energy));
        previous_energy = energy;

        windowing->compute();
        spectrum_alg->compute();
        float flux = 0.0f;
        if (!previous_spectrum.empty() && previous_spectrum.size() == spectrum.size()) {
            for (size_t i = 0; i < spectrum.size(); ++i) {
                flux += std::max<essentia::Real>(0.0f, spectrum[i] - previous_spectrum[i]);
            }
        }
        features.spectralFluxCurve.push_back(flux);
        features.spectralCentroidCurve.push_back(
            spectralCentroidFromSpectrum(spectrum, sample_rate, frame_size));
        features.chromaCurve.push_back(
            chromaFromSpectrum(spectrum, sample_rate, frame_size));
        previous_spectrum = spectrum;

        if (start + available >= audio.size()) {
            break;
        }
    }

    return features;
}

AutoCueFeatures AudioAnalyzer::extractWaveformFeatures(const fs::path& file) const
{
    ensureEssentiaInitialized();

    constexpr double sample_rate = 44100.0;
    constexpr int frame_size = 2048;
    constexpr int hop_size = 512;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::vector<essentia::Real> audio;
    std::unique_ptr<essentia::standard::Algorithm> loader(
        factory.create("MonoLoader",
                       "filename", file.string(),
                       "sampleRate", sample_rate,
                       "resampleQuality", 1));
    loader->output("audio").set(audio);
    loader->compute();
    if (audio.empty()) {
        throw std::runtime_error("Essentia loaded no audio");
    }

    AutoCueFeatures features;
    features.duration = (double)audio.size() / sample_rate;
    features.hopSeconds = (double)hop_size / sample_rate;

    float previous_energy = 0.0f;
    for (size_t start = 0; start < audio.size(); start += hop_size) {
        size_t available = std::min((size_t)frame_size, audio.size() - start);
        double sum_squares = 0.0;
        for (size_t i = 0; i < available; ++i) {
            double sample = audio[start + i];
            sum_squares += sample * sample;
        }
        float energy = available > 0
            ? (float)std::sqrt(sum_squares / (double)available)
            : 0.0f;
        features.energyCurve.push_back(energy);
        features.onsetCurve.push_back(std::max(0.0f, energy - previous_energy));
        previous_energy = energy;

        if (start + available >= audio.size()) {
            break;
        }
    }

    return features;
}
