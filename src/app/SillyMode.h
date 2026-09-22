#pragma once
#include "audio/SoundPlayer.h"

#include <glm/glm.hpp>
#include <random>
#include <vector>

struct ImDrawList;

namespace space {

class Tour;
class SolarSystem;
class CraftCatalog;
class Camera;
namespace render {
struct FrameScene;
}

// Pee pee poo poo mode (F9): a fart button (F) and a pee button (P, hold) that do something different at
// every tour stop. Cartoon effects are drawn over the frame; planets and spacecraft really jiggle and spin.
class SillyMode {
public:
    void toggle();
    bool enabled() const { return m_on; }

    // After the scene is built: reads the buttons, advances the gags and bends what the renderer will draw.
    void update(double dt, bool fartPressed, bool peeDown, const Tour& tour, const SolarSystem& solar,
                const CraftCatalog& crafts, render::FrameScene& scene, Camera& renderCamera,
                const glm::dvec3& cameraPos, int width, int height);
    void draw(ImDrawList* dl, int width, int height) const;

    enum class Place { Other, SaturnV, Iss, Moon, Mars, Perseverance, Jupiter, Io, Saturn, Uranus, Neptune, Sun, Telescope, Earth };

private:
    struct Particle {
        enum Kind { Puff, Steam, Plume, Drop, Bubble, Bird, Ice, Spark, Splat, Stink };
        Kind kind = Puff;
        glm::vec2 pos{0.f}, vel{0.f};
        float age = 0.f, life = 1.f, size = 10.f, grow = 0.f, grav = 0.f, drag = 0.f, seed = 0.f;
        glm::vec3 color{1.f};
    };
    struct Crater {
        glm::vec2 at; // disc units
        float r;
    };

    void reset();
    void startFart();
    void emitContinuous(float dt);
    glm::vec2 unit(glm::vec2 u) const { return m_center + u * m_scale; }
    glm::vec2 disc(glm::vec2 u) const { return m_discCenter + u * m_discRadius; }
    glm::vec2 peeTarget() const;
    float rnd(float a, float b) { return std::uniform_real_distribution<float>(a, b)(m_rng); }
    void add(Particle::Kind kind, glm::vec2 pos, glm::vec2 vel, float size, float life, glm::vec3 color,
             float grow = 0.f, float grav = 0.f, float drag = 0.f);
    void fartCloud(glm::vec2 at, glm::vec2 dir, int count, float spread, glm::vec3 color);
    void sfx(const audio::SoundPlayer::Clip& clip, float gain = 1.f, float pitch = 1.f);

    bool m_on = false;
    audio::SoundPlayer m_sound;
    struct Clips {
        audio::SoundPlayer::Clip fartShort, fartMedium, fartLong, fartEpic, fartSqueaky, fartBouncy, fartCrackly,
            fartDeep, bloops, hiss, sizzle, giggle, slurp, squeak, whistleUp, boing, chirps, tada, thud, whoosh,
            rumble, splash, tink, tch, sparkle;
    } m_clips;

    std::mt19937 m_rng{20260917};
    int m_stopKey = -1000;
    Place m_place = Place::Other;
    int m_body = -1, m_craft = -1;
    double m_time = 0.0;

    // Screen anchor of the thing being visited: the effects centre and scale (kept on screen) and its
    // true projected disc (for things painted onto a planet).
    glm::vec2 m_center{0.f}, m_discCenter{0.f};
    float m_scale = 100.f, m_discRadius = 100.f;
    bool m_discVisible = false;
    int m_w = 1, m_h = 1;

    float m_fartT = 99.f;
    float m_jiggleT = 99.f, m_jiggleAmp = 0.f;
    float m_wiggleT = 99.f, m_wiggleAmp = 0.f;
    float m_ringT = 99.f;
    float m_shakeT = 99.f, m_shakeAmp = 0.f;
    float m_emitAcc = 0.f;

    bool m_peeHeld = false;
    float m_head = 0.f, m_tail = 0.f; // stream grows from the bottom of the screen, then falls away
    float m_hitT = 0.f;               // seconds the stream has been landing
    float m_peeSfxT = 0.f;
    float m_peeEmitAcc = 0.f;
    float m_wipeLast = 0.f;

    std::vector<Crater> m_craters;
    float m_pool = 0.f, m_river = 0.f, m_stripes = 0.f, m_yellow = 0.f, m_rainbow = 0.f;
    bool m_tadaPlayed = false;

    std::vector<Particle> m_particles;
};

} // namespace space
