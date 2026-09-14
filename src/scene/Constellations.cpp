#include "scene/Constellations.h"
#include "core/Log.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace space {

bool Constellations::load(const std::filesystem::path& figuresPath, const std::filesystem::path& hipIndexPath,
                          const std::vector<StarGpu>& stars) {
    m_figures.clear();
    m_vertices.clear();

    // Hipparcos id -> catalogue index.
    std::unordered_map<uint32_t, uint32_t> hip;
    {
        std::ifstream f(hipIndexPath, std::ios::binary);
        char magic[4];
        uint32_t version = 0, count = 0;
        if (!f.read(magic, 4) || std::memcmp(magic, "HIPX", 4) != 0) {
            LOG_WARN("No Hipparcos index at {} (re-run scripts/convert_athyg.py)", hipIndexPath.string());
            return false;
        }
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&count), 4);
        hip.reserve(count);
        for (uint32_t i = 0; i < count && f; ++i) {
            uint32_t id = 0, idx = 0;
            f.read(reinterpret_cast<char*>(&id), 4);
            f.read(reinterpret_cast<char*>(&idx), 4);
            if (idx < stars.size()) hip[id] = idx;
        }
    }

    std::ifstream file(figuresPath);
    if (!file) {
        LOG_WARN("No constellation figures at {} (run scripts/fetch_constellations.py)", figuresPath.string());
        return false;
    }

    std::string line;
    size_t missing = 0;
    while (std::getline(file, line)) {
        if (line.size() < 3) continue;
        if (line[0] == 'C') {
            ConstellationFigure fig;
            std::istringstream ss(line.substr(2));
            ss >> fig.abbr;
            std::string rest;
            std::getline(ss, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            auto bar = rest.find('|');
            fig.latin = rest.substr(0, bar);
            fig.english = bar == std::string::npos ? fig.latin : rest.substr(bar + 1);
            fig.firstVertex = (uint32_t)m_vertices.size();
            m_figures.push_back(fig);
        } else if (line[0] == 'L' && !m_figures.empty()) {
            std::istringstream ss(line.substr(2));
            uint32_t id = 0;
            std::vector<glm::vec3> pts;
            while (ss >> id) {
                auto it = hip.find(id);
                if (it == hip.end()) {
                    ++missing;
                    pts.clear(); // break the polyline at an unresolved star
                    continue;
                }
                glm::vec3 p = glm::vec3(stars[it->second].posRadiance);
                if (!pts.empty()) {
                    m_vertices.push_back({pts.back(), (float)(m_figures.size() - 1)});
                    m_vertices.push_back({p, (float)(m_figures.size() - 1)});
                }
                pts.push_back(p);
            }
            m_figures.back().vertexCount = (uint32_t)m_vertices.size() - m_figures.back().firstVertex;
        }
    }

    // Centroid directions as seen from the Sun (figures are defined for Earth's sky).
    for (auto& fig : m_figures) {
        glm::vec3 sum{0.f};
        for (uint32_t i = fig.firstVertex; i < fig.firstVertex + fig.vertexCount; ++i)
            sum += glm::normalize(m_vertices[i].position);
        fig.direction = fig.vertexCount ? glm::normalize(sum) : glm::vec3(1.f, 0.f, 0.f);
    }

    LOG_INFO("Constellations: {} figures, {} segments ({} stars unresolved)", m_figures.size(),
             m_vertices.size() / 2, missing);
    return !m_vertices.empty();
}

} // namespace space
