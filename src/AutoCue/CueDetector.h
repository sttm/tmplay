#pragma once

#include "CuePreview.h"

class CueDetector {
public:
    AutoCueResult detect(const AutoCueFeatures& features) const;
};
