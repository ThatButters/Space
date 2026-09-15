#include "scene/SolarSystem.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace space {

namespace {

// JPL approximate Keplerian elements, J2000 values and rates per Julian century (Standish 1992,
// https://ssd.jpl.nasa.gov/planets/approx_pos.html, valid 1800-2050).
// a [AU], e, I [deg], L [deg], long. of perihelion [deg], long. of ascending node [deg]
struct JplElements {
    double a, e, I, L, w, O;
    double da, de, dI, dL, dw, dO;
};
const JplElements kJpl[] = {
    {0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593,
     0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081}, // Mercury
    {0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255,
     0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418}, // Venus
    {1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
     0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0}, // Earth-Moon barycentre
    {1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891,
     0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343}, // Mars
    {5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,
     -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106}, // Jupiter
    {9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448,
     -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794}, // Saturn
    {19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503,
     -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589}, // Uranus
    {30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574,
     0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664}, // Neptune
};

// Heliocentric ecliptic J2000 position in AU.
glm::dvec3 jplPosition(const JplElements& el, double jd) {
    const double T = (jd - 2451545.0) / 36525.0;
    const double a = el.a + el.da * T;
    const double e = el.e + el.de * T;
    const double I = glm::radians(el.I + el.dI * T);
    const double L = glm::radians(el.L + el.dL * T);
    const double w = glm::radians(el.w + el.dw * T);
    const double O = glm::radians(el.O + el.dO * T);

    const double omega = w - O;
    double M = std::fmod(L - w, glm::two_pi<double>());
    if (M > glm::pi<double>()) M -= glm::two_pi<double>();

    double E = M + e * std::sin(M);
    for (int i = 0; i < 8; ++i) {
        double dE = (M - (E - e * std::sin(E))) / (1.0 - e * std::cos(E));
        E += dE;
        if (std::abs(dE) < 1e-10) break;
    }
    const double xp = a * (std::cos(E) - e);
    const double yp = a * std::sqrt(1.0 - e * e) * std::sin(E);

    const double cw = std::cos(omega), sw = std::sin(omega);
    const double cO = std::cos(O), sO = std::sin(O);
    const double cI = std::cos(I), sI = std::sin(I);
    return {(cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
            (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
            (sw * sI) * xp + (cw * sI) * yp};
}

// ICRS -> galactic (same matrix as scripts/fetch_gaia.py).
const glm::dmat3 kIcrsToGal = glm::transpose(glm::dmat3(
    -0.0548755604162154, -0.8734370902348850, -0.4838350155487132,
    +0.4941094278755837, -0.4448296299600112, +0.7469822444972189,
    -0.8676661490190047, -0.1980763734312015, +0.4559837761750669));

} // namespace

SolarSystem::SolarSystem() {
    // Ecliptic J2000 -> equatorial J2000 (rotate about x by the obliquity), -> galactic, -> engine axes.
    const double eps = glm::radians(23.4392911);
    const glm::dmat3 eclToEq = glm::transpose(glm::dmat3(1, 0, 0, 0, std::cos(eps), -std::sin(eps), 0, std::sin(eps), std::cos(eps)));
    const glm::dmat3 galToEngine = glm::transpose(glm::dmat3(1, 0, 0, 0, 0, 1, 0, -1, 0)); // (gx,gy,gz) -> (gx,gz,-gy)
    m_eclipticToEngine = galToEngine * kIcrsToGal * eclToEq;
    m_eclipticToEq = eclToEq;

    auto add = [&](Body b) {
        m_bodies.push_back(std::move(b));
        return (int)m_bodies.size() - 1;
    };
    Body sun;
    sun.name = "Sun";
    sun.type = BodyType::Sun;
    sun.radiusKm = 695700;
    sun.rotationPeriodHours = 609.12;
    sun.color = {1.0f, 0.93f, 0.82f};
    sun.seed = 0.1f;
    add(sun);

    auto planet = [&](const char* name, BodyType type, double radiusKm, int jpl, double rotHours, double tilt,
                      glm::vec3 color, float seed, float atmo) {
        Body b;
        b.name = name;
        b.type = type;
        b.radiusKm = radiusKm;
        b.jplIndex = jpl;
        b.rotationPeriodHours = rotHours;
        b.axialTiltDeg = tilt;
        b.color = color;
        b.seed = seed;
        b.atmosphere = atmo;
        return add(b);
    };
    auto moon = [&](const char* name, int parent, double radiusKm, double orbitKm, double periodDays, double phase,
                    glm::vec3 color, float seed) {
        Body b;
        b.name = name;
        b.type = BodyType::Rocky;
        b.parent = parent;
        b.radiusKm = radiusKm;
        b.orbitRadiusKm = orbitKm;
        b.orbitPeriodDays = periodDays;
        b.orbitPhase = phase;
        b.rotationPeriodHours = periodDays * 24.0; // tidally locked
        b.color = color;
        b.seed = seed;
        return add(b);
    };

    planet("Mercury", BodyType::Rocky, 2439.7, 0, 1407.6, 0.03, {0.55f, 0.52f, 0.48f}, 1.f, 0.f);
    planet("Venus", BodyType::Rocky, 6051.8, 1, -5832.5, 177.4, {0.90f, 0.78f, 0.55f}, 2.f, 0.6f);
    int earth = planet("Earth", BodyType::Earth, 6371.0, 2, 23.934, 23.44, {0.05f, 0.25f, 0.55f}, 3.f, 1.0f);
    // The Moon's mean longitude gives a roughly correct phase: L = 218.316 + 13.176396 d (deg).
    moon("Moon", earth, 1737.4, 384400, 27.321582, glm::radians(218.316), {0.42f, 0.41f, 0.40f}, 4.f);
    int mars = planet("Mars", BodyType::Rocky, 3389.5, 3, 24.623, 25.19, {0.72f, 0.40f, 0.25f}, 5.f, 0.6f);
    moon("Phobos", mars, 11.1, 9376, 0.3189, 0.0, {0.35f, 0.32f, 0.30f}, 6.f);
    moon("Deimos", mars, 6.2, 23463, 1.2624, 1.5, {0.38f, 0.35f, 0.32f}, 7.f);
    int jupiter = planet("Jupiter", BodyType::GasGiant, 69911, 4, 9.925, 3.13, {0.80f, 0.68f, 0.52f}, 8.f, 0.3f);
    moon("Io", jupiter, 1821.6, 421800, 1.769, 0.3, {0.85f, 0.75f, 0.35f}, 9.f);
    moon("Europa", jupiter, 1560.8, 671100, 3.551, 2.1, {0.80f, 0.78f, 0.70f}, 10.f);
    moon("Ganymede", jupiter, 2634.1, 1070400, 7.155, 4.0, {0.50f, 0.47f, 0.42f}, 11.f);
    moon("Callisto", jupiter, 2410.3, 1882700, 16.689, 0.9, {0.36f, 0.33f, 0.30f}, 12.f);
    int saturn = planet("Saturn", BodyType::GasGiant, 58232, 5, 10.656, 26.73, {0.86f, 0.78f, 0.60f}, 13.f, 0.25f);
    m_bodies[saturn].ringInner = 74500.f / 140220.f;
    m_bodies[saturn].ringOuterKm = 140220.f;
    int titan = moon("Titan", saturn, 2574.7, 1221870, 15.945, 2.6, {0.80f, 0.62f, 0.35f}, 14.f);
    m_bodies[titan].atmosphere = 0.7f;
    moon("Rhea", saturn, 763.8, 527108, 4.518, 5.1, {0.75f, 0.74f, 0.72f}, 15.f);
    planet("Uranus", BodyType::IceGiant, 25362, 6, -17.24, 97.77, {0.55f, 0.80f, 0.85f}, 16.f, 0.4f);
    int neptune = planet("Neptune", BodyType::IceGiant, 24622, 7, 16.11, 28.32, {0.20f, 0.35f, 0.90f}, 17.f, 0.4f);
    moon("Triton", neptune, 1353.4, 354759, -5.877, 3.3, {0.78f, 0.74f, 0.70f}, 18.f);

    // Physically based atmospheres (shaders/atmosphere.glsl): class and the height of each one's top.
    auto atmo = [&](const char* name, int cls, double topKm) {
        Body& b = m_bodies[find(name)];
        b.atmoClass = cls;
        b.atmoTopKm = topKm;
    };
    atmo("Earth", 0, 320.0);
    atmo("Mars", 1, 60.0);
    atmo("Venus", 2, 90.0);
    atmo("Titan", 3, 400.0);
    atmo("Jupiter", 4, 350.0);
    atmo("Saturn", 5, 500.0);
    atmo("Uranus", 6, 400.0);
    atmo("Neptune", 7, 400.0);

    // Surface maps (assets/textures, see scripts/fetch_textures.py). Moons without maps stay procedural.
    auto tex = [&](const char* name, const char* day, const char* night = "", const char* clouds = "") {
        Body& b = m_bodies[find(name)];
        b.texDay = day;
        b.texNight = night;
        b.texClouds = clouds;
    };
    tex("Sun", "8k_sun.jpg");
    tex("Mercury", "8k_mercury.jpg");
    // Venus: the cloud deck is the day map until the Magellan surface is baked; then it becomes the opaque
    // cloud layer over the radar map (only seen from beneath the clouds).
    tex("Venus", "4k_venus_atmosphere.jpg", "", "4k_venus_atmosphere.jpg");
    tex("Earth", "8k_earth_daymap.jpg", "8k_earth_nightmap.jpg", "8k_earth_clouds.jpg");
    tex("Moon", "8k_moon.jpg");
    tex("Mars", "8k_mars.jpg");
    tex("Jupiter", "8k_jupiter.jpg");
    tex("Saturn", "8k_saturn.jpg");
    tex("Uranus", "2k_uranus.jpg");
    tex("Neptune", "2k_neptune.jpg");
    m_bodies[saturn].texRing = "8k_saturn_ring_alpha.png";
    // NASA Blue Marble Next Generation, 500 m/px, baked by scripts/bake_earth_hires.py.
    m_bodies[earth].texDayTiles = "earth_hires/bmng_{}.dds";
    m_bodies[earth].tileIds = {"A1", "B1", "C1", "D1", "A2", "B2", "C2", "D2"};
    m_bodies[earth].tileCols = 4;
    m_bodies[earth].tileRows = 2;
    m_bodies[earth].texNightHires = "earth_hires/blackmarble_16k.dds";
    // NASA CGI Moon Kit (LROC WAC colour + LOLA elevation), baked by scripts/bake_moon_hires.py.
    {
        Body& moonBody = m_bodies[find("Moon")];
        moonBody.texDayHires = "moon_hires/moon_color_16k.dds";
        moonBody.texNormal = "moon_hires/moon_normal_64.dds";
        moonBody.texHeight = "moon_hires/moon_height_32.dds";
        moonBody.heightRangeFile = "moon_hires/moon_height.txt";
        moonBody.detailKind = 1.f;
        // LROC NAC terrain at the Apollo sites (scripts/bake_apollo_sites.py), used when baked.
        for (const char* key : {"apollo11", "apollo12", "apollo14", "apollo15", "apollo16", "apollo17"}) {
            SurfacePatch sp;
            sp.name = key;
            sp.dir = std::string("apollo_sites/") + key + "/";
            moonBody.patches.push_back(sp);
        }
        m_bodies[mars].detailKind = 2.f;
        // HiRISE terrain around Perseverance in Jezero crater (same bake script, OTHER_SITES).
        {
            SurfacePatch sp;
            sp.name = "jezero";
            sp.dir = "mars_sites/jezero/";
            m_bodies[mars].patches.push_back(sp);
        }
        m_bodies[find("Mercury")].detailKind = 1.f;
    }
    // USGS / NASA global mosaics and elevation models baked by scripts/bake_planets_hires.py. Each file is
    // optional: whatever is missing falls back to the maps above (or the procedural surface).
    auto hires = [&](const char* bodyName, const char* key, bool relief) {
        int i = find(bodyName);
        if (i < 0) return;
        Body& hb = m_bodies[i];
        const std::string dir = std::string(key) + "_hires/" + key;
        hb.texDayHires = dir + "_color.dds";
        if (hb.texDay.empty()) hb.texDay = hb.texDayHires; // bodies that were procedural
        if (relief) {
            hb.texNormal = dir + "_normal.dds";
            hb.texHeight = dir + "_height.dds";
            hb.heightRangeFile = dir + "_height.txt";
        }
        if (hb.detailKind <= 0.f && hb.type == BodyType::Rocky && hb.atmosphere <= 0.f) hb.detailKind = 1.f;
    };
    hires("Mars", "mars", true);
    hires("Mercury", "mercury", true);
    hires("Venus", "venus", false); // Magellan radar surface under the clouds
    for (const char* moonName : {"Io", "Europa", "Ganymede", "Callisto", "Rhea", "Triton", "Phobos", "Deimos"}) {
        std::string key = moonName;
        for (auto& ch : key) ch = (char)std::tolower((unsigned char)ch);
        hires(moonName, key.c_str(), false);
    }
    hires("Titan", "titan", false);
    m_bodies[saturn].ringInner = 66900.f / 140220.f; // the ring map spans the C ring to the A ring edge
}

int SolarSystem::find(const std::string& name) const {
    for (size_t i = 0; i < m_bodies.size(); ++i)
        if (m_bodies[i].name == name) return (int)i;
    return -1;
}

double SolarSystem::nowJulianDate() {
    using namespace std::chrono;
    const double unixSeconds = duration<double>(system_clock::now().time_since_epoch()).count();
    return unixSeconds / 86400.0 + 2440587.5;
}

// Geocentric Moon, ecliptic J2000 (km), from the principal terms of Meeus, Astronomical Algorithms
// ch. 47 (about 0.1 deg in longitude, 0.05 deg in latitude, a few hundred km in distance). Also returns
// the Moon's mean longitude (radians, J2000 ecliptic) for the rotation model.
glm::dvec3 moonGeocentric(double jd, double& meanLongitude) {
    const double T = (jd - 2451545.0) / 36525.0;
    const double d2r = glm::pi<double>() / 180.0;
    auto norm = [](double deg) { return std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0); };
    const double Lp = norm(218.3164477 + 481267.88123421 * T);
    const double D = norm(297.8501921 + 445267.1114034 * T) * d2r;
    const double M = norm(357.5291092 + 35999.0502909 * T) * d2r;
    const double Mp = norm(134.9633964 + 477198.8675055 * T) * d2r;
    const double F = norm(93.2720950 + 483202.0175233 * T) * d2r;
    const double E = 1.0 - 0.002516 * T;
    const double sl = 6288774 * std::sin(Mp) + 1274027 * std::sin(2 * D - Mp) + 658314 * std::sin(2 * D) +
                      213618 * std::sin(2 * Mp) - 185116 * E * std::sin(M) - 114332 * std::sin(2 * F) +
                      58793 * std::sin(2 * D - 2 * Mp) + 57066 * E * std::sin(2 * D - M - Mp) +
                      53322 * std::sin(2 * D + Mp) + 45758 * E * std::sin(2 * D - M) - 40923 * E * std::sin(M - Mp) -
                      34720 * std::sin(D) - 30383 * E * std::sin(M + Mp) + 15327 * std::sin(2 * D - 2 * F) -
                      12528 * std::sin(Mp + 2 * F) + 10980 * std::sin(Mp - 2 * F) + 10675 * std::sin(4 * D - Mp) +
                      10034 * std::sin(3 * Mp) + 8548 * std::sin(4 * D - 2 * Mp);
    const double sb = 5128122 * std::sin(F) + 280602 * std::sin(Mp + F) + 277693 * std::sin(Mp - F) +
                      173237 * std::sin(2 * D - F) + 55413 * std::sin(2 * D - Mp + F) + 46271 * std::sin(2 * D - Mp - F) +
                      32573 * std::sin(2 * D + F) + 17198 * std::sin(2 * Mp + F) + 9266 * std::sin(2 * D + Mp - F) +
                      8822 * std::sin(2 * Mp - F);
    const double sr = -20905355 * std::cos(Mp) - 3699111 * std::cos(2 * D - Mp) - 2955968 * std::cos(2 * D) -
                      569925 * std::cos(2 * Mp) + 48888 * E * std::cos(M) - 3149 * std::cos(2 * F) +
                      246158 * std::cos(2 * D - 2 * Mp) - 152138 * E * std::cos(2 * D - M - Mp) -
                      170733 * std::cos(2 * D + Mp) - 204586 * E * std::cos(2 * D - M) - 129620 * E * std::cos(M - Mp) +
                      108743 * std::cos(D) + 104755 * E * std::cos(M + Mp) + 10321 * std::cos(2 * D - 2 * F) +
                      79661 * std::cos(Mp - 2 * F) - 34782 * std::cos(4 * D - Mp) - 23210 * std::cos(3 * Mp) -
                      21636 * std::cos(4 * D - 2 * Mp);
    // Of-date ecliptic -> J2000 ecliptic: remove general precession in longitude.
    const double precession = 1.3969713 * T;
    const double lambda = (Lp + sl * 1e-6 - precession) * d2r;
    const double beta = sb * 1e-6 * d2r;
    const double dist = 385000.56 + sr * 1e-3;
    meanLongitude = (Lp - precession) * d2r;
    return {dist * std::cos(beta) * std::cos(lambda), dist * std::cos(beta) * std::sin(lambda), dist * std::sin(beta)};
}

glm::dvec3 SolarSystem::latLonToLocal(double latDeg, double lonDeg) {
    const double lat = glm::radians(latDeg), lon = glm::radians(lonDeg);
    return {std::cos(lat) * std::cos(lon), std::sin(lat), -std::cos(lat) * std::sin(lon)};
}

double SolarSystem::sunElevationDeg(int index, double latDeg, double lonDeg) const {
    const Body& b = m_bodies[index];
    const glm::dvec3 up = b.rotationD * latLonToLocal(latDeg, lonDeg);
    const glm::dvec3 toSun = glm::normalize(sunPosition() - (b.position + up * (b.radiusKm / kKmPerParsec)));
    return glm::degrees(std::asin(std::clamp(glm::dot(up, toSun), -1.0, 1.0)));
}

void SolarSystem::update(double jd) {
    const double days = jd - 2451545.0;
    const double pcPerKm = 1.0 / kKmPerParsec;
    m_jd = jd;
    // The JPL table gives the Earth-Moon barycentre; split it into Earth and Moon with a real lunar orbit.
    double moonMeanLon = 0.0;
    const glm::dvec3 moonGeoEcl = moonGeocentric(jd, moonMeanLon);
    const glm::dvec3 moonGeo = m_eclipticToEngine * moonGeoEcl * pcPerKm;
    const double kMoonMassFraction = 0.0121505856; // Moon / (Earth + Moon)

    for (auto& b : m_bodies) {
        glm::dvec3 ecl{0.0}; // ecliptic frame, world units
        if (b.jplIndex >= 0) {
            ecl = jplPosition(kJpl[b.jplIndex], jd) * (kKmPerAU * pcPerKm);
        } else if (b.parent >= 0) {
            double angle = b.orbitPhase + glm::two_pi<double>() * days / b.orbitPeriodDays;
            ecl = glm::dvec3(std::cos(angle), std::sin(angle), 0.0) * (b.orbitRadiusKm * pcPerKm);
        }
        glm::dvec3 rel = m_eclipticToEngine * ecl;
        b.position = (b.parent >= 0 ? m_bodies[b.parent].position : glm::dvec3(0.0)) + rel;
        if (b.name == "Earth") b.position -= moonGeo * kMoonMassFraction;
        if (b.name == "Moon") b.position = m_bodies[b.parent].position + moonGeo;

        // Spin about a tilted axis. Earth's pole in ecliptic coordinates is (0, sin e, cos e): the tilt is a
        // rotation of the ecliptic pole about the equinox direction by -obliquity (other planets: same
        // azimuth, an approximation).
        double spin = glm::two_pi<double>() * days * 24.0 / b.rotationPeriodHours;
        glm::dvec3 eclPole = m_eclipticToEngine * glm::dvec3(0.0, 0.0, 1.0);
        glm::dvec3 eclX = m_eclipticToEngine * glm::dvec3(1.0, 0.0, 0.0);
        glm::quat tilt = glm::angleAxis((float)glm::radians(-b.axialTiltDeg), glm::vec3(eclX));
        glm::vec3 axis = tilt * glm::vec3(eclPole);
        glm::quat frame = glm::rotation(glm::vec3(0.f, 1.f, 0.f), axis);
        if (b.name == "Moon") {
            // Tidally locked (Cassini's laws): the pole sits 1.54 deg from the ecliptic pole, and the
            // prime meridian tracks the Moon's mean longitude, so the real Earth direction swings about
            // it by the optical libration (+-7 deg) as the orbit speeds up and slows down.
            const glm::dvec3 poleEcl = glm::normalize(glm::dvec3(0.0, -std::sin(glm::radians(1.543)), std::cos(glm::radians(1.543))));
            const glm::dvec3 poleW = glm::normalize(m_eclipticToEngine * poleEcl);
            frame = glm::rotation(glm::vec3(0.f, 1.f, 0.f), glm::vec3(poleW));
            const glm::dvec3 toEarthMean = m_eclipticToEngine * glm::dvec3(-std::cos(moonMeanLon), -std::sin(moonMeanLon), 0.0);
            const glm::dvec3 dl = glm::inverse(glm::dquat(frame)) * toEarthMean;
            spin = std::atan2(-dl.z, dl.x);
        }
        if (b.name == "Earth") {
            // Anchor the prime meridian: the Earth Rotation Angle gives the equatorial direction of
            // Greenwich; local +x is longitude 0 on the maps.
            const double era = glm::two_pi<double>() * std::fmod(0.7790572732640 + 1.00273781191135448 * days, 1.0);
            const glm::dvec3 greenwichEq(std::cos(era), std::sin(era), 0.0);
            const glm::dvec3 greenwich = m_eclipticToEngine * (glm::inverse(m_eclipticToEq) * greenwichEq);
            const glm::dvec3 d = glm::inverse(glm::dquat(frame)) * greenwich; // in the tilted body frame
            spin = std::atan2(-d.z, d.x); // rot_y(spin) * x = (cos, 0, -sin)
        }
        // Build in double: a float quaternion moves a surface point on a 1700 km globe by ~20 cm per
        // rounding step, which makes landers and close-range ground detail jitter.
        const glm::dvec3 axisD = glm::normalize(glm::dvec3(frame * glm::vec3(0.f, 1.f, 0.f)));
        const glm::dquat frameD = glm::rotation(glm::dvec3(0.0, 1.0, 0.0), axisD);
        b.rotationD = glm::normalize(frameD * glm::angleAxis(std::fmod(spin, glm::two_pi<double>()), glm::dvec3(0.0, 1.0, 0.0)));
        b.rotation = glm::quat(b.rotationD);
    }
}

uint32_t SolarSystem::buildGpuList(const glm::dvec3& cameraPos, std::vector<BodyGpu>& out) const {
    out.clear();
    const double pcPerKm = 1.0 / kKmPerParsec;
    for (const auto& b : m_bodies) {
        BodyGpu g;
        g.posRadius = glm::vec4(glm::vec3(b.position - cameraPos), (float)(b.radiusKm * pcPerKm));
        g.rotation = glm::vec4(b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w);
        // color.w: ring outer radius in planet radii (0 = no ring); params.w: ring inner ratio (non-Sun);
        // tex.w: ring alpha map, so the surface can look up the ring's shadow.
        const bool hasRing = b.ringInner > 0.f;
        g.color = glm::vec4(b.color, hasRing ? (float)(b.ringOuterKm / b.radiusKm) : 0.f);
        g.params = glm::vec4((float)b.type, b.seed, b.atmosphere,
                             b.type == BodyType::Sun ? 1.f : (hasRing ? b.ringInner : 0.f));
        g.tex = glm::ivec4(b.texDayIndex, b.texNightIndex, b.texCloudsIndex, hasRing ? b.texRingIndex : -1);
        g.tiles = glm::ivec4(b.texTileBase, b.tileCols, b.tileRows, b.texCloudsLiveIndex);
        g.relief = glm::ivec4(b.texNormalIndex, b.texHeightIndex, -1, -1);
        g.reliefParams = glm::vec4(b.heightMinKm, b.heightRangeKm, 1.f, b.detailKind);
        const bool hasAtmo = b.atmoClass >= 0 && m_atmoLutBase >= 0;
        g.atmoTex = hasAtmo ? glm::ivec4(m_atmoLutBase + 2 * b.atmoClass, m_atmoLutBase + 2 * b.atmoClass + 1, b.atmoClass, 0)
                            : glm::ivec4(-1);
        g.atmoParams = glm::vec4(hasAtmo ? (float)((b.radiusKm + b.atmoTopKm) / b.radiusKm) : 0.f, 0.f, 0.f, 0.f);
        g.patchTex = glm::ivec4(-1);
        g.patchParams = glm::vec4(0.f);
        if (b.activePatch >= 0 && b.activePatch < (int)b.patches.size()) {
            const SurfacePatch& sp = b.patches[b.activePatch];
            g.patchTex = glm::ivec4(sp.albedoIndex, sp.normalIndex, sp.heightIndex, -1);
            g.patchParams = glm::vec4(sp.heightMinM, sp.heightRangeM, sp.metresPerTexel, 1.f);
        }
        out.push_back(g);
    }
    const uint32_t sphereCount = (uint32_t)out.size();
    for (const auto& b : m_bodies) {
        if (b.ringInner <= 0.f) continue;
        BodyGpu g;
        g.posRadius = glm::vec4(glm::vec3(b.position - cameraPos), (float)(b.ringOuterKm * pcPerKm));
        g.rotation = glm::vec4(b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w);
        g.color = glm::vec4(0.78f, 0.70f, 0.58f, (float)(b.radiusKm / b.ringOuterKm)); // w: planet radius / ring outer
        g.params = glm::vec4((float)BodyType::Ring, b.seed, b.ringInner, 0.f);
        g.tex = glm::ivec4(b.texRingIndex, -1, -1, -1);
        g.tiles = glm::ivec4(-1, 0, 0, 0);
        g.relief = glm::ivec4(-1);
        g.reliefParams = glm::vec4(0.f);
        g.patchTex = glm::ivec4(-1);
        g.patchParams = glm::vec4(0.f);
        g.atmoTex = glm::ivec4(-1);
        g.atmoParams = glm::vec4(0.f);
        out.push_back(g);
    }
    return sphereCount;
}

void SolarSystem::viewpoint(int index, double distanceRadii, glm::dvec3& outPos, glm::quat& outOrient) const {
    const Body& b = m_bodies[index];
    const double r = b.radiusKm / kKmPerParsec;
    glm::dvec3 toSun = glm::normalize(sunPosition() - b.position);
    if (index == 0) toSun = m_eclipticToEngine * glm::dvec3(1.0, 0.0, 0.0);
    glm::dvec3 pole = m_eclipticToEngine * glm::dvec3(0.0, 0.0, 1.0);
    glm::dvec3 side = glm::normalize(glm::cross(pole, toSun));
    // Sit off to the side and slightly sunward so the lit limb faces us, a little above the plane.
    glm::dvec3 dir = glm::normalize(side * 0.75 + toSun * 0.55 + pole * 0.3);
    outPos = b.position + dir * (r * distanceRadii);
    glm::vec3 look = glm::normalize(glm::vec3(b.position - outPos));
    outOrient = glm::quatLookAt(look, glm::vec3(pole));
}

} // namespace space
