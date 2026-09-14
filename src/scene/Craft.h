#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace space {

class SolarSystem;

// GPU instance record (mirrors shaders/craft.vert).
struct CraftGpu {
    glm::mat4 model;  // camera-relative, includes scale
    glm::vec4 sunDir; // xyz world direction to the Sun, w irradiance
    glm::vec4 tint;   // rgb multiplier, w ground albedo under a lander (0 in space)
    glm::vec4 up;     // xyz away from the parent body, w 1 near a body
    glm::vec4 extra;  // x size (world units), y sunlit fraction, z metres per model unit, w glint allowed
    glm::vec4 crop;   // x model-space Y cutoff (1e9 = none)
};

enum class CraftPlacement { Surface, Orbit, Lagrange, Heliocentric, HeliocentricEllipse };

struct Craft {
    std::string name;
    std::string model; // file name in assets/models
    int modelIndex = -1;
    std::string modelFallback; // used when `model` is not on disk (the older NASA GLB)
    float modelYawDeg = 0.f;   // extra rotation about model +Y, for sources with a different forward axis
    float modelPitchDeg = 0.f; // then about model +X
    float cropAboveY = 1e9f;   // model-space Y above which the mesh is not drawn (stages that left)
    float sizeMeters = 10.f; // largest dimension after scaling
    CraftPlacement placement = CraftPlacement::Surface;
    int parent = -1;   // body index

    // Surface
    double latDeg = 0.0, lonDeg = 0.0, headingDeg = 0.0, altitudeKm = 0.0;
    // Orbit (circular, about the parent's spin axis frame)
    double orbitRadiusKm = 0.0, periodMinutes = 90.0, inclinationDeg = 0.0, raanDeg = 0.0, phaseDeg = 0.0;
    // Keep a polar orbit's plane turned toward the Sun (noon-midnight), so every orbit crosses the day side.
    // The catalogue's orbital elements are illustrative, so this trades the plane's slow drift for good views.
    bool sunFacingPlane = false;
    // Lagrange: along the Sun-parent line, +1 = beyond the parent (L2), -1 = sunward (L1)
    double lagrangeSign = 1.0, lagrangeKm = 1.5e6;
    // Heliocentric (escaping probes): ICRS RA/Dec in degrees, distance in AU at an epoch, AU per year
    double raDeg = 0.0, decDeg = 0.0, distanceAuAtEpoch = 100.0, epochJd = 2460676.5, auPerYear = 3.0;
    // Heliocentric ellipse (ecliptic): a (AU), e, inclination, period days, phase
    double aAu = 0.4, ecc = 0.8, periodDays = 88.0;

    std::string blurb; // one line for the UI
    bool listed = true; // false: scenery that belongs to another entry (e.g. the flag beside a lander)

    // Filled by update():
    glm::dvec3 position{0.0};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
};

class CraftCatalog {
public:
    // Builds the roster: landing sites, orbiters, Lagrange-point observatories, escaping probes.
    void build(const SolarSystem& solar);
    void update(const SolarSystem& solar, double jd);

    const std::vector<Craft>& crafts() const { return m_crafts; }
    std::vector<Craft>& crafts() { return m_crafts; }
    int find(const std::string& name) const;

    // GPU instances relative to the camera; returns per-craft model indices in the same order.
    void buildGpuList(const SolarSystem& solar, const glm::dvec3& cameraPos, std::vector<CraftGpu>& out,
                      std::vector<float>& modelExtents) const;

    // A pleasant viewpoint a few sizes away, sunlit side, looking at the craft with its parent behind.
    void viewpoint(const SolarSystem& solar, int index, double distanceSizes, glm::dvec3& outPos,
                   glm::quat& outOrient) const;
    // How far to hold the camera, in sizes: big stations are framed close so they fill the view.
    double viewDistanceSizes(int index) const;
    // Orbital frame of an orbiter: along-track (velocity), radial up, from the current placement.
    void orbitFrame(const SolarSystem& solar, int index, glm::dvec3& alongTrack, glm::dvec3& up) const;
    // "Up" for a camera near this craft: away from the parent for landers and orbiters, model up otherwise.
    glm::dvec3 skyUp(const SolarSystem& solar, int index) const;

    // Model native extents (set after models load) so instances scale to sizeMeters, and the lowest
    // native Y so landers stand on their feet instead of their origin.
    std::vector<float> modelNativeExtent;
    std::vector<float> modelNativeMinY;

    // A surface craft is best seen in low morning sun (long shadows, like the Apollo landings). Returns
    // the first Julian date at or after jd when the Sun stands at a good elevation over it, or jd itself
    // when it already does (or the craft is not on a surface).
    double daylightJulianDate(const SolarSystem& solar, int index, double jd) const;
    // Orbiters: the catalogue's orbital phases are illustrative, so instead of moving the clock (which makes
    // a fast orbiter lap its planet mid-flight), slide the phase so the craft is over a well-lit stretch.
    void rephaseForDaylight(const SolarSystem& solar, int index);
    static double preferredSunElevationDeg(const std::string& bodyName);

private:
    std::vector<Craft> m_crafts;
};

} // namespace space
