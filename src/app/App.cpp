#include "app/App.h"
#include "core/Log.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "gfx/GltfModel.h"
#include "scene/FlagModel.h"
#include <fstream>

#include <cctype>
#include <windows.h>
#include <future>
#include <unordered_map>

namespace space {

std::string formatDistance(double pc) {
    char buf[64];
    const double km = pc * kKmPerParsec;
    const double au = pc * kAuPerParsec;
    if (km < 2.0e6) std::snprintf(buf, sizeof buf, "%.0f km", km);
    else if (au < 0.05) std::snprintf(buf, sizeof buf, "%.3g km", km);
    else if (pc < 0.05) std::snprintf(buf, sizeof buf, "%.3f AU", au);
    else if (pc < 1000.0) std::snprintf(buf, sizeof buf, "%.2f pc (%.1f ly)", pc, pc * 3.26156);
    else std::snprintf(buf, sizeof buf, "%.2f kpc", pc / 1000.0);
    return buf;
}

App::App(int argc, char** argv) {
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
#ifdef SPACE_DEBUG
    spdlog::set_level(spdlog::level::debug);
#endif
    // Debug switches for isolating passes.
    int width = 0, height = 0; // 0 = borderless fullscreen
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--size" && i + 2 < argc) {
            width = std::atoi(argv[++i]);
            height = std::atoi(argv[++i]);
        } else if (a == "--no-sky") m_settings.drawSky = false;
        else if (a == "--no-stars") m_settings.drawStars = false;
        else if (a == "--no-volumetrics") m_settings.volumetrics = false;
        else if (a == "--no-bodies") m_settings.drawBodies = false;
        else if (a == "--no-bloom") m_settings.bloomStrength = 0.f;
        else if (a == "--goto" && i + 1 < argc) { m_startBody = argv[++i]; m_startTour = false; }
        else if (a == "--surface" && i + 4 < argc) {
            m_startBody = argv[++i];
            m_startLat = std::atof(argv[++i]);
            m_startLon = std::atof(argv[++i]);
            m_startAltKm = std::atof(argv[++i]);
            m_startSurface = true;
            m_startTour = false;
        }
        else if (a == "--diag") m_diagFrames = {20, 450, 650};
        else if (a == "--diag-time" && i + 1 < argc) m_diagTime = std::atof(argv[++i]);
        else if (a == "--time-scale" && i + 1 < argc) m_timeScale = (float)std::atof(argv[++i]);
        else if (a == "--sdr") m_settings.hdrOutput = false;
        else if (a == "--tour") m_startTour = true;
        else if (a == "--no-tour") m_startTour = false;
        else if (a == "--tour-start" && i + 1 < argc) m_tourStart = std::atoi(argv[++i]);
        else if (a == "--ui") m_showUi = true;
        else if (a == "--windowed") { if (width <= 0) { width = 1600; height = 900; } }
        else if (a == "--tour-pace" && i + 1 < argc) m_tourSpeed = (float)std::atof(argv[++i]);
        else LOG_WARN("Unknown argument {}", a);
    }

    m_window = std::make_unique<Window>(width, height, "Space", m_input);

#ifdef SPACE_VALIDATION
    const bool validation = true;
#else
    const bool validation = false;
#endif
    m_ctx.init(*m_window, validation);
    m_renderer.init(m_ctx, *m_window);

    m_epochJd = SolarSystem::nowJulianDate();
    m_solar.update(m_epochJd);
    loadTextures();
    m_crafts.build(m_solar);
    loadModels();
    m_crafts.update(m_solar, m_epochJd);
    m_tour.setCrafts(&m_crafts);

    m_haveGaia = std::filesystem::exists(assetDirectory() / "gaia" / "gaia.stars");
    loadCatalog(m_haveGaia);

    m_audio.start();
    {
        auto body = [&](const char* n) { return Tour::Stop{m_solar.find(n), -1}; };
        auto craft = [&](const char* n) {
            int c = m_crafts.find(n);
            return Tour::Stop{c >= 0 ? m_crafts.crafts()[c].parent : -1, c};
        };
        std::vector<Tour::Stop> stops = {
            craft("Saturn V"), craft("ISS"), body("Moon"), craft("Apollo 11 (Tranquility Base)"), craft("Apollo 15"), craft("Apollo 17"), craft("LRO"),
            body("Mars"), craft("Perseverance"), craft("MRO"), body("Jupiter"), craft("Juno"), body("Io"),
            body("Saturn"), Tour::Stop{m_solar.find("Saturn"), -1, 1}, body("Titan"), craft("Huygens"), body("Uranus"), body("Neptune"),
            craft("Voyager 1"), body("Sun"), craft("Parker Solar Probe"), body("Mercury"), body("Venus"),
            craft("JWST"), craft("Hubble"), craft("Apollo-Soyuz"), body("Earth")};
        std::vector<Tour::Stop> valid;
        for (auto& s : stops)
            if (s.body >= 0 || s.craft >= 0) valid.push_back(s);
        m_tour.setStops(valid);
    }
    loadTle();
    loadLiveClouds();
    startLiveFetch();
    if (m_tourStart >= 0) m_tour.start(m_solar, m_camera, (size_t)m_tourStart);
    else if (m_startTour) {
        // The opening is dawn over Cape Canaveral, today: Sun ~5 deg below the Cape's horizon at the start,
        // rising as we drift east. Sub-solar longitude then is about -80.6 + 95 deg east, i.e. 10:58 UTC.
        const double today = std::floor(m_epochJd - 0.5) + 0.5; // 00:00 UTC
        double jd = today + 10.97 / 24.0;
        if (jd < m_epochJd - 0.6) jd += 1.0;
        m_simDays = jd - m_epochJd;
        m_solar.update(jd);
        m_crafts.update(m_solar, jd);
        m_tour.startWithIntro(m_solar, m_solar.find("Earth"));
    }
    LOG_INFO("Ready. Hold right mouse to look, WASD/RF to fly, scroll to change speed, 0-9 visit bodies, F1 UI.");
}

App::~App() {
    m_renderer.shutdown();
    m_ctx.shutdown();
    m_window.reset();
}

void App::loadTextures() {
    // Collect the distinct map names referenced by the bodies, decode them in parallel, upload in order.
    std::vector<std::string> names;
    auto want = [&](const std::string& n) {
        if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    const std::filesystem::path dir = assetDirectory() / "textures";
    auto tileName = [](const std::string& pattern, const std::string& id) {
        const size_t at = pattern.find("{}");
        return at == std::string::npos ? pattern : pattern.substr(0, at) + id + pattern.substr(at + 2);
    };
    // Pre-baked tile sets (DDS) are used only when every tile is on disk, otherwise the 8K map stands in.
    std::vector<std::string> tileNames; // uploaded consecutively so a body can address them by base index
    for (Body& b : m_solar.bodies()) {
        if (b.texDayTiles.empty()) continue;
        bool complete = !b.tileIds.empty();
        for (const auto& id : b.tileIds)
            if (!std::filesystem::exists(dir / tileName(b.texDayTiles, id))) complete = false;
        if (!complete) {
            LOG_INFO("{}: high-res tiles missing, using {} (run scripts/bake_earth_hires.py)", b.name, b.texDay);
            b.texDayTiles.clear();
        }
        if (!b.texNightHires.empty() && !std::filesystem::exists(dir / b.texNightHires)) b.texNightHires.clear();
    }
    // Pre-baked high-resolution maps replace the regular ones when present (scripts/bake_*_hires.py).
    for (Body& b : m_solar.bodies()) {
        if (!b.texDayHires.empty()) {
            if (std::filesystem::exists(dir / b.texDayHires)) {
                b.texDay = b.texDayHires;
            } else {
                if (b.texDay == b.texDayHires) b.texDay.clear(); // no regular map either: stay procedural
                LOG_INFO("{}: high-res map not baked, using {}", b.name, b.texDay.empty() ? "a procedural surface" : b.texDay);
            }
        }
        if (!b.texNormal.empty() && !std::filesystem::exists(dir / b.texNormal)) b.texNormal.clear();
        if (!b.texHeight.empty() && !std::filesystem::exists(dir / b.texHeight)) b.texHeight.clear();
        if (!b.texHeight.empty() && !b.heightRangeFile.empty()) {
            std::ifstream f(dir / b.heightRangeFile);
            float lo = 0.f, hi = 0.f;
            if (f >> lo >> hi) {
                b.heightMinKm = lo;
                b.heightRangeKm = hi - lo;
            } else {
                b.texHeight.clear();
            }
        }
    }
    for (const Body& b : m_solar.bodies()) {
        want(b.texNormal);
        want(b.texHeight);
        want(b.texDay); // the 8K map stays loaded as the fallback if a tile upload fails
        want(!b.texNightHires.empty() ? b.texNightHires : b.texNight);
        want(b.texClouds);
        want(b.texRing);
    }
    auto isDds = [](const std::string& n) { return n.size() > 4 && n.compare(n.size() - 4, 4, ".dds") == 0; };
    struct Loaded {
        gfx::DecodedImage image;
        gfx::DdsImage dds;
    };
    auto load = [&](const std::string& n) {
        return std::async(std::launch::async, [path = dir / n, dds = isDds(n)] {
            Loaded l;
            if (dds) l.dds = gfx::loadDds(path);
            else l.image = gfx::decodeImage(path);
            return l;
        });
    };
    std::vector<std::future<Loaded>> jobs;
    for (const auto& n : names) jobs.push_back(load(n));

    std::unordered_map<std::string, int> index;
    for (size_t i = 0; i < names.size(); ++i) {
        Loaded l = jobs[i].get();
        gfx::Texture tex;
        const bool srgb = names[i].find("ring_alpha") == std::string::npos; // masks stay linear
        const bool ok = l.dds.ok() ? tex.uploadDds(m_ctx, l.dds, names[i].c_str())
                                   : tex.upload(m_ctx, l.image, srgb, names[i].c_str());
        if (ok) index[names[i]] = m_renderer.addTexture(std::move(tex));
    }
    auto resolve = [&](const std::string& n) { auto it = index.find(n); return it == index.end() ? -1 : it->second; };
    for (Body& b : m_solar.bodies()) {
        b.texDayIndex = resolve(b.texDay);
        b.texNightIndex = resolve(!b.texNightHires.empty() ? b.texNightHires : b.texNight);
        b.texCloudsIndex = resolve(b.texClouds);
        b.texRingIndex = resolve(b.texRing);
        b.texNormalIndex = resolve(b.texNormal);
        b.texHeightIndex = resolve(b.texHeight);
        if (b.texNormalIndex >= 0) LOG_INFO("{}: relief maps online (normals{})", b.name, b.texHeightIndex >= 0 ? " + heights" : "");
    }
    LOG_INFO("Loaded {} of {} surface maps", index.size(), names.size());

    // Tile sets: gigabytes of BC7, read from disk in parallel and uploaded back-to-back.
    for (Body& b : m_solar.bodies()) {
        if (b.texDayTiles.empty()) continue;
        std::vector<std::future<gfx::DdsImage>> reads;
        for (const auto& id : b.tileIds)
            reads.push_back(std::async(std::launch::async, [path = dir / tileName(b.texDayTiles, id)] { return gfx::loadDds(path); }));
        int base = -1;
        bool ok = true;
        for (size_t i = 0; i < reads.size() && ok; ++i) {
            gfx::DdsImage dds = reads[i].get();
            gfx::Texture tex;
            const std::string label = b.name + " tile " + b.tileIds[i];
            const int idx = tex.uploadDds(m_ctx, dds, label.c_str()) ? m_renderer.addTexture(std::move(tex), true) : -1;
            if (idx < 0 || (base >= 0 && idx != base + (int)i)) ok = false;
            if (base < 0) base = idx;
        }
        if (ok && base >= 0) {
            b.texTileBase = base;
            LOG_INFO("{}: {}x{} high-res day tiles online", b.name, b.tileCols, b.tileRows);
        } else {
            LOG_WARN("{}: tile upload failed, using {}", b.name, b.texDay);
            b.texTileBase = -1;
        }
    }
    if (index.size() < names.size()) LOG_WARN("Missing maps: run scripts/fetch_textures.py");

    // Local terrain patches (landing sites): all four files must be present.
    for (Body& b : m_solar.bodies()) {
        std::vector<SurfacePatch> ready;
        for (SurfacePatch sp : b.patches) {
            const auto base = dir / sp.dir;
            std::ifstream info(base / "patch.txt");
            double hMin = 0.0, hMax = 0.0;
            if (!info || !(info >> sp.lonMinDeg >> sp.latMinDeg >> sp.lonSpanDeg >> sp.latSpanDeg >> hMin >> hMax)) continue;
            gfx::DdsImage albedo = gfx::loadDds(base / "patch_albedo.dds");
            gfx::DdsImage normal = gfx::loadDds(base / "patch_normal.dds");
            gfx::DdsImage height = gfx::loadDds(base / "patch_height.dds");
            if (!albedo.ok() || !normal.ok() || !height.ok()) continue;
            gfx::Texture ta, tn, th;
            const std::string label = b.name + " " + sp.name;
            if (!ta.uploadDds(m_ctx, albedo, (label + " albedo").c_str()) || !tn.uploadDds(m_ctx, normal, (label + " normal").c_str()) ||
                !th.uploadDds(m_ctx, height, (label + " height").c_str()))
                continue;
            sp.albedoIndex = m_renderer.addTexture(std::move(ta), true);
            sp.normalIndex = m_renderer.addTexture(std::move(tn), true);
            sp.heightIndex = m_renderer.addTexture(std::move(th), true);
            sp.heightMinM = (float)hMin;
            sp.heightRangeM = (float)(hMax - hMin);
            const double radiusM = b.radiusKm * 1000.0;
            sp.metresPerTexel = (float)(glm::radians(sp.latSpanDeg) * radiusM / std::max(height.height, 1));
            LOG_INFO("{}: {} terrain patch online ({:.2f} x {:.2f} km, {:.2f} m/texel heights)", b.name, sp.name,
                     glm::radians(sp.lonSpanDeg) * radiusM * std::cos(glm::radians(sp.latMinDeg + sp.latSpanDeg * 0.5)) / 1000.0,
                     glm::radians(sp.latSpanDeg) * radiusM / 1000.0, sp.metresPerTexel);
            ready.push_back(sp);
        }
        b.patches = std::move(ready);
    }
}

void App::loadModels() {
    // Decode every distinct model in parallel (Draco + WebP are CPU heavy), then upload in order.
    const std::filesystem::path dir = assetDirectory() / "models";
    for (Craft& c : m_crafts.crafts()) {
        if (c.model == "__flag__" || c.modelFallback.empty() || std::filesystem::exists(dir / c.model)) continue;
        LOG_INFO("{}: {} missing, using {} (run scripts/fetch_models.py)", c.name, c.model, c.modelFallback);
        c.model = c.modelFallback;
        c.modelYawDeg = c.modelPitchDeg = 0.f;
    }
    std::vector<std::string> names;
    for (const Craft& c : m_crafts.crafts())
        if (std::find(names.begin(), names.end(), c.model) == names.end()) names.push_back(c.model);
    std::vector<std::future<std::pair<bool, gfx::ModelData>>> jobs;
    for (const auto& n : names)
        jobs.push_back(std::async(std::launch::async, [path = dir / n, name = n] {
            gfx::ModelData data;
            if (name == "__flag__" || name == "__plume__" || name == "__trail__") {
                data = name == "__flag__" ? makeApolloFlagModel()
                     : name == "__plume__" ? makeExhaustPlumeModel() : makeExhaustTrailModel();
                return std::make_pair(true, std::move(data));
            }
            bool ok = gfx::loadGlb(path, data);
            return std::make_pair(ok, std::move(data));
        }));
    std::unordered_map<std::string, int> loaded;
    for (size_t i = 0; i < names.size(); ++i) {
        auto [ok, data] = jobs[i].get();
        int index = ok ? m_renderer.addModel(data) : -1;
        loaded[names[i]] = index;
        if (index >= 0) {
            if ((int)m_crafts.modelNativeExtent.size() <= index) m_crafts.modelNativeExtent.resize(index + 1, 1.f);
            m_crafts.modelNativeExtent[index] = m_renderer.modelExtent(index);
            if ((int)m_crafts.modelNativeMinY.size() <= index) m_crafts.modelNativeMinY.resize(index + 1, 0.f);
            m_crafts.modelNativeMinY[index] = m_renderer.modelMinY(index);
        }
    }
    for (Craft& c : m_crafts.crafts()) {
        c.modelIndex = loaded[c.model];
        if (c.name == "Saturn V plume" || c.name == "Saturn V trail") {
            // Centre the flame on the rocket's axis, at its base (the rocket model is not centred on its origin).
            const int rocket = m_crafts.find("Saturn V");
            const int rm = rocket >= 0 ? loaded[m_crafts.crafts()[rocket].model] : -1;
            if (rm >= 0) {
                const glm::vec3 centre = m_renderer.modelBoundsCenter(rm);
                const float scale = m_crafts.crafts()[rocket].sizeMeters / std::max(m_renderer.modelExtent(rm), 1e-6f);
                // Metres (the plume model is in metres); raised 8 m so the flame starts inside the engine bells.
                c.modelOffset = glm::vec3(centre.x * scale, 8.f, centre.z * scale);
            }
        }
        // Generated scenery is built in metres: keep it at its true size.
        if ((c.model == "__flag__" || c.model == "__plume__" || c.model == "__trail__") && c.modelIndex >= 0)
            c.sizeMeters = m_renderer.modelExtent(c.modelIndex);
    }
    int ok = 0;
    for (const auto& kv : loaded) ok += kv.second >= 0;
    LOG_INFO("Spacecraft: {} of {} models loaded, {} craft placed", ok, loaded.size(), m_crafts.crafts().size());
    if (ok < (int)loaded.size()) LOG_WARN("Missing models: run scripts/fetch_models.py");
}

void App::goToCraft(int index) {
    if (index < 0 || index >= (int)m_crafts.crafts().size()) return;
    m_tour.stop("go to spacecraft");
    // Landing sites are shown in morning light: jump the clock forward to local sunrise-plus if needed.
    const double jdNow = m_epochJd + m_simDays;
    if (m_crafts.crafts()[index].placement == CraftPlacement::Orbit) {
        m_crafts.rephaseForDaylight(m_solar, index); // over the lit side, without touching the clock
        m_crafts.update(m_solar, jdNow);
    }
    const double jdLit = m_crafts.daylightJulianDate(m_solar, index, jdNow);
    if (jdLit != jdNow) {
        m_simDays = jdLit - m_epochJd;
        m_solar.update(jdLit);
        m_crafts.update(m_solar, jdLit);
        LOG_INFO("{}: clock moved {:.1f} days forward to local morning", m_crafts.crafts()[index].name, jdLit - jdNow);
    }
    m_crafts.viewpoint(m_solar, index, m_crafts.viewDistanceSizes(index), m_camera.position, m_camera.orientation);
    m_camera.speed = m_crafts.crafts()[index].sizeMeters / (kKmPerParsec * 1000.0) * 0.4;
    if (m_timeScale > 0.003f) m_timeScale = 0.00025f; // orbiters are best watched near real time
}

void App::loadCatalog(bool gaia) {
    if (gaia && loadGaiaCatalog(assetDirectory() / "gaia" / "gaia.stars", m_catalog)) {
        m_useGaia = true;
        m_renderer.setVolumes(makeGalacticVolumes());
        m_settings.starBrightness = 60.f; // planets are lit to ~1; keep the stars from drowning them
        int start = m_solar.find(m_startBody);
        int startCraft = start < 0 ? m_crafts.find(m_startBody) : -1;
        if (startCraft >= 0) goToCraft(startCraft);
        else goToBody(start >= 0 ? start : m_solar.find("Earth"));
        if (m_startSurface && start >= 0) goToSurface(start, m_startLat, m_startLon, m_startAltKm);
    } else {
        m_catalog = makeProceduralCatalog();
        m_useGaia = false;
        m_renderer.setVolumes(makeProceduralVolumes());
        m_settings.starBrightness = m_catalog.defaultBrightness;
        // Start inside the disc, a little above the plane, looking toward the core.
        m_camera.position = {4.5, 0.9, 7.5};
        glm::vec3 toCore = glm::normalize(glm::vec3(0.f, -0.15f, 0.f) - glm::vec3(m_camera.position));
        m_camera.orientation = glm::quatLookAt(toCore, glm::vec3(0.f, 1.f, 0.f));
        m_camera.speed = m_catalog.defaultSpeed;
    }
    m_renderer.setStars(m_catalog.stars);

    if (m_useGaia && m_constellations.load(assetDirectory() / "constellations.txt",
                                           assetDirectory() / "gaia" / "hip_index.bin", m_catalog.stars)) {
        m_renderer.setConstellationLines(m_constellations.vertices());
        m_conHighlight.assign(m_constellations.figures().size(), 0.f);
        m_conTarget.assign(m_constellations.figures().size(), 0.f);
    } else {
        m_renderer.setConstellationLines({});
        m_conHighlight.clear();
        m_conTarget.clear();
    }
}

void App::lookAt(const glm::vec3& dir) {
    m_tour.stop("constellation finder");
    m_lookActive = true;
    m_lookDir = glm::normalize(dir);
    m_lookFrom = m_camera.orientation;
    m_lookT = 0.f;
}

void App::updateConstellations(double dt) {
    for (size_t i = 0; i < m_conHighlight.size(); ++i) {
        float target = m_conTarget[i];
        float rate = target > m_conHighlight[i] ? 8.f : 3.f;
        m_conHighlight[i] += (target - m_conHighlight[i]) * (1.f - std::exp(-(float)dt * rate));
    }
    m_frameScene.constellationHighlight = m_conHighlight;
    m_frameScene.constellationIntensity = m_conShow ? m_conIntensity : 0.f;

    if (m_lookActive) {
        m_lookT += (float)dt;
        const float s = std::clamp(m_lookT / 1.6f, 0.f, 1.f);
        const float e = s * s * (3.f - 2.f * s);
        glm::vec3 up = m_camera.up();
        if (std::abs(glm::dot(up, m_lookDir)) > 0.95f) up = glm::vec3(0.f, 1.f, 0.f);
        m_camera.orientation = glm::slerp(m_lookFrom, glm::quatLookAt(m_lookDir, up), e);
        if (s >= 1.f) m_lookActive = false;
    }
}

void App::drawConstellationUi() {
    if (m_constellations.empty()) return;
    if (!ImGui::CollapsingHeader("constellations")) return;
    ImGui::Checkbox("show figures", &m_conShow);
    ImGui::SameLine();
    ImGui::Checkbox("labels", &m_conLabels);
    ImGui::SliderFloat("figure brightness", &m_conIntensity, 0.f, 2.f);
    ImGui::InputTextWithHint("##confilter", "find (name or abbreviation)", m_conFilter, sizeof m_conFilter);
    std::string filter = m_conFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    ImGui::BeginChild("conlist", ImVec2(0.f, 150.f), ImGuiChildFlags_Borders);
    const auto& figs = m_constellations.figures();
    for (size_t i = 0; i < figs.size(); ++i) {
        std::string label = figs[i].latin + " (" + figs[i].english + ")";
        std::string lower = label + " " + figs[i].abbr;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
        const bool selected = (int)i == m_conSelected;
        if (ImGui::Selectable(label.c_str(), selected)) {
            m_conSelected = selected ? -1 : (int)i;
            std::fill(m_conTarget.begin(), m_conTarget.end(), 0.f);
            if (m_conSelected >= 0) {
                m_conTarget[i] = 1.f;
                lookAt(figs[i].direction);
            }
        }
        if (ImGui::IsItemHovered()) m_conTarget[i] = std::max(m_conTarget[i], 0.45f);
    }
    ImGui::EndChild();
    if (m_conSelected >= 0) {
        ImGui::Text("%s", figs[m_conSelected].latin.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("look again")) lookAt(figs[m_conSelected].direction);
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) {
            m_conSelected = -1;
            std::fill(m_conTarget.begin(), m_conTarget.end(), 0.f);
        }
    }
}

void App::drawLabels() {
    if (m_constellations.empty() || !m_conShow) return;
    int w = 0, h = 0;
    m_window->framebufferSize(w, h);
    if (w == 0 || h == 0) return;
    const glm::mat4 vp = render::makeViewProj(m_renderCamera, (float)w / (float)h);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const auto& figs = m_constellations.figures();

    // Hover: the figure whose lines pass closest to the mouse lights up and shows its name.
    ImGuiIO& io = ImGui::GetIO();
    int hovered = -1;
    if (!io.WantCaptureMouse && !m_window->cursorCaptured() && io.MousePos.x >= 0.f) {
        const glm::vec3 cam = glm::vec3(m_camera.position);
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        auto project = [&](const glm::vec3& p, glm::vec2& out) {
            glm::vec4 c = vp * glm::vec4(p - cam, 1.f);
            if (c.w <= 0.f) return false;
            out = glm::vec2((c.x / c.w * 0.5f + 0.5f) * (float)w, (c.y / c.w * 0.5f + 0.5f) * (float)h);
            return true;
        };
        float best = 14.f * 14.f; // pixels squared
        const auto& verts = m_constellations.vertices();
        for (size_t i = 0; i + 1 < verts.size(); i += 2) {
            glm::vec2 a, b;
            if (!project(verts[i].position, a) || !project(verts[i + 1].position, b)) continue;
            glm::vec2 ab = b - a;
            float t = glm::dot(mouse - a, ab) / std::max(glm::dot(ab, ab), 1e-6f);
            glm::vec2 closest = a + ab * std::clamp(t, 0.f, 1.f);
            float d2 = glm::dot(mouse - closest, mouse - closest);
            if (d2 < best) {
                best = d2;
                hovered = (int)verts[i].constellation;
            }
        }
    }
    for (size_t i = 0; i < figs.size(); ++i)
        if ((int)i != m_conSelected) m_conTarget[i] = (int)i == hovered ? 0.55f : 0.f;
    if (hovered >= 0 && hovered != m_conSelected) {
        const std::string name = figs[hovered].latin + "  (" + figs[hovered].english + ")";
        ImVec2 pos(io.MousePos.x + 14.f, io.MousePos.y + 10.f);
        dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, 200), name.c_str());
        dl->AddText(pos, IM_COL32(255, 240, 200, 255), name.c_str());
    }
    for (size_t i = 0; i < figs.size(); ++i) {
        const bool lit = m_conHighlight[i] > 0.05f;
        if (!m_conLabels && !lit) continue;
        glm::vec4 clip = vp * glm::vec4(figs[i].direction * 1e4f, 1.f);
        if (clip.w <= 0.f) continue;
        glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (std::abs(ndc.x) > 1.1f || std::abs(ndc.y) > 1.1f) continue;
        ImVec2 pos((ndc.x * 0.5f + 0.5f) * (float)w, (ndc.y * 0.5f + 0.5f) * (float)h);
        const float a = lit ? 0.55f + 0.45f * m_conHighlight[i] : 0.35f;
        const std::string& text = lit ? figs[i].latin : figs[i].abbr;
        ImVec2 size = ImGui::CalcTextSize(text.c_str());
        pos.x -= size.x * 0.5f;
        dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), IM_COL32(0, 0, 0, (int)(a * 200)), text.c_str());
        dl->AddText(pos, IM_COL32(230, 225, 255, (int)(a * 255)), text.c_str());
    }
}

std::string App::simDateString() const {
    // Julian date -> calendar (Meeus), UTC.
    double jd = m_epochJd + m_simDays + 0.5;
    const long Z = (long)std::floor(jd);
    const double F = jd - Z;
    long A = Z;
    if (Z >= 2299161) {
        const long alpha = (long)std::floor((Z - 1867216.25) / 36524.25);
        A = Z + 1 + alpha - alpha / 4;
    }
    const long B = A + 1524, C = (long)std::floor((B - 122.1) / 365.25), D = (long)std::floor(365.25 * C);
    const long E = (long)std::floor((B - D) / 30.6001);
    const double day = B - D - std::floor(30.6001 * E) + F;
    const int month = (int)(E < 14 ? E - 1 : E - 13);
    const long year = month > 2 ? C - 4716 : C - 4715;
    const int d = (int)day;
    const double hours = (day - d) * 24.0;
    const int h = (int)hours, m = (int)((hours - h) * 60.0);
    static const char* names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d %s %ld  %02d:%02d UTC", d, names[std::clamp(month - 1, 0, 11)], year, h, m);
    return buf;
}

void App::drawOverlay(double dt) {
    int w = 0, h = 0;
    m_window->framebufferSize(w, h);
    if (w == 0 || h == 0 || !m_useGaia) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // The cut between tour stops: black over everything but the text.
    if (const float fade = m_tour.fade(); fade > 0.f)
        dl->AddRectFilled(ImVec2(0.f, 0.f), ImVec2((float)w, (float)h), IM_COL32(0, 0, 0, (int)(fade * 255.f)));
    ImFont* font = m_renderer.uiFont();
    ImFont* title = m_renderer.titleFont();
    const float scale = m_renderer.uiScale();
    const float pad = 28.f * scale;
    auto text = [&](ImFont* f, float size, ImVec2 pos, ImU32 col, const char* str) {
        dl->AddText(f, size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), IM_COL32(0, 0, 0, (col >> 24) * 3 / 4), str);
        dl->AddText(f, size, pos, col, str);
    };
    auto width = [&](ImFont* f, float size, const char* str) { return f->CalcTextSizeA(size, FLT_MAX, 0.f, str).x; };
    const float bodyPx = 19.f * scale, smallPx = 15.f * scale, bigPx = 30.f * scale;

    // Top left: where we are and when.
    int nearest = -1;
    double best = 1e300;
    for (size_t i = 0; i < m_solar.bodies().size(); ++i) {
        const Body& b = m_solar.body((int)i);
        const double d = glm::length(b.position - m_camera.position) - b.radiusKm / kKmPerParsec;
        if (d < best) { best = d; nearest = (int)i; }
    }
    if (nearest >= 0 && !m_showUi) {
        char line[128];
        std::snprintf(line, sizeof line, "%s   %s", m_solar.body(nearest).name.c_str(), formatDistance(std::max(best, 0.0)).c_str());
        text(font, bigPx, ImVec2(pad, pad), IM_COL32(240, 240, 250, 220), line);
        text(font, smallPx, ImVec2(pad, pad + bigPx + 4.f * scale), IM_COL32(200, 205, 220, 160), simDateString().c_str());
    }

    // Bottom right: the few controls that matter.
    {
        const char* hint = m_tour.active() ? "left / right  previous / next stop     T  leave the tour     right mouse  look around     F1  settings"
                                           : "T  tour     right mouse  look     W A S D  fly     scroll  speed     F1  settings";
        const float tw = width(font, smallPx, hint);
        text(font, smallPx, ImVec2(w - pad - tw, h - pad - smallPx), IM_COL32(200, 205, 220, 120), hint);
    }

    // Title over the opening.
    if (m_tour.introActive()) {
        const double p = m_tour.introProgress();
        const float a = (float)std::clamp(std::min((p - 0.08) / 0.15, (0.85 - p) / 0.15), 0.0, 1.0);
        if (a > 0.f) {
            const float titlePx = 64.f * scale, subPx = 20.f * scale;
            const char* t1 = "SPACE";
            const char* t2 = "the solar system tonight, and everywhere we've been";
            const float w1 = width(title, titlePx, t1), w2 = width(font, subPx, t2);
            text(title, titlePx, ImVec2((w - w1) * 0.5f, h * 0.30f), IM_COL32(255, 255, 255, (int)(a * 235)), t1);
            text(font, subPx, ImVec2((w - w2) * 0.5f, h * 0.30f + titlePx + 6.f * scale), IM_COL32(220, 225, 240, (int)(a * 200)), t2);
            // Credit, arriving a beat after the title.
            const float ac = (float)std::clamp(std::min((p - 0.2) / 0.15, (0.85 - p) / 0.15), 0.0, 1.0);
            if (ac > 0.f) {
                const char* t3 = "a Beeman daydream, 2026";
                const float w3 = width(font, smallPx, t3);
                text(font, smallPx, ImVec2((w - w3) * 0.5f, h - pad - smallPx * 3.2f), IM_COL32(200, 205, 225, (int)(ac * 170)), t3);
            }
        }
    }

    // Caption: what the tour is showing (name, one line), fading in at each stop.
    if (m_tour.active() && !m_tour.introActive()) {
        const int tb = m_tour.targetBody(), tc = m_tour.targetCraft();
        if (tb != m_captionBody || tc != m_captionCraft) { m_captionBody = tb; m_captionCraft = tc; m_captionAge = 0.0; }
        m_captionAge += dt;
        std::string name, blurb;
        if (tc >= 0) {
            name = m_crafts.crafts()[tc].name;
            blurb = m_crafts.crafts()[tc].blurb;
            if (!m_crafts.crafts()[tc].when.empty()) blurb += "    -    " + m_crafts.crafts()[tc].when;
        } else if (tb >= 0 && m_tour.targetMode() == 1) {
            name = m_solar.body(tb).name + "'s rings";
            blurb = "ice from dust grains to houses, in a sheet ten metres thick";
        } else if (tb >= 0) {
            name = m_solar.body(tb).name;
            static const std::unordered_map<std::string, std::string> blurbs = {
                {"Sun", "our star, 150 million km from home"}, {"Mercury", "closest to the Sun: 430 C by day, -180 C by night"},
                {"Venus", "wrapped in clouds of sulphuric acid"}, {"Earth", "home"},
                {"Moon", "384,000 km out, always showing the same face"}, {"Mars", "the red planet, half the size of Earth"},
                {"Jupiter", "eleven Earths across, with a storm older than the telescope"},
                {"Io", "the most volcanic world in the solar system"}, {"Europa", "an ocean under the ice"},
                {"Saturn", "rings of ice, ten metres thick and 280,000 km wide"},
                {"Titan", "a moon with rivers and seas of methane"}, {"Uranus", "tipped on its side, rolling around the Sun"},
                {"Neptune", "the windiest world: 2,000 km/h"}, {"Ganymede", "the largest moon, bigger than Mercury"},
                {"Callisto", "the most heavily cratered surface known"}, {"Triton", "captured, and orbiting backwards"},
                {"Rhea", "Saturn's second-largest moon"}, {"Phobos", "a captured asteroid, spiralling in"}, {"Deimos", "twelve kilometres of rock"}};
            auto it = blurbs.find(name);
            if (it != blurbs.end()) blurb = it->second;
        }
        if (!name.empty()) {
            const bool flight = !m_tour.visiting();
            const float a = (float)std::clamp(m_captionAge / 1.2, 0.0, 1.0) * (flight ? 0.55f : 1.f);
            const std::string head = flight ? "next:  " + name : name;
            const float hw = width(font, bodyPx, head.c_str());
            const float y = h - pad - smallPx - 14.f * scale - bodyPx - (flight ? 0.f : smallPx + 4.f * scale);
            text(font, bodyPx, ImVec2((w - hw) * 0.5f, y), IM_COL32(255, 255, 255, (int)(a * 230)), head.c_str());
            if (!flight && !blurb.empty()) {
                const float bw = width(font, smallPx, blurb.c_str());
                text(font, smallPx, ImVec2((w - bw) * 0.5f, y + bodyPx + 4.f * scale), IM_COL32(215, 220, 235, (int)(a * 190)), blurb.c_str());
            }
        }
    }

    // Hover: the planet, moon or spacecraft under the cursor.
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !m_window->cursorCaptured() && io.MousePos.x >= 0.f) {
        const glm::mat4 vp = render::makeViewProj(m_renderCamera, (float)w / (float)h);
        const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
        float bestPx = 26.f * scale;
        std::string label, sub;
        auto consider = [&](const glm::dvec3& pos, double radiusPc, const std::string& n, const std::string& detail) {
            const glm::vec3 rel = glm::vec3(pos - m_camera.position);
            const glm::vec4 c = vp * glm::vec4(rel, 1.f);
            if (c.w <= 0.f) return;
            const glm::vec2 sp((c.x / c.w * 0.5f + 0.5f) * w, (c.y / c.w * 0.5f + 0.5f) * h);
            const float onScreenR = (float)(radiusPc / std::max(glm::length(glm::dvec3(rel)), 1e-30) / (2.0 * std::tan(m_camera.fovY * 0.5) / h));
            const float d = std::max(glm::length(mouse - sp) - onScreenR, 0.f);
            if (d < bestPx) { bestPx = d; label = n; sub = detail; }
        };
        for (size_t i = 0; i < m_solar.bodies().size(); ++i) {
            const Body& b = m_solar.body((int)i);
            const double dist = glm::length(b.position - m_camera.position) - b.radiusKm / kKmPerParsec;
            consider(b.position, b.radiusKm / kKmPerParsec, b.name, formatDistance(std::max(dist, 0.0)) + " away");
        }
        for (const Craft& c : m_crafts.crafts()) {
            if (c.modelIndex < 0 || !c.listed) continue;
            const double dist = glm::length(c.position - m_camera.position);
            consider(c.position, c.sizeMeters / (kKmPerParsec * 1000.0), c.name, c.blurb + "   " + formatDistance(dist) + " away");
        }
        if (!label.empty()) {
            const ImVec2 pos(io.MousePos.x + 16.f * scale, io.MousePos.y + 12.f * scale);
            text(font, bodyPx, pos, IM_COL32(255, 250, 235, 235), label.c_str());
            text(font, smallPx, ImVec2(pos.x, pos.y + bodyPx + 2.f * scale), IM_COL32(220, 220, 230, 180), sub.c_str());
        }
    }
}

bool App::loadTle() {
    const auto path = assetDirectory() / "models" / "tle.txt";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (stamp == m_tleStamp) return false;
    m_tleStamp = stamp;
    std::ifstream f(path);
    std::string name, l1, l2;
    int applied = 0;
    while (std::getline(f, name) && std::getline(f, l1) && std::getline(f, l2))
        if (m_crafts.applyTle(name, l1, l2)) ++applied;
    if (applied) {
        m_crafts.update(m_solar, m_epochJd + m_simDays);
        LOG_INFO("Live orbits: {} spacecraft on today's elements", applied);
    }
    return applied > 0;
}

bool App::loadLiveClouds() {
    const auto dir = assetDirectory() / "textures" / "earth_hires";
    const auto png = dir / "clouds_today.png";
    std::error_code ec;
    if (!std::filesystem::exists(png, ec)) return false;
    const auto stamp = std::filesystem::last_write_time(png, ec);
    if (stamp == m_cloudsStamp) return false;
    gfx::DecodedImage img = gfx::decodeImage(png);
    if (!img.ok()) return false;
    m_cloudsStamp = stamp;
    gfx::Texture tex;
    if (!tex.upload(m_ctx, img, false, "clouds today")) return false;
    Body& earth = m_solar.bodies()[m_solar.find("Earth")];
    if (earth.texCloudsLiveIndex < 0) earth.texCloudsLiveIndex = m_renderer.addTexture(std::move(tex));
    else m_renderer.replaceTexture(earth.texCloudsLiveIndex, std::move(tex));
    std::string date;
    std::ifstream(dir / "clouds_today.txt") >> date;
    LOG_INFO("Live clouds: NASA GIBS imagery for {}", date);
    return true;
}

void App::startLiveFetch() {
    // Fresh enough? Clouds dated today or yesterday (UTC) and elements under 12 hours old skip the fetch.
    const auto dir = assetDirectory();
    std::error_code ec;
    bool stale = true;
    const auto tlePath = dir / "models" / "tle.txt";
    const auto cloudsTxt = dir / "textures" / "earth_hires" / "clouds_today.txt";
    if (std::filesystem::exists(tlePath, ec) && std::filesystem::exists(cloudsTxt, ec)) {
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto tleAge = now - std::filesystem::last_write_time(tlePath, ec);
        const auto cloudAge = now - std::filesystem::last_write_time(cloudsTxt, ec);
        stale = tleAge > std::chrono::hours(12) || cloudAge > std::chrono::hours(20);
    }
    if (!stale) return;
    const std::string script = (dir.parent_path() / "scripts" / "fetch_today.py").string();
    std::string cmd = "python \"" + script + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.parent_path().string().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        m_liveProcess = pi.hProcess;
        LOG_INFO("Live data: fetching today's clouds and orbits in the background");
    } else {
        LOG_WARN("Live data: could not start python (run scripts/fetch_today.py by hand)");
    }
}

void App::pollLiveData() {
    const double now = glfwGetTime();
    if (now < m_liveNextPoll) return;
    m_liveNextPoll = now + 3.0;
    if (m_liveProcess) {
        if (WaitForSingleObject((HANDLE)m_liveProcess, 0) == WAIT_OBJECT_0) {
            CloseHandle((HANDLE)m_liveProcess);
            m_liveProcess = nullptr;
        } else {
            return; // still downloading: the files are being written
        }
    }
    loadTle();
    loadLiveClouds();
}

void App::goToSurface(int index, double latDeg, double lonDeg, double altKm) {
    const Body& b = m_solar.body(index);
    const double lat = glm::radians(latDeg), lon = glm::radians(lonDeg);
    const glm::dvec3 local{std::cos(lat) * std::cos(lon), std::sin(lat), -std::cos(lat) * std::sin(lon)}; // east = -z
    const glm::dvec3 up = glm::normalize(glm::dvec3(b.rotation * glm::vec3(local)));
    m_camera.position = b.position + up * ((b.radiusKm + altKm) / kKmPerParsec);
    // Look 35 degrees below the horizon, toward local north.
    const glm::dvec3 northLocal = glm::normalize(glm::dvec3(0, 1, 0) - local * local.y);
    const glm::vec3 north = glm::normalize(b.rotation * glm::vec3(northLocal));
    const glm::vec3 fwd = glm::normalize(north * std::cos(glm::radians(35.f)) - glm::vec3(up) * std::sin(glm::radians(35.f)));
    m_camera.orientation = glm::quatLookAt(fwd, glm::vec3(up));
    m_camera.speed = altKm / kKmPerParsec * 0.3;
    if (m_timeScale > 0.003f) m_timeScale = 0.00025f; // the ground would race by at the default clock
    m_tour.stop("go to surface");
}

void App::goToBody(int index) {
    if (index < 0 || index >= (int)m_solar.bodies().size()) return;
    m_tour.stop("go to body");
    m_solar.viewpoint(index, 4.0, m_camera.position, m_camera.orientation);
    // Cross the body's diameter in about four seconds.
    m_camera.speed = m_solar.body(index).radiusKm / kKmPerParsec * 0.5;
}

int App::run() {
    double last = glfwGetTime();
    while (!m_window->shouldClose()) {
        m_input.beginFrame();
        m_window->pollEvents();

        int w = 0, h = 0;
        m_window->framebufferSize(w, h);
        if (w == 0 || h == 0) { // minimised
            m_window->waitEvents();
            continue;
        }

        double now = glfwGetTime();
        double dt = std::min(now - last, 0.1);
        last = now;

        if (m_input.keyPressed(GLFW_KEY_ESCAPE)) glfwSetWindowShouldClose(m_window->handle(), GLFW_TRUE);
        if (m_input.keyPressed(GLFW_KEY_F1)) m_showUi = !m_showUi;
        if (m_input.keyPressed(GLFW_KEY_T) && m_useGaia && !ImGui::GetIO().WantTextInput) {
            if (m_tour.active()) m_tour.stop("T key");
            else m_tour.start(m_solar, m_camera);
        }
        if (m_tour.active() && !ImGui::GetIO().WantTextInput) {
            if (m_input.keyPressed(GLFW_KEY_RIGHT)) m_tour.skip(1);
            if (m_input.keyPressed(GLFW_KEY_LEFT)) m_tour.skip(-1);
        }

        const glm::dvec3 posBefore = m_camera.position;
        updateCamera(dt);
        const glm::dvec3 inputDelta = m_camera.position - posBefore;
        updateScene(dt);
        m_cameraVelocity = (inputDelta + m_tourDelta) / std::max(dt, 1e-4);
        m_frameScene.cameraOwnDelta = glm::vec3(inputDelta + m_tourDelta);
        updateConstellations(dt);

        ++m_frameCounter;
        for (uint64_t f : m_diagFrames)
            if (f == m_frameCounter) m_renderer.requestDiagnostic();
        if (m_diagTime > 0.0 && now >= m_diagTime) {
            m_renderer.requestDiagnostic();
            m_diagTime = 0.0;
        }

        m_renderer.beginFrame();
        if (m_showUi) drawUi(dt);
        drawLabels();
        if (m_showOverlay) drawOverlay(dt);
        m_renderer.endFrame(m_renderCamera, now, m_effective, m_frameScene);
    }
    return 0;
}

void App::updateScene(double dt) {
    // The camera rides along with the nearest thing (body or spacecraft, whichever is closer in units of
    // its own size), so simulated time does not leave it behind. Decide before anything moves.
    int frameBody = -1, frameCraft = -1;
    glm::dvec3 framePosBefore{0.0};
    double best = 1e300; // distance to the nearest body, in its radii
    if (m_useGaia) {
        for (size_t i = 0; i < m_solar.bodies().size(); ++i) {
            const Body& b = m_solar.body((int)i);
            double d = glm::length(b.position - m_camera.position) / (b.radiusKm / kKmPerParsec);
            if (d < best) {
                best = d;
                frameBody = (int)i;
            }
        }
        double bestCraft = 1e300;
        for (size_t i = 0; i < m_crafts.crafts().size(); ++i) {
            const Craft& c = m_crafts.crafts()[i];
            if (c.modelIndex < 0) continue;
            double d = glm::length(c.position - m_camera.position) / (c.sizeMeters / (kKmPerParsec * 1000.0));
            if (d < bestCraft) {
                bestCraft = d;
                frameCraft = (int)i;
            }
        }
        if (bestCraft > 60.0) frameCraft = -1; // only ride with a craft when we are actually beside it
        if (frameCraft >= 0) framePosBefore = m_crafts.crafts()[frameCraft].position;
        else if (frameBody >= 0) framePosBefore = m_solar.body(frameBody).position;
    }

    double tourJd = 0.0;
    m_clockHeld = false;
    if (m_tour.active() && m_tour.takeClockJump(tourJd)) {
        m_simDays = tourJd - m_epochJd; // a landing site lit by morning sun: jumped while the picture is black
    }
    {
        double scale = m_timeScale;
        if (m_calmClock && m_useGaia) {
            double cap = 1e30;
            std::string by;
            if (frameCraft >= 0) {
                cap = 0.00025; // ~20x real time beside a spacecraft
                by = "near " + m_crafts.crafts()[frameCraft].name;
            }
            if (m_tour.inFlight() && 0.00025 < cap) {
                // The place just left stays where it was while it shrinks out of view.
                cap = 0.00025;
                by = "tour flight";
            }
            if (frameBody >= 0 && best < 15.0) {
                // Close to a world, its surface and its orbit should drift, not spin: at most ~0.3 deg/s
                // at 3 radii, relaxing with distance.
                const Body& b = m_solar.body(frameBody);
                double period = std::abs(b.rotationPeriodHours) / 24.0;
                if (b.orbitPeriodDays > 0.0) period = std::min(period, std::abs(b.orbitPeriodDays));
                const double bodyCap = period * (0.3 / 360.0) * std::max(1.0, best / 3.0);
                if (bodyCap < cap) {
                    cap = bodyCap;
                    by = "near " + b.name;
                }
            }
            if (scale > cap) {
                scale = cap;
                m_clockHeld = true;
                m_clockHeldBy = by;
            }
        }
        m_simDays += dt * scale;
    }
    m_solar.update(m_epochJd + m_simDays);
    if (m_useGaia) m_crafts.update(m_solar, m_epochJd + m_simDays);
    if (frameCraft >= 0) m_camera.position += m_crafts.crafts()[frameCraft].position - framePosBefore;
    else if (frameBody >= 0) m_camera.position += m_solar.body(frameBody).position - framePosBefore;

    const glm::dvec3 beforeTour = m_camera.position;
    if (m_tour.active()) m_tour.update(m_solar, m_camera, dt, m_tourSpeed);
    m_tourDelta = m_camera.position - beforeTour;
    updateAudio(dt);

    if (m_useGaia) {
        m_frameScene.sphereCount = m_solar.buildGpuList(m_camera.position, m_frameScene.bodies);
        m_frameScene.sunPosRel = glm::vec3(m_solar.sunPosition() - m_camera.position);
        m_frameScene.sunRadius = (float)(m_solar.body(0).radiusKm / kKmPerParsec);
        if (m_showCraft) {
            std::vector<float> extents;
            m_crafts.buildGpuList(m_solar, m_camera.position, m_frameScene.crafts, extents);
            m_frameScene.craftModels.clear();
            for (const Craft& c : m_crafts.crafts()) m_frameScene.craftModels.push_back(c.modelIndex);
        } else {
            m_frameScene.crafts.clear();
            m_frameScene.craftModels.clear();
        }
        updateShadowsAndDetail();
        updateBackgroundAdaptation(dt);
        pollLiveData();
        // Orbiters race around at the default time scale; slow the clock while the tour visits one.
        if (m_tour.atCraft()) {
            if (m_savedTimeScale < 0.f) {
                m_savedTimeScale = m_timeScale;
                m_timeScale = 0.00025f; // ~20x real time: orbiters drift, the planet stays put
            }
        } else if (m_savedTimeScale >= 0.f) {
            m_timeScale = m_savedTimeScale;
            m_savedTimeScale = -1.f;
        }
    } else {
        m_frameScene.bodies.clear();
        m_frameScene.sphereCount = 0;
    }
    m_frameScene.sunRadiance = m_sunRadiance;
}

void App::updateBackgroundAdaptation(double dt) {
    double lit = 0.0; // screen fraction weighted by how bright it is
    int fbw = 0, fbh = 0;
    m_window->framebufferSize(fbw, fbh);
    const double aspect = fbh > 0 ? (double)fbw / fbh : 16.0 / 9.0;
    const double tanY = std::tan(m_camera.fovY * 0.5), tanX = tanY * aspect;
    const double halfDiag = std::atan(std::hypot(tanX, tanY));
    const double screenArea = 4.0 * tanX * tanY; // image plane at unit distance
    const glm::dvec3 fwd = glm::dvec3(m_renderCamera.forward());
    for (size_t i = 0; i < m_solar.bodies().size(); ++i) {
        const Body& b = m_solar.body((int)i);
        const glm::dvec3 rel = b.position - m_camera.position;
        const double d = glm::length(rel);
        if (d <= 0.0) continue;
        const double R = b.radiusKm / kKmPerParsec;
        const glm::dvec3 dir = rel / d;
        const double theta = std::asin(std::min(R / d, 1.0));
        const double off = std::acos(std::clamp(glm::dot(dir, fwd), -1.0, 1.0));
        if (off - theta > halfDiag) continue; // not in view
        const double t = std::tan(std::min(theta, 1.45));
        const double inView = std::clamp((halfDiag + theta - off) / std::max(2.0 * theta, 1e-9), 0.0, 1.0);
        const double cover = std::min(glm::pi<double>() * t * t, screenArea) / screenArea * inView;
        if (b.type == BodyType::Sun) {
            lit += cover > 0.0 ? 1.0 : 0.0; // the Sun in frame outshines everything
            continue;
        }
        const glm::dvec3 toSun = glm::normalize(m_solar.sunPosition() - b.position);
        // Lit share of what we see: the phase of the disc from afar, the local Sun elevation up close
        // (standing on the night side the sky stays full of stars).
        const double disc = 0.5 + 0.5 * glm::dot(toSun, -dir);
        const double local = std::max(glm::dot(toSun, -dir), 0.0);
        const double nearness = glm::smoothstep(3.0, 1.2, d / R);
        const double phase = disc + (local - disc) * nearness;
        const double au = glm::length(m_solar.sunPosition() - b.position) * kAuPerParsec;
        const double irradiance = std::pow(1.0 / std::max(au * au, 1e-6), 0.3);
        lit += cover * phase * irradiance * 0.3;
    }
    const double target = m_eyeAdapt ? std::clamp(std::sqrt(lit / 0.008), 0.0, 1.0) : 0.0;
    m_bgAdapt += (target - m_bgAdapt) * (1.0 - std::exp(-dt / 0.6)); // the eye takes a moment
    const float a = (float)m_bgAdapt;
    auto fade = [a](float floor) { return 1.f + (floor - 1.f) * a; };
    m_effective.starBrightness *= fade(0.01f);
    m_effective.skyDensity *= fade(0.2f);
    m_effective.nebulaIntensity *= fade(0.05f);
    m_effective.galaxyGlow *= fade(0.04f);
    m_effective.nebulaGain *= fade(0.04f);
    m_frameScene.dustBrightness *= fade(0.05f);
}

void App::updateShadowsAndDetail() {
    const double mPerPc = kKmPerParsec * 1000.0;
    m_frameScene.shadowEnabled = false;
    m_frameScene.shadowCasters.clear();
    m_frameScene.detailBody = -1;

    // Shadow focus: the spacecraft that looks biggest on screen, for as long as it is more than a few
    // pixels across (so a lander keeps its shadow while the tour pulls away from it).
    int focus = -1;
    double best = 0.0; // projected size in pixels
    const auto& crafts = m_crafts.crafts();
    int fbw = 0, fbh = 0;
    m_window->framebufferSize(fbw, fbh);
    const double radPerPx = 2.0 * std::tan(m_camera.fovY * 0.5) / std::max(fbh, 1);
    for (size_t i = 0; i < crafts.size() && m_showCraft; ++i) {
        const Craft& c = crafts[i];
        if (c.modelIndex < 0 || !c.listed) continue;
        const double dist = std::max(glm::length(c.position - m_camera.position), 1e-30);
        const double px = (c.sizeMeters / mPerPc) / dist / radPerPx;
        if (px > best) {
            best = px;
            focus = (int)i;
        }
    }
    if (focus >= 0 && best > 3.0) {
        const Craft& c = crafts[focus];
        const bool surface = c.placement == CraftPlacement::Surface && c.parent >= 0;
        const glm::dvec3 L = glm::normalize(m_solar.sunPosition() - c.position);
        glm::dvec3 centre = c.position;
        double halfM, rangeM;
        if (surface) {
            // Low sun throws long shadows: centre the box a little down-sun of the lander and make it
            // big enough for the flag and the shadow tips.
            const glm::dvec3 up = glm::normalize(c.position - m_solar.body(c.parent).position);
            glm::dvec3 flat = L - up * glm::dot(L, up);
            if (glm::length(flat) > 1e-9) centre -= glm::normalize(flat) * (26.0 / mPerPc);
            centre += up * (3.0 / mPerPc);
            halfM = 72.0;
            rangeM = 250.0;
        } else {
            halfM = c.sizeMeters * 0.75;
            rangeM = c.sizeMeters * 3.0;
        }
        const double half = halfM / mPerPc, range = rangeM / mPerPc;
        const glm::dvec3 ref = std::abs(L.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        const glm::dvec3 u = glm::normalize(glm::cross(ref, L));
        const glm::dvec3 v = glm::cross(L, u);
        const glm::dvec3 rel = centre - m_camera.position;
        glm::dmat4 M(0.0); // column-major: M[col][row]
        for (int k = 0; k < 3; ++k) {
            M[k][0] = u[k] / half;
            M[k][1] = v[k] / half;
            M[k][2] = L[k] / (2.0 * range);
        }
        M[3][0] = -glm::dot(rel, u) / half;
        M[3][1] = -glm::dot(rel, v) / half;
        M[3][2] = 0.5 - glm::dot(rel, L) / (2.0 * range);
        M[3][3] = 1.0;
        m_frameScene.shadowMatrix = glm::mat4(M);
        m_frameScene.shadowDepthBias = (float)(0.03 / (2.0 * rangeM)); // 3 cm
        for (size_t i = 0; i < crafts.size(); ++i) {
            const Craft& o = crafts[i];
            if (o.modelIndex < 0) continue;
            if (glm::length(o.position - centre) * mPerPc < halfM * 1.5 + o.sizeMeters) m_frameScene.shadowCasters.push_back((int)i);
        }
        m_frameScene.shadowEnabled = !m_frameScene.shadowCasters.empty();
    }

    // Detail anchor: the point on the surface under the camera, for bodies with close-range detail.
    int body = -1;
    double lowest = 1e300;
    for (size_t i = 0; i < m_solar.bodies().size(); ++i) {
        const Body& b = m_solar.body((int)i);
        if (b.detailKind <= 0.f) continue;
        const double altKm = glm::length(m_camera.position - b.position) * kKmPerParsec - b.radiusKm;
        if (altKm < 60.0 && altKm < lowest) {
            lowest = altKm;
            body = (int)i;
        }
    }
    if (body >= 0) {
        const Body& b = m_solar.body(body);
        const glm::dvec3 toCam = glm::normalize(m_camera.position - b.position);
        const glm::dvec3 anchor = b.position + toCam * (b.radiusKm / kKmPerParsec);
        const glm::dvec3 localM = glm::inverse(b.rotationD) * (anchor - b.position) * mPerPc;
        auto wrap = [](double x) { double r = std::fmod(x, 4096.0); return r < 0.0 ? r + 4096.0 : r; };
        m_frameScene.detailBody = body;
        m_frameScene.detailAnchorWorld = glm::vec3(anchor - m_camera.position);
        m_frameScene.detailAnchorLocal = glm::vec3((float)wrap(localM.x), (float)wrap(localM.y), (float)wrap(localM.z));

        // Terrain patch under the camera, if any: anchor uv and metres -> uv, all in double here.
        Body& mb = m_solar.bodies()[body];
        mb.activePatch = -1;
        const glm::dvec3 unit = glm::normalize(localM);
        const double latDeg = glm::degrees(std::asin(std::clamp(unit.y, -1.0, 1.0)));
        const double lonDeg = glm::degrees(std::atan2(-unit.z, unit.x));
        {
            // The local frame at the anchor is always sent: the planet shader also uses its normal
            // (east x north) to intersect the ground precisely near the camera.
            const double cl = std::max(std::sqrt(unit.x * unit.x + unit.z * unit.z), 1e-9);
            const glm::dvec3 e = glm::dvec3(unit.z, 0.0, -unit.x) / cl;
            m_frameScene.patchEast = glm::vec4(glm::vec3(e), 0.f);
            m_frameScene.patchNorth = glm::vec4(glm::vec3(glm::cross(unit, e)), 0.f);
        }
        for (size_t i = 0; i < mb.patches.size(); ++i) {
            const SurfacePatch& sp = mb.patches[i];
            double dLon = std::remainder(lonDeg - sp.lonMinDeg, 360.0);
            if (dLon < -sp.lonSpanDeg) dLon += 360.0;
            const double margin = 0.25; // keep it active a little beyond its edges, where it fades out
            if (dLon > -sp.lonSpanDeg * margin && dLon < sp.lonSpanDeg * (1.0 + margin) &&
                latDeg > sp.latMinDeg - sp.latSpanDeg * margin && latDeg < sp.latMinDeg + sp.latSpanDeg * (1.0 + margin)) {
                mb.activePatch = (int)i;
                const double radiusM = mb.radiusKm * 1000.0;
                const double cosLat = std::max(std::cos(glm::radians(latDeg)), 1e-6);
                const glm::dvec3 east = glm::normalize(glm::dvec3(unit.z, 0.0, -unit.x) / cosLat);
                const glm::dvec3 north = glm::cross(unit, east);
                m_frameScene.patchAnchor = glm::vec4((float)(dLon / sp.lonSpanDeg), (float)((sp.latMinDeg + sp.latSpanDeg - latDeg) / sp.latSpanDeg),
                                                     (float)(1.0 / (radiusM * cosLat * glm::radians(sp.lonSpanDeg))),
                                                     (float)(-1.0 / (radiusM * glm::radians(sp.latSpanDeg))));
                m_frameScene.patchEast = glm::vec4(glm::vec3(east), 0.f);
                m_frameScene.patchNorth = glm::vec4(glm::vec3(north), 0.f);
                break;
            }
        }
        if (body < (int)m_frameScene.bodies.size()) {
            BodyGpu& g = m_frameScene.bodies[body];
            g.patchTex = glm::ivec4(-1);
            g.patchParams = glm::vec4(0.f);
            if (mb.activePatch >= 0) {
                const SurfacePatch& sp = mb.patches[mb.activePatch];
                g.patchTex = glm::ivec4(sp.albedoIndex, sp.normalIndex, sp.heightIndex, -1);
                g.patchParams = glm::vec4(sp.heightMinM, sp.heightRangeM, sp.metresPerTexel, 1.f);
            }
        }
    }
}

void App::updateCamera(double dt) {
    ImGuiIO& io = ImGui::GetIO();

    // Hold right mouse to look around; release to get the cursor back for the UI.
    if (!m_window->cursorCaptured() && m_input.mousePressed(GLFW_MOUSE_BUTTON_RIGHT) && !io.WantCaptureMouse)
        m_window->setCursorCaptured(true);
    if (m_window->cursorCaptured() && !m_input.mouseDown(GLFW_MOUSE_BUTTON_RIGHT))
        m_window->setCursorCaptured(false);

    const bool touring = m_tour.active();
    if (m_window->cursorCaptured()) {
        glm::vec2 d = m_input.mouseDelta();
        float roll = 0.f;
        if (m_input.keyDown(GLFW_KEY_Q)) roll += 1.f;
        if (m_input.keyDown(GLFW_KEY_E)) roll -= 1.f;
        if (touring) {
            // Free look on top of the autopilot: yaw/pitch/roll in the view's own frame.
            const glm::quat qYaw = glm::angleAxis(-d.x * m_mouseSensitivity, glm::vec3(0.f, 1.f, 0.f));
            const glm::quat qPitch = glm::angleAxis(-d.y * m_mouseSensitivity, glm::vec3(1.f, 0.f, 0.f));
            const glm::quat qRoll = glm::angleAxis(roll * (float)dt * 1.2f, glm::vec3(0.f, 0.f, -1.f));
            m_tourLook = glm::normalize(m_tourLook * qYaw * qPitch * qRoll);
        } else {
            m_camera.rotate(-d.x * m_mouseSensitivity, -d.y * m_mouseSensitivity, roll * (float)dt * 1.2f);
        }
    }
    if (touring && !m_window->cursorCaptured()) {
        // Let go: drift back to the tour's framing.
        m_tourLook = glm::slerp(m_tourLook, glm::quat(1.f, 0.f, 0.f, 0.f), (float)std::min(1.0, dt * 1.8));
    }
    if (!touring && m_tourLook != glm::quat(1.f, 0.f, 0.f, 0.f)) {
        // The tour ended while looking around: keep what is on screen.
        m_camera.orientation = glm::normalize(m_camera.orientation * m_tourLook);
        m_tourLook = glm::quat(1.f, 0.f, 0.f, 0.f);
    }

    if (io.WantTextInput) return; // only a text field steals the keyboard, not a focused slider

    if (m_useGaia) {
        static const int kGoTo[10] = {GLFW_KEY_0, GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4,
                                      GLFW_KEY_5, GLFW_KEY_6, GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9};
        static const char* kNames[10] = {"Sun", "Mercury", "Venus", "Earth", "Moon",
                                         "Mars", "Jupiter", "Saturn", "Uranus", "Neptune"};
        for (int i = 0; i < 10 && !touring; ++i) // during the tour only T (or the checkbox) takes over
            if (m_input.keyPressed(kGoTo[i])) goToBody(m_solar.find(kNames[i]));
    }

    if (float s = m_input.scrollDelta(); s != 0.f) m_camera.speed *= std::pow(1.4, (double)s);
    m_camera.speed = std::clamp(m_camera.speed, 1e-15, 1e12);

    if (touring) return; // the autopilot has the stick: flight keys wait until the tour is off (T)

    glm::dvec3 move{0.0};
    if (m_input.keyDown(GLFW_KEY_W)) move.z -= 1.0;
    if (m_input.keyDown(GLFW_KEY_S)) move.z += 1.0;
    if (m_input.keyDown(GLFW_KEY_A)) move.x -= 1.0;
    if (m_input.keyDown(GLFW_KEY_D)) move.x += 1.0;
    if (m_input.keyDown(GLFW_KEY_R) || m_input.keyDown(GLFW_KEY_SPACE)) move.y += 1.0;
    if (m_input.keyDown(GLFW_KEY_F) || m_input.keyDown(GLFW_KEY_LEFT_CONTROL)) move.y -= 1.0;
    if (glm::length(move) > 0.0) {
        double boost = m_input.keyDown(GLFW_KEY_LEFT_SHIFT) ? 5.0 : 1.0;
        m_camera.translateLocal(glm::normalize(move) * m_camera.speed * boost * dt);
    }
    if (m_window->cursorCaptured() && glm::length(m_input.mouseDelta()) > 0.f) m_lookActive = false;
}

void App::updateAudio(double dt) {
    m_analyzer.update(m_audio, (float)dt);
    m_effective = m_settings;
    m_renderCamera = m_camera;
    m_renderCamera.orientation = glm::normalize(m_camera.orientation * m_tourLook);

    // Waves (optional) fire on sustained swells, at most every ~7 s: a landmark, never a metronome.
    m_beatAge += (float)dt;
    m_waveCooldown = std::max(0.f, m_waveCooldown - (float)dt);
    if (m_waveStrength > 0.f && !m_analyzer.silent() && m_waveCooldown <= 0.f &&
        m_analyzer.levelFast() > m_analyzer.levelSlow() * 1.3f && m_analyzer.levelFast() > 0.25f) {
        m_beatAge = 0.f;
        m_waveCooldown = 7.f;
    }
    m_beatSoft = 0.f; // no per-beat screen effects

    const bool active = m_audioReact > 0.f && !m_analyzer.silent();
    const float k = active ? m_audioReact : 0.f;
    const auto& bands = m_analyzer.bands();
    for (int i = 0; i < 32; ++i) m_frameScene.audioBands[i] = active ? bands[i] : 0.f;
    m_frameScene.bass = active ? m_analyzer.bass() : 0.f;
    m_frameScene.mid = active ? m_analyzer.mid() : 0.f;
    m_frameScene.treble = active ? m_analyzer.treble() : 0.f;
    m_frameScene.beat = 0.f;
    m_frameScene.waveStrength = active ? m_waveStrength : 0.f;
    m_frameScene.level = active ? m_analyzer.level() : 0.f;
    m_frameScene.beatAge = active ? m_beatAge : 100.f;
    m_frameScene.audioReact = k;
    {
        // Mood colour: warm ember for bass-heavy music, violet for balanced, ice blue for bright.
        const float c = m_analyzer.centroid();
        const glm::vec3 warm(1.0f, 0.48f, 0.22f), violet(0.85f, 0.42f, 1.0f), ice(0.38f, 0.82f, 1.0f);
        glm::vec3 pal = c < 0.5f ? glm::mix(warm, violet, c * 2.f) : glm::mix(violet, ice, (c - 0.5f) * 2.f);
        // Phrase energy sets saturation and warmth: verses pale and cool, choruses rich and warm.
        const float section = m_analyzer.section();
        pal = glm::mix(pal, pal * glm::vec3(1.08f, 0.96f, 0.9f), section);
        m_frameScene.moodColor = glm::mix(glm::vec3(1.f), pal, 0.3f + 0.6f * section);
    }

    // Dust field: a box around the camera sized to the current speed so motes streak past at any scale.
    const double speed = glm::length(m_cameraVelocity);
    const double extentPc = std::max(1500.0 / kKmPerParsec, speed * 2.5);
    m_frameScene.dustExtent = (float)extentPc;
    m_frameScene.dustBrightness =
        m_useGaia ? m_dustBrightness * (active ? 1.2f + 1.8f * m_analyzer.section() : 1.f) : 0.f;
    m_frameScene.cameraVelocity = glm::vec3(m_cameraVelocity);
    glm::dvec3 phase = m_camera.position / extentPc;
    m_frameScene.dustPhase = glm::vec3(phase - glm::floor(phase));
    glm::dvec3 cphase = m_camera.position / (extentPc * 2.5);
    m_frameScene.cloudPhase = glm::vec3(cphase - glm::floor(cphase));
    m_frameScene.section = m_analyzer.section();
    m_frameScene.cloudStrength = active ? m_cloudStrength : 0.f;
    m_frameScene.auroraStrength = m_useGaia ? m_auroraStrength : 0.f;
    m_frameScene.starTint = m_starTint;
    if (!active) return;

    // Global touches are continuous and slow: bass widens the bloom, phrase energy lifts the glow.
    const float bass = m_analyzer.bass(), section = m_analyzer.section();
    m_effective.bloomStrength = m_settings.bloomStrength * (1.f + k * (0.5f * bass + 0.4f * section));
    m_effective.bloomRadius = m_settings.bloomRadius * (1.f + k * 0.4f * bass);
    m_effective.galaxyGlow = m_settings.galaxyGlow * (1.f + k * 0.35f * (section - 0.5f));
    m_effective.nebulaGain = m_settings.nebulaGain * (1.f + k * 0.5f * (section - 0.5f));


}

void App::drawUi(double dt) {
    const double fps = dt > 0.0 ? 1.0 / dt : 0.0;
    m_smoothedFps = m_smoothedFps == 0.0 ? fps : m_smoothedFps * 0.95 + fps * 0.05;

    ImGui::SetNextWindowPos({16.f, 16.f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({360.f, 0.f}, ImGuiCond_FirstUseEver);
    ImGui::Begin("Space", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::TextDisabled("%s", m_ctx.gpuName().c_str());
    ImGui::Text("%.0f fps   cpu %.2f ms   gpu %.2f ms", m_smoothedFps, dt * 1000.0, m_renderer.lastGpuFrameMs());
    ImGui::Separator();

    if (m_useGaia) {
        // Nearest body and distance to its surface.
        int nearest = -1;
        double best = 1e300;
        for (size_t i = 0; i < m_solar.bodies().size(); ++i) {
            const Body& b = m_solar.body((int)i);
            double d = glm::length(b.position - m_camera.position) - b.radiusKm / kKmPerParsec;
            if (d < best) {
                best = d;
                nearest = (int)i;
            }
        }
        if (nearest >= 0)
            ImGui::Text("%s  %s away", m_solar.body(nearest).name.c_str(), formatDistance(std::max(best, 0.0)).c_str());
        ImGui::Text("from Sun  %s", formatDistance(glm::length(m_camera.position - m_solar.sunPosition())).c_str());
    } else {
        const auto& p = m_camera.position;
        ImGui::Text("pos  %.3g  %.3g  %.3g", p.x, p.y, p.z);
    }
    ImGui::Text("speed %s/s", formatDistance(m_camera.speed).c_str());
    float fovDeg = glm::degrees(m_camera.fovY);
    if (ImGui::SliderFloat("fov", &fovDeg, 30.f, 120.f)) m_camera.fovY = glm::radians(fovDeg);
    ImGui::SliderFloat("sensitivity", &m_mouseSensitivity, 0.0005f, 0.006f, "%.4f");

    if (m_useGaia) {
        ImGui::Separator();
        ImGui::TextDisabled("go to (keys 0-9)");
        int perRow = 0;
        for (size_t i = 0; i < m_solar.bodies().size(); ++i) {
            if (perRow++ % 4 != 0) ImGui::SameLine();
            if (ImGui::SmallButton(m_solar.body((int)i).name.c_str())) goToBody((int)i);
        }
        bool tour = m_tour.active();
        if (ImGui::Checkbox("autopilot tour (T)", &tour)) {
            if (tour) m_tour.start(m_solar, m_camera);
            else m_tour.stop("checkbox");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_tour.status().c_str());
        ImGui::SliderFloat("tour pace", &m_tourSpeed, 0.25f, 4.f, "%.2fx");
        ImGui::SliderFloat("time scale (days/s)", &m_timeScale, 0.0f, 30.f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("calm clock near worlds and craft", &m_calmClock);
        ImGui::Checkbox("eye adaptation (stars fade beside sunlit worlds)", &m_eyeAdapt);
        if (m_eyeAdapt) ImGui::TextDisabled("background at %.0f%%", 100.0 * (1.0 - 0.97 * m_bgAdapt));
        if (m_clockHeld) ImGui::TextDisabled("clock held near real time (%s)", m_clockHeldBy.c_str());
        if (!m_crafts.crafts().empty() && ImGui::CollapsingHeader("spacecraft")) {
            ImGui::Checkbox("show spacecraft", &m_showCraft);
            for (size_t i = 0; i < m_crafts.crafts().size(); ++i) {
                const Craft& c = m_crafts.crafts()[i];
                if (c.modelIndex < 0 || !c.listed) continue;
                if (ImGui::SmallButton(c.name.c_str())) goToCraft((int)i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.blurb.c_str());
            }
        }
        ImGui::SliderFloat("sun radiance", &m_sunRadiance, 100.f, 20000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::Separator();

    ImGui::Checkbox("temporal anti-aliasing", &m_settings.taa);
    ImGui::SliderFloat("sun glare", &m_settings.sunGlare, 0.f, 3.f);
    ImGui::SliderFloat("motion blur", &m_settings.motionBlur, 0.f, 1.5f);
    ImGui::SliderFloat("exposure", &m_settings.exposure, 0.05f, 8.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("bloom", &m_settings.bloomStrength, 0.f, 2.f);
    ImGui::SliderFloat("bloom knee", &m_settings.bloomKnee, 0.f, 4.f);
    ImGui::SliderFloat("bloom radius", &m_settings.bloomRadius, 0.5f, 2.5f);
    ImGui::SliderFloat("star brightness", &m_settings.starBrightness, 0.05f, 5000.f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("star max px", &m_settings.starMaxRadiusPx, 4.f, 80.f);
    ImGui::SliderFloat("sky density", &m_settings.skyDensity, 0.f, 1.5f);
    ImGui::SliderFloat("sky haze", &m_settings.nebulaIntensity, 0.f, 3.f);
    ImGui::SliderFloat("dust motes", &m_dustBrightness, 0.f, 3.f);
    ImGui::Checkbox("volumetrics", &m_settings.volumetrics);
    ImGui::SameLine();
    ImGui::Checkbox("stars", &m_settings.drawStars);
    ImGui::SameLine();
    ImGui::Checkbox("bodies", &m_settings.drawBodies);
    ImGui::SliderFloat("galaxy glow", &m_settings.galaxyGlow, 0.f, 5.f);
    ImGui::SliderFloat("galaxy dust", &m_settings.galaxyDust, 0.f, 5.f);
    ImGui::SliderFloat("nebulae", &m_settings.nebulaGain, 0.f, 5.f);
    ImGui::Separator();

    drawConstellationUi();
    ImGui::Separator();

    if (m_audio.running()) {
        ImGui::Text("music: %s", m_analyzer.silent() ? "silent" : "playing");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", m_audio.deviceName().c_str());
        ImGui::PlotHistogram("##spectrum", m_analyzer.bands().data(), audio::Analyzer::kBands, 0, nullptr, 0.f, 1.f,
                             ImVec2(-1.f, 48.f));
        ImGui::SliderFloat("audio reactivity", &m_audioReact, 0.f, 2.5f, "%.2f");
        ImGui::SliderFloat("swell waves", &m_waveStrength, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("music nebula", &m_cloudStrength, 0.f, 3.f, "%.2f");
        ImGui::SliderFloat("aurora", &m_auroraStrength, 0.f, 3.f, "%.2f");
        ImGui::SliderFloat("star tint", &m_starTint, 0.f, 1.5f, "%.2f");
        ImGui::TextDisabled("section energy %.0f%%   spectral tilt %.0f%%", m_analyzer.section() * 100.f,
                            m_analyzer.centroid() * 100.f);
    } else {
        ImGui::TextDisabled("audio capture unavailable");
    }
    ImGui::Separator();

    ImGui::Text("%s: %u stars", m_catalog.name.c_str(), (unsigned)m_catalog.stars.size());
    if (m_haveGaia) {
        bool gaia = m_useGaia;
        if (ImGui::Checkbox("real stars (Gaia DR3)", &gaia) && gaia != m_useGaia) loadCatalog(gaia);
    } else {
        ImGui::TextDisabled("run scripts/convert_athyg.py for real stars");
    }
    ImGui::Checkbox("vsync", &m_settings.vsync);
    if (m_renderer.hdrAvailable()) {
        ImGui::SameLine();
        ImGui::Checkbox("HDR output", &m_settings.hdrOutput);
        if (m_renderer.hdrActive()) {
            ImGui::SliderFloat("paper white (nits)", &m_settings.hdrPaperWhite, 80.f, 400.f, "%.0f");
            ImGui::SliderFloat("peak (nits)", &m_settings.hdrPeak, 400.f, 4000.f, "%.0f");
        }
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(display is SDR)");
    }
    ImGui::Separator();

    ImGui::TextDisabled("RMB look  WASD fly  R/F up/down  Q/E roll");
    ImGui::TextDisabled("scroll speed  shift boost  T tour  F1 hide  esc quit");
    ImGui::End();
}

} // namespace space
