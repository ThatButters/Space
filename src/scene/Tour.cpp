#include "scene/Tour.h"
#include "scene/Camera.h"
#include "scene/Craft.h"
#include "scene/SolarSystem.h"
#include "core/Log.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace space {

namespace {
double smooth(double x) { // smootherstep
    x = std::clamp(x, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}
// Constant speed with short eased ends (12 % each): no lurch at either end, no long crawl.
double glide(double s) {
    s = std::clamp(s, 0.0, 1.0);
    const double e = 0.12, total = 1.0 - e; // quadratic ease in and out, constant speed between
    if (s < e) return (0.5 * s * s / e) / total;
    if (s > 1.0 - e) {
        const double r = 1.0 - s;
        return 1.0 - (0.5 * r * r / e) / total;
    }
    return (s - e * 0.5) / total;
}
} // namespace

void Tour::stop(const char* reason) {
    if (m_active) LOG_INFO("Tour stopped{}{}", reason[0] ? ": " : "", reason);
    m_active = false;
}

void Tour::start(const SolarSystem& solar, const Camera& camera, size_t firstStop) {
    if (m_stops.empty()) return;
    LOG_INFO("Tour started");
    (void)camera;
    m_active = true;
    m_next = firstStop % m_stops.size();
    m_phase = Phase::FadeOut;
    m_t = fadeOutSeconds; // no picture to fade from: cut straight in
    (void)solar;
}

void Tour::startWithIntro(const SolarSystem& solar, int earth) {
    if (m_stops.empty() || earth < 0) return;
    LOG_INFO("Tour started (intro)");
    (void)solar;
    m_active = true;
    m_next = 0;
    m_phase = Phase::Intro;
    m_target = earth;
    m_targetCraft = -1;
    m_mode = 0;
    m_t = 0.0;
}

float Tour::fade() const {
    if (!m_active) return 0.f;
    if (m_phase == Phase::FadeOut) return (float)std::clamp(m_t / fadeOutSeconds, 0.0, 1.0);
    if (m_phase == Phase::Approach) return 1.f - (float)std::clamp(m_t / fadeInSeconds, 0.0, 1.0);
    return 0.f;
}

bool Tour::takeClockJump(double& jd) {
    if (!m_clockJump) return false;
    m_clockJump = false;
    jd = m_clockJumpJd;
    return true;
}

glm::dvec3 Tour::orbitOffset(const SolarSystem& solar, int body, double angle) const {
    const Body& b = solar.body(body);
    if (m_mode == 1 && b.ringOuterKm > 0.f) {
        // Ring pass: skim just above the ring plane, inside the bright B ring, drifting around the planet.
        const double rr = b.ringOuterKm / kKmPerParsec * 0.78;
        const glm::dvec3 axisR = glm::normalize(glm::dvec3(b.rotation * glm::vec3(0.f, 1.f, 0.f)));
        glm::dvec3 toSunR = glm::normalize(solar.sunPosition() - b.position);
        glm::dvec3 ur = glm::normalize(toSunR - axisR * glm::dot(toSunR, axisR));
        const glm::dvec3 vr = glm::cross(axisR, ur);
        const double side = glm::dot(toSunR, axisR) >= 0.0 ? 1.0 : -1.0; // the lit face of the rings
        return (ur * std::cos(angle) + vr * std::sin(angle)) * rr + axisR * (side * rr * 0.028);
    }
    const double r = b.radiusKm / kKmPerParsec * orbitRadii;
    // Orbit in the plane perpendicular to the body's spin axis, tilted 18 degrees so the poles show.
    glm::dvec3 axis = glm::normalize(glm::dvec3(b.rotation * glm::vec3(0.f, 1.f, 0.f)));
    glm::dvec3 toSun = body == 0 ? glm::dvec3(1, 0, 0) : glm::normalize(solar.sunPosition() - b.position);
    glm::dvec3 u = toSun - axis * glm::dot(toSun, axis);
    if (glm::length(u) < 1e-6) u = std::abs(axis.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    u = glm::normalize(u);
    glm::dvec3 v = glm::cross(axis, u);
    return (u * std::cos(angle) + v * std::sin(angle)) * (r * std::cos(0.31)) + axis * (r * std::sin(0.31));
}

glm::dvec3 Tour::targetPosition(const SolarSystem& solar) const {
    if (m_targetCraft >= 0 && m_crafts) return m_crafts->crafts()[m_targetCraft].position;
    return solar.body(m_target).position;
}

void Tour::visitPose(const SolarSystem& solar, glm::dvec3& pos, glm::quat& orient, glm::dvec3& up) const {
    if (m_targetCraft >= 0 && m_crafts) {
        m_crafts->viewpoint(solar, m_targetCraft, m_crafts->viewDistanceSizes(m_targetCraft), pos, orient);
        up = m_crafts->skyUp(solar, m_targetCraft);
        return;
    }
    const Body& b = solar.body(m_target);
    pos = b.position + orbitOffset(solar, m_target, m_orbitAngle);
    up = glm::normalize(glm::dvec3(b.rotation * glm::vec3(0.f, 1.f, 0.f)));
    glm::vec3 look = glm::normalize(glm::vec3(b.position - pos));
    if (m_mode == 1) {
        const glm::dvec3 off = pos - b.position;
        const glm::dvec3 ahead = glm::normalize(glm::cross(up, off));
        look = glm::normalize(glm::vec3(ahead * 0.8 - glm::normalize(off) * 0.45 - up * 0.08));
    }
    orient = glm::quatLookAt(look, glm::vec3(up));
}

void Tour::beginCut() {
    m_phase = Phase::FadeOut;
    m_t = 0.0;
}

void Tour::beginApproach(const SolarSystem& solar) {
    const Stop stop = m_stops[m_next % m_stops.size()];
    m_next++;
    m_targetCraft = stop.craft;
    m_mode = stop.mode;
    m_target = stop.body >= 0 ? stop.body : 0;
    m_orbitAngle = 0.0;
    m_clockJump = false;

    if (m_targetCraft >= 0 && m_crafts) {
        const Craft& c = m_crafts->crafts()[m_targetCraft];
        if (c.placement == CraftPlacement::Orbit && !c.tle) {
            m_crafts->rephaseForDaylight(solar, m_targetCraft);
        } else if (!c.keepClock) {
            // Real orbits are left alone; instead the clock moves (in the black) to the next day-side pass.
            const double now = solar.julianDate();
            const double lit = m_crafts->daylightJulianDate(solar, m_targetCraft, now);
            if (lit > now + 1e-6) {
                m_clockJump = true; // applied while the picture is black
                m_clockJumpJd = lit;
            }
        }
    } else {
        // Arrive at the sunlit face, on the side nearest the Sun-ward hemisphere.
        const Body& tb = solar.body(m_target);
        const glm::dvec3 toSun = m_target == 0 ? glm::dvec3(1, 0, 0) : glm::normalize(solar.sunPosition() - tb.position);
        double bestScore = -1e9;
        for (int k = 0; k < 72; ++k) {
            const double ang = glm::two_pi<double>() * k / 72.0;
            const double score = glm::dot(glm::normalize(orbitOffset(solar, m_target, ang)), toSun);
            if (score > bestScore) {
                bestScore = score;
                m_orbitAngle = ang;
            }
        }
    }

    // The approach is a straight line, in the target's frame, that ends at the visit pose and starts
    // farther back along the same line of sight, so the target sits centred and grows the whole way.
    glm::dvec3 pos, up;
    glm::quat orient;
    visitPose(solar, pos, orient, up);
    const glm::dvec3 targetPos = targetPosition(solar);
    m_approachTo = pos - targetPos;
    const glm::dvec3 lineOfSight = glm::normalize(glm::dvec3(glm::vec3(orient * glm::vec3(0.f, 0.f, -1.f))));
    const double dist = glm::length(m_approachTo);
    m_approachFrom = m_approachTo - lineOfSight * (dist * (m_targetCraft >= 0 ? 4.0 : 1.6));
    m_legOrient = orient;
    m_legUp = up;
    m_phase = Phase::Approach;
    m_t = 0.0;
    LOG_INFO("Tour: cut to {}", m_targetCraft >= 0 && m_crafts ? m_crafts->crafts()[m_targetCraft].name
                                                                 : solar.body(m_target).name + (m_mode == 1 ? "'s rings" : ""));
}

void Tour::update(const SolarSystem& solar, Camera& camera, double dt, float speedScale) {
    if (!m_active) return;
    dt *= std::max(speedScale, 0.f);

    if (m_phase == Phase::Intro) {
        // Drifting east at 650 km from over Texas to the Florida coast (the clock is set to dawn at the
        // Cape), looking ahead and down at the sunrise. The heading is fixed for the whole pass.
        m_t += dt;
        const Body& e = solar.body(m_target);
        const double R = e.radiusKm / kKmPerParsec;
        const double k = glide(m_t / introSeconds);
        const double lat = 30.5 - 1.5 * k, lon = -96.0 + 14.0 * k;
        const glm::dvec3 dir = glm::normalize(e.rotationD * SolarSystem::latLonToLocal(lat, lon));
        camera.position = e.position + dir * (R + 650.0 / kKmPerParsec);
        if (m_t <= dt * 1.5) {
            const glm::dvec3 aim = e.position + glm::normalize(e.rotationD * SolarSystem::latLonToLocal(29.0, -80.0)) * R;
            camera.orientation = glm::quatLookAt(glm::normalize(glm::vec3(aim - camera.position)), glm::vec3(dir));
        }
        camera.speed = R * 0.02;
        if (m_t >= introSeconds) beginCut();
        return;
    }

    if (m_phase == Phase::FadeOut) {
        m_t += dt; // hold still while the picture goes dark
        if (m_t >= fadeOutSeconds) beginApproach(solar);
        return;
    }

    const glm::dvec3 targetPos = targetPosition(solar);
    if (m_phase == Phase::Approach) {
        m_t += dt;
        const double s = glide(m_t / approachSeconds);
        camera.position = targetPos + glm::mix(m_approachFrom, m_approachTo, s);
        camera.orientation = m_legOrient;
        camera.speed = glm::length(m_approachTo - m_approachFrom) / approachSeconds;
        if (m_t >= approachSeconds) {
            m_phase = Phase::Visit;
            m_t = 0.0;
            m_orbitAngle = 0.0;
        }
        return;
    }

    // Visit: a slow drift. Orientation follows the target at a gentle rate, roll locked to the leg's up.
    m_t += dt;
    double visitSeconds = orbitSeconds;
    if (m_targetCraft >= 0 && m_crafts) {
        const Craft& craft = m_crafts->crafts()[m_targetCraft];
        visitSeconds = orbitSeconds * 0.6;
        glm::dvec3 vpPos;
        glm::quat vpOrient;
        m_crafts->viewpoint(solar, m_targetCraft, m_crafts->viewDistanceSizes(m_targetCraft), vpPos, vpOrient);
        const double ang = smooth(m_t / visitSeconds) * 0.6; // about a third of a turn over the visit
        const glm::dvec3 off = vpPos - craft.position;
        camera.position = craft.position + glm::dvec3(glm::angleAxis((float)ang, glm::vec3(m_legUp)) * glm::vec3(off));
        camera.speed = craft.sizeMeters / (kKmPerParsec * 1000.0) * 0.3;
    } else {
        const Body& target = solar.body(m_target);
        const double drift = m_mode == 1 ? 0.35 : 1.0;
        m_orbitAngle += dt * glm::two_pi<double>() / (orbitSeconds * 2.6) * drift;
        camera.position = target.position + orbitOffset(solar, m_target, m_orbitAngle);
        camera.speed = target.radiusKm / kKmPerParsec * 0.5;
    }
    glm::dvec3 pos, up;
    glm::quat want;
    visitPose(solar, pos, want, up);
    (void)pos;
    if (m_mode != 1) want = glm::quatLookAt(glm::normalize(glm::vec3(targetPos - camera.position)), glm::vec3(m_legUp));
    // At most a few degrees per second of turning.
    const float maxStep = (float)(glm::radians(6.0) * dt);
    const float ang = glm::angle(glm::inverse(camera.orientation) * want);
    camera.orientation = ang > 1e-4f ? glm::slerp(camera.orientation, want, std::min(1.f, maxStep / ang)) : want;
    if (m_t >= visitSeconds) beginCut();
}

std::string Tour::status() const {
    if (!m_active) return "off";
    std::string n = m_targetCraft >= 0 && m_crafts ? m_crafts->crafts()[m_targetCraft].name : std::string("the next stop");
    if (m_phase == Phase::Intro) return "opening";
    if (m_phase == Phase::FadeOut) return "cutting";
    return (m_phase == Phase::Approach ? "approaching " : "visiting ") + n;
}

} // namespace space
