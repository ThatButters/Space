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

// Fraction of a straight flight covered at progress s, chosen so that the ratio of the distances to the
// place left behind (a at the start) and to the destination (b at the end) changes at a constant rate
// in log space. The departure shrinks away steadily and the destination grows steadily, instead of
// both happening in the first and last second of a linear dash.
double logTravel(double s, double a, double b, double D) {
    D = std::max(D, 1e-30);
    a = std::max(a, D * 1e-7);
    b = std::max(b, D * 1e-7);
    const double f0 = std::log(a) - std::log(b + D);
    const double f1 = std::log(a + D) - std::log(b);
    const double q = std::exp(f0 + (f1 - f0) * std::clamp(s, 0.0, 1.0));
    return std::clamp((q * (b + D) - a) / (D * (1.0 + q)), 0.0, 1.0);
}
} // namespace

void Tour::stop(const char* reason) {
    if (m_active) LOG_INFO("Tour stopped{}{}", reason[0] ? ": " : "", reason);
    m_active = false;
}

void Tour::start(const SolarSystem& solar, const Camera& camera) {
    if (m_stops.empty()) return;
    LOG_INFO("Tour started");
    m_active = true;
    m_next = 0;
    beginFlight(solar, camera);
}

glm::dvec3 Tour::orbitOffset(const SolarSystem& solar, int body, double angle) const {
    const Body& b = solar.body(body);
    const double r = b.radiusKm / kKmPerParsec * orbitRadii;
    // Orbit in the plane perpendicular to the body's spin axis, tilted 18 degrees so the poles show.
    // Angle 0 is the sunlit side, so every arrival sees the day face.
    glm::dvec3 axis = glm::normalize(glm::dvec3(b.rotation * glm::vec3(0.f, 1.f, 0.f)));
    glm::dvec3 toSun = body == 0 ? glm::dvec3(1, 0, 0) : glm::normalize(solar.sunPosition() - b.position);
    glm::dvec3 u = toSun - axis * glm::dot(toSun, axis);
    if (glm::length(u) < 1e-6) u = std::abs(axis.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    u = glm::normalize(u);
    glm::dvec3 v = glm::cross(axis, u);
    return (u * std::cos(angle) + v * std::sin(angle)) * (r * std::cos(0.31)) + axis * (r * std::sin(0.31));
}

void Tour::beginFlight(const SolarSystem& solar, const Camera& camera) {
    const Stop stop = m_stops[m_next % m_stops.size()];
    m_next++;
    const int previousCraft = m_targetCraft;
    m_targetCraft = stop.craft;
    m_target = stop.body >= 0 ? stop.body : 0;

    // Departure frame: whichever body the camera is nearest (by radii).
    m_fromBody = 0;
    double best = 1e300;
    for (size_t i = 0; i < solar.bodies().size(); ++i) {
        double d = glm::length(solar.body((int)i).position - camera.position) /
                   (solar.body((int)i).radiusKm / kKmPerParsec);
        if (d < best) {
            best = d;
            m_fromBody = (int)i;
        }
    }
    m_fromOffset = camera.position - solar.body(m_fromBody).position;
    m_fromCraft = -1;
    if (m_active && previousCraft >= 0 && m_crafts && previousCraft < (int)m_crafts->crafts().size()) {
        const Craft& pc = m_crafts->crafts()[previousCraft];
        const double sizePc = pc.sizeMeters / (kKmPerParsec * 1000.0);
        if (glm::length(camera.position - pc.position) < sizePc * 50.0) {
            m_fromCraft = previousCraft;
            m_fromOffset = camera.position - pc.position;
        }
    }
    m_fromOrient = camera.orientation;

    // Flight time grows gently with distance (log scale) so Earth->Moon is quick and Neptune is a cruise.
    glm::dvec3 targetPos = solar.body(m_target).position;
    if (m_targetCraft >= 0 && m_crafts) targetPos = m_crafts->crafts()[m_targetCraft].position;
    const double dist = glm::length(targetPos - camera.position);
    const double au = dist * kAuPerParsec;
    const double t = std::clamp(std::log10(au * 1000.0 + 1.0) / 4.5, 0.0, 1.0);
    m_duration = minFlightSeconds + (maxFlightSeconds - minFlightSeconds) * t;
    m_t = 0.0;
    m_phase = Phase::Flight;
    m_orbitAngle = 0.0;
    if (m_targetCraft < 0) {
        // Arrive on the side of the target that faces us (never behind it), leaning toward daylight.
        const Body& tb = solar.body(m_target);
        const glm::dvec3 toDeparture = glm::normalize(camera.position - tb.position);
        const glm::dvec3 toSun = m_target == 0 ? toDeparture : glm::normalize(solar.sunPosition() - tb.position);
        double bestScore = -1e9;
        for (int k = 0; k < 72; ++k) {
            const double ang = glm::two_pi<double>() * k / 72.0;
            const glm::dvec3 dirO = glm::normalize(orbitOffset(solar, m_target, ang));
            const double score = glm::dot(dirO, toDeparture) + 0.6 * glm::dot(dirO, toSun);
            if (score > bestScore) {
                bestScore = score;
                m_orbitAngle = ang;
            }
        }
    }

    LOG_INFO("Tour: flying to {} ({:.0f} s)",
             m_targetCraft >= 0 && m_crafts ? m_crafts->crafts()[m_targetCraft].name : solar.body(m_target).name, m_duration);
    m_clockActive = false;
    if (m_targetCraft >= 0 && m_crafts && m_crafts->crafts()[m_targetCraft].placement == CraftPlacement::Orbit) {
        // Fast orbiters would lap their planet during a time-lapse; place them in daylight instead.
        m_crafts->rephaseForDaylight(solar, m_targetCraft);
    } else if (m_targetCraft >= 0 && m_crafts) {
        const double now = solar.julianDate();
        const double lit = m_crafts->daylightJulianDate(solar, m_targetCraft, now);
        if (lit > now + 1e-6) {
            m_clockActive = true;
            m_clockFrom = now;
            m_clockTo = lit;
            // Give a long time-lapse a little more flight time so it reads as a sunrise, not a flicker.
            m_duration = std::max(m_duration, std::min((double)maxFlightSeconds, (double)minFlightSeconds + (lit - now) * 0.6));
        }
    }
}

glm::dvec3 Tour::departurePoint(const SolarSystem& solar) const {
    if (m_fromCraft >= 0 && m_crafts) return m_crafts->crafts()[m_fromCraft].position + m_fromOffset;
    return solar.body(m_fromBody).position + m_fromOffset;
}

bool Tour::clockOverride(double& jd) const {
    if (!m_active || !m_clockActive || m_phase != Phase::Flight) return false;
    const double k = smooth(m_t / (m_duration * 0.7));
    jd = m_clockFrom + (m_clockTo - m_clockFrom) * k;
    return true;
}

void Tour::update(const SolarSystem& solar, Camera& camera, double dt, float speedScale) {
    if (!m_active || m_target < 0) return;
    dt *= std::max(speedScale, 0.f);

    if (m_targetCraft >= 0 && m_crafts) {
        // Spacecraft stop: fly to a viewpoint a few sizes away, then hold there while it drifts.
        const Craft& craft = m_crafts->crafts()[m_targetCraft];
        glm::dvec3 vpPos;
        glm::quat vpOrient;
        m_crafts->viewpoint(solar, m_targetCraft, m_crafts->viewDistanceSizes(m_targetCraft), vpPos, vpOrient);
        if (m_phase == Phase::Flight && craft.placement == CraftPlacement::Orbit && craft.parent >= 0) {
            // Catch up with an orbiter: first fly (around the planet) to a chase point trailing a couple of
            // kilometres behind it on its own orbit, matching its motion, then close in from behind along
            // the track to the viewpoint, all in the craft's co-moving frame.
            m_t += dt;
            const double sizePc = craft.sizeMeters / (kKmPerParsec * 1000.0);
            glm::dvec3 along, radial;
            m_crafts->orbitFrame(solar, m_targetCraft, along, radial);
            const double chaseDist = std::max(sizePc * 18.0, 1.5 / kKmPerParsec);
            const glm::dvec3 chaseRel = -along * chaseDist + radial * (chaseDist * 0.12);
            const glm::dvec3 vpRel = vpPos - craft.position;
            const double split = 0.55; // share of the flight spent getting onto the orbit
            const glm::dvec3 centre = solar.body(craft.parent).position;
            if (m_t < m_duration * split) {
                const double s = smooth(m_t / (m_duration * split));
                const glm::dvec3 from = departurePoint(solar);
                const glm::dvec3 target = craft.position + chaseRel;
                const glm::dvec3 a = from - centre, b = target - centre;
                const double ra = glm::length(a), rb = glm::length(b);
                const glm::dvec3 da = a / ra, db = b / rb;
                const double ang = std::acos(std::clamp(glm::dot(da, db), -1.0, 1.0));
                glm::dvec3 dir = db;
                if (ang > 1e-6) {
                    glm::dvec3 axis = glm::cross(da, db);
                    if (glm::length(axis) < 1e-9) axis = glm::cross(da, radial);
                    dir = glm::angleAxis(ang * s, glm::normalize(axis)) * da;
                }
                camera.position = centre + dir * std::exp(std::log(ra) + (std::log(rb) - std::log(ra)) * s);
            } else {
                const double s = smooth((m_t - m_duration * split) / (m_duration * (1.0 - split)));
                camera.position = craft.position + chaseRel + (vpRel - chaseRel) * s;
            }
            const glm::dvec3 up = m_crafts->skyUp(solar, m_targetCraft);
            glm::vec3 look = glm::normalize(glm::vec3(craft.position - camera.position));
            glm::quat want = glm::quatLookAt(look, glm::vec3(up));
            const float k = (float)smooth(m_t / (m_duration * 0.4));
            camera.orientation = k < 1.f ? glm::slerp(m_fromOrient, want, k)
                                         : glm::slerp(camera.orientation, want, (float)std::min(1.0, dt * 2.0));
            camera.speed = sizePc * 0.5;
            if (m_t >= m_duration) {
                m_phase = Phase::Orbit;
                m_t = 0.0;
            }
            return;
        }
        if (m_phase == Phase::Flight) {
            m_t += dt;
            const double s = smooth(m_t / m_duration);
            const glm::dvec3 from = departurePoint(solar);
            if (craft.parent >= 0 && craft.parent == m_fromBody) {
                // Around the parent, never through it: slerp the direction, blend the radius in log space.
                const glm::dvec3 centre = solar.body(craft.parent).position;
                const glm::dvec3 a = from - centre, b = vpPos - centre;
                const double ra = glm::length(a), rb = glm::length(b);
                const glm::dvec3 da = a / ra, db = b / rb;
                const double cosang = std::clamp(glm::dot(da, db), -1.0, 1.0);
                const double ang = std::acos(cosang);
                glm::dvec3 dir;
                if (ang < 1e-6) dir = db;
                else {
                    glm::dvec3 axis = glm::cross(da, db);
                    if (glm::length(axis) < 1e-9) axis = std::abs(da.y) < 0.9 ? glm::cross(da, glm::dvec3(0, 1, 0)) : glm::cross(da, glm::dvec3(1, 0, 0));
                    dir = glm::angleAxis(ang * s, glm::normalize(axis)) * da;
                }
                const double r = std::exp(std::log(ra) + (std::log(rb) - std::log(ra)) * s);
                camera.position = centre + dir * r;
            } else {
                const double D = glm::length(vpPos - from);
                const double u = logTravel(s, glm::length(m_fromOffset), glm::length(vpPos - craft.position), D);
                camera.position = from + (vpPos - from) * u;
            }
            glm::vec3 look = glm::normalize(glm::vec3(craft.position - camera.position));
            glm::quat want = glm::quatLookAt(look, glm::vec3(m_crafts->skyUp(solar, m_targetCraft)));
            const float k = (float)smooth(m_t / (m_duration * 0.45));
            // Ease toward the target look, then keep easing so the final turn is never a snap.
            camera.orientation = k < 1.f ? glm::slerp(m_fromOrient, want, k)
                                         : glm::slerp(camera.orientation, want, (float)std::min(1.0, dt * 1.5));
            camera.speed = glm::length(vpPos - from) / m_duration;
            if (m_t >= m_duration) {
                m_phase = Phase::Orbit;
                m_t = 0.0;
            }
            return;
        }
        m_t += dt;
        // Slow drift around the craft while it moves (a quarter turn over the visit).
        const double ang = smooth(m_t / (orbitSeconds * 0.6)) * 0.8;
        const double size = craft.sizeMeters / (kKmPerParsec * 1000.0);
        glm::dvec3 up = m_crafts->skyUp(solar, m_targetCraft);
        glm::dvec3 off = vpPos - craft.position;
        glm::dvec3 rot = glm::dvec3(glm::angleAxis((float)ang, glm::vec3(up)) * glm::vec3(off));
        camera.position = craft.position + rot;
        glm::vec3 look = glm::normalize(glm::vec3(craft.position - camera.position));
        camera.orientation = glm::slerp(camera.orientation, glm::quatLookAt(look, glm::vec3(up)), (float)std::min(1.0, dt * 1.5));
        camera.speed = size * 0.3;
        if (m_t >= orbitSeconds * 0.6) beginFlight(solar, camera);
        return;
    }

    const Body& target = solar.body(m_target);
    const double targetRadius = target.radiusKm / kKmPerParsec;

    if (m_phase == Phase::Flight) {
        m_t += dt;
        const double s = smooth(m_t / m_duration);

        // Start point keeps riding with the departure body; end point is the first orbit position.
        const glm::dvec3 from = departurePoint(solar);
        const glm::dvec3 to = target.position + orbitOffset(solar, m_target, m_orbitAngle);

        // A curve instead of a straight line: climb away from the body we leave along its local vertical
        // (swinging around its limb if the destination is below the horizon), and come down onto the
        // arrival point along the target's vertical. Neither body is ever flown through.
        const glm::dvec3 dir = to - from;
        const double D = glm::length(dir);
        const double u = logTravel(s, glm::length(m_fromOffset), glm::length(to - target.position), D);
        const Body& fromBody = solar.body(m_fromBody);
        const glm::dvec3 relFrom = from - fromBody.position;
        const double dFrom = std::max(glm::length(relFrom), 1e-30);
        const glm::dvec3 up0 = relFrom / dFrom;
        const glm::dvec3 toTarget = dir / std::max(D, 1e-30);
        glm::dvec3 side = toTarget - up0 * glm::dot(toTarget, up0);
        side = glm::length(side) > 1e-9 ? glm::normalize(side) : glm::dvec3(0.0);
        const double belowHorizon = glm::smoothstep(0.2, -0.2, glm::dot(toTarget, up0));
        const double L1 = std::min(0.3 * D, dFrom * 2.0);
        const glm::dvec3 up3 = glm::normalize(to - target.position);
        const double L2 = std::min(0.3 * D, glm::length(to - target.position) * 2.0);
        const glm::dvec3 P1 = from + glm::normalize(up0 + side * (0.9 * belowHorizon)) * L1;
        const glm::dvec3 P2 = to + up3 * L2;
        const double w = 1.0 - u;
        glm::dvec3 pos = from * (w * w * w) + P1 * (3.0 * w * w * u) + P2 * (3.0 * w * u * u) + to * (u * u * u);
        // Last line of defence: never below either body's surface (plus a margin).
        for (const Body* body : {&fromBody, &target}) {
            const glm::dvec3 rel = pos - body->position;
            const double r = glm::length(rel);
            const double minR = body->radiusKm / kKmPerParsec * 1.05;
            if (r < minR && r > 0.0) pos = body->position + rel / r * minR;
        }
        camera.position = pos;

        // Look: ease from the departure orientation to looking at the target.
        glm::vec3 look = glm::normalize(glm::vec3(target.position - camera.position));
        glm::vec3 up = glm::vec3(glm::normalize(glm::dvec3(target.rotation * glm::vec3(0.f, 1.f, 0.f))));
        glm::quat wantOrient = glm::quatLookAt(look, up);
        // Turn toward the destination gently, so what was just visited slides out of view instead of snapping away.
        camera.orientation = glm::slerp(m_fromOrient, wantOrient, (float)smooth(m_t / (m_duration * 0.55)));

        if (m_t >= m_duration) {
            m_phase = Phase::Orbit;
            m_t = 0.0;
        }
        camera.speed = glm::length(dir) / m_duration;
        return;
    }

    // Orbit: one slow revolution per orbitSeconds * 2.6, looking slightly ahead of centre.
    m_t += dt;
    m_orbitAngle += dt * glm::two_pi<double>() / (orbitSeconds * 2.6);
    camera.position = target.position + orbitOffset(solar, m_target, m_orbitAngle);
    glm::vec3 look = glm::normalize(glm::vec3(target.position - camera.position));
    glm::vec3 up = glm::vec3(glm::normalize(glm::dvec3(target.rotation * glm::vec3(0.f, 1.f, 0.f))));
    glm::quat wantOrient = glm::quatLookAt(look, up);
    camera.orientation = glm::slerp(camera.orientation, wantOrient, (float)std::min(1.0, dt * 2.0));
    camera.speed = targetRadius * 0.5;

    if (m_t >= orbitSeconds) beginFlight(solar, camera);
}

std::string Tour::status() const {
    if (!m_active) return "off";
    if (m_targetCraft >= 0 && m_crafts) {
        const std::string& n = m_crafts->crafts()[m_targetCraft].name;
        return (m_phase == Phase::Flight ? "en route to " : "visiting ") + n;
    }
    return m_phase == Phase::Flight ? "en route" : "orbiting";
}

} // namespace space
