#include "CueDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

int frameAt(double seconds, const AutoCueFeatures& features)
{
    if (features.hopSeconds <= 0.0) {
        return 0;
    }
    return std::clamp((int)std::llround(seconds / features.hopSeconds),
                      0,
                      std::max(0, (int)features.energyCurve.size() - 1));
}

float curveAt(const std::vector<float>& values, int index)
{
    if (values.empty()) {
        return 0.0f;
    }
    return values[(size_t)std::clamp(index, 0, (int)values.size() - 1)];
}

double estimateBeatInterval(const AutoCueFeatures& features)
{
    if (features.beats.size() < 2) {
        return 0.5;
    }

    std::vector<double> intervals;
    intervals.reserve(features.beats.size() - 1);
    for (size_t i = 1; i < features.beats.size(); ++i) {
        double interval = (double)features.beats[i] - (double)features.beats[i - 1];
        if (interval > 0.15 && interval < 2.0) {
            intervals.push_back(interval);
        }
    }
    if (intervals.empty()) {
        return 0.5;
    }
    std::sort(intervals.begin(), intervals.end());
    return intervals[intervals.size() / 2];
}

float averageCurve(const std::vector<float>& values,
                   const AutoCueFeatures& features,
                   double center,
                   double radiusSeconds)
{
    if (values.empty() || features.hopSeconds <= 0.0) {
        return 0.0f;
    }
    int first = frameAt(center - radiusSeconds, features);
    int last = frameAt(center + radiusSeconds, features);
    if (last < first) {
        std::swap(first, last);
    }
    double sum = 0.0;
    int count = 0;
    for (int i = first; i <= last; ++i) {
        sum += curveAt(values, i);
        count++;
    }
    return count > 0 ? (float)(sum / (double)count) : 0.0f;
}

float curveChange(const std::vector<float>& values,
                  const AutoCueFeatures& features,
                  double seconds,
                  double windowSeconds)
{
    float before = averageCurve(values, features, seconds - windowSeconds * 0.5, windowSeconds * 0.5);
    float after = averageCurve(values, features, seconds + windowSeconds * 0.5, windowSeconds * 0.5);
    return std::abs(after - before);
}

std::array<float, 12> averageChroma(const AutoCueFeatures& features,
                                    double center,
                                    double radiusSeconds)
{
    std::array<float, 12> result{};
    if (features.chromaCurve.empty() || features.hopSeconds <= 0.0) {
        return result;
    }
    int first = frameAt(center - radiusSeconds, features);
    int last = frameAt(center + radiusSeconds, features);
    if (last < first) {
        std::swap(first, last);
    }
    int count = 0;
    for (int i = first; i <= last; ++i) {
        size_t index = (size_t)std::clamp(i, 0, (int)features.chromaCurve.size() - 1);
        for (size_t pc = 0; pc < result.size(); ++pc) {
            result[pc] += features.chromaCurve[index][pc];
        }
        count++;
    }
    if (count > 0) {
        for (float& value : result) {
            value /= (float)count;
        }
    }
    return result;
}

float chromaChange(const AutoCueFeatures& features,
                   double seconds,
                   double windowSeconds)
{
    auto before = averageChroma(features, seconds - windowSeconds * 0.5, windowSeconds * 0.5);
    auto after = averageChroma(features, seconds + windowSeconds * 0.5, windowSeconds * 0.5);
    float distance = 0.0f;
    for (size_t i = 0; i < before.size(); ++i) {
        distance += std::abs(after[i] - before[i]);
    }
    return distance * 0.5f;
}

double bestScoreInRange(const AutoCueFeatures& features,
                        double from,
                        double to)
{
    if (features.energyCurve.empty()) {
        return std::clamp(from, 0.0, features.duration);
    }

    int first = frameAt(from, features);
    int last = frameAt(to, features);
    if (last <= first) {
        return std::clamp(from, 0.0, features.duration);
    }

    float max_score = -1.0f;
    int best = first;
    for (int i = first; i <= last; ++i) {
        float prev = curveAt(features.energyCurve, i - 8);
        float energy = curveAt(features.energyCurve, i);
        float rise = std::max(0.0f, energy - prev);
        float onset = curveAt(features.onsetCurve, i);
        float flux = curveAt(features.spectralFluxCurve, i);
        float score = rise * 0.5f + onset * 0.3f + flux * 0.2f;
        if (score > max_score) {
            max_score = score;
            best = i;
        }
    }
    return (double)best * features.hopSeconds;
}

double lowestEnergyInRange(const AutoCueFeatures& features,
                           double from,
                           double to)
{
    if (features.energyCurve.empty()) {
        return std::clamp(from, 0.0, features.duration);
    }

    int first = frameAt(from, features);
    int last = frameAt(to, features);
    if (last <= first) {
        return std::clamp(from, 0.0, features.duration);
    }

    float min_energy = curveAt(features.energyCurve, first);
    int best = first;
    for (int i = first; i <= last; ++i) {
        float energy = curveAt(features.energyCurve, i);
        if (energy < min_energy) {
            min_energy = energy;
            best = i;
        }
    }
    return (double)best * features.hopSeconds;
}

double snapToNearestBeat(double seconds, const AutoCueFeatures& features)
{
    if (features.beats.empty()) {
        return std::clamp(seconds, 0.0, features.duration);
    }
    auto nearest = std::min_element(
        features.beats.begin(),
        features.beats.end(),
        [&](float a, float b) {
            return std::abs((double)a - seconds) < std::abs((double)b - seconds);
        });
    return std::clamp((double)*nearest, 0.0, features.duration);
}

double snapToNearestBeatInRange(double seconds,
                                const AutoCueFeatures& features,
                                double from,
                                double to)
{
    from = std::clamp(from, 0.0, features.duration);
    to = std::clamp(to, from, features.duration);
    if (features.beats.empty()) {
        return std::clamp(seconds, from, to);
    }

    auto nearest = features.beats.end();
    double best_distance = std::numeric_limits<double>::max();
    for (auto beat = features.beats.begin(); beat != features.beats.end(); ++beat) {
        double value = *beat;
        if (value < from || value > to) {
            continue;
        }
        double distance = std::abs(value - seconds);
        if (distance < best_distance) {
            best_distance = distance;
            nearest = beat;
        }
    }
    if (nearest == features.beats.end()) {
        return std::clamp(seconds, from, to);
    }
    return *nearest;
}

double snapToBar(const AutoCueFeatures& features,
                 double seconds,
                 double from,
                 double to,
                 int beatsPerBar = 4)
{
    from = std::clamp(from, 0.0, features.duration);
    to = std::clamp(to, from, features.duration);
    if (features.beats.empty()) {
        return std::clamp(seconds, from, to);
    }

    int bestIndex = -1;
    double bestDistance = std::numeric_limits<double>::max();
    for (int i = 0; i < (int)features.beats.size(); i += beatsPerBar) {
        double beat = features.beats[(size_t)i];
        if (beat < from || beat > to) {
            continue;
        }
        double distance = std::abs(beat - seconds);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    if (bestIndex < 0) {
        return snapToNearestBeatInRange(seconds, features, from, to);
    }
    return features.beats[(size_t)bestIndex];
}

double representativeBeat(const AutoCueFeatures& features,
                          double ratio,
                          double from,
                          double to)
{
    return snapToBar(features, features.duration * ratio, from, to);
}

double findFirstAudibleSound(const AutoCueFeatures& features)
{
    if (features.energyCurve.empty() || features.hopSeconds <= 0.0) {
        return 0.0;
    }

    float maxEnergy = 0.0f;
    for (float value : features.energyCurve) {
        maxEnergy = std::max(maxEnergy, value);
    }
    if (maxEnergy <= 0.0f) {
        return 0.0;
    }

    float threshold = std::max(0.03f, maxEnergy * 0.03f);
    int requiredFrames = std::max(1, (int)std::ceil(0.08 / features.hopSeconds));
    int consecutive = 0;
    for (int i = 0; i < (int)features.energyCurve.size(); ++i) {
        if (features.energyCurve[(size_t)i] >= threshold ||
            curveAt(features.onsetCurve, i) >= threshold) {
            consecutive++;
            if (consecutive >= requiredFrames) {
                int firstFrame = i - consecutive + 1;
                return std::clamp((double)firstFrame * features.hopSeconds,
                                  0.0,
                                  features.duration);
            }
        } else {
            consecutive = 0;
        }
    }
    return 0.0;
}

double findSectionBoundary(const AutoCueFeatures& features,
                           double from,
                           double to,
                           double fallback)
{
    from = std::clamp(from, 0.0, features.duration);
    to = std::clamp(to, from, features.duration);
    double best = fallback;
    float bestScore = -1.0f;
    double window = std::max(2.0, estimateBeatInterval(features) * 8.0);
    for (int i = 0; i < (int)features.beats.size(); i += 4) {
        double candidate = features.beats[(size_t)i];
        if (candidate < from || candidate > to) {
            continue;
        }
        float score =
            curveChange(features.energyCurve, features, candidate, window) * 0.35f +
            curveChange(features.onsetCurve, features, candidate, window) * 0.20f +
            curveChange(features.spectralFluxCurve, features, candidate, window) * 0.25f +
            curveChange(features.spectralCentroidCurve, features, candidate, window) * 0.10f +
            chromaChange(features, candidate, window) * 0.10f;
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return snapToBar(features, best, from, to);
}

double findChorusStart(const AutoCueFeatures& features,
                       double from,
                       double to,
                       double fallback)
{
    from = std::clamp(from, 0.0, features.duration);
    to = std::clamp(to, from, features.duration);
    double best = fallback;
    float bestScore = -1.0f;
    double window = std::max(2.0, estimateBeatInterval(features) * 8.0);
    for (int i = 0; i < (int)features.beats.size(); i += 4) {
        double candidate = features.beats[(size_t)i];
        if (candidate < from || candidate > to) {
            continue;
        }
        float score =
            averageCurve(features.energyCurve, features, candidate, window) * 0.45f +
            averageCurve(features.onsetCurve, features, candidate, window * 0.5) * 0.25f +
            averageCurve(features.spectralFluxCurve, features, candidate, window * 0.5) * 0.20f +
            (curveChange(features.energyCurve, features, candidate, window) +
             chromaChange(features, candidate, window)) * 0.05f;
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return snapToBar(features, best, from, to);
}

double findBridgeStart(const AutoCueFeatures& features,
                       double from,
                       double to,
                       double fallback)
{
    from = std::clamp(from, 0.0, features.duration);
    to = std::clamp(to, from, features.duration);
    double best = fallback;
    float bestScore = -1.0f;
    double window = std::max(2.0, estimateBeatInterval(features) * 8.0);
    for (int i = 0; i < (int)features.beats.size(); i += 4) {
        double candidate = features.beats[(size_t)i];
        if (candidate < from || candidate > to) {
            continue;
        }
        float beforeEnergy = averageCurve(features.energyCurve, features,
                                          candidate - window * 0.5, window * 0.5);
        float afterEnergy = averageCurve(features.energyCurve, features,
                                         candidate + window * 0.5, window * 0.5);
        float energyDrop = std::max(0.0f, beforeEnergy - afterEnergy);
        float score =
            energyDrop * 0.45f +
            chromaChange(features, candidate, window) * 0.30f +
            curveChange(features.spectralCentroidCurve, features, candidate, window) * 0.25f;
        if (score > bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return snapToBar(features, best, from, to);
}

void normalize(std::vector<float>& values)
{
    float max_value = 0.0f;
    for (float value : values) {
        max_value = std::max(max_value, value);
    }
    if (max_value <= 0.0f) {
        return;
    }
    for (float& value : values) {
        value /= max_value;
    }
}

AutoCueFeatures normalized(AutoCueFeatures features)
{
    normalize(features.energyCurve);
    normalize(features.onsetCurve);
    normalize(features.spectralFluxCurve);
    normalize(features.spectralCentroidCurve);
    return features;
}

}  // namespace

AutoCueResult CueDetector::detect(const AutoCueFeatures& raw_features) const
{
    AutoCueFeatures features = normalized(raw_features);
    AutoCueResult result;
    result.duration = features.duration;

    if (features.duration <= 0.0) {
        return result;
    }

    double beatInterval = estimateBeatInterval(features);
    double barSeconds = beatInterval * 4.0;
    result.start = {"START", findFirstAudibleSound(features)};

    if (features.duration < 45.0) {
        result.drop1 = {"VERSE", representativeBeat(
            features, 0.30, result.start.positionSeconds + barSeconds, features.duration - 3.0)};
        result.breakdown = {"CHORUS", representativeBeat(
            features, 0.52, result.drop1.positionSeconds + barSeconds, features.duration - 2.0)};
        result.drop2 = {"BRIDGE", representativeBeat(
            features, 0.78, result.breakdown.positionSeconds + barSeconds, features.duration - 0.5)};
        return result;
    }

    double verseFrom = result.start.positionSeconds + barSeconds * 8.0;
    double verseTo = result.start.positionSeconds + barSeconds * 32.0;
    double verseFallback = result.start.positionSeconds + barSeconds * 16.0;
    result.drop1 = {"VERSE", findSectionBoundary(features, verseFrom, verseTo, verseFallback)};

    double chorusFrom = result.drop1.positionSeconds + barSeconds * 8.0;
    double chorusTo = result.drop1.positionSeconds + barSeconds * 32.0;
    double chorusFallback = result.drop1.positionSeconds + barSeconds * 16.0;
    result.breakdown = {"CHORUS", findChorusStart(features, chorusFrom, chorusTo, chorusFallback)};

    double bridgeFrom = result.breakdown.positionSeconds + barSeconds * 8.0;
    double bridgeTo = features.duration * 0.85;
    double bridgeFallback = result.breakdown.positionSeconds + barSeconds * 32.0;
    result.drop2 = {"BRIDGE", findBridgeStart(features, bridgeFrom, bridgeTo, bridgeFallback)};

    return result;
}
