#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace space {

// GPU layout of one rendered body (80 bytes). Mirrors shaders/planet.vert.
struct BodyGpu {
    glm::vec4 posRadius; // xyz camera-relative position (world units), w radius
    glm::vec4 rotation;  // quaternion (x,y,z,w) body -> world
    glm::vec4 color;     // base albedo (or emission for the Sun)
    glm::vec4 params;    // x type, y seed, z atmosphere strength / ring inner ratio, w emissive
    glm::ivec4 tex;      // texture array indices: x day/albedo, y night, z clouds, w ring alpha (-1 = none)
    glm::ivec4 tiles;    // tiled day map: x first texture index (-1 = none), y columns, z rows, w unused
    glm::ivec4 relief;   // x normal map (BC5 east/north), y height map (BC4), -1 = none
    glm::vec4 reliefParams; // x height min (km), y height range (km), z normal strength, w detail kind (0 none, 1 lunar, 2 martian)
    glm::ivec4 patchTex;    // active local terrain patch: x albedo, y normal (east/north), z height (-1 = none)
    glm::vec4 patchParams;  // x height min (m), y height range (m), z metres per texel, w 1 when a patch is active
};

// A local high-resolution terrain patch (e.g. an LROC NAC orthophoto + DTM around a landing site), resampled
// to a regular latitude/longitude grid: row 0 at the north edge, column 0 at the west edge.
struct SurfacePatch {
    std::string name;
    std::string dir;              // under assets/textures: patch_albedo.dds, patch_normal.dds, patch_height.dds, patch.txt
    double lonMinDeg = 0.0, latMinDeg = 0.0, lonSpanDeg = 0.0, latSpanDeg = 0.0;
    float heightMinM = 0.f, heightRangeM = 0.f, metresPerTexel = 1.f;
    int albedoIndex = -1, normalIndex = -1, heightIndex = -1;
};

enum class BodyType : int { Sun = 0, Rocky = 1, Earth = 2, GasGiant = 3, IceGiant = 4, Ring = 5 };

// Constants (world unit = parsec).
constexpr double kKmPerParsec = 3.0856775814913673e13;
constexpr double kKmPerAU = 1.495978707e8;
constexpr double kAuPerParsec = kKmPerParsec / kKmPerAU;

struct Body {
    std::string name;
    BodyType type;
    int parent = -1;          // index of the body this one orbits (-1 = Sun / barycentre)
    double radiusKm = 0.0;
    double orbitRadiusKm = 0.0; // circular orbit radius (moons, and planets without JPL elements)
    double orbitPeriodDays = 0.0;
    double orbitPhase = 0.0;  // radians at J2000 (moons: arbitrary but fixed)
    double rotationPeriodHours = 24.0;
    double axialTiltDeg = 0.0;
    glm::vec3 color{0.5f};
    float seed = 0.f;
    float atmosphere = 0.f; // rim scattering strength
    int jplIndex = -1;      // row in the JPL approximate-elements table, or -1 for a circular orbit
    float ringInner = 0.f;  // >0: has a ring with this inner/outer ratio
    float ringOuterKm = 0.f;

    // Texture file names (assets/textures), empty = procedural surface. Resolved to indices by the app.
    std::string texDay, texNight, texClouds, texRing;
    int texDayIndex = -1, texNightIndex = -1, texCloudsIndex = -1, texRingIndex = -1;
    // Optional high-resolution day map split into a grid of pre-baked DDS tiles (row-major from the
    // north-west), used when every tile is present. Pattern gets the tile id (e.g. "A1") via {}.
    std::string texDayTiles;
    std::vector<std::string> tileIds;
    int tileCols = 0, tileRows = 0, texTileBase = -1;
    std::string texNightHires; // optional pre-baked DDS that replaces texNight when present
    std::string texDayHires;   // optional pre-baked DDS that replaces texDay when present
    std::string texNormal, texHeight; // optional relief maps (DDS) and the height range sidecar
    std::string heightRangeFile;
    int texNormalIndex = -1, texHeightIndex = -1;
    int texCloudsLiveIndex = -1; // today's cloud cover (R opacity, G valid), fetched at launch
    float heightMinKm = 0.f, heightRangeKm = 0.f;
    float detailKind = 0.f; // close-range procedural surface detail (0 none, 1 lunar regolith, 2 martian)
    std::vector<SurfacePatch> patches; // local high-resolution terrain, loaded when present
    int activePatch = -1;              // chosen per frame near the camera

    // Filled by update():
    glm::dvec3 position{0.0}; // world units, Sun at origin (engine galactic frame)
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::dquat rotationD{1.0, 0.0, 0.0, 0.0}; // same, double precision (surface placement needs it)
};

// Our solar system with real planet positions from the JPL approximate Keplerian elements
// (Standish, valid 1800-2050), circular orbits for the moons, and slow axial rotation.
class SolarSystem {
public:
    SolarSystem();

    // jd: Julian date to evaluate. Planets use JPL elements, moons use mean motion from J2000.
    void update(double jd);

    const std::vector<Body>& bodies() const { return m_bodies; }
    std::vector<Body>& bodies() { return m_bodies; }
    const Body& body(int i) const { return m_bodies[i]; }
    int find(const std::string& name) const;

    // Builds the per-frame GPU list relative to the camera. Planets/moons/sun first, then rings.
    // Returns the number of sphere bodies; rings follow.
    uint32_t buildGpuList(const glm::dvec3& cameraPos, std::vector<BodyGpu>& out) const;

    glm::dvec3 sunPosition() const { return m_bodies[0].position; }
    double julianDate() const { return m_jd; }
    // Direction of the vernal equinox (ICRS x axis) in engine coordinates: the reference for orbital elements.
    glm::dvec3 equinoxDirection() const { return glm::normalize(m_eclipticToEngine * glm::dvec3(1.0, 0.0, 0.0)); }

    // Body-local unit direction for a latitude / east longitude (degrees). Longitude 0 is local +x,
    // east runs toward -z, north is +y (the same convention the surface maps use).
    static glm::dvec3 latLonToLocal(double latDeg, double lonDeg);
    // Sun elevation (degrees) above the local horizon at a surface point of a body.
    double sunElevationDeg(int body, double latDeg, double lonDeg) const;

    // A pleasant viewpoint: 'distance' body radii away, looking at the body with the Sun off to one side.
    void viewpoint(int index, double distanceRadii, glm::dvec3& outPos, glm::quat& outOrient) const;

    // Julian date for the current wall-clock time.
    static double nowJulianDate();

private:
    std::vector<Body> m_bodies;
    glm::dmat3 m_eclipticToEngine{1.0};
    glm::dmat3 m_eclipticToEq{1.0};
    double m_jd = 2451545.0;
};

} // namespace space
