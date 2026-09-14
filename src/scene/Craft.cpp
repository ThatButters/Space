#include "scene/Craft.h"
#include "scene/SolarSystem.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace space {

namespace {

constexpr double kMetersPerParsec = kKmPerParsec * 1000.0;

// ICRS RA/Dec -> engine direction (same matrices as SolarSystem / fetch_gaia.py).
glm::dvec3 icrsToEngine(double raDeg, double decDeg) {
    const double ra = glm::radians(raDeg), dec = glm::radians(decDeg);
    const glm::dvec3 c(std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec));
    const glm::dmat3 icrsToGal = glm::transpose(glm::dmat3(
        -0.0548755604162154, -0.8734370902348850, -0.4838350155487132,
        +0.4941094278755837, -0.4448296299600112, +0.7469822444972189,
        -0.8676661490190047, -0.1980763734312015, +0.4559837761750669));
    const glm::dvec3 g = icrsToGal * c;
    return {g.x, g.z, -g.y};
}

// Body-local unit direction for a latitude/longitude, matching the surface map orientation.
glm::dvec3 latLonDir(double latDeg, double lonDeg) {
    const double lat = glm::radians(latDeg), lon = glm::radians(lonDeg);
    return {std::cos(lat) * std::cos(lon), std::sin(lat), -std::cos(lat) * std::sin(lon)}; // east = -z
}

glm::quat frameFromUpForward(const glm::vec3& up, const glm::vec3& forwardHint) {
    glm::vec3 u = glm::normalize(up);
    glm::vec3 f = forwardHint - u * glm::dot(forwardHint, u);
    if (glm::length(f) < 1e-5f) f = std::abs(u.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    f = glm::normalize(f - u * glm::dot(f, u));
    glm::vec3 r = glm::cross(f, u);
    // glTF models are +Y up, -Z forward.
    return glm::quat_cast(glm::mat3(r, u, -f));
}

} // namespace

int CraftCatalog::find(const std::string& name) const {
    for (size_t i = 0; i < m_crafts.size(); ++i)
        if (m_crafts[i].name == name) return (int)i;
    return -1;
}

void CraftCatalog::build(const SolarSystem& solar) {
    m_crafts.clear();
    auto surface = [&](const char* name, const char* model, float size, const char* body, double lat, double lon,
                       double heading, const char* blurb) {
        Craft c;
        c.name = name;
        c.model = model;
        c.sizeMeters = size;
        c.placement = CraftPlacement::Surface;
        c.parent = solar.find(body);
        c.latDeg = lat;
        c.lonDeg = lon;
        c.headingDeg = heading;
        c.blurb = blurb;
        m_crafts.push_back(c);
    };
    auto orbit = [&](const char* name, const char* model, float size, const char* body, double radiusKm,
                     double periodMin, double incl, double raan, double phase, const char* blurb) {
        Craft c;
        c.name = name;
        c.model = model;
        c.sizeMeters = size;
        c.placement = CraftPlacement::Orbit;
        c.parent = solar.find(body);
        c.orbitRadiusKm = radiusKm;
        c.periodMinutes = periodMin;
        c.inclinationDeg = incl;
        c.raanDeg = raan;
        c.phaseDeg = phase;
        c.blurb = blurb;
        m_crafts.push_back(c);
    };

    // Scenery: the Lunar Flag Assembly beside each Apollo lander (metres east / north of the LM). The
    // mast is placed where the crews' photos and LRO images put it, to within a few metres.
    auto flag = [&](const char* site, double eastM, double northM, double heading) {
        const Craft& lm = m_crafts[find(site)];
        const double mPerDeg = 1737400.0 * glm::pi<double>() / 180.0;
        Craft c;
        c.name = std::string(site) + " flag";
        c.model = "__flag__";
        c.sizeMeters = 2.68f; // refined from the generated geometry once models load
        c.placement = CraftPlacement::Surface;
        c.parent = lm.parent;
        c.latDeg = lm.latDeg + northM / mPerDeg;
        c.lonDeg = lm.lonDeg + eastM / (mPerDeg * std::cos(glm::radians(lm.latDeg)));
        c.headingDeg = heading;
        c.blurb = "Lunar Flag Assembly";
        c.listed = false;
        m_crafts.push_back(c);
    };

    // --- Landing sites (selenographic / areographic coordinates) ---
    surface("Apollo 11 (Tranquility Base)", "apollo_lm.glb", 9.4f, "Moon", 0.674, 23.473, 0, "Eagle's descent stage, where the first footprints are");
    m_crafts.back().when = "20 July 1969";
    surface("Apollo 12", "apollo_lm.glb", 9.4f, "Moon", -3.012, -23.422, 40, "Intrepid, Ocean of Storms");
    m_crafts.back().when = "19 November 1969";
    surface("Apollo 14", "apollo_lm.glb", 9.4f, "Moon", -3.646, -17.472, 80, "Antares, Fra Mauro");
    m_crafts.back().when = "5 February 1971";
    surface("Apollo 15", "apollo_lm.glb", 9.4f, "Moon", 26.132, 3.634, 120, "Falcon, Hadley Rille");
    m_crafts.back().when = "30 July 1971";
    surface("Apollo 16", "apollo_lm.glb", 9.4f, "Moon", -8.973, 15.499, 160, "Orion, Descartes Highlands");
    m_crafts.back().when = "21 April 1972";
    surface("Apollo 17", "apollo_lm.glb", 9.4f, "Moon", 20.191, 30.772, 200, "Challenger, Taurus-Littrow");
    m_crafts.back().when = "11 December 1972";
    surface("Perseverance", "perseverance.glb", 3.0f, "Mars", 18.445, 77.451, 30, "Jezero crater, since 2021");
    surface("Ingenuity", "ingenuity.glb", 1.2f, "Mars", 18.44, 77.42, 0, "First aircraft on another world");
    surface("Viking 1", "viking.glb", 3.0f, "Mars", 22.27, -47.95, 0, "Chryse Planitia, 1976");
    m_crafts.back().when = "20 July 1976";
    surface("Viking 2", "viking.glb", 3.0f, "Mars", 47.64, 134.29, 90, "Utopia Planitia, 1976");
    m_crafts.back().when = "3 September 1976";
    surface("InSight", "insight.glb", 6.0f, "Mars", 4.502, 135.623, 0, "Elysium Planitia, 2018-2022");
    m_crafts.back().when = "26 November 2018";
    surface("Huygens", "huygens.glb", 2.7f, "Titan", -10.3, 167.7, 0, "Landed on Titan, January 2005");
    m_crafts.back().when = "14 January 2005";
    // A Saturn V two minutes into an Apollo launch, 9 km over the Atlantic off Kennedy, pitched downrange.
    surface("Saturn V", "saturn_v.glb", 111.f, "Earth", 28.95, -79.65, 72, "Apollo 11 launch, T+2:20: 60 km up, first stage still burning");
    m_crafts.back().when = "16 July 1969";
    m_crafts.back().altitudeKm = 60.0;
    m_crafts.back().modelPitchDeg = -58.f;
    m_crafts.back().keepClock = true;
    {
        Craft plume = m_crafts.back();
        plume.name = "Saturn V plume";
        plume.model = "__plume__";
        plume.sizeMeters = 1.f;
        plume.listed = false;
        plume.noLift = true;
        plume.blurb = "";
        m_crafts.push_back(plume);
        // The exhaust column it leaves behind: kilometres of sunlit steam and soot trailing downrange.
        Craft trail = plume;
        trail.name = "Saturn V trail";
        trail.model = "__trail__";
        trail.exhaustTrail = true;
        m_crafts.push_back(trail);
    }
    // Higher-quality NASA sources (scripts/fetch_models.py), falling back to the original GLBs.
    auto upgrade = [&](const char* name, const char* model, float size, float yaw, float pitch) {
        int i = find(name);
        if (i < 0) return;
        Craft& c = m_crafts[i];
        c.modelFallback = c.model;
        c.model = model;
        c.sizeMeters = size;
        c.modelYawDeg = yaw;
        c.modelPitchDeg = pitch;
    };
    // Morning light puts the camera north or south of the lander, so the cloth faces roughly that way.
    // Only the descent stages stayed on the Moon: cut the ascent stage off the NASA model.
    for (Craft& c : m_crafts)
        if (c.model == "apollo_lm.glb") c.cropAboveY = 2.42f;
    flag("Apollo 11 (Tranquility Base)", -8.5, 3.0, 350);
    flag("Apollo 12", -11.0, -4.0, 12);
    flag("Apollo 14", -9.0, 8.0, 344);
    flag("Apollo 15", 10.0, -12.0, 190);
    flag("Apollo 16", -12.0, -3.0, 8);
    flag("Apollo 17", 9.0, 11.0, 172);

    // --- Orbiters (radius = body radius + altitude) ---
    orbit("ISS", "iss.glb", 109.f, "Earth", 6371 + 420, 92.9, 51.6, 0, 0, "International Space Station, 420 km");
    orbit("Hubble", "hubble.glb", 13.2f, "Earth", 6371 + 535, 95.4, 28.5, 60, 90, "Hubble Space Telescope, 535 km");
    orbit("Apollo-Soyuz", "apollo_soyuz.glb", 20.f, "Earth", 6371 + 222, 88.9, 51.8, 120, 40, "Apollo and Soyuz docked in orbit");
    m_crafts.back().when = "17 July 1975";
    orbit("LRO", "lro.glb", 4.3f, "Moon", 1737 + 50, 113, 90, 0, 0, "Lunar Reconnaissance Orbiter, 50 km polar");
    m_crafts.back().sunFacingPlane = true;
    orbit("MRO", "mro.glb", 13.6f, "Mars", 3390 + 300, 112, 93, 0, 0, "Mars Reconnaissance Orbiter");
    orbit("Juno", "juno.glb", 20.f, "Jupiter", 69911 + 200000, 53 * 24 * 60, 90, 0, 0, "Juno, polar orbit (circularised here)");

    // --- Lagrange-point observatories ---
    {
        Craft c;
        c.name = "JWST";
        c.model = "jwst.glb";
        c.sizeMeters = 21.f;
        c.placement = CraftPlacement::Lagrange;
        c.parent = solar.find("Earth");
        c.lagrangeSign = 1.0;
        c.lagrangeKm = 1.5e6;
        c.blurb = "James Webb Space Telescope at Sun-Earth L2";
        m_crafts.push_back(c);
        c.name = "SOHO";
        c.model = "soho.glb";
        c.sizeMeters = 9.5f;
        c.lagrangeSign = -1.0;
        c.blurb = "Solar and Heliospheric Observatory at L1";
        m_crafts.push_back(c);
    }

    // --- Probes leaving the solar system (ICRS direction, distance at 2025.0, growth per year) ---
    auto probe = [&](const char* name, const char* model, float size, double ra, double dec, double au,
                     double auPerYear, const char* blurb) {
        Craft c;
        c.name = name;
        c.model = model;
        c.sizeMeters = size;
        c.placement = CraftPlacement::Heliocentric;
        c.raDeg = ra;
        c.decDeg = dec;
        c.distanceAuAtEpoch = au;
        c.epochJd = 2460676.5; // 2025-01-01
        c.auPerYear = auPerYear;
        c.blurb = blurb;
        m_crafts.push_back(c);
    };
    probe("Voyager 1", "voyager.glb", 13.f, 258.3, 12.2, 166.0, 3.57, "Farthest human-made object");
    probe("Voyager 2", "voyager.glb", 13.f, 300.0, -59.0, 139.0, 3.19, "Launched 1977, in interstellar space");
    probe("Pioneer 10", "pioneer10.glb", 2.9f, 76.5, 26.0, 137.0, 2.55, "Silent since 2003, heading for Aldebaran");
    upgrade("ISS", "iss_hd.glb", 140.f, 90.f, 0.f); // 2.7M triangles, 4K textures; truss along model Z
    upgrade("Hubble", "hubble_eyes/Hubble.gltf", 18.f, 0.f, 90.f); // tube along the orbit, not pointing at Earth
    upgrade("LRO", "lro_eyes/LRO.gltf", 5.f, 0.f, 0.f);
    upgrade("Juno", "juno_eyes/Juno.gltf", 20.f, 0.f, 0.f);
    upgrade("Voyager 1", "voyager_eyes/Voyager.gltf", 13.f, 0.f, 0.f);
    upgrade("Voyager 2", "voyager_eyes/Voyager.gltf", 13.f, 0.f, 0.f);
    upgrade("Pioneer 10", "pioneer_eyes/pioneer.gltf", 2.9f, 0.f, 0.f);

    // --- Parker Solar Probe: ecliptic ellipse, perihelion 0.046 AU ---
    {
        Craft c;
        c.name = "Parker Solar Probe";
        c.model = "parker.glb";
        c.sizeMeters = 3.f;
        c.placement = CraftPlacement::HeliocentricEllipse;
        c.parent = 0;
        c.aAu = 0.388;
        c.ecc = 0.88;
        c.periodDays = 88.0;
        c.inclinationDeg = 3.4;
        c.blurb = "Closest approach to the Sun: 6.1 million km";
        m_crafts.push_back(c);
    }
    for (Craft& c : m_crafts) {
        if (c.name == "SOHO") { c.modelFallback = c.model; c.model = "soho_eyes/soho.gltf"; }
        if (c.name == "Parker Solar Probe") { c.modelFallback = c.model; c.model = "parker_eyes/PSP.gltf"; }
    }
}

void CraftCatalog::update(const SolarSystem& solar, double jd) {
    const double days = jd - 2451545.0;
    const double pcPerKm = 1.0 / kKmPerParsec;
    for (auto& c : m_crafts) {
        switch (c.placement) {
        case CraftPlacement::Surface: {
            const Body& b = solar.body(c.parent);
            glm::dvec3 local = SolarSystem::latLonToLocal(c.latDeg, c.lonDeg);
            glm::dvec3 up = b.rotationD * local; // double: centimetre-stable on a 1700 km globe
            c.position = b.position + up * ((b.radiusKm + c.altitudeKm) * pcPerKm);
            // Forward: heading measured from local north.
            glm::dvec3 northLocal = glm::normalize(glm::dvec3(0, 1, 0) - local * local.y);
            glm::dvec3 eastLocal = glm::cross(northLocal, local);
            double hd = glm::radians(c.headingDeg);
            glm::dvec3 fwdLocal = northLocal * std::cos(hd) + eastLocal * std::sin(hd);
            glm::vec3 fwd = glm::vec3(b.rotationD * fwdLocal);
            c.rotation = frameFromUpForward(glm::vec3(up), fwd);
            break;
        }
        case CraftPlacement::Orbit: {
            const Body& b = solar.body(c.parent);
            if (c.tle) {
                // Real elements: argument of latitude advances at the mean motion; the node regresses under
                // J2 (about -5 deg/day for the ISS). Positions hold to a few hundred km for several days.
                const double n = c.tleMeanMotion * glm::two_pi<double>() / 86400.0; // rad/s
                const double mu = 398600.4418, J2 = 1.08263e-3, Re = 6378.137;
                const double a = std::cbrt(mu / (n * n));
                const double dtDays = jd - c.tleEpochJd;
                const double inc = glm::radians(c.tleIncDeg);
                const double raanDot = -1.5 * n * J2 * (Re / a) * (Re / a) * std::cos(inc); // rad/s
                const double raan = glm::radians(c.tleRaanDeg) + raanDot * dtDays * 86400.0;
                const double u = glm::radians(c.tleArgLatDeg) + n * dtDays * 86400.0;
                const glm::dvec3 axis = glm::normalize(glm::dvec3(b.rotation * glm::vec3(0, 1, 0)));
                glm::dvec3 e1 = solar.equinoxDirection();
                e1 = glm::normalize(e1 - axis * glm::dot(e1, axis));
                const glm::dvec3 e3 = glm::cross(axis, e1); // eastward from the equinox
                const double cu = std::cos(u), su = std::sin(u), cO = std::cos(raan), sO = std::sin(raan), ci = std::cos(inc), si = std::sin(inc);
                const glm::dvec3 world = e1 * (cu * cO - su * ci * sO) + e3 * (cu * sO + su * ci * cO) + axis * (su * si);
                const glm::dvec3 vel = e1 * (-su * cO - cu * ci * sO) + e3 * (-su * sO + cu * ci * cO) + axis * (cu * si);
                c.position = b.position + world * (a * pcPerKm);
                c.rotation = frameFromUpForward(glm::vec3(-world), glm::vec3(vel));
                break;
            }
            const double minutes = days * 1440.0;
            const double ang = glm::radians(c.phaseDeg) + glm::two_pi<double>() * minutes / c.periodMinutes;
            const double inc = glm::radians(c.inclinationDeg);
            double raan = glm::radians(c.raanDeg);
            // Orbit in the body's equatorial frame (y = spin axis).
            glm::dvec3 p(std::cos(ang), 0.0, std::sin(ang));
            glm::dvec3 v(-std::sin(ang), 0.0, std::cos(ang));
            auto tilt = [&](glm::dvec3 q) {
                q = glm::dvec3(q.x, q.y * std::cos(inc) - q.z * std::sin(inc), q.y * std::sin(inc) + q.z * std::cos(inc));
                return glm::dvec3(q.x * std::cos(raan) - q.z * std::sin(raan), q.y, q.x * std::sin(raan) + q.z * std::cos(raan));
            };
            p = tilt(p);
            v = tilt(v);
            // Body rotation without the spin: use the body's axis frame (rotation includes spin; orbits
            // precess with it slowly, which is acceptable for a visual).
            glm::dvec3 axis = glm::normalize(glm::dvec3(b.rotation * glm::vec3(0, 1, 0)));
            glm::dvec3 ref = std::abs(axis.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            glm::dvec3 e1 = glm::normalize(glm::cross(ref, axis));
            glm::dvec3 e3 = glm::cross(axis, e1);
            if (c.sunFacingPlane) {
                // With i = 90 the node line is local x rotated by raan; point it at the Sun's projection.
                const glm::dvec3 s = solar.sunPosition() - b.position;
                raan = std::atan2(glm::dot(s, e3), glm::dot(s, e1));
                p = glm::dvec3(std::cos(ang), 0.0, std::sin(ang));
                v = glm::dvec3(-std::sin(ang), 0.0, std::cos(ang));
                auto tilt2 = [&](glm::dvec3 q) {
                    q = glm::dvec3(q.x, q.y * std::cos(inc) - q.z * std::sin(inc), q.y * std::sin(inc) + q.z * std::cos(inc));
                    return glm::dvec3(q.x * std::cos(raan) - q.z * std::sin(raan), q.y, q.x * std::sin(raan) + q.z * std::cos(raan));
                };
                p = tilt2(p);
                v = tilt2(v);
            }
            glm::dvec3 world = e1 * p.x + axis * p.y + e3 * p.z;
            glm::dvec3 vel = e1 * v.x + axis * v.y + e3 * v.z;
            c.position = b.position + world * (c.orbitRadiusKm * pcPerKm);
            c.rotation = frameFromUpForward(glm::vec3(-world), glm::vec3(vel)); // nadir-pointing
            break;
        }
        case CraftPlacement::Lagrange: {
            const Body& b = solar.body(c.parent);
            glm::dvec3 away = glm::normalize(b.position - solar.sunPosition());
            c.position = b.position + away * (c.lagrangeSign * c.lagrangeKm * pcPerKm);
            // Sunshield toward the Sun: model +Y up = anti-sun.
            c.rotation = frameFromUpForward(glm::vec3(away * c.lagrangeSign), glm::vec3(0, 1, 0));
            break;
        }
        case CraftPlacement::Heliocentric: {
            const double years = (jd - c.epochJd) / 365.25;
            const double au = c.distanceAuAtEpoch + c.auPerYear * years;
            glm::dvec3 dir = icrsToEngine(c.raDeg, c.decDeg);
            c.position = solar.sunPosition() + dir * (au * kKmPerAU * pcPerKm);
            c.rotation = frameFromUpForward(glm::vec3(-dir), glm::vec3(0, 1, 0)); // dish back at the Sun
            break;
        }
        case CraftPlacement::HeliocentricEllipse: {
            const double M = std::fmod(glm::two_pi<double>() * days / c.periodDays, glm::two_pi<double>());
            double E = M;
            for (int i = 0; i < 10; ++i) E -= (E - c.ecc * std::sin(E) - M) / (1.0 - c.ecc * std::cos(E));
            const double x = c.aAu * (std::cos(E) - c.ecc), y = c.aAu * std::sqrt(1 - c.ecc * c.ecc) * std::sin(E);
            const double inc = glm::radians(c.inclinationDeg);
            glm::dvec3 ecl(x, y * std::cos(inc), y * std::sin(inc));
            // Ecliptic -> engine via the solar system's frame helper (approximate: use Earth's orbit plane).
            const glm::dvec3 earth = solar.body(solar.find("Earth")).position - solar.sunPosition();
            glm::dvec3 ex = glm::normalize(earth);
            glm::dvec3 ez = glm::normalize(glm::cross(ex, glm::dvec3(0, 1, 0)));
            glm::dvec3 ey = glm::cross(ez, ex);
            glm::dvec3 world = ex * ecl.x + ey * ecl.y + ez * ecl.z;
            c.position = solar.sunPosition() + world * (kKmPerAU * pcPerKm);
            c.rotation = frameFromUpForward(glm::vec3(-glm::normalize(world)), glm::vec3(0, 1, 0)); // shield sunward
            break;
        }
        }
    }
}

void CraftCatalog::buildGpuList(const SolarSystem& solar, const glm::dvec3& cameraPos, std::vector<CraftGpu>& out,
                                std::vector<float>& modelExtents) const {
    out.clear();
    modelExtents.clear();
    const float AU = 4.848e-6f;
    for (const auto& c : m_crafts) {
        if (c.modelIndex < 0 || c.modelIndex >= (int)modelNativeExtent.size()) {
            out.push_back({});
            modelExtents.push_back(0.f);
            continue;
        }
        const float native = std::max(modelNativeExtent[c.modelIndex], 1e-6f);
        const float scale = (float)(c.sizeMeters / kMetersPerParsec) / native;
        // Surface craft: lift the model so its lowest point rests on the ground.
        const float lift = (c.placement == CraftPlacement::Surface && !c.noLift && c.modelIndex < (int)modelNativeMinY.size())
                               ? -modelNativeMinY[c.modelIndex] : 0.f;
        const glm::quat fix = glm::angleAxis(glm::radians(c.modelYawDeg), glm::vec3(0, 1, 0)) *
                              glm::angleAxis(glm::radians(c.modelPitchDeg), glm::vec3(1, 0, 0));
        glm::mat4 m = glm::translate(glm::mat4(1.f), glm::vec3(c.position - cameraPos)) *
                      glm::mat4_cast(c.rotation) * glm::scale(glm::mat4(1.f), glm::vec3(scale)) *
                      glm::mat4_cast(fix) * glm::translate(glm::mat4(1.f), glm::vec3(0.f, lift, 0.f) + c.modelOffset);
        CraftGpu g;
        g.model = m;
        glm::dvec3 toSun = solar.sunPosition() - c.position;
        double d2 = glm::dot(toSun, toSun);
        g.sunDir = glm::vec4(glm::vec3(glm::normalize(toSun)), std::pow((double)AU * AU / std::max(d2, 1e-30), 0.3));
        // tint.w: albedo of the ground or planet below (bounce light and reflections), weighted by how much
        // of the lower hemisphere it fills; 0 in open space.
        float ground = 0.f;
        float sunlit = 1.f;
        if ((c.placement == CraftPlacement::Surface || c.placement == CraftPlacement::Orbit) && c.parent >= 0) {
            const Body& pb = solar.body(c.parent);
            const std::string& n = pb.name;
            const float albedo = n == "Moon" ? 0.14f : (n == "Mars" ? 0.22f : (n == "Earth" ? 0.3f : 0.18f));
            const glm::dvec3 rel = c.position - pb.position;
            const double r = glm::length(rel), R = pb.radiusKm / kKmPerParsec;
            const glm::dvec3 L = glm::normalize(solar.sunPosition() - pb.position);
            if (c.placement == CraftPlacement::Surface) {
                ground = albedo;
                // Night falls on landers too: the Sun sets over a quarter degree.
                sunlit = (float)glm::smoothstep(-0.005, 0.005, glm::dot(rel / r, L));
            } else {
                const double sinT = std::min(R / r, 1.0);
                ground = albedo * (float)(1.0 - std::sqrt(1.0 - sinT * sinT));
                // Eclipse: inside the planet's shadow cylinder, with a thin penumbra.
                const double along = glm::dot(rel, L);
                if (along < 0.0) {
                    const double perp = glm::length(rel - L * along);
                    sunlit = (float)glm::smoothstep(R * 0.985, R * 1.015, perp);
                }
            }
        }
        g.sunDir.w *= sunlit;
        g.crop = glm::vec4(c.cropAboveY, c.exhaustTrail ? 1.f : 0.f, 0.f, 0.f);
        g.extra = glm::vec4((float)(c.sizeMeters / kMetersPerParsec), sunlit, c.sizeMeters / native,
                            c.listed && c.placement != CraftPlacement::Surface ? 1.f : 0.f);
        g.tint = glm::vec4(1.f, 1.f, 1.f, ground);
        // up: away from the parent body (ground bounce and sky fill), or model up in deep space.
        glm::dvec3 upDir = c.parent >= 0 ? glm::normalize(c.position - solar.body(c.parent).position)
                                         : glm::dvec3(c.rotation * glm::vec3(0, 1, 0));
        g.up = glm::vec4(glm::vec3(upDir), c.parent >= 0 ? 1.f : 0.f);
        out.push_back(g);
        modelExtents.push_back((float)(c.sizeMeters / kMetersPerParsec));
    }
}

void CraftCatalog::viewpoint(const SolarSystem& solar, int index, double distanceSizes, glm::dvec3& outPos,
                             glm::quat& outOrient) const {
    const Craft& c = m_crafts[index];
    const double size = c.sizeMeters / kMetersPerParsec;
    glm::dvec3 toSun = glm::normalize(solar.sunPosition() - c.position);
    glm::dvec3 up = skyUp(solar, index);
    glm::dvec3 sideRaw = glm::cross(up, toSun);
    glm::dvec3 side = glm::length(sideRaw) < 1e-6 ? glm::dvec3(1, 0, 0) : glm::normalize(sideRaw);
    glm::dvec3 sunFlat = toSun - up * glm::dot(toSun, up);
    if (glm::length(sunFlat) < 1e-6) sunFlat = side;
    sunFlat = glm::normalize(sunFlat);
    glm::dvec3 dir;
    if (earthInSky(solar, index)) {
        // Standing on the Moon: the one thing that says where we are is Earth in the black sky. Camera
        // low, on the side of the lander away from Earth, so the tour looks past the lander up at it.
        const glm::dvec3 toEarth = glm::normalize(solar.body(solar.find("Earth")).position - c.position);
        glm::dvec3 earthFlat = toEarth - up * glm::dot(toEarth, up);
        earthFlat = glm::length(earthFlat) > 1e-6 ? glm::normalize(earthFlat) : sunFlat;
        dir = glm::normalize(-earthFlat * 0.92 + up * 0.14 + side * 0.15);
        // The lander sits a little left of centre, clear of the caption under it.
        outPos = c.position + dir * (size * distanceSizes * 1.25);
        const glm::dvec3 aim = viewAim(solar, index, outPos, glm::radians(85.f));
        outOrient = glm::quatLookAt(glm::normalize(glm::vec3(aim - outPos)), glm::vec3(up));
        return;
    }
    switch (c.placement) {
    case CraftPlacement::Surface:
        if (c.altitudeKm > 1.0) {
            // In flight: from above and off the sunlit flank, so the planet fills the view behind it.
            dir = glm::normalize(up * 1.0 + side * 0.5 + sunFlat * 0.35);
            break;
        }
        // Cross-lit from low on the ground, like a crew photograph: the Sun off to one side so the
        // shadows stretch across the frame instead of hiding behind the lander.
        dir = glm::normalize(side * 0.8 + sunFlat * 0.2 + up * 0.6); // three-quarters from above
        break;
    case CraftPlacement::Orbit: {
        // Trailing just behind and above the orbiter, off its sunlit flank: we ride along with it, the
        // station three-quarters on and lit, the planet sliding past underneath. The tour arrives here
        // by catching up from behind along the orbit, so the approach never crosses the station.
        glm::dvec3 along, radial;
        orbitFrame(solar, index, along, radial);
        glm::dvec3 cross = glm::normalize(glm::cross(along, radial));
        // Which flank faces the Sun, as a continuous weight: when the Sun lies in the orbit plane (LRO's
        // noon-midnight orbit) a hard sign would flip every frame and the camera would jump side to side.
        const double s = std::clamp(glm::dot(toSun, cross) * 8.0, -1.0, 1.0);
        const double sunSide = s + (1.0 - std::abs(s)); // -1 .. +1, preferring +1 when undecided
        dir = glm::normalize(-along * 0.5 + cross * (0.45 * sunSide) + radial * 0.85); // high enough that the planet is behind it
        break;
    }
    default:
        dir = glm::normalize(side * 0.6 + sunFlat * 0.6 + up * 0.5);
        break;
    }
    outPos = c.position + dir * (size * distanceSizes);
    outOrient = glm::quatLookAt(glm::normalize(glm::vec3(c.position - outPos)), glm::vec3(up));
}

bool CraftCatalog::earthInSky(const SolarSystem& solar, int index) const {
    const Craft& c = m_crafts[index];
    if (c.placement != CraftPlacement::Surface || c.parent < 0 || c.altitudeKm > 1.0) return false;
    if (solar.body(c.parent).name != "Moon") return false;
    const int earth = solar.find("Earth");
    if (earth < 0) return false;
    const glm::dvec3 toEarth = glm::normalize(solar.body(earth).position - c.position);
    return glm::dot(toEarth, skyUp(solar, index)) > 0.15; // above the horizon by a useful margin
}

glm::dvec3 CraftCatalog::viewAim(const SolarSystem& solar, int index, const glm::dvec3& cameraPos, float fovY) const {
    const Craft& c = m_crafts[index];
    if (!earthInSky(solar, index)) return c.position;
    const glm::dvec3 up = skyUp(solar, index);
    const glm::dvec3 toEarth = glm::normalize(solar.body(solar.find("Earth")).position - c.position);
    const double earthElev = std::asin(std::clamp(glm::dot(toEarth, up), -1.0, 1.0));
    const double half = fovY * 0.5;
    // Tilt up just enough to bring Earth inside the top of the frame, keeping the lander in the bottom.
    const double lookElev = std::clamp(earthElev - half + glm::radians(3.0), 0.0, half - glm::radians(5.0));
    // The aim point sits above the lander at the height that gives that elevation from where the camera is.
    const glm::dvec3 rel = c.position - cameraPos;
    const double flat = glm::length(rel - up * glm::dot(rel, up));
    return cameraPos + (rel - up * glm::dot(rel, up)) + up * (flat * std::tan(lookElev));
}

double CraftCatalog::viewDistanceSizes(int index) const {
    const double size = m_crafts[index].sizeMeters;
    if (size >= 60.0) return 1.25; // the ISS from above: the whole station with the planet behind it
    if (size >= 15.0) return 2.0;
    const Craft& c = m_crafts[index];
    if (c.placement == CraftPlacement::Surface) return c.altitudeKm > 1.0 ? 2.6 : 1.35; // landers: beside them
    return 2.8;
}

void CraftCatalog::orbitFrame(const SolarSystem& solar, int index, glm::dvec3& alongTrack, glm::dvec3& up) const {
    const Craft& c = m_crafts[index];
    up = c.parent >= 0 ? glm::normalize(c.position - solar.body(c.parent).position)
                       : glm::dvec3(c.rotation * glm::vec3(0, 1, 0));
    // Orbiters are placed nadir-pointing with model -Z along the velocity.
    glm::dvec3 f = glm::dvec3(c.rotation * glm::vec3(0, 0, -1));
    f -= up * glm::dot(f, up);
    alongTrack = glm::length(f) > 1e-9 ? glm::normalize(f) : glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));
}

double CraftCatalog::preferredSunElevationDeg(const std::string& bodyName) {
    if (bodyName == "Moon" || bodyName == "Mercury") return 13.0; // the Apollo landings came down at 5-15 deg
    if (bodyName == "Mars") return 30.0;
    return 35.0;
}

double CraftCatalog::daylightJulianDate(const SolarSystem& solarIn, int index, double jd) const {
    const Craft& c = m_crafts[index];
    if (c.placement == CraftPlacement::Orbit && c.parent >= 0) {
        // Orbiters: wait for the pass over the day side, where the craft is sunlit against a bright planet.
        SolarSystem solar = solarIn;
        CraftCatalog cat = *this;
        auto overDay = [&](double t) {
            solar.update(t);
            cat.update(solar, t);
            const Craft& k = cat.m_crafts[index];
            const glm::dvec3 up = glm::normalize(k.position - solar.body(k.parent).position);
            return glm::dot(up, glm::normalize(solar.sunPosition() - k.position));
        };
        // Over an airless body, high noon is flat (no shading at all): aim for a ~25 deg Sun where relief reads.
        // With an atmosphere, the bright day side under the craft is the better picture.
        const bool airless = solarIn.body(c.parent).atmosphere <= 0.f;
        const double target = airless ? std::sin(glm::radians(25.0)) : std::cos(glm::radians(40.0));
        const double now = overDay(jd);
        if (airless ? (now >= target && now <= std::sin(glm::radians(45.0))) : now >= target) return jd;
        const double period = c.periodMinutes / 1440.0;
        const int steps = 360;
        double prevT = jd, prevV = now, bestT = jd, bestV = now;
        for (int i = 1; i <= steps; ++i) {
            const double t = jd + period * 1.02 * i / steps;
            const double v = overDay(t);
            if (v > bestV) bestV = v, bestT = t;
            if (prevV < target && v >= target) {
                double lo = prevT, hi = t;
                for (int k = 0; k < 30; ++k) {
                    const double mid = 0.5 * (lo + hi);
                    (overDay(mid) < target ? lo : hi) = mid;
                }
                // A little past the crossing, so the planet below is well into daylight.
                return hi + period * 0.04;
            }
            prevT = t;
            prevV = v;
        }
        return bestV > now + 0.15 ? bestT : jd; // orbit plane never gets that close to noon: take its best
    }
    if (c.placement != CraftPlacement::Surface || c.parent < 0) return jd;
    SolarSystem solar = solarIn;
    const Body& body = solarIn.body(c.parent);
    const double target = preferredSunElevationDeg(body.name);
    auto elevation = [&](double t) {
        solar.update(t);
        return solar.sunElevationDeg(c.parent, c.latDeg, c.lonDeg);
    };
    const double now = elevation(jd);
    if (now >= target * 0.6 && now <= target * 3.0) return jd; // already well lit
    // Scan one local day forward for the sunrise-side crossing of the target elevation, then bisect.
    const double dayDays = std::abs(body.rotationPeriodHours) / 24.0 * (body.name == "Moon" ? 1.09 : 1.05);
    const int steps = 480;
    double prevT = jd, prevE = now;
    for (int i = 1; i <= steps; ++i) {
        const double t = jd + dayDays * i / steps;
        const double e = elevation(t);
        if (prevE < target && e >= target) {
            double lo = prevT, hi = t;
            for (int k = 0; k < 40; ++k) {
                const double mid = 0.5 * (lo + hi);
                (elevation(mid) < target ? lo : hi) = mid;
            }
            return hi;
        }
        prevT = t;
        prevE = e;
    }
    return jd;
}

bool CraftCatalog::applyTle(const std::string& name, const std::string& line1, const std::string& line2) {
    const int i = find(name);
    if (i < 0 || line1.size() < 69 || line2.size() < 69) return false;
    Craft& c = m_crafts[i];
    try {
        const int yy = std::stoi(line1.substr(18, 2));
        const double doy = std::stod(line1.substr(20, 12));
        const int year = yy < 57 ? 2000 + yy : 1900 + yy;
        // JD of Jan 0.0 of the year (Meeus).
        const int y1 = year - 1;
        const long A = y1 / 100, B = 2 - A + A / 4;
        const double jd0 = std::floor(365.25 * (y1 + 4716)) + std::floor(30.6001 * 13) + 31 + B - 1524.5; // Dec 31 of y1
        c.tleEpochJd = jd0 + doy;
        c.tleIncDeg = std::stod(line2.substr(8, 8));
        c.tleRaanDeg = std::stod(line2.substr(17, 8));
        const double argp = std::stod(line2.substr(34, 8));
        const double meanAnom = std::stod(line2.substr(43, 8));
        c.tleArgLatDeg = argp + meanAnom;
        c.tleMeanMotion = std::stod(line2.substr(52, 11));
        c.tle = c.placement == CraftPlacement::Orbit;
        return c.tle;
    } catch (...) {
        return false;
    }
}

void CraftCatalog::rephaseForDaylight(const SolarSystem& solar, int index) {
    Craft& target = m_crafts[index];
    if (target.placement != CraftPlacement::Orbit || target.parent < 0 || target.tle) return; // real orbits stay real
    const Body& body = solar.body(target.parent);
    const bool airless = body.atmosphere <= 0.f;
    // Airless: a ~25 deg Sun where relief reads. With an atmosphere: the bright day side below.
    const double want = airless ? std::sin(glm::radians(25.0)) : 0.85;
    CraftCatalog trial = *this;
    const double base = target.phaseDeg;
    double bestPhase = base, bestScore = -1e9;
    for (int k = 0; k < 90; ++k) {
        const double phase = base + k * 4.0;
        trial.m_crafts[index].phaseDeg = phase;
        trial.update(solar, solar.julianDate());
        const Craft& t = trial.m_crafts[index];
        const glm::dvec3 up = glm::normalize(t.position - body.position);
        const double cosSun = glm::dot(up, glm::normalize(solar.sunPosition() - t.position));
        const double score = airless ? -std::abs(cosSun - want) : -std::abs(cosSun - want) * 0.5 + (cosSun > 0.2 ? 0.0 : -5.0);
        if (score > bestScore) {
            bestScore = score;
            bestPhase = phase;
        }
    }
    target.phaseDeg = std::fmod(bestPhase, 360.0);
}

glm::dvec3 CraftCatalog::skyUp(const SolarSystem& solar, int index) const {
    const Craft& c = m_crafts[index];
    if ((c.placement == CraftPlacement::Surface || c.placement == CraftPlacement::Orbit) && c.parent >= 0) {
        glm::dvec3 away = c.position - solar.body(c.parent).position;
        if (glm::length(away) > 0.0) return glm::normalize(away);
    }
    return glm::normalize(glm::dvec3(c.rotation * glm::vec3(0, 1, 0)));
}

} // namespace space
