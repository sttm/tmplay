#pragma once

#include <filesystem>
#include <string>

struct sqlite3;

class LibraryDatabase {
public:
    LibraryDatabase() = default;
    ~LibraryDatabase();

    LibraryDatabase(const LibraryDatabase&) = delete;
    LibraryDatabase& operator=(const LibraryDatabase&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);
    bool initialize(std::string& error);
    bool exec(const std::string& sql, std::string& error);
    sqlite3* handle() const;
    bool isOpen() const;
    const std::filesystem::path& path() const;

private:
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
};

