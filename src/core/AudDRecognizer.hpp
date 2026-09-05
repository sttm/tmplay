#pragma once

#include <string>

struct AudDMatch {
    std::string artist;
    std::string title;
};

// Small AudD client used by Audio FIND. It deliberately sends the recording
// directly to AudD with curl: no token or audio data is written to disk.
class AudDRecognizer {
public:
    AudDRecognizer() = default;
    explicit AudDRecognizer(std::string apiKey);

    bool configured() const;
    bool recognize(const std::string& audioPath,
                   AudDMatch& match,
                   std::string& error) const;

private:
    std::string apiKey_;
};
