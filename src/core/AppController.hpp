#pragma once

#include "TrackStore.hpp"
#include "Config.hpp"
#include "AudioEngine.hpp"
#include "MacNowPlaying.hpp"
#include "AudioAnalyzer.hpp"
#include "AudDRecognizer.hpp"
#include "DownloadManager.hpp"
#include "PlaylistImporter.hpp"
#include "ScreenRecorder.hpp"
#include "MetadataWriter.hpp"
#include "StemSeparator.hpp"
#include "AudioProcessor.hpp"
#include "../Library/CueRepository.h"
#include "../Library/LibraryDatabase.h"
#include "../Library/TrackRepository.h"
#include "../Export/RekordboxExportProvider.h"
#include "../Export/SeratoExportProvider.h"
#include "../Export/TraktorExportProvider.h"
#include "../Export/LibraryExporter.h"
#include "../AutoCue/FolderProcessor.h"
#include "../Serato/SeratoCueWriter.h"
#include "../Telegram/TelegramBotClient.h"
#include "../Telegram/TelegramInboxService.h"
#include "../Telegram/TelegramRepository.h"
#include "../Traktor/TraktorMetadataWriter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

enum class PlaybackMode {
    Shuffle,
    RepeatOne,
    RepeatAll
};

enum class CueSyncDirection {
    Auto,
    SeratoToTraktor,
    TraktorToSerato
};

struct OnlineSearchOptions {
    // Normal search shows up to 50 canonical YouTube Music Art Tracks.
    // Only when none exist does it show up to 10 original music videos.
    int maxTracks = 50;
    int minDurationSeconds = 0;
    int maxDurationSeconds = 0;
};

class AppController {
public:
    AppController();
    ~AppController();

    TrackStore& trackStore();
    const Config& config() const;

    void scanDirectory(const std::string& path, bool forceRefresh = false);
    std::string currentPath() const;
    void setCurrentPath(const std::string& path);
    // Human-readable name of a virtual folder.  It is intentionally kept
    // separate from its stable path, so a search result can retain its query
    // while the user moves through its child folders.
    std::string virtualPlaylistName(const std::string& path) const;
    int volume() const;
    void volumeUp();
    void volumeDown();
    double playbackRate() const;
    void setPlaybackRate(double rate);
    bool preservePitch() const;
    void setPreservePitch(bool preserve);
    void setEqualizerGains(double lowDb, double midDb, double highDb);
    bool playTrack(const Track& track, const std::vector<Track>& orderedTracks);
    bool playPreviewTrack(const Track& track, double startSeconds = 0.0);
    void togglePreviewPause();
    void stopPreviewPlayback();
    void seekPreviewPlayback(double ratio);
    void setPreviewLoopRange(double startSeconds, double endSeconds);
    void clearPreviewLoopRange();
    PlaybackSnapshot previewPlaybackSnapshot() const;
    std::string previewPlayingTrackId() const;
    double previewPlaybackRate() const;
    void setPreviewPlaybackRate(double rate);
    bool previewPreservePitch() const;
    void setPreviewPreservePitch(bool preserve);
    void togglePause();
    void stopPlayback();
    bool playPreviousTrack();
    bool playNextTrack();
    void cyclePlaybackMode();
    PlaybackMode playbackMode() const;
    void seekPlayback(double ratio);
    PlaybackSnapshot playbackSnapshot() const;
    std::string playingTrackId() const;
    Track playingTrack() const;
    // Opens the folder or temporary online playlist that supplied the
    // currently playing track.
    bool openNowPlayingLocation(std::string& result);
    bool isStreamingPlayback() const;
    bool downloadToCurrentDirectory(const std::string& source,
                                    std::function<void()> on_finished = {});
    bool downloadToDirectory(const std::string& source,
                             const std::string& directory,
                             std::function<void()> on_finished = {});
    bool downloadTracksToCurrentDirectory(const std::vector<Track>& tracks,
                                          std::function<void()> on_finished = {});
    // Download badges for temporary online Search entries. This state is
    // cached outside the FTXUI render loop and refreshed after a download.
    bool isOnlineTrackDownloaded(const Track& track) const;
    bool isOnlineAlbumDownloaded(const std::vector<Track>& tracks) const;
    void cancelDownload();
    DownloadSnapshot downloadSnapshot() const;
    // Returns false for remote Search entries until their automatic local
    // cache download is complete.
    bool canEditTrack(const Track& track, std::string& reason) const;
    bool openPlaylistFile(const std::string& playlistPath, std::string& result);
    // Opens a pasted online album or playlist as a temporary Search folder.
    bool openOnlinePlaylist(const std::string& source, std::string& result);
    bool searchMusic(const std::string& query,
                     bool groupAlbums,
                     std::string& result,
                     const OnlineSearchOptions& options = {});
    // The interactive `track` command returns tracks and albums in one
    // temporary folder. `searchMusic(..., true)` remains album-only.
    bool searchMusicAll(const std::string& query,
                        std::string& result,
                        const OnlineSearchOptions& options = {});
    // Decodes one row in the Search history.  The caller can then repeat the
    // original command without treating the history row as playable media.
    bool recentSearchRequest(const Track& entry,
                             std::string& query,
                             bool& groupAlbums) const;
    // Lazily fetches and verifies an official album's ATV tracks after the
    // user opens an album row in search results.
    bool openOfficialAlbum(const Track& album, std::string& result);
    // Opens the precise release attached to an Art Track search result.
    bool openAlbumFromSearchTrack(const Track& track, std::string& result);
    bool searchLocalMusic(const std::string& query, std::string& result);
    bool isVirtualPlaylistPath(const std::string& path) const;
    bool startDesktopRecording(std::string& result);
    // Starts an isolated AudD capture. Unlike REC, its file is never added
    // to the Records folder and must be removed with discardAudDRecording.
    bool startAudDRecording(std::string& result);
    void stopDesktopRecording();
    RecordingSnapshot recordingSnapshot() const;
    // Reads the current short-lived FIND capture and reports whether the
    // desktop output contains audible PCM samples yet.
    bool auddRecordingHasAudio() const;
    void discardAudDRecording(const std::string& filePath) const;
    bool auddFindEnabled() const;
    int auddListenSeconds() const;
    bool recognizeAudDRecording(const std::string& filePath,
                                AudDMatch& match,
                                std::string& error) const;
    bool separateTrack(const Track& track);
    bool separateTrack(const Track& track, const DemucsConfig& config);
    StemSeparationSnapshot stemSeparationSnapshot() const;
    bool normalizeTracks(const std::vector<Track>& tracks,
                         const NormalizationOptions& options);
    bool convertTracks(const std::vector<Track>& tracks,
                       const ConvertOptions& options);
    AudioProcessSnapshot audioProcessSnapshot() const;
    bool startAutoCueFolder();
    bool startAutoCueTrack(const Track& track);
    AutoCueProgress autoCueSnapshot() const;
    std::string cuePreviewForTrack(const Track& track, int width) const;
    AutoCueFeatures waveformForTrack(const Track& track, std::string& error) const;
    bool trimTrack(const Track& track,
                   double startSeconds,
                   double endSeconds,
                   std::string& error,
                   Track* outputTrack = nullptr);
    bool writeManualCues(const Track& track,
                         const std::vector<SeratoCue>& cues,
                         std::string& error);
    std::vector<SeratoCue> readManualCues(const Track& track,
                                          std::string& error);
    bool syncCueMetadata(const Track& track, std::string& result);
    bool syncCueMetadata(const Track& track,
                         CueSyncDirection direction,
                         std::string& result);
    bool metadataBusy() const;
    bool analyzeTrack(const Track& track, std::string& error);
    bool analyzeTracks(const std::vector<Track>& tracks, std::string& error);
    bool directoryScanBusy() const;
    bool createFolder(const std::string& name, std::string& error);
    bool renameFolder(const std::string& path,
                      const std::string& new_name,
                      std::string& error);
    bool moveTrack(const Track& track,
                   const std::string& destination,
                   std::string& error);
    bool deleteEntry(const Track& entry, std::string& error);
    bool startExternalDrag(const Track& track, std::string& error) const;
    bool openFolderExternally(const std::string& path, std::string& error) const;
    bool openExternalUrl(const std::string& url, std::string& error) const;
    std::vector<std::pair<std::string, std::string>>
    metadataDetails(const Track& track) const;
    bool exportLibrary(std::string& error);
    bool exportLibrary(std::string& result, bool validateAfterExport);
    bool importSeratoCues(std::string& result);
    bool importChangedJson(std::string& error);
    bool validateLibraryExport(std::string& result);
    bool validateSeratoCues(std::string& result) const;
    bool validateTraktorEmbeddedCues(std::string& result) const;
    bool libraryStatus(std::string& result) const;
    std::string enabledLibraryExportsLabel() const;
    bool isTelegramPath(const std::string& path) const;
    std::string telegramRootPath() const;

private:
    struct CachedOnlineAlbum {
        PlaylistImportResult release;
        std::chrono::steady_clock::time_point expiresAt;
    };
    TrackStore trackStore_;
    Config config_;
    LibraryDatabase libraryDatabase_;
    std::unique_ptr<TrackRepository> trackRepository_;
    std::unique_ptr<CueRepository> cueRepository_;
    std::unique_ptr<TelegramRepository> telegramRepository_;
    std::unique_ptr<TelegramBotClient> telegramClient_;
    std::unique_ptr<TelegramInboxService> telegramInbox_;
    AudioEngine audioEngine_;
    MacNowPlaying nowPlaying_;
    AudioEngine previewAudioEngine_;
    DownloadManager downloadManager_;
    ScreenRecorder screenRecorder_;
    std::atomic_bool temporaryDesktopRecording_{false};
    AudDRecognizer auddRecognizer_;
    StemSeparator stemSeparator_;
    AudioProcessor audioProcessor_;
    AudioAnalyzer audioAnalyzer_;
    mutable std::mutex managedDownloadCacheMutex_;
    mutable bool managedDownloadCacheReady_ = false;
    mutable std::unordered_set<std::string> managedDownloadedTitles_;
    mutable std::unordered_set<std::string> managedDownloadedAlbumTracks_;
    MetadataWriter metadataWriter_;
    AutoCueMarker autoCueMarker_{audioAnalyzer_};
    SeratoCueWriter seratoCueWriter_;
    TraktorMetadataWriter traktorMetadataWriter_;
    FolderProcessor autoCueProcessor_{autoCueMarker_, seratoCueWriter_};

    int volume_ = 80;
    std::string currentPath_;
    mutable std::mutex currentPathMutex_;
    mutable std::mutex metadataMutex_;
    std::condition_variable_any metadataCv_;
    std::vector<Track> pendingMetadata_;
    std::unordered_set<std::string> forcedMetadataAnalysis_;
    std::string pendingMetadataPath_;
    unsigned long metadataGeneration_ = 0;
    std::atomic_bool metadataBusy_{false};
    std::jthread metadataWorker_;
    mutable std::mutex playbackMutex_;
    std::vector<Track> displayedTracks_;
    std::vector<Track> playbackQueue_;
    std::string playingTrackId_;
    Track playingTrack_;
    std::string playingSourcePath_;
    // Direct stream URLs are ephemeral. They exist only for this run and are
    // reused by an explicit download click instead of resolving YouTube again.
    std::unordered_map<std::string, std::string> resolvedStreamUrls_;
    std::string prefetchingTrackId_;
    std::unordered_set<std::string> failedPrefetchTrackIds_;
    std::string previewPlayingTrackId_;
    PlaybackMode playbackMode_ = PlaybackMode::RepeatAll;
    std::mt19937 randomGenerator_{std::random_device{}()};
    std::jthread playbackWorker_;
    mutable std::mutex autoCueMutex_;
    AutoCueProgress autoCueProgress_;
    std::atomic_bool autoCueBusy_{false};
    std::atomic_bool autoCueCancel_{false};
    std::jthread autoCueWorker_;
    std::unordered_map<std::string, std::vector<Track>> directoryCache_;
    std::unordered_map<std::string, PlaylistImportResult> virtualPlaylists_;
    // Album browse IDs are immutable. Keep their resolved Art Track lists
    // briefly so returning to an album never repeats a YTMusic request.
    std::unordered_map<std::string, CachedOnlineAlbum> onlineAlbumCache_;
    mutable std::mutex directoryCacheMutex_;
    std::jthread initialScanWorker_;
    std::atomic_uint64_t directoryScanGeneration_{0};
    std::atomic_int directoryScansInFlight_{0};

    bool isAllowedFormat(const std::string& ext) const;
    bool managedDownloadDirectory(std::string& destination, std::string& error);
    void invalidateManagedDownloadCache();
    void refreshManagedDownloadCache() const;
    std::filesystem::path searchCachePath() const;
    std::filesystem::path searchHistoryPath() const;
    void restoreSearchCache();
    void restoreSearchHistory();
    void saveSearchCache(const std::vector<Track>& tracks) const;
    void saveSearchHistory() const;
    void rememberSearch(const std::string& query, bool groupAlbums);
    void setCurrentSearchLabel(const std::string& query);
    bool scanTelegramDirectory(const std::string& path,
                               std::vector<Track>& tracks,
                               std::string& error);
    bool resolveTelegramTrackForPlayback(const Track& track,
                                         Track& localTrack,
                                         std::string& error);
    static bool isOnlineMediaUrl(const std::string& value);
    static bool needsMetadataScan(const Track& track);
    static void mergeMetadataIntoTrack(Track& track,
                                       const AudioMetadata& metadata);
    void initializeLibrary();
    void upsertLibraryTrack(const Track& track);
    void importSeratoCuesIfLibraryEmpty(const Track& track,
                                        const LibraryTrack& libraryTrack);
    void replaceLibraryCues(const Track& track,
                            const std::vector<SeratoCue>& cues);
    LibraryTrack libraryTrackForCues(const Track& track,
                                     const std::vector<SeratoCue>& cues) const;
    Track cueSafeTrackForRead(const Track& source) const;
    bool cueSafeTrackForWrite(const Track& source,
                              Track& target,
                              std::string& error) const;
    bool exportLibraryCues(const LibraryTrack& track,
                           std::string& error,
                           bool updateCollectionExport = true);
    bool exportLibraryCollection(std::string& error);
    bool saveAutoCueResult(const std::filesystem::path& file,
                           const AutoCueResult& result,
                           const std::vector<SeratoCue>& cues,
                           std::string& error,
                           bool updateCollectionExport = true);
    void updateCachedTrack(const std::string& directory, const Track& track);
    void removeCachedEntry(const std::string& directory, const std::string& id);
    void invalidateDirectoryCache(const std::string& path);
    void queueMetadataScan(const std::string& path,
                           const std::vector<Track>& tracks);
    void metadataLoop(std::stop_token stop_token);
    void playbackLoop(std::stop_token stop_token);
    void prefetchNextOnlineStream(const PlaybackSnapshot& playback);
    void publishNowPlaying(const Track& track);
    void updateNowPlayingPlayback();
    bool playRelativeTrackLocked(int direction, bool natural_end);
};
