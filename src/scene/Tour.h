#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace space {

class Camera;
class SolarSystem;
class CraftCatalog;

// Cinematic autopilot, built for comfort (VR rules): the camera never rotates on its own. Every leg is a
// cut: fade to black, reappear at a vantage point already aimed at the next stop, drift straight in at a
// steady speed, then hold a slow drift while visiting. Roll is locked to one up vector per leg.
class Tour {
public:
    // A stop is a body (craft = -1) or a spacecraft (body = its parent or -1).
    struct Stop {
        int body = -1;
        int craft = -1;
        int mode = 0; // 0 orbit the body, 1 fly through its rings
    };
    void setStops(std::vector<Stop> stops) { m_stops = std::move(stops); }
    void setCrafts(CraftCatalog* crafts) { m_crafts = crafts; }
    // The body the opening flies over; the tour returns to the opening after its last stop.
    void setIntroBody(int body) { m_introBody = body; }
    bool atCraft() const { return m_active && m_targetCraft >= 0; }
    bool inFlight() const { return m_active && (m_phase == Phase::Approach || m_phase == Phase::FadeOut); }
    bool visiting() const { return m_active && m_phase == Phase::Visit; }
    bool introActive() const { return m_active && m_phase == Phase::Intro; }
    double introProgress() const { return m_phase == Phase::Intro ? m_t / introSeconds : 1.0; }
    int targetBody() const { return m_active ? m_target : -1; }
    int targetCraft() const { return m_active ? m_targetCraft : -1; }
    int targetMode() const { return m_active ? m_mode : 0; }
    double legSeconds() const { return m_t; }
    // Black over the picture, 0..1: the cut between stops.
    float fade() const;

    void start(const SolarSystem& solar, const Camera& camera, size_t firstStop = 0);
    // The opening: a slow pass over the Gulf coast at dawn, then the stops in order.
    void startWithIntro(const SolarSystem& solar, int earth);
    void stop(const char* reason = "");
    // Cut to the next (+1) or previous (-1) stop right now.
    void skip(int delta);
    bool active() const { return m_active; }

    size_t stopCount() const { return m_stops.size(); }
    const Stop& stopAt(size_t i) const { return m_stops[i]; }
    // The stop being shown or flown to (-1 during the opening).
    int currentStop() const;
    // Cut to a stop by index (the tour strip).
    void jumpTo(size_t index);
    // Space: stay at this stop until released (the camera holds still, the clock keeps going).
    void setHold(bool hold) { m_hold = hold; }
    bool held() const { return m_hold && m_active && m_phase == Phase::Visit; }
    // True once after the last stop has cut back to the opening: the app resets the clock to dawn.
    bool takeIntroRestart();

    // Advances the tour and writes the camera. Call after the solar system has been updated this frame.
    void update(const SolarSystem& solar, Camera& camera, double dt, float speedScale);

    // A landing site visited in the dark gets its clock moved to local morning, during the black of the
    // cut. Returns true once with the Julian date to jump to.
    bool takeClockJump(double& jd);
    // Once after the tour stops: the camera's own lens to put back (a lunar site widens it).
    bool takeLensRestore(float& fovY);

    std::string status() const;

    float orbitSeconds = 45.f;     // time spent at each body
    float orbitRadii = 3.6f;       // distance from the body centre, in body radii
    float approachSeconds = 11.f;  // the straight drift in
    float fadeOutSeconds = 0.9f, fadeInSeconds = 1.4f;
    float introSeconds = 30.f;

private:
    enum class Phase { Intro, FadeOut, Approach, Visit };
    void beginCut();
    void beginApproach(const SolarSystem& solar);
    void layOutApproach(const SolarSystem& solar);
    glm::dvec3 orbitOffset(const SolarSystem& solar, int body, double angle) const;
    glm::dvec3 targetPosition(const SolarSystem& solar) const;
    // Where the visit begins (position, orientation) and the leg's locked up vector.
    void visitPose(const SolarSystem& solar, glm::dvec3& pos, glm::quat& orient, glm::dvec3& up) const;

    std::vector<Stop> m_stops;
    CraftCatalog* m_crafts = nullptr;
    int m_targetCraft = -1;
    size_t m_next = 0;
    bool m_active = false;
    Phase m_phase = Phase::Approach;
    double m_t = 0.0;
    int m_target = -1;
    int m_mode = 0;
    double m_orbitAngle = 0.0;
    // Approach: a straight line in the target's frame (offsets from the target), fixed orientation.
    glm::dvec3 m_approachFrom{0.0}, m_approachTo{0.0};
    glm::quat m_legOrient{1.f, 0.f, 0.f, 0.f};
    glm::dvec3 m_legUp{0.0, 1.0, 0.0};
    bool m_stopChosen = false; // FadeOut: the next stop is picked (and any clock jump requested)
    bool m_hold = false;
    bool m_skipped = false;       // the next stop was chosen by a key or a click, not by running out
    bool m_introRestart = false;  // pending: tell the app to reset the clock for the opening
    bool m_introFromCut = false;  // the opening is being entered from a cut (fade in from black)
    int m_introBody = -1;
    float m_defaultFov = 0.f, m_fovWanted = 0.f;  // the camera's own lens; lunar landing sites get a wider one (Earth in the sky)
    bool m_clockJump = false;
    double m_clockJumpJd = 0.0;
    bool m_lensRestore = false;
    // A stop that moves the clock (a lunar morning, Parker at perihelion) is a trip away from the tour's own
    // time: the next cut brings the clock home again, plus the time spent away.
    bool m_away = false;
    double m_homeJd = 0.0, m_awayJd = 0.0;
    bool m_pendingAway = false; // the trip a requested clock jump starts (or ends), committed when it is taken
    double m_pendingHomeJd = 0.0;
    void resetLegState();
};

} // namespace space
