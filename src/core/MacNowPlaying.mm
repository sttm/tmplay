#include "MacNowPlaying.hpp"

#include <algorithm>

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

@interface TPlayNowPlayingBridge : NSObject
@property(nonatomic, assign) NSUInteger generation;
@property(nonatomic, strong) NSMutableArray* commandTokens;
- (void)publishTitle:(NSString*)title
               artist:(NSString*)artist
           artworkUrl:(NSString*)artworkUrl
            mediaPath:(NSString*)mediaPath
             playback:(PlaybackSnapshot)playback;
- (void)applyArtwork:(NSImage*)image generation:(NSUInteger)generation;
- (void)updatePlayback:(PlaybackSnapshot)playback;
- (void)clear;
- (void)setRemotePlay:(std::function<void()>)play
                pause:(std::function<void()>)pause
       togglePlayPause:(std::function<void()>)togglePlayPause
             previous:(std::function<void()>)previous
                 next:(std::function<void()>)next;
@end

@implementation TPlayNowPlayingBridge

- (instancetype)init
{
    self = [super init];
    if (self) {
        // tmplay is a terminal UI, so macOS does not create an NSApplication
        // for it automatically.  MediaPlayer only advertises Now Playing
        // metadata from an app process; make this process an accessory app
        // without adding a Dock icon or taking focus from the terminal.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [[NSProcessInfo processInfo] setProcessName:@"tmplay"];
        _commandTokens = [NSMutableArray array];
    }
    return self;
}

- (void)setPlaybackState:(PlaybackState)state
{
    MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
    switch (state) {
    case PlaybackState::Playing:
        center.playbackState = MPNowPlayingPlaybackStatePlaying;
        break;
    case PlaybackState::Paused:
        center.playbackState = MPNowPlayingPlaybackStatePaused;
        break;
    case PlaybackState::Stopped:
        center.playbackState = MPNowPlayingPlaybackStateStopped;
        break;
    case PlaybackState::Error:
        center.playbackState = MPNowPlayingPlaybackStateUnknown;
        break;
    }
}

- (NSMutableDictionary*)infoForPlayback:(PlaybackSnapshot)playback
{
    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    info[MPNowPlayingInfoPropertyMediaType] = @(MPNowPlayingInfoMediaTypeAudio);
    if (playback.durationSeconds > 0.0) {
        info[MPMediaItemPropertyPlaybackDuration] = @(playback.durationSeconds);
    }
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] =
        @(std::max(0.0, playback.positionSeconds));
    info[MPNowPlayingInfoPropertyPlaybackRate] =
        @(playback.state == PlaybackState::Playing ? 1.0 : 0.0);
    return info;
}

- (void)publishTitle:(NSString*)title
               artist:(NSString*)artist
           artworkUrl:(NSString*)artworkUrl
            mediaPath:(NSString*)mediaPath
             playback:(PlaybackSnapshot)playback
{
    ++_generation;
    const NSUInteger generation = _generation;
    NSMutableDictionary* info = [self infoForPlayback:playback];
    info[MPMediaItemPropertyTitle] = title.length > 0 ? title : @"tmplay";
    if (artist.length > 0) {
        info[MPMediaItemPropertyArtist] = artist;
    }
    MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
    // Control Center can otherwise retain the previous item's title/artwork
    // while an asynchronous image request for the new online track is in
    // flight. Replacing a cleared card makes the track change immediate.
    center.nowPlayingInfo = nil;
    center.nowPlayingInfo = info;
    [self setPlaybackState:playback.state];

    __weak TPlayNowPlayingBridge* weakSelf = self;
    if (artworkUrl.length > 0) {
        NSURL* url = [NSURL URLWithString:artworkUrl];
        if (!url) return;
        NSURLSessionDataTask* artworkTask =
            [[NSURLSession sharedSession] dataTaskWithURL:url
                                         completionHandler:^(NSData* data,
                                                             NSURLResponse*,
                                                             NSError*) {
            NSImage* image = data.length > 0 ? [[NSImage alloc] initWithData:data] : nil;
            if (!image) return;
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf applyArtwork:image generation:generation];
            });
        }];
        [artworkTask resume];
        return;
    }
    if (mediaPath.length == 0) return;
    NSString* filePath = [mediaPath copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:
            [NSURL fileURLWithPath:filePath] options:nil];
        NSData* data = nil;
        for (AVMetadataItem* item in asset.commonMetadata) {
            if ([item.commonKey isEqualToString:AVMetadataCommonKeyArtwork]) {
                data = item.dataValue;
                break;
            }
        }
        NSImage* image = data.length > 0 ? [[NSImage alloc] initWithData:data] : nil;
        if (!image) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf applyArtwork:image generation:generation];
        });
    });
}

- (void)applyArtwork:(NSImage*)image generation:(NSUInteger)generation
{
    if (!image || _generation != generation) return;
    const NSSize sourceSize = image.size;
    const CGFloat side = std::min(sourceSize.width, sourceSize.height);
    if (side <= 0.0) return;
    const NSRect sourceRect = NSMakeRect((sourceSize.width - side) * 0.5,
                                         (sourceSize.height - side) * 0.5,
                                         side,
                                         side);
    NSImage* squareImage = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
    [squareImage lockFocus];
    [image drawInRect:NSMakeRect(0.0, 0.0, side, side)
             fromRect:sourceRect
            operation:NSCompositingOperationCopy
             fraction:1.0
       respectFlipped:YES
                hints:nil];
    [squareImage unlockFocus];
    MPMediaItemArtwork* artwork = [[MPMediaItemArtwork alloc]
        initWithBoundsSize:squareImage.size
        requestHandler:^NSImage*(CGSize) { return squareImage; }];
    NSMutableDictionary* latest =
        [[MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo mutableCopy];
    if (!latest) return;
    latest[MPMediaItemPropertyArtwork] = artwork;
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = latest;
}

- (void)updatePlayback:(PlaybackSnapshot)playback
{
    NSMutableDictionary* info =
        [[MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo mutableCopy];
    if (!info) return;
    if (playback.durationSeconds > 0.0) {
        info[MPMediaItemPropertyPlaybackDuration] = @(playback.durationSeconds);
    }
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] =
        @(std::max(0.0, playback.positionSeconds));
    info[MPNowPlayingInfoPropertyPlaybackRate] =
        @(playback.state == PlaybackState::Playing ? 1.0 : 0.0);
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = info;
    [self setPlaybackState:playback.state];
}

- (void)clear
{
    ++_generation;
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
    [self setPlaybackState:PlaybackState::Stopped];
}

- (void)setRemotePlay:(std::function<void()>)play
                pause:(std::function<void()>)pause
       togglePlayPause:(std::function<void()>)togglePlayPause
             previous:(std::function<void()>)previous
                 next:(std::function<void()>)next
{
    MPRemoteCommandCenter* center = [MPRemoteCommandCenter sharedCommandCenter];
    for (id token in _commandTokens) {
        [center.playCommand removeTarget:token];
        [center.pauseCommand removeTarget:token];
        [center.togglePlayPauseCommand removeTarget:token];
        [center.previousTrackCommand removeTarget:token];
        [center.nextTrackCommand removeTarget:token];
    }
    [_commandTokens removeAllObjects];
    auto add = ^(MPRemoteCommand* command, std::function<void()> action) {
        command.enabled = YES;
        id token = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            if (!action) return MPRemoteCommandHandlerStatusCommandFailed;
            // MediaPlayer is allowed to invoke this handler on an internal
            // command thread.  AudioEngine, AppController and FTXUI all have
            // main-thread state, so never switch a track synchronously here.
            // Repeated F9 presses used to race this callback against the
            // playback worker's Now Playing updates and could terminate the
            // process after a few track changes.
            std::function<void()> queuedAction = action;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (queuedAction) queuedAction();
            });
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        [_commandTokens addObject:token];
    };
    add(center.playCommand, std::move(play));
    add(center.pauseCommand, std::move(pause));
    // MacBook F8 generally emits this command rather than separate Play or
    // Pause commands. Registering only the latter leaves F8 inert.
    add(center.togglePlayPauseCommand, std::move(togglePlayPause));
    add(center.previousTrackCommand, std::move(previous));
    add(center.nextTrackCommand, std::move(next));
}

@end

MacNowPlaying::MacNowPlaying()
{
    @autoreleasepool {
        bridge_ = (__bridge_retained void*)[[TPlayNowPlayingBridge alloc] init];
    }
}

MacNowPlaying::~MacNowPlaying()
{
    if (!bridge_) return;
    @autoreleasepool {
        TPlayNowPlayingBridge* bridge = (__bridge_transfer TPlayNowPlayingBridge*)bridge_;
        [bridge clear];
        bridge_ = nullptr;
    }
}

void MacNowPlaying::publish(const std::string& title,
                            const std::string& artist,
                            const std::string& artworkUrl,
                            const std::string& mediaPath,
                            const PlaybackSnapshot& playback)
{
    if (!bridge_) return;
    @autoreleasepool {
        TPlayNowPlayingBridge* bridge = (__bridge TPlayNowPlayingBridge*)bridge_;
        NSString* nsTitle = [NSString stringWithUTF8String:title.c_str()];
        NSString* nsArtist = [NSString stringWithUTF8String:artist.c_str()];
        NSString* nsArtworkUrl = [NSString stringWithUTF8String:artworkUrl.c_str()];
        NSString* nsMediaPath = [NSString stringWithUTF8String:mediaPath.c_str()];
        auto publish = ^{
            [bridge publishTitle:nsTitle
                           artist:nsArtist
                       artworkUrl:nsArtworkUrl
                        mediaPath:nsMediaPath
                         playback:playback];
        };
        if ([NSThread isMainThread]) {
            publish();
        } else {
            // The block strongly retains bridge and the NSString values until
            // the main run loop processes this publication.
            dispatch_async(dispatch_get_main_queue(), publish);
        }
    }
}

void MacNowPlaying::updatePlayback(const PlaybackSnapshot& playback)
{
    if (!bridge_) return;
    @autoreleasepool {
        TPlayNowPlayingBridge* bridge = (__bridge TPlayNowPlayingBridge*)bridge_;
        auto update = ^{
            [bridge updatePlayback:playback];
        };
        if ([NSThread isMainThread]) {
            update();
        } else {
            dispatch_async(dispatch_get_main_queue(), update);
        }
    }
}

void MacNowPlaying::clear()
{
    if (!bridge_) return;
    @autoreleasepool {
        TPlayNowPlayingBridge* bridge = (__bridge TPlayNowPlayingBridge*)bridge_;
        auto clear = ^{
            [bridge clear];
        };
        if ([NSThread isMainThread]) {
            clear();
        } else {
            dispatch_async(dispatch_get_main_queue(), clear);
        }
    }
}

void MacNowPlaying::setRemoteCommandHandlers(std::function<void()> play,
                                             std::function<void()> pause,
                                             std::function<void()> togglePlayPause,
                                             std::function<void()> previous,
                                             std::function<void()> next)
{
    if (!bridge_) return;
    @autoreleasepool {
        [(__bridge TPlayNowPlayingBridge*)bridge_ setRemotePlay:std::move(play)
                                                          pause:std::move(pause)
                                                togglePlayPause:std::move(togglePlayPause)
                                                       previous:std::move(previous)
                                                           next:std::move(next)];
    }
}

void MacNowPlaying::pumpSystemRunLoop()
{
    @autoreleasepool {
        // Drain a bounded number of already-pending events. A zero timeout
        // never blocks FTXUI's keyboard rendering loop.
        for (int pass = 0; pass < 8; ++pass) {
            SInt32 result = CFRunLoopRunInMode(
                kCFRunLoopDefaultMode, 0.0, true);
            if (result != kCFRunLoopRunHandledSource) break;
        }
    }
}
