#pragma once

#include <filesystem>
#include <string>

struct ExportValidationSummary {
    int rekordboxTracks = 0;
    int rekordboxCues = 0;
    int traktorTracks = 0;
    int traktorCues = 0;
    int jsonTracks = 0;
    int jsonCues = 0;
};

struct ExportValidationOptions {
    bool validateRekordbox = true;
    bool validateTraktor = true;
    bool validateJson = true;
};

class ExportValidator {
public:
    bool validateRekordboxXml(const std::filesystem::path& file,
                              ExportValidationSummary& summary,
                              std::string& error) const;
    bool validateTraktorNml(const std::filesystem::path& file,
                            ExportValidationSummary& summary,
                            std::string& error) const;
    bool validateJsonFolder(const std::filesystem::path& folder,
                            ExportValidationSummary& summary,
                            std::string& error) const;
    bool validateExportFolder(const std::filesystem::path& exportFolder,
                              const std::filesystem::path& syncFolder,
                              ExportValidationSummary& summary,
                              std::string& error) const;
    bool validateExportFolder(const std::filesystem::path& exportFolder,
                              const std::filesystem::path& syncFolder,
                              const ExportValidationOptions& options,
                              ExportValidationSummary& summary,
                              std::string& error) const;
};
