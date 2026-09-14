#pragma once
#include "scene/StarField.h"

#include <glm/glm.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace space {

// One line-list vertex: star position + which constellation the segment belongs to.
struct LineVertex {
    glm::vec3 position;
    float constellation; // index into Constellations::figures
};

struct ConstellationFigure {
    std::string abbr, latin, english;
    glm::vec3 direction{0.f}; // unit vector from the Sun toward the figure's centroid
    uint32_t firstVertex = 0, vertexCount = 0;
};

// The 88 IAU constellations as star-to-star line figures resolved against the star catalogue.
class Constellations {
public:
    // figuresPath: assets/constellations.txt; hipIndexPath: assets/gaia/hip_index.bin
    bool load(const std::filesystem::path& figuresPath, const std::filesystem::path& hipIndexPath,
              const std::vector<StarGpu>& stars);

    const std::vector<ConstellationFigure>& figures() const { return m_figures; }
    const std::vector<LineVertex>& vertices() const { return m_vertices; }
    bool empty() const { return m_vertices.empty(); }

private:
    std::vector<ConstellationFigure> m_figures;
    std::vector<LineVertex> m_vertices;
};

} // namespace space
