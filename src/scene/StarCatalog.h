#pragma once
#include "scene/StarField.h"

#include <filesystem>
#include <string>
#include <vector>

namespace space {

// A star catalogue ready for upload, plus the presentation defaults that suit its units.
struct StarCatalog {
    std::string name;
    std::vector<StarGpu> stars;
    float defaultBrightness = 1.f; // flux multiplier that makes this catalogue look right
    double defaultSpeed = 1.0;     // camera units per second
    double unitsPerParsec = 0.0;   // 0 = arbitrary units
};

// Loads the packed binary written by scripts/fetch_gaia.py. Returns false if the file is missing or bad.
bool loadGaiaCatalog(const std::filesystem::path& path, StarCatalog& out);

// Procedural spiral galaxy fallback.
StarCatalog makeProceduralCatalog(const StarField::Params& params = {});

// Directory that holds runtime assets (next to the executable).
std::filesystem::path assetDirectory();

} // namespace space
