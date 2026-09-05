#include "AudioEngine.hpp"
#include "OnlineStreamDecoder.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>

#ifdef __APPLE__
#import <AVFoundation/AVFoundation.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

#ifdef __APPLE__
namespace {

std::uint32_t defaultOutputDevice()
{
    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0,
                                   nullptr, &size, &device) != noErr) {
        return 0;
    }
    return device;
}

}  // namespace

@interface TPlayAudioBackend : NSObject {
    std::shared_ptr<OnlineStreamDecoder> _onlineDecoder;
    std::atomic_uint64_t _onlineFramesPlayed;
    double _onlineDurationSeconds;
}
@property(nonatomic, strong) AVAudioEngine* engine;
@property(nonatomic, strong) AVAudioPlayerNode* playerNode;
@property(nonatomic, strong) AVAudioSourceNode* sourceNode;
@property(nonatomic, strong) AVAudioUnitVarispeed* varispeedNode;
@property(nonatomic, strong) AVAudioUnitTimePitch* timePitchNode;
@property(nonatomic, strong) AVAudioUnitEQ* eqNode;
@property(nonatomic, strong) AVAudioMixerNode* mixerNode;
@property(nonatomic, strong) AVAudioFile* audioFile;
@property(nonatomic, strong) AVAudioFormat* audioFormat;
@property(nonatomic, strong) AVPlayer* streamPlayer;
@property(nonatomic, strong) id streamEndObserver;
@property(nonatomic, assign) double sampleRate;
@property(nonatomic, assign) AVAudioFramePosition currentFrame;
@property(nonatomic, assign) AVAudioFramePosition startFrame;
@property(nonatomic, assign) NSInteger generation;
@property(nonatomic, assign) BOOL playing;
@property(nonatomic, assign) BOOL paused;
@property(nonatomic, assign) BOOL finishedNaturally;
@property(nonatomic, assign) float volume;
@property(nonatomic, assign) float rate;
@property(nonatomic, assign) BOOL preservePitch;
@property(nonatomic, assign) BOOL loopEnabled;
@property(nonatomic, assign) AVAudioFramePosition loopStartFrame;
@property(nonatomic, assign) AVAudioFramePosition loopEndFrame;
- (void)configureEqualizer;
- (void)applyRate;
- (AVAudioFramePosition)totalFrames;
- (AVAudioFramePosition)estimatedCurrentFrame;
- (void)scheduleFromFrame:(AVAudioFramePosition)frame shouldPlay:(BOOL)shouldPlay;
- (void)scheduleLoopSegmentFromFrame:(AVAudioFramePosition)frame generation:(NSInteger)generation;
- (BOOL)playFile:(NSString*)filePath
expectedDurationSeconds:(double)expectedDurationSeconds
     startSeconds:(double)startSeconds
             error:(NSString**)errorMessage;
- (void)pause;
- (void)resume;
- (void)stop;
- (void)setVolumePercent:(int)volume;
- (void)setPlaybackRate:(double)rate;
- (void)setPreservePitch:(BOOL)preserve;
- (void)setLowGain:(double)lowDb midGain:(double)midDb highGain:(double)highDb;
- (void)setLoopStartSeconds:(double)startSeconds endSeconds:(double)endSeconds;
- (void)clearLoopRange;
- (void)seekToRatio:(double)ratio;
- (double)positionSeconds;
- (double)durationSeconds;
- (BOOL)consumeFinishedNaturally;
- (NSString*)onlineError;
- (PlaybackState)playbackState;
@end

@implementation TPlayAudioBackend

- (instancetype)init
{
    self = [super init];
    if (self) {
        _sampleRate = 0.0;
        _currentFrame = 0;
        _startFrame = 0;
        _generation = 0;
        _volume = 1.0f;
        _rate = 1.0f;
        _preservePitch = YES;
        _loopEnabled = NO;
        _loopStartFrame = 0;
        _loopEndFrame = 0;
        _onlineFramesPlayed = 0;
        _onlineDurationSeconds = 0.0;
    }
    return self;
}

- (void)dealloc
{
    [self stop];
}

- (void)configureEqualizer
{
    if (!_eqNode || _eqNode.bands.count < 3) {
        return;
    }
    AVAudioUnitEQFilterParameters* low = _eqNode.bands[0];
    low.filterType = AVAudioUnitEQFilterTypeLowShelf;
    low.frequency = 120.0f;
    low.bandwidth = 1.0f;
    low.gain = 0.0f;
    low.bypass = NO;

    AVAudioUnitEQFilterParameters* mid = _eqNode.bands[1];
    mid.filterType = AVAudioUnitEQFilterTypeParametric;
    mid.frequency = 1000.0f;
    mid.bandwidth = 1.0f;
    mid.gain = 0.0f;
    mid.bypass = NO;

    AVAudioUnitEQFilterParameters* high = _eqNode.bands[2];
    high.filterType = AVAudioUnitEQFilterTypeHighShelf;
    high.frequency = 8000.0f;
    high.bandwidth = 1.0f;
    high.gain = 0.0f;
    high.bypass = NO;
}

- (void)applyRate
{
    float clampedRate = std::clamp(_rate, 0.5f, 2.0f);
    _rate = clampedRate;
    if (!_varispeedNode || !_timePitchNode) {
        return;
    }

    if (_preservePitch) {
        _varispeedNode.bypass = YES;
        _timePitchNode.bypass = clampedRate == 1.0f;
        _varispeedNode.rate = 1.0f;
        _timePitchNode.rate = clampedRate;
    } else {
        _varispeedNode.bypass = clampedRate == 1.0f;
        _timePitchNode.bypass = YES;
        _varispeedNode.rate = clampedRate;
        _timePitchNode.rate = 1.0f;
    }
    _timePitchNode.pitch = 0.0f;
}

- (AVAudioFramePosition)totalFrames
{
    return _audioFile ? (AVAudioFramePosition)_audioFile.length : 0;
}

- (AVAudioFramePosition)estimatedCurrentFrame
{
    if (!_audioFile) {
        return 0;
    }
    if (!_playing || _paused || !_playerNode) {
        return std::clamp(_currentFrame,
                          (AVAudioFramePosition)0,
                          [self totalFrames]);
    }

    AVAudioTime* nodeTime = [_playerNode lastRenderTime];
    AVAudioTime* playerTime = nodeTime ? [_playerNode playerTimeForNodeTime:nodeTime] : nil;
    if (!playerTime) {
        return std::clamp(_currentFrame,
                          (AVAudioFramePosition)0,
                          [self totalFrames]);
    }

    AVAudioFramePosition frame = _startFrame + playerTime.sampleTime;
    if (_loopEnabled && _loopEndFrame > _loopStartFrame) {
        AVAudioFramePosition length = _loopEndFrame - _loopStartFrame;
        AVAudioFramePosition offset =
            std::max((AVAudioFramePosition)0, _startFrame - _loopStartFrame) +
            playerTime.sampleTime;
        frame = _loopStartFrame + (offset % length);
    }
    return std::clamp(frame, (AVAudioFramePosition)0, [self totalFrames]);
}

- (void)scheduleFromFrame:(AVAudioFramePosition)frame shouldPlay:(BOOL)shouldPlay
{
    if (!_playerNode || !_audioFile) {
        return;
    }

    AVAudioFramePosition total = [self totalFrames];
    frame = std::clamp(frame, (AVAudioFramePosition)0, total);
    _generation++;
    NSInteger generation = _generation;
    _currentFrame = frame;
    _startFrame = frame;
    _finishedNaturally = NO;
    _paused = !shouldPlay;
    _playing = shouldPlay;

    [_playerNode stop];
    [_playerNode reset];

    if (_loopEnabled && _loopEndFrame > _loopStartFrame) {
        frame = std::clamp(frame, _loopStartFrame, _loopEndFrame - 1);
    }

    if (frame >= total) {
        _playing = NO;
        _paused = NO;
        _finishedNaturally = YES;
        return;
    }

    if (_loopEnabled && _loopEndFrame > _loopStartFrame) {
        [self scheduleLoopSegmentFromFrame:frame generation:generation];
        if (shouldPlay) {
            [_playerNode play];
        }
        return;
    }

    AVAudioFrameCount framesToPlay = (AVAudioFrameCount)(total - frame);
    [_playerNode scheduleSegment:_audioFile
                    startingFrame:frame
                       frameCount:framesToPlay
                           atTime:nil
           completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
                completionHandler:^(AVAudioPlayerNodeCompletionCallbackType callbackType) {
                    if (callbackType != AVAudioPlayerNodeCompletionDataPlayedBack) {
                        return;
                    }
                    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                        @synchronized (self) {
                            if (generation != self->_generation) {
                                return;
                            }
                            self->_currentFrame = [self totalFrames];
                            self->_playing = NO;
                            self->_paused = NO;
                            self->_finishedNaturally = YES;
                        }
                    });
                }];

    if (shouldPlay) {
        [_playerNode play];
    }
}

- (void)scheduleLoopSegmentFromFrame:(AVAudioFramePosition)frame generation:(NSInteger)generation
{
    if (!_playerNode || !_audioFile || !_loopEnabled ||
        _loopEndFrame <= _loopStartFrame) {
        return;
    }

    frame = std::clamp(frame, _loopStartFrame, _loopEndFrame - 1);
    AVAudioFrameCount framesToPlay = (AVAudioFrameCount)(_loopEndFrame - frame);
    [_playerNode scheduleSegment:_audioFile
                    startingFrame:frame
                       frameCount:framesToPlay
                           atTime:nil
           completionCallbackType:AVAudioPlayerNodeCompletionDataConsumed
                completionHandler:^(AVAudioPlayerNodeCompletionCallbackType callbackType) {
                    if (callbackType != AVAudioPlayerNodeCompletionDataConsumed) {
                        return;
                    }
                    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                        @synchronized (self) {
                            if (generation != self->_generation ||
                                !self->_loopEnabled ||
                                self->_loopEndFrame <= self->_loopStartFrame ||
                                !self->_playerNode ||
                                !self->_audioFile) {
                                return;
                            }
                            [self scheduleLoopSegmentFromFrame:self->_loopStartFrame
                                                    generation:generation];
                        }
                    });
                }];
}

- (BOOL)playFile:(NSString*)filePath
expectedDurationSeconds:(double)expectedDurationSeconds
     startSeconds:(double)startSeconds
             error:(NSString**)errorMessage
{
    @synchronized (self) {
        [self stop];

        NSError* error = nil;
        if ([filePath hasPrefix:@"https://"] || [filePath hasPrefix:@"http://"]) {
            std::shared_ptr<OnlineStreamDecoder> decoder =
                std::make_shared<OnlineStreamDecoder>([filePath UTF8String]);
            // Use the native, non-interleaved AVAudioEngine PCM layout.  The
            // prior interleaved layout caused AVAudioEngine to throw an
            // Objective-C exception while connecting its audio units on some
            // macOS output devices.  The render block below supports either
            // layout, so audio data remains stereo PCM from the decoder.
            AVAudioFormat* streamFormat = [[AVAudioFormat alloc]
                initStandardFormatWithSampleRate:OnlineStreamDecoder::kSampleRate
                                         channels:OnlineStreamDecoder::kChannels];
            if (!streamFormat) {
                if (errorMessage) *errorMessage = @"Could not prepare online PCM format";
                return NO;
            }
            _engine = [[AVAudioEngine alloc] init];
            _sourceNode = [[AVAudioSourceNode alloc]
                initWithFormat:streamFormat
                    renderBlock:^OSStatus(BOOL* isSilence,
                                          const AudioTimeStamp*,
                                          AVAudioFrameCount frameCount,
                                          AudioBufferList* outputData) {
                        if (!outputData || outputData->mNumberBuffers == 0) {
                            return noErr;
                        }
                        // AVAudioEngine can request either one interleaved
                        // stereo buffer or two non-interleaved buffers even
                        // when the source format is declared interleaved.
                        // Decode into a temporary stereo frame array first;
                        // writing stereo samples directly to buffer[0] caused
                        // an overflow on devices using planar output.
                        thread_local std::vector<float> decoded;
                        decoded.resize((std::size_t)frameCount *
                                       OnlineStreamDecoder::kChannels);
                        decoder->read(decoded.data(), frameCount);
                        // AVAudioEngine keeps invoking the source callback
                        // while paused. The decoder returns silence then, so
                        // do not advance the visible transport position.
                        if (!decoder->isPaused()) {
                            self->_onlineFramesPlayed.fetch_add(
                                frameCount, std::memory_order_relaxed);
                        }
                        if (outputData->mNumberBuffers == 1) {
                            AudioBuffer& buffer = outputData->mBuffers[0];
                            if (buffer.mData) {
                                const std::size_t bytes = std::min<std::size_t>(
                                    buffer.mDataByteSize,
                                    decoded.size() * sizeof(float));
                                std::memcpy(buffer.mData, decoded.data(), bytes);
                                if (bytes < buffer.mDataByteSize) {
                                    std::memset(static_cast<char*>(buffer.mData) + bytes,
                                                0, buffer.mDataByteSize - bytes);
                                }
                            }
                        } else {
                            for (UInt32 channel = 0;
                                 channel < outputData->mNumberBuffers;
                                 ++channel) {
                                AudioBuffer& buffer = outputData->mBuffers[channel];
                                if (!buffer.mData) continue;
                                float* target = static_cast<float*>(buffer.mData);
                                const std::size_t available =
                                    buffer.mDataByteSize / sizeof(float);
                                const std::size_t count = std::min<std::size_t>(
                                    frameCount, available);
                                const int sourceChannel = std::min<int>(
                                    channel, OnlineStreamDecoder::kChannels - 1);
                                for (std::size_t frame = 0; frame < count; ++frame) {
                                    target[frame] = decoded[frame *
                                        OnlineStreamDecoder::kChannels + sourceChannel];
                                }
                                if (count < available) {
                                    std::fill(target + count, target + available, 0.0f);
                                }
                            }
                        }
                        if (isSilence) *isSilence = NO;
                        return noErr;
                    }];
            _eqNode = [[AVAudioUnitEQ alloc] initWithNumberOfBands:3];
            _mixerNode = [[AVAudioMixerNode alloc] init];
            @try {
                [_engine attachNode:_sourceNode];
                [_engine attachNode:_eqNode];
                [_engine attachNode:_mixerNode];
                [self configureEqualizer];
                // Let AVAudioEngine negotiate conversions for the actual
                // output device instead of forcing the decoder's 44.1 kHz
                // format through every bus.
                [_engine connect:_sourceNode to:_eqNode format:nil];
                [_engine connect:_eqNode to:_mixerNode format:nil];
                [_engine connect:_mixerNode to:[_engine mainMixerNode] format:nil];
            } @catch (NSException* exception) {
                if (errorMessage) {
                    *errorMessage = [NSString stringWithFormat:@"Could not configure online audio output: %@", exception.reason];
                }
                [self stop];
                return NO;
            }
            _mixerNode.volume = _volume;
            NSError* streamError = nil;
            if (![_engine startAndReturnError:&streamError]) {
                if (errorMessage) {
                    *errorMessage = streamError ? streamError.localizedDescription
                                                : @"Could not start online audio output";
                }
                [self stop];
                return NO;
            }
            _onlineDecoder = std::move(decoder);
            _onlineFramesPlayed = 0;
            _onlineDurationSeconds = std::max(0.0, expectedDurationSeconds);
            _onlineDecoder->start();
            _loopEnabled = NO;
            _finishedNaturally = NO;
            _paused = NO;
            _playing = YES;
            [_streamPlayer playImmediatelyAtRate:_rate];
            return YES;
        }
        NSURL* url = [NSURL fileURLWithPath:filePath];
        AVAudioFile* file = [[AVAudioFile alloc] initForReading:url error:&error];
        if (!file || error) {
            if (errorMessage) {
                *errorMessage = error ? error.localizedDescription : @"Failed to open audio file";
            }
            return NO;
        }

        AVAudioFormat* format = file.processingFormat;
        if (format.channelCount == 0 || format.sampleRate <= 0.0 || file.length <= 0) {
            if (errorMessage) {
                *errorMessage = @"Unsupported or empty audio file";
            }
            return NO;
        }

        _engine = [[AVAudioEngine alloc] init];
        _playerNode = [[AVAudioPlayerNode alloc] init];
        _varispeedNode = [[AVAudioUnitVarispeed alloc] init];
        _timePitchNode = [[AVAudioUnitTimePitch alloc] init];
        _eqNode = [[AVAudioUnitEQ alloc] initWithNumberOfBands:3];
        _mixerNode = [[AVAudioMixerNode alloc] init];
        _audioFile = file;
        _audioFormat = format;
        _sampleRate = format.sampleRate;
        _currentFrame = 0;
        _startFrame = 0;
        _finishedNaturally = NO;
        _loopEnabled = NO;
        _loopStartFrame = 0;
        _loopEndFrame = 0;
        _generation++;

        [_engine attachNode:_playerNode];
        [_engine attachNode:_varispeedNode];
        [_engine attachNode:_timePitchNode];
        [_engine attachNode:_eqNode];
        [_engine attachNode:_mixerNode];
        [self configureEqualizer];

        AVAudioMixerNode* mainMixer = [_engine mainMixerNode];
        [_engine connect:_playerNode to:_varispeedNode format:format];
        [_engine connect:_varispeedNode to:_timePitchNode format:format];
        [_engine connect:_timePitchNode to:_eqNode format:format];
        [_engine connect:_eqNode to:_mixerNode format:format];
        [_engine connect:_mixerNode to:mainMixer format:format];

        _mixerNode.volume = _volume;
        [self applyRate];

        if (![_engine startAndReturnError:&error]) {
            if (errorMessage) {
                *errorMessage = error ? error.localizedDescription : @"Failed to start audio engine";
            }
            [self stop];
            return NO;
        }

        AVAudioFramePosition startFrame = (AVAudioFramePosition)std::llround(
            std::clamp(startSeconds, 0.0,
                       (double)file.length / format.sampleRate) * format.sampleRate);
        [self scheduleFromFrame:startFrame shouldPlay:YES];
        return YES;
    }
}

- (void)pause
{
    @synchronized (self) {
        if (_onlineDecoder) {
            if (_playing) {
                _onlineDecoder->setPaused(true);
                _paused = YES;
                _playing = NO;
            }
            return;
        }
        if (_streamPlayer) {
            if (_playing) {
                [_streamPlayer pause];
                _paused = YES;
                _playing = NO;
            }
            return;
        }
        if (!_playerNode || !_playing) {
            return;
        }
        _currentFrame = [self estimatedCurrentFrame];
        [_playerNode pause];
        _paused = YES;
        _playing = NO;
    }
}

- (void)resume
{
    @synchronized (self) {
        if (_onlineDecoder) {
            if (_paused) {
                _onlineDecoder->setPaused(false);
                _paused = NO;
                _playing = YES;
            }
            return;
        }
        if (_streamPlayer) {
            if (_paused) {
                [_streamPlayer playImmediatelyAtRate:_rate];
                _paused = NO;
                _playing = YES;
            }
            return;
        }
        if (!_playerNode || !_audioFile || !_paused) {
            return;
        }
        [_playerNode play];
        _paused = NO;
        _playing = YES;
    }
}

- (void)stop
{
    @synchronized (self) {
        _generation++;
        if (_streamEndObserver) {
            [[NSNotificationCenter defaultCenter] removeObserver:_streamEndObserver];
            _streamEndObserver = nil;
        }
        [_streamPlayer pause];
        _streamPlayer = nil;
        if (_onlineDecoder) {
            _onlineDecoder->stop();
            _onlineDecoder.reset();
        }
        if (_playerNode) {
            [_playerNode stop];
            [_playerNode reset];
        }
        if (_engine) {
            [_engine stop];
            [_engine reset];
        }
        _playing = NO;
        _paused = NO;
        _finishedNaturally = NO;
        _loopEnabled = NO;
        _loopStartFrame = 0;
        _loopEndFrame = 0;
        _currentFrame = 0;
        _startFrame = 0;
        _audioFile = nil;
        _onlineFramesPlayed = 0;
        _onlineDurationSeconds = 0.0;
        _audioFormat = nil;
        _playerNode = nil;
        _sourceNode = nil;
        _varispeedNode = nil;
        _timePitchNode = nil;
        _eqNode = nil;
        _mixerNode = nil;
        _engine = nil;
        _sampleRate = 0.0;
    }
}

- (void)setVolumePercent:(int)volume
{
    @synchronized (self) {
        _volume = std::clamp(volume, 0, 100) / 100.0f;
        if (_mixerNode) {
            _mixerNode.volume = _volume;
        }
        if (_streamPlayer) {
            _streamPlayer.volume = _volume;
        }
    }
}

- (void)setPlaybackRate:(double)rate
{
    @synchronized (self) {
        _rate = (float)std::clamp(rate, 0.5, 2.0);
        [self applyRate];
        if (_streamPlayer && _playing) {
            [_streamPlayer playImmediatelyAtRate:_rate];
        }
    }
}

- (void)setPreservePitch:(BOOL)preserve
{
    @synchronized (self) {
        if (_preservePitch == preserve) {
            return;
        }
        BOOL wasPlaying = _playing && !_paused;
        BOOL wasPaused = _paused;
        AVAudioFramePosition frame = [self estimatedCurrentFrame];
        _preservePitch = preserve;
        [self applyRate];
        if (_audioFile && (wasPlaying || wasPaused)) {
            [self scheduleFromFrame:frame shouldPlay:wasPlaying];
            if (wasPaused) {
                _paused = YES;
                _playing = NO;
            }
        }
    }
}

- (void)setLowGain:(double)lowDb midGain:(double)midDb highGain:(double)highDb
{
    @synchronized (self) {
        if (!_eqNode || _eqNode.bands.count < 3) {
            return;
        }
        auto clampGain = [](double value) {
            return (float)std::clamp(value, -60.0, 6.0);
        };
        _eqNode.bands[0].gain = clampGain(lowDb);
        _eqNode.bands[1].gain = clampGain(midDb);
        _eqNode.bands[2].gain = clampGain(highDb);
    }
}

- (void)setLoopStartSeconds:(double)startSeconds endSeconds:(double)endSeconds
{
    @synchronized (self) {
        if (_streamPlayer || _onlineDecoder) return;
        if (!_audioFile || _sampleRate <= 0.0) {
            return;
        }

        AVAudioFramePosition total = [self totalFrames];
        AVAudioFramePosition start = (AVAudioFramePosition)std::llround(
            std::clamp(startSeconds, 0.0, (double)total / _sampleRate) *
            _sampleRate);
        AVAudioFramePosition end = (AVAudioFramePosition)std::llround(
            std::clamp(endSeconds, 0.0, (double)total / _sampleRate) *
            _sampleRate);
        start = std::clamp(start, (AVAudioFramePosition)0, total);
        end = std::clamp(end, (AVAudioFramePosition)0, total);
        if (end <= start + (AVAudioFramePosition)std::llround(_sampleRate * 0.02)) {
            _loopEnabled = NO;
            return;
        }

        BOOL wasPlaying = _playing && !_paused;
        BOOL wasPaused = _paused;
        AVAudioFramePosition frame = [self estimatedCurrentFrame];
        _loopStartFrame = start;
        _loopEndFrame = end;
        _loopEnabled = YES;
        if (frame < start || frame >= end) {
            frame = start;
        }
        if (wasPlaying || wasPaused) {
            [self scheduleFromFrame:frame shouldPlay:wasPlaying];
            if (wasPaused) {
                _paused = YES;
                _playing = NO;
            }
        }
    }
}

- (void)clearLoopRange
{
    @synchronized (self) {
        if (_streamPlayer || _onlineDecoder) return;
        if (!_loopEnabled) {
            return;
        }
        BOOL wasPlaying = _playing && !_paused;
        BOOL wasPaused = _paused;
        AVAudioFramePosition frame = [self estimatedCurrentFrame];
        _loopEnabled = NO;
        _loopStartFrame = 0;
        _loopEndFrame = 0;
        if (_audioFile && (wasPlaying || wasPaused)) {
            [self scheduleFromFrame:frame shouldPlay:wasPlaying];
            if (wasPaused) {
                _paused = YES;
                _playing = NO;
            }
        }
    }
}

- (void)seekToRatio:(double)ratio
{
    @synchronized (self) {
        if (_streamPlayer) {
            const double duration = [self durationSeconds];
            if (duration > 0.0) {
                const CMTime target = CMTimeMakeWithSeconds(
                    std::clamp(ratio, 0.0, 1.0) * duration, NSEC_PER_SEC);
                [_streamPlayer seekToTime:target];
            }
            return;
        }
        if (_onlineDecoder) {
            const double duration = _onlineDurationSeconds;
            if (duration > 0.0) {
                const double seconds = std::clamp(ratio, 0.0, 1.0) * duration;
                _onlineFramesPlayed.store((std::uint64_t)std::llround(
                    seconds * OnlineStreamDecoder::kSampleRate),
                    std::memory_order_relaxed);
                _onlineDecoder->seekToSeconds(seconds);
            }
            return;
        }
        if (!_audioFile) {
            return;
        }
        BOOL shouldPlay = _playing && !_paused;
        AVAudioFramePosition target = (AVAudioFramePosition)std::llround(
            std::clamp(ratio, 0.0, 1.0) * (double)[self totalFrames]);
        [self scheduleFromFrame:target shouldPlay:shouldPlay];
        if (!shouldPlay) {
            _paused = YES;
            _playing = NO;
        }
    }
}

- (double)positionSeconds
{
    @synchronized (self) {
        if (_onlineDecoder) {
            const double seconds = (double)_onlineFramesPlayed.load(std::memory_order_relaxed) /
                OnlineStreamDecoder::kSampleRate;
            return _onlineDurationSeconds > 0.0
                ? std::min(seconds, _onlineDurationSeconds)
                : seconds;
        }
        if (_streamPlayer) {
            const double seconds = CMTimeGetSeconds(_streamPlayer.currentTime);
            return std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
        }
        if (_sampleRate <= 0.0) {
            return 0.0;
        }
        _currentFrame = [self estimatedCurrentFrame];
        return (double)_currentFrame / _sampleRate;
    }
}

- (double)durationSeconds
{
    @synchronized (self) {
        if (_onlineDecoder) {
            return _onlineDurationSeconds;
        }
        if (_streamPlayer) {
            const double seconds = CMTimeGetSeconds(_streamPlayer.currentItem.duration);
            return std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
        }
        if (!_audioFile || _sampleRate <= 0.0) {
            return 0.0;
        }
        return (double)[self totalFrames] / _sampleRate;
    }
}

- (BOOL)consumeFinishedNaturally
{
    @synchronized (self) {
        if (_onlineDecoder && _onlineDecoder->finished() && !_onlineDecoder->failed()) {
            _playing = NO;
            _paused = NO;
            _finishedNaturally = YES;
        }
        BOOL finished = _finishedNaturally;
        _finishedNaturally = NO;
        return finished;
    }
}

- (NSString*)onlineError
{
    @synchronized (self) {
        if (!_onlineDecoder || !_onlineDecoder->failed()) return nil;
        const std::string message = _onlineDecoder->error();
        return message.empty() ? @"Online audio decoding failed"
                               : [NSString stringWithUTF8String:message.c_str()];
    }
}

- (PlaybackState)playbackState
{
    @synchronized (self) {
        if (_onlineDecoder && _onlineDecoder->finished() && !_onlineDecoder->failed()) {
            _playing = NO;
            _paused = NO;
        }
        if (_paused) {
            return PlaybackState::Paused;
        }
        if (_playing) {
            return PlaybackState::Playing;
        }
        return PlaybackState::Stopped;
    }
}

@end

namespace {
TPlayAudioBackend* backendFrom(void* backend)
{
    return (__bridge TPlayAudioBackend*)backend;
}
}  // namespace
#endif

AudioEngine::AudioEngine()
{
#ifdef __APPLE__
    backend_ = (__bridge_retained void*)[[TPlayAudioBackend alloc] init];
    systemOutputDevice_ = defaultOutputDevice();
#else
    ready_ = false;
    state_ = PlaybackState::Error;
    errorMessage_ = "AudioEngine is only implemented for macOS";
#endif
}

AudioEngine::~AudioEngine()
{
    std::lock_guard<std::mutex> lock(mutex_);
    allowIdleSleepLocked();
#ifdef __APPLE__
    if (backend_ != nullptr) {
        TPlayAudioBackend* backend = backendFrom(backend_);
        [backend stop];
        CFRelease(backend_);
        backend_ = nullptr;
    }
#endif
}

bool AudioEngine::play(const std::string& path, const std::string& title, int volume,
                       double expectedDurationSeconds, double startSeconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        setErrorLocked("Audio engine is not initialized");
        return false;
    }

    currentTitle_ = title;
    errorMessage_.clear();
    finishedNaturally_ = false;
    currentDurationSeconds_ = 0.0;

#ifdef __APPLE__
    systemOutputDevice_ = defaultOutputDevice();
    TPlayAudioBackend* backend = backendFrom(backend_);
    [backend setVolumePercent:volume];
    [backend setPlaybackRate:playbackRate_];
    [backend setPreservePitch:preservePitch_ ? YES : NO];

    NSString* error = nil;
    NSString* filePath = [NSString stringWithUTF8String:path.c_str()];
    if (![backend playFile:filePath
     expectedDurationSeconds:expectedDurationSeconds
               startSeconds:startSeconds
                      error:&error]) {
        std::string message = error ? [error UTF8String] : "Failed to play audio file";
        setErrorLocked(message);
        return false;
    }

    currentDurationSeconds_ = [backend durationSeconds];
    preventIdleSleepLocked();
    state_ = PlaybackState::Playing;
    return true;
#else
    (void)path;
    (void)volume;
    setErrorLocked("Audio playback is unavailable on this platform");
    return false;
#endif
}


void AudioEngine::pause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || state_ == PlaybackState::Stopped) {
        return;
    }
#ifdef __APPLE__
    [backendFrom(backend_) pause];
#endif
    allowIdleSleepLocked();
    state_ = PlaybackState::Paused;
}

void AudioEngine::resume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || state_ == PlaybackState::Stopped) {
        return;
    }
#ifdef __APPLE__
    [backendFrom(backend_) resume];
#endif
    preventIdleSleepLocked();
    state_ = PlaybackState::Playing;
}

void AudioEngine::togglePause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) {
        return;
    }
    refreshStateLocked();
    if (state_ == PlaybackState::Paused) {
#ifdef __APPLE__
        [backendFrom(backend_) resume];
#endif
        preventIdleSleepLocked();
        state_ = PlaybackState::Playing;
    } else if (state_ == PlaybackState::Playing) {
#ifdef __APPLE__
        [backendFrom(backend_) pause];
#endif
        allowIdleSleepLocked();
        state_ = PlaybackState::Paused;
    }
}

void AudioEngine::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    allowIdleSleepLocked();
#ifdef __APPLE__
    if (backend_ != nullptr) {
        [backendFrom(backend_) stop];
    }
#endif
    currentDurationSeconds_ = 0.0;
    finishedNaturally_ = false;
    state_ = PlaybackState::Stopped;
}

void AudioEngine::setVolume(int volume)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        return;
    }
#ifdef __APPLE__
    [backendFrom(backend_) setVolumePercent:volume];
#else
    (void)volume;
#endif
}

void AudioEngine::setPlaybackRate(double rate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    playbackRate_ = std::clamp(rate, 0.5, 2.0);
#ifdef __APPLE__
    if (ready_ && backend_ != nullptr) {
        [backendFrom(backend_) setPlaybackRate:playbackRate_];
    }
#endif
}

void AudioEngine::setPreservePitch(bool preserve)
{
    std::lock_guard<std::mutex> lock(mutex_);
    preservePitch_ = preserve;
#ifdef __APPLE__
    if (ready_ && backend_ != nullptr) {
        [backendFrom(backend_) setPreservePitch:preservePitch_ ? YES : NO];
    }
#endif
}

void AudioEngine::setEqualizerGains(double lowDb, double midDb, double highDb)
{
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef __APPLE__
    if (ready_ && backend_ != nullptr) {
        [backendFrom(backend_) setLowGain:lowDb midGain:midDb highGain:highDb];
    }
#else
    (void)lowDb;
    (void)midDb;
    (void)highDb;
#endif
}

void AudioEngine::setLoopRange(double startSeconds, double endSeconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef __APPLE__
    if (ready_ && backend_ != nullptr) {
        [backendFrom(backend_) setLoopStartSeconds:startSeconds
                                        endSeconds:endSeconds];
    }
#else
    (void)startSeconds;
    (void)endSeconds;
#endif
}

void AudioEngine::clearLoopRange()
{
    std::lock_guard<std::mutex> lock(mutex_);
#ifdef __APPLE__
    if (ready_ && backend_ != nullptr) {
        [backendFrom(backend_) clearLoopRange];
    }
#endif
}

void AudioEngine::seekToRatio(double ratio)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || currentDurationSeconds_ <= 0.0) {
        return;
    }
#ifdef __APPLE__
    [backendFrom(backend_) seekToRatio:ratio];
#else
    (void)ratio;
#endif
}

void AudioEngine::followSystemAudioOutput()
{
#ifdef __APPLE__
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        return;
    }
    std::uint32_t device = defaultOutputDevice();
    if (device == 0 || device == systemOutputDevice_) {
        return;
    }
    systemOutputDevice_ = device;
    PlaybackState previous = state_;
    double ratio = 0.0;
    if (currentDurationSeconds_ > 0.0) {
        ratio = std::clamp([backendFrom(backend_) positionSeconds] / currentDurationSeconds_, 0.0, 1.0);
    }
    [backendFrom(backend_) seekToRatio:ratio];
    if (previous == PlaybackState::Paused) {
        [backendFrom(backend_) pause];
    }
#endif
}

double AudioEngine::readDuration(const std::string& path)
{
#ifdef __APPLE__
    NSError* error = nil;
    NSString* filePath = [NSString stringWithUTF8String:path.c_str()];
    AVAudioFile* file = [[AVAudioFile alloc] initForReading:[NSURL fileURLWithPath:filePath]
                                                      error:&error];
    if (!file || error || file.processingFormat.sampleRate <= 0.0) {
        return 0.0;
    }
    return (double)file.length / file.processingFormat.sampleRate;
#else
    (void)path;
    return 0.0;
#endif
}

double AudioEngine::playbackRate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return playbackRate_;
}

bool AudioEngine::preservePitch() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return preservePitch_;
}

PlaybackSnapshot AudioEngine::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    refreshStateLocked();

    PlaybackSnapshot snapshot;
    snapshot.state = state_;
    snapshot.title = currentTitle_;
    snapshot.errorMessage = errorMessage_;
#ifdef __APPLE__
    if (ready_ && backend_ != nullptr) {
        snapshot.positionSeconds = [backendFrom(backend_) positionSeconds];
        snapshot.durationSeconds = [backendFrom(backend_) durationSeconds];
    }
#endif
    return snapshot;
}

bool AudioEngine::consumeFinishedNaturally()
{
    std::lock_guard<std::mutex> lock(mutex_);
    refreshStateLocked();
    bool finished = finishedNaturally_;
    finishedNaturally_ = false;
    return finished;
}

void AudioEngine::setErrorLocked(const std::string& message)
{
    allowIdleSleepLocked();
    state_ = PlaybackState::Error;
    errorMessage_ = message;
}

void AudioEngine::refreshStateLocked() const
{
    if (!ready_ || state_ == PlaybackState::Error) {
        return;
    }
#ifdef __APPLE__
    if (backend_ == nullptr) {
        return;
    }
    TPlayAudioBackend* backend = backendFrom(backend_);
    if (NSString* onlineError = [backend onlineError]; onlineError != nil) {
        const_cast<AudioEngine*>(this)->setErrorLocked([onlineError UTF8String]);
        return;
    }
    const_cast<AudioEngine*>(this)->currentDurationSeconds_ = [backend durationSeconds];
    if ([backend consumeFinishedNaturally]) {
        finishedNaturally_ = true;
    }
    PlaybackState backendState = [backend playbackState];
    if (backendState == PlaybackState::Stopped &&
        (state_ == PlaybackState::Playing || state_ == PlaybackState::Paused)) {
        const_cast<AudioEngine*>(this)->allowIdleSleepLocked();
    }
    state_ = backendState;
#endif
}

void AudioEngine::preventIdleSleepLocked()
{
#ifdef __APPLE__
    if (sleepAssertion_ != kIOPMNullAssertionID) {
        return;
    }

    IOReturn result = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleSystemSleep,
        kIOPMAssertionLevelOn,
        CFSTR("TPlay audio playback"),
        &sleepAssertion_);
    if (result != kIOReturnSuccess) {
        sleepAssertion_ = kIOPMNullAssertionID;
    }
#endif
}

void AudioEngine::allowIdleSleepLocked()
{
#ifdef __APPLE__
    if (sleepAssertion_ == kIOPMNullAssertionID) {
        return;
    }
    IOPMAssertionRelease(sleepAssertion_);
    sleepAssertion_ = kIOPMNullAssertionID;
#endif
}
