#include "OnlineStreamDecoder.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

namespace {

constexpr std::size_t kRingSeconds = 12;
constexpr std::size_t kRingFrames =
    OnlineStreamDecoder::kSampleRate * kRingSeconds;

std::string ffmpegError(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(code, message.data(), message.size());
    return message.data();
}

struct FormatCloser {
    void operator()(AVFormatContext* context) const {
        if (context) avformat_close_input(&context);
    }
};
struct CodecCloser {
    void operator()(AVCodecContext* context) const {
        if (context) avcodec_free_context(&context);
    }
};
struct FrameCloser {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);
    }
};
struct PacketCloser {
    void operator()(AVPacket* packet) const {
        if (packet) av_packet_free(&packet);
    }
};
struct SwrCloser {
    void operator()(SwrContext* context) const {
        if (context) swr_free(&context);
    }
};

}  // namespace

OnlineStreamDecoder::OnlineStreamDecoder(std::string url)
    : url_(std::move(url)), ring_(kRingFrames * kChannels)
{
}

OnlineStreamDecoder::~OnlineStreamDecoder()
{
    stop();
}

void OnlineStreamDecoder::start()
{
    if (worker_.joinable()) return;
    worker_ = std::thread(&OnlineStreamDecoder::decodeLoop, this);
}

void OnlineStreamDecoder::stop()
{
    stopRequested_ = true;
    spaceAvailable_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void OnlineStreamDecoder::setPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

bool OnlineStreamDecoder::isPaused() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

void OnlineStreamDecoder::seekToSeconds(double seconds)
{
    const auto milliseconds = (std::int64_t)std::llround(
        std::max(0.0, seconds) * 1000.0);
    seekRequestedMs_.store(milliseconds);
    // Discard old decoded audio immediately.  Without this, playback would
    // continue from the old ring buffer before the HTTP seek takes effect.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        readFrame_ = 0;
        writeFrame_ = 0;
        bufferedFrames_ = 0;
    }
    spaceAvailable_.notify_all();
}

void OnlineStreamDecoder::read(float* output, std::uint32_t frames)
{
    if (!output) return;
    std::fill(output, output + (std::size_t)frames * kChannels, 0.0f);
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) return;

    const std::size_t count = std::min<std::size_t>(frames, bufferedFrames_);
    for (std::size_t frame = 0; frame < count; ++frame) {
        const std::size_t source = readFrame_ * kChannels;
        const std::size_t target = frame * kChannels;
        output[target] = ring_[source];
        output[target + 1] = ring_[source + 1];
        readFrame_ = (readFrame_ + 1) % kRingFrames;
    }
    bufferedFrames_ -= count;
    if (count > 0) spaceAvailable_.notify_one();
}

bool OnlineStreamDecoder::finished() const
{
    if (!finished_.load()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return bufferedFrames_ == 0;
}

bool OnlineStreamDecoder::failed() const
{
    return failed_.load();
}

std::string OnlineStreamDecoder::error() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

int OnlineStreamDecoder::interruptCallback(void* opaque)
{
    auto* decoder = static_cast<OnlineStreamDecoder*>(opaque);
    return decoder && decoder->stopRequested_.load() ? 1 : 0;
}

void OnlineStreamDecoder::setError(std::string error)
{
    if (stopRequested_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::move(error);
    }
    failed_ = true;
}

void OnlineStreamDecoder::push(const float* samples, std::size_t frames)
{
    std::size_t sourceFrame = 0;
    while (sourceFrame < frames && !stopRequested_) {
        std::unique_lock<std::mutex> lock(mutex_);
        spaceAvailable_.wait(lock, [this] {
            return stopRequested_.load() || bufferedFrames_ < kRingFrames;
        });
        if (stopRequested_) return;
        const std::size_t count = std::min(frames - sourceFrame,
                                           kRingFrames - bufferedFrames_);
        for (std::size_t frame = 0; frame < count; ++frame) {
            const std::size_t target = writeFrame_ * kChannels;
            const std::size_t source = (sourceFrame + frame) * kChannels;
            ring_[target] = samples[source];
            ring_[target + 1] = samples[source + 1];
            writeFrame_ = (writeFrame_ + 1) % kRingFrames;
        }
        bufferedFrames_ += count;
        sourceFrame += count;
    }
}

void OnlineStreamDecoder::decodeLoop()
{
    static std::once_flag networkInitialized;
    std::call_once(networkInitialized, [] { avformat_network_init(); });
    AVFormatContext* rawFormat = avformat_alloc_context();
    if (!rawFormat) {
        setError("FFmpeg could not allocate stream context");
        finished_ = true;
        return;
    }
    rawFormat->interrupt_callback.callback = &OnlineStreamDecoder::interruptCallback;
    rawFormat->interrupt_callback.opaque = this;
    if (const int status = avformat_open_input(&rawFormat, url_.c_str(), nullptr, nullptr);
        status < 0) {
        avformat_free_context(rawFormat);
        setError("Online stream open failed: " + ffmpegError(status));
        finished_ = true;
        return;
    }
    std::unique_ptr<AVFormatContext, FormatCloser> format(rawFormat);
    if (const int status = avformat_find_stream_info(format.get(), nullptr); status < 0) {
        setError("Online stream metadata failed: " + ffmpegError(status));
        finished_ = true;
        return;
    }

    const int streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO,
                                                -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        setError("Online stream has no audio track");
        finished_ = true;
        return;
    }
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        setError("FFmpeg decoder is unavailable for this online format");
        finished_ = true;
        return;
    }
    std::unique_ptr<AVCodecContext, CodecCloser> context(avcodec_alloc_context3(codec));
    if (!context || avcodec_parameters_to_context(context.get(), stream->codecpar) < 0 ||
        avcodec_open2(context.get(), codec, nullptr) < 0) {
        setError("FFmpeg could not open online audio decoder");
        finished_ = true;
        return;
    }

    AVChannelLayout inputLayout{};
    if (av_channel_layout_copy(&inputLayout, &context->ch_layout) < 0) {
        setError("FFmpeg could not read online audio channel layout");
        finished_ = true;
        return;
    }
    if (inputLayout.nb_channels == 0) {
        av_channel_layout_default(&inputLayout, context->ch_layout.nb_channels > 0
                                                ? context->ch_layout.nb_channels : 2);
    }
    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* rawResampler = nullptr;
    const int resampleInit = swr_alloc_set_opts2(
        &rawResampler, &outputLayout, AV_SAMPLE_FMT_FLT, kSampleRate,
        &inputLayout, context->sample_fmt, context->sample_rate, 0, nullptr);
    if (resampleInit < 0 || !rawResampler || swr_init(rawResampler) < 0) {
        av_channel_layout_uninit(&inputLayout);
        setError("FFmpeg could not initialize online audio conversion");
        finished_ = true;
        return;
    }
    av_channel_layout_uninit(&inputLayout);
    std::unique_ptr<SwrContext, SwrCloser> resampler(rawResampler);
    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameCloser> frame(av_frame_alloc());
    if (!packet || !frame) {
        setError("FFmpeg could not allocate online audio buffers");
        finished_ = true;
        return;
    }

    auto drain = [&] {
        for (;;) {
            const int receive = avcodec_receive_frame(context.get(), frame.get());
            if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) return true;
            if (receive < 0) {
                setError("Online audio decode failed: " + ffmpegError(receive));
                return false;
            }
            const int capacity = av_rescale_rnd(
                swr_get_delay(resampler.get(), context->sample_rate) + frame->nb_samples,
                kSampleRate, context->sample_rate, AV_ROUND_UP);
            std::vector<float> output((std::size_t)std::max(0, capacity) * kChannels);
            std::uint8_t* planes[] = { reinterpret_cast<std::uint8_t*>(output.data()) };
            const int converted = swr_convert(resampler.get(), planes, capacity,
                                              const_cast<const std::uint8_t**>(frame->extended_data),
                                              frame->nb_samples);
            if (converted < 0) {
                setError("Online audio conversion failed: " + ffmpegError(converted));
                return false;
            }
            if (converted > 0) push(output.data(), (std::size_t)converted);
            av_frame_unref(frame.get());
            if (stopRequested_) return false;
        }
    };

    while (!stopRequested_) {
        const std::int64_t seekMilliseconds = seekRequestedMs_.exchange(-1);
        if (seekMilliseconds >= 0) {
            const std::int64_t timestamp = av_rescale_q(
                seekMilliseconds, AVRational{1, 1000}, stream->time_base);
            const int seekStatus = av_seek_frame(format.get(), streamIndex,
                                                  timestamp, AVSEEK_FLAG_BACKWARD);
            if (seekStatus < 0) {
                // Some HTTP servers do not support ranges.  Keep the current
                // stream alive and report the limitation instead of crashing.
                setError("Online stream cannot seek: " + ffmpegError(seekStatus));
                break;
            }
            avcodec_flush_buffers(context.get());
            swr_close(resampler.get());
            if (swr_init(resampler.get()) < 0) {
                setError("Online audio conversion could not resume after seek");
                break;
            }
        }
        const int read = av_read_frame(format.get(), packet.get());
        if (read == AVERROR_EOF) break;
        if (read < 0) {
            if (!stopRequested_) setError("Online stream read failed: " + ffmpegError(read));
            break;
        }
        if (packet->stream_index == streamIndex) {
            const int sent = avcodec_send_packet(context.get(), packet.get());
            if (sent < 0 || !drain()) {
                if (sent < 0) setError("Online audio packet failed: " + ffmpegError(sent));
                av_packet_unref(packet.get());
                break;
            }
        }
        av_packet_unref(packet.get());
    }
    if (!stopRequested_ && !failed_) {
        avcodec_send_packet(context.get(), nullptr);
        drain();
    }
    finished_ = true;
}
