#include "scene/StarCatalog.h"
#include "core/Log.h"
#include "gfx/Pipeline.h"

#include <cstring>
#include <fstream>

namespace space {

std::filesystem::path assetDirectory() {
    // assets/ next to the exe wins (redistributable builds); otherwise read the source tree directly.
    const auto local = gfx::shaderDirectory().parent_path() / "assets";
    if (std::filesystem::exists(local / "textures")) return local;
#ifdef SPACE_SOURCE_ASSET_DIR
    return std::filesystem::path(SPACE_SOURCE_ASSET_DIR);
#else
    return local;
#endif
}

bool loadGaiaCatalog(const std::filesystem::path& path, StarCatalog& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    char magic[4];
    uint32_t version = 0, count = 0, reserved = 0;
    file.read(magic, 4);
    file.read(reinterpret_cast<char*>(&version), 4);
    file.read(reinterpret_cast<char*>(&count), 4);
    file.read(reinterpret_cast<char*>(&reserved), 4);
    if (!file || std::memcmp(magic, "STAR", 4) != 0 || version != 1) {
        LOG_WARN("{} is not a valid star catalogue", path.string());
        return false;
    }

    out.name = "Star catalogue (Gaia DR3)";
    out.stars.resize(count);
    file.read(reinterpret_cast<char*>(out.stars.data()), (std::streamsize)(count * sizeof(StarGpu)));
    if (!file) {
        LOG_WARN("{} is truncated", path.string());
        out.stars.clear();
        return false;
    }

    // Luminosities are in solar units and positions in parsecs, so flux = L / d^2 is tiny compared
    // with the procedural galaxy; scale it up so the nearby sky reads like a dark-site naked-eye view.
    out.defaultBrightness = 250.f;
    out.defaultSpeed = 4.0; // parsecs per second
    out.unitsPerParsec = 1.0;
    LOG_INFO("Loaded {} Gaia stars from {}", count, path.string());
    return true;
}

StarCatalog makeProceduralCatalog(const StarField::Params& params) {
    StarField field;
    field.generate(params);
    StarCatalog cat;
    cat.name = "Procedural galaxy";
    cat.stars = field.stars();
    cat.defaultBrightness = 1.f;
    cat.defaultSpeed = 1.0;
    return cat;
}

} // namespace space
