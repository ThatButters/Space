#pragma once
#include "core/Input.h"
#include "core/Window.h"
#include "gfx/Context.h"
#include "render/Renderer.h"
#include "scene/Camera.h"
#include "scene/SolarSystem.h"
#include "scene/StarCatalog.h"
#include "scene/Constellations.h"
#include "scene/Craft.h"
#include "scene/Tour.h"
#include "scene/Volumes.h"
#include "audio/Analyzer.h"
#include "audio/AudioCapture.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace space {

class App {
public:
    App(int argc, char** argv);
    ~App();
    int run();

private:
    void updateCamera(double dt);
    void updateScene(double dt);
    void drawUi(double dt);
    void loadCatalog(bool gaia);
    void loadTextures();
    void loadModels();
    void goToCraft(int index);
    void updateAudio(double dt);
    void updateConstellations(double dt);
    void drawConstellationUi();
    void drawLabels();
    void drawOverlay(double dt);
    std::string simDateString() const;
    void lookAt(const glm::vec3& dir);
    void goToBody(int index);
    void goToSurface(int index, double latDeg, double lonDeg, double altKm);
    // Per frame: pick the spacecraft whose shadows matter and the surface point that anchors close-range
    // ground detail, both computed in double precision around the final camera position.
    void updateShadowsAndDetail();
    // Eye adaptation for the background: sunlit worlds filling the view wash out the stars, galaxy glow and
    // sky haze, the way a camera exposed for the Moon shows a black sky.
    void updateBackgroundAdaptation(double dt);
    // Live data: today's clouds and current station orbits, fetched by scripts/fetch_today.py in the
    // background at launch when stale, and loaded when the files change.
    void startLiveFetch();
    void pollLiveData();
    bool loadLiveClouds();
    bool loadTle();
    void* m_liveProcess = nullptr;
    double m_liveNextPoll = 0.0;
    std::filesystem::file_time_type m_cloudsStamp{}, m_tleStamp{};

    Input m_input;
    std::unique_ptr<Window> m_window;
    gfx::Context m_ctx;
    render::Renderer m_renderer;
    render::RenderSettings m_settings;
    render::FrameScene m_frameScene;
    Camera m_camera;

    StarCatalog m_catalog;
    bool m_haveGaia = false;
    bool m_useGaia = false;

    SolarSystem m_solar;
    CraftCatalog m_crafts;
    bool m_showCraft = true;
    float m_savedTimeScale = -1.f; // restored after a spacecraft tour stop
    Tour m_tour;
    float m_tourSpeed = 1.f;
    float m_lastMouseX = -1.f, m_lastMouseY = -1.f;
    double m_mouseIdle = 0.0;  // seconds since the mouse last moved (hover labels step aside)
    double m_scrubHeld = 0.0;  // seconds Up / Down has been held at a stop
    double m_scrubRate = 0.0;  // simulated seconds per real second from scrubbing (0 = not scrubbing)
    std::string m_liveCloudDate; // the day today's cloud imagery was taken
    void setIntroClock();       // dawn at the Cape, today
    // Near a world or beside a spacecraft the clock is held close to real time, so taking the controls
    // never leaves you riding a station that laps Earth every three seconds.
    bool m_calmClock = true;
    bool m_eyeAdapt = true;
    double m_bgAdapt = 0.0; // 0 = dark-adapted (full starfield), 1 = adapted to a sunlit surface
    bool m_clockHeld = false;
    std::string m_clockHeldBy;

    Constellations m_constellations;
    std::vector<float> m_conHighlight, m_conTarget; // smoothed / desired per-figure highlight
    int m_conSelected = -1;
    bool m_conShow = false, m_conLabels = false;
    float m_conIntensity = 0.5f;
    char m_conFilter[64] = "";
    // Finder: swing the camera toward a direction over a short time.
    bool m_lookActive = false;
    glm::vec3 m_lookDir{1.f, 0.f, 0.f};
    glm::quat m_lookFrom{1.f, 0.f, 0.f, 0.f};
    float m_lookT = 0.f;

    audio::AudioCapture m_audio;
    audio::Analyzer m_analyzer;
    float m_audioReact = 1.f; // 0 = off
    float m_waveStrength = 0.f; // rare slow tide, off by default
    float m_waveCooldown = 0.f;
    unsigned m_lastBeat = 0;
    float m_beatAge = 100.f;
    float m_beatSoft = 0.f; // eased beat envelope for screen-wide effects (no instant jumps)
    Camera m_renderCamera; // what the renderer sees: m_camera plus the tour free-look offset
    // Looking around during the tour: an offset on top of the autopilot's framing that eases back to
    // centre once the mouse is released. The tour keeps flying.
    glm::quat m_tourLook{1.f, 0.f, 0.f, 0.f};
    glm::dvec3 m_cameraVelocity{0.0}; // own motion (input + tour), excluding riding with a body
    glm::dvec3 m_tourDelta{0.0};
    float m_dustBrightness = 1.f;
    float m_cloudStrength = 0.f;
    float m_starTint = 0.f;
    float m_auroraStrength = 1.f;
    render::RenderSettings m_effective; // settings after audio modulation, what the renderer sees
    double m_epochJd = 0.0;   // wall-clock Julian date at startup
    double m_simDays = 0.0;   // simulated days elapsed since startup
    float m_timeScale = 0.02f; // simulated days per real second
    float m_sunRadiance = 3000.f;
    std::string m_startBody = "Earth";
    // --surface <Body> <lat> <lon> <altKm>: start above a surface point looking toward the horizon.
    bool m_startSurface = false;
    double m_startLat = 0.0, m_startLon = 0.0, m_startAltKm = 400.0;
    bool m_startTour = true; // the opening tour, unless a start place was given or --no-tour
    bool m_windowed = false;
    int m_tourStart = -1; // --tour-start N: skip the intro and begin at stop N (debugging)
    std::vector<uint64_t> m_diagFrames;
    double m_diagTime = 0.0; // wall-clock seconds after start for a one-shot dump (0 = off)
    uint64_t m_frameCounter = 0;

    bool m_showUi = false; // F1: the settings panel
    bool m_showOverlay = true;
    double m_captionAge = 0.0;
    int m_captionBody = -2, m_captionCraft = -2;
    float m_mouseSensitivity = 0.0022f;
    double m_smoothedFps = 0.0;
};

// Human-readable distance / speed helpers (input in parsecs).
std::string formatDistance(double parsecs);

} // namespace space
