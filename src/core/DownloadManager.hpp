#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class DownloadState {
    Idle,
    Running,
    Done,
    Error
};

struct DownloadSnapshot {
    DownloadState state = DownloadState::Idle;
    std::string message;
    std::string detail;
    std::string filePath;
    float progress = 0.0f;
    struct Item {
        int index = 0;
        std::string title;
        std::string artist;
        std::string album;
        std::string genre;
        std::string thumbnailUrl;
        std::string releaseDate;
        std::string webpageUrl;
        std::string source;
        std::string status;
        std::string detail;
        std::string filePath;
        float progress = 0.0f;
    };
    std::vector<Item> items;
};

class DownloadManager {
public:
    using FinishedCallback = std::function<void()>;

    DownloadManager() = default;
    ~DownloadManager();

    DownloadManager(const DownloadManager&) = delete;
    DownloadManager& operator=(const DownloadManager&) = delete;

    bool start(const std::string& source,
               const std::string& outputDirectory,
               const std::string& format,
               std::vector<std::string> cookiesFromBrowser = {},
               std::string cookiesPath = {},
               FinishedCallback on_finished = {});
    bool startBatch(std::vector<DownloadSnapshot::Item> items,
                    const std::string& outputDirectory,
                    const std::string& format,
                    std::vector<std::string> cookiesFromBrowser = {},
                    std::string cookiesPath = {},
                    FinishedCallback on_finished = {});
    void cancel();
    DownloadSnapshot snapshot() const;

private:
    struct DownloadRequest {
        std::string source;
        std::string outputDirectory;
        std::string format;
        std::vector<std::string> cookiesFromBrowser;
        std::string cookiesPath;
        std::vector<DownloadSnapshot::Item> items;
        FinishedCallback onFinished;
    };

    void run(DownloadRequest request);
    void setItems(std::vector<DownloadSnapshot::Item> items);
    void updateItem(size_t itemIndex,
                    std::string status,
                    std::string detail = {},
                    std::string filePath = {},
                    float progress = 0.0f);
    void setState(DownloadState state,
                  std::string message,
                  std::string detail = {},
                  std::string filePath = {},
                  float progress = 0.0f);

    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic_bool running_{false};
    std::atomic_bool cancelRequested_{false};
    DownloadSnapshot snapshot_;
};
