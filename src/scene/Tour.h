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
        int mode = 0; // 0 orbit the body, 1 fly through its rings
    };
    int targetMode() const { return m_active ? m_mode : 0; }
    void setStops(std::vector<Stop> stops) { m_stops = std::move(stops); }
    void setCrafts(CraftCatalog* crafts) { m_crafts = crafts; }
    // True for the whole leg (approach and visit) of a spacecraft stop, so the clock can slow early.
    bool atCraft() const { return m_active && m_targetCraft >= 0; }
    bool inFlight() const { return m_active && m_phase == Phase::Flight; }
    void start(const SolarSystem& solar, const Camera& camera, size_t firstStop = 0);
    // The opening: a slow pass over Earth's dawn terminator at 650 km, then the stops in order.
    void startWithIntro(const SolarSystem& solar, int earth);
    bool introActive() const { return m_active && m_phase == Phase::Intro; }
    double introProgress() const { return m_phase == Phase::Intro ? m_t / introSeconds : 1.0; }
    bool visiting() const { return m_active && m_phase == Phase::Orbit; }
    int targetBody() const { return m_active ? m_target : -1; }
    int targetCraft() const { return m_active ? m_targetCraft : -1; }
    // Seconds since the current leg began, so captions can fade in.
    double legSeconds() const { return m_t; }
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
    float introSeconds = 30.f;

private:
    enum class Phase { Flight, Orbit, Intro };
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
    glm::dvec3 departurePoint(const SolarSystem& solar, double s) const;
    glm::dvec3 m_fromBodyOffset{0.0}; // the start point fixed in the departure body's frame
    glm::quat m_fromOrient{1.f, 0.f, 0.f, 0.f};
    double m_orbitAngle = 0.0;
    int m_mode = 0;
    bool m_clockActive = false;
    double m_clockFrom = 0.0, m_clockTo = 0.0;
};

} // namespace space
