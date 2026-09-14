#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace space {

class Camera;
class SolarSystem;
class CraftCatalog;

// Cinematic autopilot: flies eased paths between bodies and lingers in a slow orbit around each.
class Tour {
public:
    // A stop is a body (craft = -1) or a spacecraft (body = its parent or -1).
    struct Stop {
        int body = -1;
        int craft = -1;
    };
    void setStops(std::vector<Stop> stops) { m_stops = std::move(stops); }
    void setCrafts(CraftCatalog* crafts) { m_crafts = crafts; }
    // True for the whole leg (approach and visit) of a spacecraft stop, so the clock can slow early.
    bool atCraft() const { return m_active && m_targetCraft >= 0; }
    bool inFlight() const { return m_active && m_phase == Phase::Flight; }
    void start(const SolarSystem& solar, const Camera& camera);
    void stop(const char* reason = "");
    bool active() const { return m_active; }

    // Advances the tour and writes the camera. Call after the solar system has been updated this frame.
    void update(const SolarSystem& solar, Camera& camera, double dt, float speedScale);

    std::string status() const;

    // While flying to a landing site in the dark, the tour runs the clock forward to local morning (a
    // time-lapse of the terminator sweeping in). Returns true and the Julian date to use this frame.
    bool clockOverride(double& jd) const;

    float orbitSeconds = 45.f;   // time spent circling each body
    float orbitRadii = 3.6f;     // distance from the body centre, in body radii
    float minFlightSeconds = 14.f, maxFlightSeconds = 40.f;

private:
    enum class Phase { Flight, Orbit };
    void beginFlight(const SolarSystem& solar, const Camera& camera);
    glm::dvec3 orbitOffset(const SolarSystem& solar, int body, double angle) const;

    std::vector<Stop> m_stops;
    CraftCatalog* m_crafts = nullptr;
    int m_targetCraft = -1;
    size_t m_next = 0;
    bool m_active = false;
    Phase m_phase = Phase::Flight;
    double m_t = 0.0, m_duration = 1.0;
    int m_target = -1;
    // Flight start: camera offset relative to the departure body (so departure keeps moving with it).
    int m_fromBody = -1;
    int m_fromCraft = -1; // departing from beside a spacecraft: ride along with it, not with its planet
    glm::dvec3 m_fromOffset{0.0};
    glm::dvec3 departurePoint(const SolarSystem& solar) const;
    glm::quat m_fromOrient{1.f, 0.f, 0.f, 0.f};
    double m_orbitAngle = 0.0;
    bool m_clockActive = false;
    double m_clockFrom = 0.0, m_clockTo = 0.0;
};

} // namespace space
