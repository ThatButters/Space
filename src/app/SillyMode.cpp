#include "app/SillyMode.h"
#include "core/Log.h"
#include "render/Renderer.h"
#include "scene/Camera.h"
#include "scene/Craft.h"
#include "scene/SolarSystem.h"
#include "scene/StarCatalog.h"
#include "scene/Tour.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

namespace space {

namespace {

constexpr float kTau = 6.2831853f;

using Clip = audio::SoundPlayer::Clip;

float smooth01(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

struct Synth {
    float rate;
    std::mt19937 rng;
    float noise() { return std::uniform_real_distribution<float>(-1.f, 1.f)(rng); }
    float coef(float hz) const { return 1.f - std::exp(-kTau * hz / rate); }
    std::vector<float> buffer(float seconds) const { return std::vector<float>((size_t)(seconds * rate), 0.f); }
    static Clip finish(std::vector<float> s, float peak = 0.85f) {
        float m = 1e-6f;
        for (float v : s) m = std::max(m, std::abs(v));
        for (float& v : s) v *= peak / m;
        return std::make_shared<const std::vector<float>>(std::move(s));
    }
    void mix(std::vector<float>& into, const std::vector<float>& clip, float at, float gain = 1.f) const {
        size_t o = (size_t)(at * rate);
        if (into.size() < o + clip.size()) into.resize(o + clip.size(), 0.f);
        for (size_t i = 0; i < clip.size(); ++i) into[o + i] += clip[i] * gain;
    }

    // A pulse wave through a gently opening low-pass, fluttering and sputtering to a stop.
    std::vector<float> fart(float dur, float f0, float wet, float wobbleHz, float wobbleDepth, float sputter) {
        auto s = buffer(dur);
        float ph = 0.f, lp = 0.f, lp2 = 0.f, nz = 0.f, flutter = 0.f, gateLp = 1.f, burstPh = 0.f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = (float)i / rate, u = t / dur;
            flutter += (noise() - flutter) * 0.003f;
            const float f = f0 * (1.15f - 0.4f * u) * (1.f + 0.35f * flutter) *
                            (1.f + wobbleDepth * std::sin(kTau * wobbleHz * t));
            ph += f / rate;
            ph -= std::floor(ph);
            const float raw = ph < 0.32f ? 1.f : -0.47f;
            const float env = std::min(1.f, t / 0.012f) * std::min(1.f, (dur - t) / 0.09f);
            lp += (raw - lp) * coef(260.f + 900.f * env);
            lp2 += (lp - lp2) * coef(1400.f);
            nz += (noise() - nz) * coef(700.f);
            burstPh += (16.f + 14.f * std::abs(flutter) * 8.f) / rate;
            float gate = 0.62f + 0.38f * std::sin(kTau * burstPh);
            if (u > 1.f - sputter) {
                const float chop = std::sin(kTau * (7.f + 10.f * (1.f - u)) * t) > 0.1f ? 1.f : 0.05f;
                gate *= chop;
            }
            gateLp += (gate - gateLp) * coef(90.f);
            s[i] = (lp2 * 0.85f + nz * wet * 0.9f) * gateLp * env;
        }
        return s;
    }
    std::vector<float> noiseBurst(float dur, float lowHz, float highHz, float decay, float crackle = 0.f) {
        auto s = buffer(dur);
        float lp = 0.f, hp = 0.f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = (float)i / rate;
            lp += (noise() - lp) * coef(highHz);
            hp += (lp - hp) * coef(lowHz);
            float v = (lp - hp) * std::exp(-decay * t) * std::min(1.f, t / 0.01f) * std::min(1.f, (dur - t) / 0.03f);
            if (crackle > 0.f && noise() > 1.f - crackle * 0.002f) v += noise() * 1.5f;
            s[i] = v;
        }
        return s;
    }
    std::vector<float> sweep(float dur, float f0, float f1, float vibHz, float vibDepth, float decay, float harmonics = 0.f) {
        auto s = buffer(dur);
        float ph = 0.f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = (float)i / rate, u = t / dur;
            const float f = (f0 + (f1 - f0) * u) * (1.f + vibDepth * std::sin(kTau * vibHz * t));
            ph += f / rate;
            const float env = std::min(1.f, t / 0.006f) * std::exp(-decay * t) * std::min(1.f, (dur - t) / 0.02f);
            s[i] = (std::sin(kTau * ph) + harmonics * std::sin(2.f * kTau * ph) + harmonics * 0.5f * std::sin(3.f * kTau * ph)) * env;
        }
        return s;
    }
};

float ease(float t, float a, float b) { return smooth01((t - a) / (b - a)); }

ImVec2 V(glm::vec2 p) { return ImVec2(p.x, p.y); }
ImU32 C(glm::vec3 rgb, float a) {
    return IM_COL32((int)std::clamp(rgb.r, 0.f, 255.f), (int)std::clamp(rgb.g, 0.f, 255.f),
                    (int)std::clamp(rgb.b, 0.f, 255.f), (int)std::clamp(a * 255.f, 0.f, 255.f));
}

const glm::vec3 kGreen(140, 200, 70), kYellow(250, 215, 50), kSteam(235, 240, 245), kBrown(185, 110, 60);

SillyMode::Place placeFor(const std::string& n) {
    using P = SillyMode::Place;
    if (n == "Saturn V") return P::SaturnV;
    if (n == "ISS") return P::Iss;
    if (n == "Moon") return P::Moon;
    if (n == "Mars") return P::Mars;
    if (n == "Perseverance") return P::Perseverance;
    if (n == "Jupiter") return P::Jupiter;
    if (n == "Io") return P::Io;
    if (n == "Saturn") return P::Saturn;
    if (n == "Uranus") return P::Uranus;
    if (n == "Neptune") return P::Neptune;
    if (n == "Sun") return P::Sun;
    if (n == "Hubble" || n == "JWST") return P::Telescope;
    if (n == "Earth") return P::Earth;
    return P::Other;
}

// A recorded take: decoded to mono, trimmed to the sound (the files carry up to two seconds of room tone),
// resampled to the output rate and levelled like the synthesised clips. Null if the file will not decode.
Clip loadTake(const std::filesystem::path& path, float rate) {
    int channels = 0, fileRate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_filename(path.string().c_str(), &channels, &fileRate, &pcm);
    if (frames <= 0 || !pcm) {
        LOG_WARN("Silly mode: cannot decode {}", path.string());
        return nullptr;
    }
    std::vector<float> mono((size_t)frames);
    for (int i = 0; i < frames; ++i) {
        float s = 0.f;
        for (int c = 0; c < channels; ++c) s += pcm[(size_t)i * channels + c];
        mono[i] = s / (32768.f * (float)channels);
    }
    std::free(pcm);

    // Trim to where the sound is above 3% of its peak (on a 10 ms envelope), keeping 20 ms either side.
    const size_t win = (size_t)(fileRate * 0.01f);
    float peak = 1e-6f;
    for (float v : mono) peak = std::max(peak, std::abs(v));
    size_t first = mono.size(), last = 0;
    float env = 0.f;
    for (size_t i = 0; i < mono.size(); ++i) {
        env += (std::abs(mono[i]) - env) / (float)win;
        if (env > peak * 0.03f) {
            first = std::min(first, i > win ? i - win : 0);
            last = i;
        }
    }
    if (first >= last) return nullptr;
    const size_t pad = (size_t)(fileRate * 0.02f);
    first = first > pad ? first - pad : 0;
    last = std::min(mono.size() - 1, last + pad);

    // Resample (linear is plenty for a fart) with 5 ms fades so the cut ends never click.
    const double step = (double)fileRate / rate;
    const size_t n = (size_t)((double)(last - first) / step);
    std::vector<float> out(n);
    const float fade = rate * 0.005f;
    for (size_t i = 0; i < n; ++i) {
        const double x = (double)first + (double)i * step;
        const size_t k = std::min((size_t)x, last - 1);
        const float f = (float)(x - (double)k);
        out[i] = (mono[k] + (mono[k + 1] - mono[k]) * f) * std::min({1.f, (float)i / fade, (float)(n - 1 - i) / fade});
    }
    return Synth::finish(std::move(out));
}

glm::vec2 riverPoint(float x) { return {x, 0.08f + 0.2f * std::sin(x * 4.5f + 0.6f)}; }

} // namespace

void SillyMode::toggle() {
    m_on = !m_on;
    LOG_INFO("Silly mode {}", m_on ? "on" : "off");
    // Until the clips exist: a Bluetooth headset can take longer to wake than start() waits, so the next
    // toggle tries again rather than leaving every fart silent for the session.
    if (m_on && !m_clips.fartShort) {
        if (m_sound.start()) {
            Synth s{(float)m_sound.sampleRate(), std::mt19937(7)};
            auto& c = m_clips;
            c.fartShort = Synth::finish(s.fart(0.55f, 150.f, 0.5f, 0.f, 0.f, 0.35f));
            c.fartMedium = Synth::finish(s.fart(1.1f, 105.f, 0.6f, 1.3f, 0.08f, 0.3f));
            c.fartLong = Synth::finish(s.fart(1.9f, 90.f, 0.7f, 0.9f, 0.12f, 0.25f));
            c.fartDeep = Synth::finish(s.fart(2.4f, 58.f, 0.8f, 0.6f, 0.1f, 0.25f));
            {
                // The big one: three swells and a long sputtering tail, over a rumble.
                auto epic = s.fart(1.3f, 70.f, 0.8f, 0.8f, 0.1f, 0.0f);
                s.mix(epic, s.fart(1.2f, 95.f, 0.9f, 2.f, 0.15f, 0.0f), 1.1f);
                s.mix(epic, s.fart(1.8f, 62.f, 0.9f, 0.5f, 0.1f, 0.6f), 2.1f);
                s.mix(epic, s.noiseBurst(3.9f, 20.f, 110.f, 0.4f), 0.f, 1.2f);
                c.fartEpic = Synth::finish(std::move(epic));
            }
            c.fartSqueaky = Synth::finish(s.fart(0.8f, 330.f, 0.25f, 5.f, 0.12f, 0.2f));
            c.fartBouncy = Synth::finish(s.fart(1.4f, 110.f, 0.5f, 7.f, 0.3f, 0.2f));
            c.fartCrackly = Synth::finish([&] {
                auto f = s.fart(1.3f, 85.f, 0.6f, 1.f, 0.1f, 0.3f);
                s.mix(f, s.noiseBurst(1.4f, 1500.f, 9000.f, 1.2f, 1.f), 0.f, 0.6f);
                return f;
            }());
            c.bloops = Synth::finish([&] {
                std::vector<float> b;
                for (int i = 0; i < 6; ++i) s.mix(b, s.sweep(0.13f, 220.f + 40.f * i, 700.f + 120.f * i, 0.f, 0.f, 12.f), 0.25f * i + 0.05f * (i % 2));
                return b;
            }());
            c.hiss = Synth::finish(s.noiseBurst(1.3f, 2500.f, 11000.f, 1.8f));
            c.sizzle = Synth::finish(s.noiseBurst(1.4f, 1800.f, 10000.f, 1.2f, 3.f));
            c.giggle = Synth::finish([&] {
                std::vector<float> g;
                for (int i = 0; i < 6; ++i) {
                    const float f = 980.f - 45.f * i;
                    s.mix(g, s.sweep(0.1f, f * 1.08f, f * 0.92f, 28.f, 0.04f, 9.f, 0.45f), 0.15f * i);
                }
                return g;
            }());
            c.slurp = Synth::finish([&] {
                auto sl = s.noiseBurst(0.45f, 250.f, 1600.f, 1.5f);
                for (size_t i = 0; i < sl.size(); ++i) sl[i] *= 0.6f + 0.4f * std::sin(kTau * 24.f * (float)i / s.rate);
                s.mix(sl, s.sweep(0.1f, 190.f, 85.f, 0.f, 0.f, 20.f), 0.42f, 1.4f);
                return sl;
            }());
            c.squeak = Synth::finish(s.sweep(0.22f, 1700.f, 2500.f, 38.f, 0.05f, 4.f));
            c.whistleUp = Synth::finish(s.sweep(0.5f, 450.f, 1700.f, 6.f, 0.02f, 1.f));
            c.boing = Synth::finish(s.sweep(0.8f, 170.f, 150.f, 11.f, 0.35f, 3.f, 0.3f));
            c.chirps = Synth::finish([&] {
                std::vector<float> ch;
                for (int i = 0; i < 7; ++i) s.mix(ch, s.sweep(0.07f, 2400.f + 200.f * (i % 3), 4100.f, 0.f, 0.f, 20.f), 0.12f * i + 0.03f * (i % 2));
                return ch;
            }());
            c.tada = Synth::finish([&] {
                std::vector<float> td;
                const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f};
                for (int i = 0; i < 4; ++i) s.mix(td, s.sweep(0.25f, notes[i], notes[i], 0.f, 0.f, 9.f, 0.3f), 0.09f * i);
                for (float n : notes) s.mix(td, s.sweep(0.9f, n, n, 5.f, 0.004f, 3.f, 0.3f), 0.38f, 0.5f);
                return td;
            }());
            c.sparkle = Synth::finish([&] {
                std::vector<float> sp;
                for (int i = 0; i < 9; ++i) {
                    const float f = 2000.f + 180.f * ((i * 5) % 9);
                    s.mix(sp, s.sweep(0.3f, f, f, 0.f, 0.f, 12.f), 0.07f * i, 0.6f);
                }
                return sp;
            }(), 0.6f);
            c.thud = Synth::finish([&] {
                auto t = s.sweep(0.45f, 75.f, 38.f, 0.f, 0.f, 7.f);
                s.mix(t, s.noiseBurst(0.2f, 30.f, 400.f, 15.f), 0.f, 0.7f);
                return t;
            }());
            c.whoosh = Synth::finish([&] {
                auto w = s.buffer(0.9f);
                float lp = 0.f;
                for (size_t i = 0; i < w.size(); ++i) {
                    const float u = (float)i / (float)w.size();
                    lp += (s.noise() - lp) * s.coef(300.f + 3000.f * std::sin(3.14159f * u));
                    w[i] = lp * std::sin(3.14159f * u);
                }
                return w;
            }());
            c.rumble = Synth::finish(s.noiseBurst(2.6f, 15.f, 90.f, 0.5f));
            c.splash = Synth::finish([&] {
                auto sp = s.noiseBurst(0.5f, 300.f, 5000.f, 7.f);
                for (int i = 0; i < 4; ++i) s.mix(sp, s.sweep(0.08f, 700.f + 300.f * i, 1500.f + 400.f * i, 0.f, 0.f, 30.f), 0.08f + 0.07f * i, 0.4f);
                return sp;
            }());
            c.tink = Synth::finish([&] {
                auto t = s.sweep(0.4f, 3300.f, 3300.f, 0.f, 0.f, 11.f);
                s.mix(t, s.sweep(0.3f, 5100.f, 5100.f, 0.f, 0.f, 16.f), 0.f, 0.4f);
                return t;
            }(), 0.5f);
            c.tch = Synth::finish(s.noiseBurst(0.06f, 3000.f, 12000.f, 40.f), 0.4f);
            loadRecordings();
        }
    }
    if (!m_on) {
        m_sound.setTrickle(false);
        reset();
        m_particles.clear();
    }
}

void SillyMode::sfx(const Clip& clip, float gain, float pitch) {
    for (const Takes& t : m_takes)
        if (t.synth == clip && !t.takes.empty()) {
            m_sound.play(t.takes[m_rng() % t.takes.size()], gain, pitch);
            return;
        }
    m_sound.play(clip, gain, pitch);
}

void SillyMode::loadRecordings() {
    m_takes.clear();
    const std::filesystem::path dir = assetDirectory() / "sounds";
    std::ifstream list(dir / "farts.txt");
    if (!list) return;
    const auto& c = m_clips;
    const std::pair<const char*, Clip> kinds[] = {
        {"short", c.fartShort}, {"medium", c.fartMedium}, {"long", c.fartLong},       {"deep", c.fartDeep},
        {"epic", c.fartEpic},   {"squeaky", c.fartSqueaky}, {"bouncy", c.fartBouncy}, {"crackly", c.fartCrackly}};
    for (const auto& [name, synth] : kinds) m_takes.push_back({synth, {}});
    std::map<std::string, Clip> decoded; // a file cast as several kinds is decoded once
    std::string line;
    int count = 0;
    while (std::getline(list, line)) {
        std::istringstream in(line);
        std::string kind, file;
        if (!(in >> kind >> file) || kind[0] == '#') continue;
        size_t k = 0;
        while (k < std::size(kinds) && kind != kinds[k].first) ++k;
        if (k == std::size(kinds)) {
            LOG_WARN("farts.txt: unknown kind '{}'", kind);
            continue;
        }
        auto it = decoded.find(file);
        if (it == decoded.end()) it = decoded.emplace(file, loadTake(dir / file, (float)m_sound.sampleRate())).first;
        if (it->second) {
            m_takes[k].takes.push_back(it->second);
            ++count;
        }
    }
    LOG_INFO("Silly mode: {} recorded farts from {} files", count, decoded.size());
}

void SillyMode::reset() {
    m_fartT = m_jiggleT = m_wiggleT = m_ringT = m_shakeT = 99.f;
    m_head = m_tail = m_hitT = 0.f;
    m_peeHeld = false;
    m_craters.clear();
    m_pool = m_river = m_stripes = m_yellow = m_rainbow = 0.f;
    m_tadaPlayed = false;
}

void SillyMode::add(Particle::Kind kind, glm::vec2 pos, glm::vec2 vel, float size, float life, glm::vec3 color,
                    float grow, float grav, float drag) {
    if (m_particles.size() > 900) m_particles.erase(m_particles.begin(), m_particles.begin() + 100);
    Particle p;
    p.kind = kind;
    p.pos = pos;
    p.vel = vel;
    p.size = size;
    p.life = life;
    p.color = color;
    p.grow = grow;
    p.grav = grav;
    p.drag = drag;
    p.seed = rnd(0.f, 100.f);
    m_particles.push_back(p);
}

void SillyMode::fartCloud(glm::vec2 at, glm::vec2 dir, int count, float spread, glm::vec3 color) {
    for (int i = 0; i < count; ++i) {
        const glm::vec2 v = (dir + glm::vec2(rnd(-spread, spread), rnd(-spread, spread))) * m_scale * rnd(0.6f, 1.4f);
        const glm::vec3 c = color * rnd(0.85f, 1.1f);
        add(Particle::Puff, at + glm::vec2(rnd(-0.1f, 0.1f), rnd(-0.1f, 0.1f)) * m_scale, v, m_scale * rnd(0.12f, 0.24f),
            rnd(1.4f, 2.4f), c, m_scale * 0.22f, 0.f, 1.6f);
    }
    for (int i = 0; i < 3; ++i)
        add(Particle::Stink, at + glm::vec2(rnd(-0.4f, 0.4f), rnd(-0.3f, 0.1f)) * m_scale, glm::vec2(0.f, -m_scale * 0.35f),
            m_scale * 0.25f, 1.6f, kGreen * 0.8f);
}

void SillyMode::startFart() {
    const float pitch = rnd(0.92f, 1.08f);
    m_fartT = 0.f;
    m_emitAcc = 0.f;
    const auto& c = m_clips;
    switch (m_place) {
    case Place::SaturnV:
        sfx(c.fartDeep, 1.f, pitch);
        sfx(c.rumble, 0.8f);
        m_shakeT = 0.f;
        m_shakeAmp = 0.004f;
        break;
    case Place::Iss:
        sfx(c.fartSqueaky, 1.f, pitch);
        fartCloud(unit({-0.9f, 0.f}), {-1.f, 0.f}, 8, 0.4f, kGreen);
        break;
    case Place::Moon: {
        sfx(c.fartMedium, 1.f, pitch);
        sfx(c.thud, 0.9f);
        const float a = rnd(0.f, kTau), r = std::sqrt(rnd(0.f, 1.f)) * 0.6f;
        const Crater cr{{std::cos(a) * r, std::sin(a) * r}, rnd(0.09f, 0.16f)};
        m_craters.push_back(cr);
        if (m_craters.size() > 12) m_craters.erase(m_craters.begin());
        fartCloud(disc(cr.at), {0.f, -0.8f}, 10, 0.7f, glm::mix(kGreen, glm::vec3(170.f), 0.4f));
        m_jiggleT = 0.f;
        m_jiggleAmp = 0.02f;
        break;
    }
    case Place::Mars:
        sfx(c.fartLong, 1.f, pitch * 0.9f);
        sfx(c.whoosh, 0.9f, 0.7f);
        break;
    case Place::Perseverance:
        sfx(c.fartShort, 1.f, pitch);
        sfx(c.whoosh, 0.9f, 1.2f);
        break;
    case Place::Jupiter:
        sfx(c.fartDeep, 1.f, pitch * 0.85f);
        fartCloud(disc({0.35f, 0.3f}), {0.3f, 0.2f}, 14, 0.9f, glm::mix(kGreen, kBrown, 0.45f));
        m_jiggleT = 0.f;
        m_jiggleAmp = 0.035f;
        m_shakeT = 0.f;
        m_shakeAmp = 0.003f;
        break;
    case Place::Io:
        sfx(c.fartMedium, 1.f, pitch * 1.1f);
        sfx(c.thud, 0.6f, 1.4f);
        break;
    case Place::Saturn:
        sfx(c.fartBouncy, 1.f, pitch);
        sfx(c.boing, 0.8f);
        fartCloud(unit({0.f, 0.95f}), {0.f, 1.f}, 10, 0.8f, kGreen);
        m_ringT = 0.f;
        m_jiggleT = 0.f;
        m_jiggleAmp = 0.03f;
        break;
    case Place::Uranus:
        sfx(c.fartEpic, 1.f, pitch);
        m_jiggleT = 0.f;
        m_jiggleAmp = 0.06f;
        m_wiggleT = 0.f;
        m_wiggleAmp = 0.25f;
        m_shakeT = 0.f;
        m_shakeAmp = 0.011f;
        break;
    case Place::Neptune:
        sfx(c.fartShort, 0.7f, pitch * 0.8f);
        sfx(c.bloops, 1.f);
        break;
    case Place::Sun:
        sfx(c.fartCrackly, 1.f, pitch);
        sfx(c.sizzle, 0.6f);
        break;
    case Place::Telescope:
        sfx(c.fartSqueaky, 0.8f, pitch * 1.3f);
        sfx(c.whistleUp, 0.9f);
        fartCloud(unit({0.f, 0.9f}), {0.f, 0.8f}, 5, 0.5f, kGreen);
        break;
    case Place::Earth:
        sfx(c.fartMedium, 1.f, pitch);
        sfx(c.chirps, 0.9f);
        fartCloud(unit({0.f, 0.95f}), {0.f, 0.9f}, 10, 0.8f, kGreen);
        for (int i = 0; i < 9; ++i) {
            const float a = rnd(-2.6f, -0.5f); // upper half of the disc, screen angles
            const glm::vec2 d(std::cos(a), std::sin(a));
            add(Particle::Bird, unit(d * 0.8f), d * m_scale * rnd(0.9f, 1.6f), m_scale * rnd(0.07f, 0.12f), 3.5f,
                glm::vec3(35.f));
        }
        m_jiggleT = 0.f;
        m_jiggleAmp = 0.015f;
        break;
    case Place::Other: {
        const Clip* pick[] = {&c.fartShort, &c.fartMedium, &c.fartLong, &c.fartBouncy};
        sfx(*pick[m_rng() % 4], 1.f, pitch);
        fartCloud(unit({0.f, 0.9f}), {0.f, 0.9f}, 12, 0.8f, kGreen);
        m_jiggleT = 0.f;
        m_jiggleAmp = 0.02f;
        break;
    }
    }
}

void SillyMode::emitContinuous(float dt) {
    auto every = [&](float& acc, float rate, auto&& fn) {
        acc += dt * rate;
        while (acc >= 1.f) {
            acc -= 1.f;
            fn();
        }
    };
    const float t = m_fartT;
    switch (m_place) {
    case Place::SaturnV:
        if (t < 3.0f)
            every(m_emitAcc, 28.f, [&] {
                add(Particle::Puff, unit({rnd(-0.15f, 0.15f), 0.95f}),
                    glm::vec2(rnd(-1.2f, 1.2f), rnd(0.2f, 0.9f)) * m_scale, m_scale * rnd(0.12f, 0.22f), rnd(1.6f, 2.6f),
                    kGreen * rnd(0.85f, 1.1f), m_scale * 0.3f, 0.f, 1.4f);
            });
        break;
    case Place::Iss:
        if (t < 1.2f)
            every(m_emitAcc, 18.f, [&] {
                add(Particle::Puff, unit({-0.9f, rnd(-0.1f, 0.1f)}), glm::vec2(rnd(-1.f, -0.3f), rnd(-0.3f, 0.3f)) * m_scale,
                    m_scale * rnd(0.08f, 0.15f), 1.6f, kGreen, m_scale * 0.2f, 0.f, 1.5f);
            });
        break;
    case Place::Mars:
        if (t < 2.4f)
            every(m_emitAcc, 30.f, [&] {
                add(Particle::Puff, unit({rnd(-0.3f, 0.3f), 1.0f}), glm::vec2(rnd(0.5f, 1.6f), rnd(0.1f, 0.8f)) * m_scale,
                    m_scale * rnd(0.12f, 0.25f), rnd(1.8f, 2.8f), glm::mix(kBrown, glm::vec3(220, 150, 90), rnd(0.f, 1.f)),
                    m_scale * 0.35f, 0.f, 0.8f);
            });
        break;
    case Place::Perseverance:
        if (t < 1.4f)
            every(m_emitAcc, 30.f, [&] {
                add(Particle::Puff, unit({rnd(-0.2f, 0.2f), rnd(-0.1f, 0.3f)}), glm::vec2(rnd(-0.4f, 0.4f), rnd(-0.4f, 0.1f)) * m_scale,
                    m_scale * rnd(0.1f, 0.2f), 1.8f, kGreen, m_scale * 0.25f, 0.f, 2.f);
            });
        break;
    case Place::Jupiter:
        if (t < 1.6f)
            every(m_emitAcc, 16.f, [&] {
                add(Particle::Puff, disc({0.35f, 0.3f}), glm::vec2(rnd(-0.6f, 1.f), rnd(-0.6f, 0.8f)) * m_scale,
                    m_scale * rnd(0.12f, 0.22f), 2.2f, glm::mix(kGreen, kBrown, rnd(0.2f, 0.6f)), m_scale * 0.3f, 0.f, 1.2f);
            });
        break;
    case Place::Io:
        if (t < 2.f)
            every(m_emitAcc, 45.f, [&] {
                add(Particle::Plume, unit({0.12f, -0.95f}), glm::vec2(rnd(-0.35f, 0.35f), -rnd(1.6f, 2.5f)) * m_scale,
                    m_scale * rnd(0.05f, 0.1f), 2.2f, kGreen * rnd(0.8f, 1.15f), m_scale * 0.04f, m_scale * 2.4f, 0.f);
            });
        break;
    case Place::Uranus:
        if (t < 3.9f)
            every(m_emitAcc, 32.f, [&] {
                add(Particle::Puff, unit({rnd(-0.4f, 0.4f), 1.0f}), glm::vec2(rnd(-1.8f, 1.8f), rnd(0.3f, 1.6f)) * m_scale,
                    m_scale * rnd(0.2f, 0.4f), rnd(2.f, 3.2f), kGreen * rnd(0.8f, 1.1f), m_scale * 0.45f, 0.f, 1.f);
                if (m_rng() % 6 == 0)
                    add(Particle::Stink, unit({rnd(-0.9f, 0.9f), rnd(0.8f, 1.2f)}), glm::vec2(0.f, -m_scale * 0.4f),
                        m_scale * 0.3f, 1.8f, kGreen * 0.8f);
            });
        break;
    case Place::Neptune:
        if (t < 2.2f)
            every(m_emitAcc, 7.f, [&] {
                add(Particle::Bubble, unit({rnd(-0.6f, 0.6f), rnd(-0.2f, 0.6f)}), glm::vec2(0.f, -m_scale * rnd(0.3f, 0.6f)),
                    m_scale * rnd(0.08f, 0.2f), rnd(1.8f, 2.8f), glm::vec3(170, 230, 190), m_scale * 0.05f);
            });
        break;
    case Place::Sun:
        if (t > 0.3f && t < 1.4f)
            every(m_emitAcc, 30.f, [&] {
                const glm::vec2 apex = unit({0.95f, -1.35f});
                add(Particle::Spark, apex, glm::vec2(rnd(-1.f, 1.f), rnd(-1.f, 0.6f)) * m_scale, m_scale * 0.03f, 1.2f,
                    glm::vec3(255, rnd(150.f, 230.f), 60), 0.f, m_scale * 0.8f);
            });
        break;
    default:
        break;
    }
}

glm::vec2 SillyMode::peeTarget() const {
    switch (m_place) {
    case Place::SaturnV: return unit({0.f, 0.9f});
    case Place::Moon: return m_discVisible ? disc({0.25f, 0.22f}) : unit({0.25f, 0.22f});
    case Place::Mars: {
        const glm::vec2 u = riverPoint(-0.75f + 1.5f * std::min(m_river, 1.f));
        return m_discVisible ? disc(u) : unit(u);
    }
    case Place::Io: return unit({0.1f, -0.2f});
    default: return m_center;
    }
}

void SillyMode::update(double dtD, bool fartPressed, bool peeDown, const Tour& tour, const SolarSystem& solar,
                       const CraftCatalog& crafts, render::FrameScene& scene, Camera& renderCamera,
                       const glm::dvec3& cameraPos, int width, int height) {
    if (!m_on) return;
    const float dt = (float)dtD;
    m_time += dtD;
    m_w = width;
    m_h = height;

    // Where are we?
    const int key = !tour.active() ? -2 : tour.introActive() ? -1 : tour.currentStop();
    if (key != m_stopKey) {
        m_stopKey = key;
        reset();
        m_sound.setTrickle(false);
        m_craft = key >= 0 ? tour.targetCraft() : -1;
        m_body = key >= 0 && m_craft < 0 ? tour.targetBody() : -1;
        m_place = m_craft >= 0 ? placeFor(crafts.crafts()[m_craft].name)
                  : m_body >= 0 ? placeFor(solar.body(m_body).name)
                                : Place::Other;
    }

    // Spacecraft gags move the model: work out the offset first so the anchor follows it.
    glm::dvec3 craftOffset{0.0};
    glm::vec3 craftUp = renderCamera.up();
    double craftSize = 0.0;
    if (m_craft >= 0 && m_craft < (int)scene.crafts.size()) {
        const CraftGpu& g = scene.crafts[m_craft];
        if (glm::length(glm::vec3(g.up)) > 0.5f) craftUp = glm::normalize(glm::vec3(g.up));
        craftSize = crafts.crafts()[m_craft].sizeMeters / (kKmPerParsec * 1000.0);
        const float t = m_fartT;
        if (m_place == Place::SaturnV) {
            float lift = 0.f;
            if (t < 2.2f) lift = 1.6f * std::pow(std::clamp((t - 0.5f) / 1.7f, 0.f, 1.f), 2.f);
            else if (t < 3.4f) lift = 1.6f + 0.05f * std::sin((t - 2.2f) * 6.f);
            else if (t < 5.6f) lift = 1.6f * (1.f - ease(t, 3.4f, 5.6f));
            craftOffset = glm::dvec3(craftUp) * (double)lift * craftSize;
        } else if (m_place == Place::Perseverance && t < 4.6f) {
            glm::vec3 fwd = glm::vec3(g.model[0]);
            fwd -= craftUp * glm::dot(fwd, craftUp);
            if (glm::length(fwd) < 1e-30f) fwd = renderCamera.right();
            fwd = glm::normalize(fwd);
            float x = 0.f;
            if (t < 1.3f) x = 6.f * std::pow(std::clamp((t - 0.25f) / 1.05f, 0.f, 1.f), 2.f);
            else if (t < 2.8f) x = 6.f;
            else x = -6.f * (1.f - ease(t, 2.8f, 4.6f)); // back in from the other side
            craftOffset = glm::dvec3(fwd) * (double)x * craftSize;
        }
    }

    // Screen anchor.
    const glm::mat4 vp = render::makeViewProj(renderCamera, (float)width / (float)height);
    glm::dvec3 rel{0.0};
    double radius = 0.0;
    bool haveTarget = false;
    if (m_craft >= 0) {
        rel = crafts.crafts()[m_craft].position - cameraPos + craftOffset;
        radius = craftSize * 0.5;
        haveTarget = true;
    } else if (m_body >= 0) {
        rel = solar.body(m_body).position - cameraPos;
        radius = solar.body(m_body).radiusKm / kKmPerParsec;
        haveTarget = true;
    }
    glm::vec2 center(width * 0.5f, height * 0.5f);
    float rpx = height * 0.22f;
    m_discVisible = false;
    if (haveTarget) {
        // Project the direction only: world units are parsecs, so a raw position loses precision in float.
        const double d = glm::length(rel);
        const glm::vec3 dir = glm::vec3(rel / std::max(d, 1e-300));
        const float along = glm::dot(dir, renderCamera.forward());
        if (along > 0.05f) {
            const glm::vec4 p = vp * glm::vec4(dir, 1.f);
            center = glm::vec2((p.x / p.w * 0.5f + 0.5f) * width, (p.y / p.w * 0.5f + 0.5f) * height);
            const double s = radius / std::sqrt(std::max(d * d - radius * radius, d * d * 1e-4));
            rpx = (float)(s / std::tan(renderCamera.fovY * 0.5f) * height * 0.5 / along);
            m_discVisible = true;
        }
    }
    m_discCenter = center;
    m_discRadius = std::min(rpx, (float)std::max(width, height) * 3.f);
    m_center = glm::clamp(center, glm::vec2(width * 0.15f, height * 0.18f), glm::vec2(width * 0.85f, height * 0.82f));
    m_scale = std::clamp(rpx, height * 0.07f, height * 0.36f);

    // Buttons.
    if (fartPressed) startFart();
    if (peeDown && !m_peeHeld) {
        m_peeHeld = true;
        m_head = 0.f;
        m_tail = 0.f;
        m_hitT = 0.f;
        m_peeSfxT = 0.f;
        m_sound.setTrickle(true);
    } else if (!peeDown && m_peeHeld) {
        m_peeHeld = false;
        m_sound.setTrickle(false);
    }
    if (m_peeHeld) m_head = std::min(1.f, m_head + dt * 4.f);
    else if (m_head > 0.f) {
        m_tail = std::min(1.f, m_tail + dt * 3.5f);
        if (m_tail >= 1.f) m_head = m_tail = 0.f;
    }
    const bool hitting = m_peeHeld && m_head >= 1.f;

    // Fart timers and emission.
    m_fartT += dt;
    m_jiggleT += dt;
    m_wiggleT += dt;
    m_ringT += dt;
    m_shakeT += dt;
    emitContinuous(dt);

    // Pee, per place.
    if (hitting) {
        const bool first = m_hitT == 0.f;
        m_hitT += dt;
        m_peeSfxT -= dt;
        const glm::vec2 at = peeTarget();
        auto splash = [&](float rate) {
            m_peeEmitAcc += dt * rate;
            while (m_peeEmitAcc >= 1.f) {
                m_peeEmitAcc -= 1.f;
                add(Particle::Drop, at, glm::vec2(rnd(-0.9f, 0.9f), rnd(-1.4f, -0.4f)) * m_scale, m_scale * rnd(0.015f, 0.03f),
                    0.9f, kYellow, 0.f, m_scale * 3.5f);
            }
        };
        const auto& c = m_clips;
        switch (m_place) {
        case Place::SaturnV:
        case Place::Io:
        case Place::Sun:
            if (m_peeSfxT <= 0.f) {
                sfx(m_place == Place::Sun ? c.sizzle : c.hiss, 0.8f, rnd(0.9f, 1.1f));
                m_peeSfxT = 1.1f;
            }
            m_peeEmitAcc += dt * 14.f;
            while (m_peeEmitAcc >= 1.f) {
                m_peeEmitAcc -= 1.f;
                add(Particle::Steam, at, glm::vec2(rnd(-0.4f, 0.4f), -rnd(0.5f, 1.1f)) * m_scale, m_scale * rnd(0.08f, 0.16f),
                    rnd(1.4f, 2.2f), kSteam, m_scale * 0.3f, 0.f, 0.6f);
            }
            break;
        case Place::Iss:
            if (m_peeSfxT <= 0.f) {
                sfx(c.tink, 0.7f, rnd(0.8f, 1.3f));
                m_peeSfxT = rnd(0.2f, 0.45f);
            }
            m_peeEmitAcc += dt * 8.f;
            while (m_peeEmitAcc >= 1.f) {
                m_peeEmitAcc -= 1.f;
                add(Particle::Ice, at, glm::vec2(rnd(-0.5f, 0.5f), rnd(-0.5f, 0.5f)) * m_scale, m_scale * rnd(0.04f, 0.08f),
                    14.f, kYellow, 0.f, 0.f, 0.3f);
            }
            break;
        case Place::Moon:
            if (first) sfx(c.splash, 0.9f);
            m_pool = std::min(1.f, m_pool + dt * 0.3f);
            splash(10.f);
            break;
        case Place::Mars:
            if (first) sfx(c.splash, 0.8f, 0.8f);
            m_river = std::min(1.f, m_river + dt * 0.35f);
            splash(12.f);
            break;
        case Place::Perseverance:
            if (m_peeSfxT <= 0.f) {
                sfx(c.slurp, 1.f, rnd(0.9f, 1.1f));
                m_peeSfxT = 0.75f;
            }
            break;
        case Place::Jupiter:
            if (first) sfx(c.whoosh, 0.8f, 0.6f);
            m_stripes = std::min(1.f, m_stripes + dt * 0.5f);
            splash(10.f);
            break;
        case Place::Saturn: {
            if (m_peeSfxT <= 0.f) {
                sfx(c.tch, 0.8f, rnd(0.9f, 1.2f));
                m_peeSfxT = 0.18f;
            }
            m_peeEmitAcc += dt * 45.f;
            while (m_peeEmitAcc >= 1.f) {
                m_peeEmitAcc -= 1.f;
                const float a = (float)m_time * 5.f + rnd(-0.15f, 0.15f);
                const glm::vec2 dir(std::cos(a), std::sin(a) * 0.3f);
                add(Particle::Drop, unit(dir * 1.9f), (dir * 2.2f + glm::vec2(0.f, -0.5f)) * m_scale, m_scale * rnd(0.02f, 0.04f),
                    1.2f, kYellow, m_scale * 0.06f, m_scale * 1.5f);
            }
            if (m_rng() % 1000 < (unsigned)(dt * 1800.f))
                add(Particle::Splat, glm::vec2(rnd(0.05f, 0.95f) * width, rnd(0.05f, 0.7f) * height), glm::vec2(0.f),
                    height * rnd(0.025f, 0.05f), 3.f, kYellow);
            m_ringT = 0.5f; // keep the rings rocking while it sprays
            break;
        }
        case Place::Uranus:
            if (m_peeSfxT <= 0.f) {
                sfx(c.giggle, 1.f, rnd(0.95f, 1.1f));
                m_peeSfxT = 1.5f;
            }
            m_wiggleT = 0.3f;
            m_wiggleAmp = 0.3f;
            m_jiggleT = 0.3f;
            m_jiggleAmp = 0.035f;
            splash(8.f);
            break;
        case Place::Neptune:
            if (first) sfx(c.sparkle, 1.f);
            m_yellow = std::min(1.f, m_yellow + dt * 0.45f);
            if (m_rng() % 100 < 30)
                add(Particle::Spark, unit({rnd(-1.1f, 1.1f), rnd(-1.1f, 1.1f)}), glm::vec2(0.f), m_scale * 0.05f, 0.7f,
                    glm::vec3(255, 245, 160));
            splash(8.f);
            break;
        case Place::Telescope: {
            const float x = std::sin((float)m_time * 4.f);
            if ((x > 0.f) != (m_wipeLast > 0.f)) sfx(c.squeak, 0.7f, rnd(0.9f, 1.15f));
            m_wipeLast = x;
            splash(6.f);
            break;
        }
        case Place::Earth:
            m_rainbow = std::min(1.f, m_rainbow + dt * 0.8f);
            if (m_rainbow > 0.4f && !m_tadaPlayed) {
                sfx(c.tada, 1.f);
                m_tadaPlayed = true;
            }
            if (m_rng() % 100 < 25)
                add(Particle::Spark, unit({rnd(-1.5f, 1.5f), rnd(-1.5f, -0.4f)}), glm::vec2(0.f), m_scale * 0.05f, 0.8f,
                    glm::vec3(255, 255, 220));
            splash(10.f);
            break;
        case Place::Other:
            splash(14.f);
            break;
        }
    } else {
        m_hitT = 0.f;
    }

    // Particles.
    std::vector<Particle> pops;
    for (Particle& p : m_particles) {
        p.age += dt;
        p.vel.y += p.grav * dt;
        p.vel *= std::exp(-p.drag * dt);
        p.pos += p.vel * dt;
        p.size += p.grow * dt;
        if (p.kind != Particle::Bubble) continue;
        p.pos.x += std::sin(p.age * 5.f + p.seed) * p.size * 1.5f * dt;
        if (p.age >= p.life) {
            for (int i = 0; i < 6; ++i) {
                const float a = kTau * (float)i / 6.f;
                Particle s;
                s.kind = Particle::Spark;
                s.pos = p.pos;
                s.vel = glm::vec2(std::cos(a), std::sin(a)) * p.size * 3.f;
                s.size = p.size * 0.2f;
                s.life = 0.3f;
                s.color = glm::vec3(220, 250, 230);
                pops.push_back(s);
            }
        }
    }
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(), [](const Particle& p) { return p.age >= p.life; }),
                      m_particles.end());
    m_particles.insert(m_particles.end(), pops.begin(), pops.end());

    // Bend what the renderer draws.
    auto withAxis = [](const glm::vec4& q, float angle, const glm::vec3& axis) {
        const glm::quat r = glm::angleAxis(angle, axis) * glm::quat(q.w, q.x, q.y, q.z);
        return glm::vec4(r.x, r.y, r.z, r.w);
    };
    if (m_body >= 0 && m_body < (int)scene.sphereCount) {
        BodyGpu& g = scene.bodies[m_body];
        const glm::vec3 toBody = glm::length(glm::vec3(g.posRadius)) > 0.f ? glm::normalize(glm::vec3(g.posRadius)) : renderCamera.forward();
        if (m_jiggleT < 5.f) g.posRadius.w *= 1.f + m_jiggleAmp * std::sin(kTau * 6.f * m_jiggleT) * std::exp(-2.f * m_jiggleT);
        if (m_wiggleT < 6.f) g.rotation = withAxis(g.rotation, m_wiggleAmp * std::sin(kTau * 2.2f * m_wiggleT) * std::exp(-0.8f * m_wiggleT), toBody);
        if (m_ringT < 5.f && solar.body(m_body).ringInner > 0.f) {
            size_t ring = scene.sphereCount;
            for (int i = 0; i < m_body; ++i)
                if (solar.body(i).ringInner > 0.f) ++ring;
            if (ring < scene.bodies.size()) {
                BodyGpu& r = scene.bodies[ring];
                const float wob = 0.3f * std::sin(kTau * 2.4f * m_ringT) * std::exp(-1.1f * m_ringT);
                r.rotation = withAxis(r.rotation, wob, renderCamera.right());
                g.rotation = withAxis(g.rotation, wob * 0.3f, renderCamera.right()); // the planet's ring shadow follows
            }
        }
    }
    if (m_craft >= 0 && m_craft < (int)scene.crafts.size()) {
        glm::mat4& m = scene.crafts[m_craft].model;
        const glm::vec3 pivot = glm::vec3(m[3]);
        auto around = [&](const glm::mat4& op) {
            m = glm::translate(glm::mat4(1.f), pivot) * op * glm::translate(glm::mat4(1.f), -pivot) * m;
        };
        const float t = m_fartT;
        if (m_place == Place::Iss && t < 4.f) around(glm::rotate(glm::mat4(1.f), 2.f * kTau * ease(t, 0.f, 4.f), craftUp));
        if (m_place == Place::Telescope && t < 3.6f) {
            float a = 0.f;
            if (t < 0.2f) a = 1.75f * t / 0.2f;
            else if (t < 2.2f) a = 1.75f + 0.04f * std::sin(t * 45.f);
            else a = 1.75f * (1.f - ease(t, 2.2f, 3.6f));
            around(glm::rotate(glm::mat4(1.f), a, craftUp));
        }
        if (m_place == Place::Perseverance && hitting) {
            const float s = 1.f + 0.06f * std::sin(kTau * 3.f * m_hitT);
            around(glm::scale(glm::mat4(1.f), glm::vec3(s)));
        }
        m[3] += glm::vec4(glm::vec3(craftOffset), 0.f);
    }
    if (m_shakeT < 2.5f) {
        const float a = m_shakeAmp * std::exp(-1.4f * m_shakeT);
        const float tt = m_shakeT;
        renderCamera.orientation = glm::normalize(
            renderCamera.orientation * glm::angleAxis(a * (std::sin(tt * 37.f) + 0.6f * std::sin(tt * 53.f + 1.f)), glm::vec3(1, 0, 0)) *
            glm::angleAxis(a * (std::sin(tt * 41.f + 2.f) + 0.6f * std::sin(tt * 29.f)), glm::vec3(0, 1, 0)));
    }
}

void SillyMode::draw(ImDrawList* dl, int width, int height) const {
    if (!m_on) return;
    const float t = (float)m_time;

    // Things painted onto the planet.
    if (m_discVisible) {
        for (const Crater& c : m_craters) {
            const glm::vec2 p = disc(c.at);
            const float r = c.r * m_discRadius;
            dl->AddEllipseFilled(V(p), ImVec2(r * 1.12f, r * 0.92f), C(glm::vec3(200), 0.8f));
            dl->AddEllipseFilled(V(p + glm::vec2(r * 0.06f, r * 0.05f)), ImVec2(r, r * 0.8f), C(glm::vec3(55), 0.9f));
            dl->AddEllipseFilled(V(p + glm::vec2(r * 0.2f, r * 0.18f)), ImVec2(r * 0.6f, r * 0.45f), C(glm::vec3(90), 0.9f));
        }
        if (m_pool > 0.f) {
            const glm::vec2 p = disc({0.25f, 0.22f});
            const float rx = 0.3f * m_pool * m_discRadius, ry = 0.14f * m_pool * m_discRadius;
            dl->AddEllipseFilled(V(p), ImVec2(rx * 1.08f, ry * 1.12f), C(glm::vec3(120), 0.9f));
            dl->AddEllipseFilled(V(p), ImVec2(rx, ry), C(kYellow, 0.92f));
            dl->AddEllipseFilled(V(p - glm::vec2(rx * 0.3f, ry * 0.3f)), ImVec2(rx * 0.35f, ry * 0.25f), C(glm::vec3(255, 250, 200), 0.8f));
            if (m_peeHeld)
                for (int i = 0; i < 3; ++i) {
                    const float k = std::fmod(t * 0.9f + i / 3.f, 1.f);
                    dl->AddEllipse(V(p), ImVec2(rx * k, ry * k), C(glm::vec3(255, 245, 170), 1.f - k), 0.f, 0, 2.f);
                }
        }
        if (m_river > 0.f) {
            std::vector<ImVec2> pts;
            const int n = 48;
            for (int i = 0; i <= (int)(n * m_river); ++i) pts.push_back(V(disc(riverPoint(-0.75f + 1.5f * (float)i / n))));
            if (pts.size() > 1) {
                const float th = 0.055f * m_discRadius;
                dl->AddPolyline(pts.data(), (int)pts.size(), C(glm::vec3(120, 70, 30), 0.9f), 0, th * 1.35f);
                dl->AddPolyline(pts.data(), (int)pts.size(), C(kYellow, 0.95f), 0, th);
            }
        }
        if (m_stripes > 0.f) {
            for (int b = -3; b <= 3; ++b) {
                const float y = b * 0.24f;
                const float hw = std::sqrt(std::max(0.f, 1.f - y * y)) * 0.96f;
                std::vector<ImVec2> pts;
                for (int i = 0; i <= 40; ++i) {
                    const float x = -hw + 2.f * hw * (float)i / 40.f;
                    const float yy = y + 0.04f * std::sin(x * 7.f + t * (m_peeHeld ? 6.f : 1.5f) + b);
                    pts.push_back(V(disc({x, yy})));
                }
                dl->AddPolyline(pts.data(), (int)pts.size(), C(kYellow, 0.6f * m_stripes), 0, 0.08f * m_discRadius);
            }
        }
        if (m_yellow > 0.f) dl->AddCircleFilled(V(m_discCenter), m_discRadius * 0.995f, C(kYellow, 0.72f * m_yellow), 96);
    }
    if (m_rainbow > 0.f) {
        const glm::vec3 bands[] = {{235, 60, 60}, {245, 150, 50}, {250, 225, 60}, {90, 200, 90}, {70, 150, 235}, {140, 90, 210}};
        const float bw = m_scale * 0.09f;
        for (int i = 0; i < 6; ++i) {
            dl->PathArcTo(V(m_center), m_scale * 1.55f - bw * i, 3.14159f, 3.14159f + 3.14159f * std::min(1.f, m_rainbow * 1.6f), 64);
            dl->PathStroke(C(bands[i], 0.85f * std::min(1.f, m_rainbow * 1.5f)), 0, bw);
        }
    }

    // Sun flare: a loop off the limb.
    if (m_place == Place::Sun && m_fartT < 2.6f) {
        const float grow = ease(m_fartT, 0.f, 0.6f), fade = 1.f - ease(m_fartT, 1.6f, 2.6f);
        const glm::vec2 a = unit({0.55f, -0.8f}), b = unit({1.0f, -0.1f}), ctrl = unit({1.6f, -1.8f});
        std::vector<ImVec2> pts;
        for (int i = 0; i <= (int)(30 * grow); ++i) {
            const float u = (float)i / 30.f;
            pts.push_back(V((1 - u) * (1 - u) * a + 2 * (1 - u) * u * ctrl + u * u * b));
        }
        if (pts.size() > 1) {
            dl->AddPolyline(pts.data(), (int)pts.size(), C(glm::vec3(255, 120, 30), 0.9f * fade), 0, m_scale * 0.12f);
            dl->AddPolyline(pts.data(), (int)pts.size(), C(glm::vec3(255, 230, 120), 0.9f * fade), 0, m_scale * 0.05f);
        }
    }

    // Particles.
    for (const Particle& p : m_particles) {
        const float life = p.age / p.life;
        const float alpha = std::min(1.f, p.age / 0.06f) * std::pow(std::max(0.f, 1.f - life), 0.6f);
        const glm::vec2 q = p.pos;
        switch (p.kind) {
        case Particle::Puff:
        case Particle::Steam: {
            const glm::vec2 o[3] = {{-0.38f, 0.12f}, {0.34f, 0.16f}, {0.f, -0.24f}};
            const float r[3] = {0.62f, 0.56f, 0.68f};
            const float rot = p.seed;
            const glm::vec3 edge = p.color * (p.kind == Particle::Steam ? 0.8f : 0.6f);
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < 3; ++i) {
                    const float cs = std::cos(rot), sn = std::sin(rot);
                    const glm::vec2 d(o[i].x * cs - o[i].y * sn, o[i].x * sn + o[i].y * cs);
                    const float rr = p.size * r[i] + (pass == 0 ? std::max(2.f, p.size * 0.08f) : 0.f);
                    dl->AddCircleFilled(V(q + d * p.size), rr, C(pass == 0 ? edge : p.color, alpha * 0.9f), 24);
                }
            break;
        }
        case Particle::Plume:
            dl->AddCircleFilled(V(q), p.size * 1.15f, C(p.color * 0.6f, alpha), 16);
            dl->AddCircleFilled(V(q), p.size, C(p.color, alpha), 16);
            break;
        case Particle::Drop:
            dl->AddCircleFilled(V(q), p.size, C(kYellow, alpha), 12);
            break;
        case Particle::Bubble:
            dl->AddCircleFilled(V(q), p.size, C(p.color, 0.18f * alpha), 32);
            dl->AddCircle(V(q), p.size, C(p.color, alpha), 32, std::max(2.f, p.size * 0.08f));
            dl->AddCircleFilled(V(q + glm::vec2(-0.35f, -0.35f) * p.size), p.size * 0.2f, C(glm::vec3(255), 0.8f * alpha), 12);
            break;
        case Particle::Bird: {
            const float flap = std::sin(p.age * 14.f + p.seed) * 0.55f;
            const glm::vec2 l = q + glm::vec2(-p.size, -p.size * flap), r = q + glm::vec2(p.size, -p.size * flap);
            ImVec2 pts[3] = {V(l), V(q), V(r)};
            dl->AddPolyline(pts, 3, C(p.color, alpha), 0, std::max(2.f, p.size * 0.22f));
            break;
        }
        case Particle::Ice: {
            const float a = p.age * 1.5f + p.seed, s = p.size;
            ImVec2 pts[4];
            for (int i = 0; i < 4; ++i) {
                const float k = a + kTau * i / 4.f, len = (i % 2 ? 0.65f : 1.f) * s;
                pts[i] = V(q + glm::vec2(std::cos(k), std::sin(k)) * len);
            }
            dl->AddConvexPolyFilled(pts, 4, C(glm::vec3(255, 225, 60), alpha));
            dl->AddPolyline(pts, 4, C(glm::vec3(255, 255, 220), alpha), ImDrawFlags_Closed, 1.5f);
            if (std::fmod(p.age + p.seed, 1.3f) < 0.15f) {
                dl->AddLine(V(q - glm::vec2(s * 1.4f, 0)), V(q + glm::vec2(s * 1.4f, 0)), C(glm::vec3(255), alpha), 1.5f);
                dl->AddLine(V(q - glm::vec2(0, s * 1.4f)), V(q + glm::vec2(0, s * 1.4f)), C(glm::vec3(255), alpha), 1.5f);
            }
            break;
        }
        case Particle::Spark: {
            const float s = p.size * (1.f - life * 0.5f);
            dl->AddLine(V(q - glm::vec2(s, 0)), V(q + glm::vec2(s, 0)), C(p.color, alpha), std::max(1.5f, s * 0.3f));
            dl->AddLine(V(q - glm::vec2(0, s)), V(q + glm::vec2(0, s)), C(p.color, alpha), std::max(1.5f, s * 0.3f));
            break;
        }
        case Particle::Splat: {
            const float drip = std::min(1.f, p.age / p.life * 1.5f) * p.size * 2.5f;
            dl->AddRectFilled(V(q + glm::vec2(-p.size * 0.18f, 0)), V(q + glm::vec2(p.size * 0.18f, drip)), C(kYellow, 0.85f * alpha));
            dl->AddCircleFilled(V(q + glm::vec2(0, drip)), p.size * 0.24f, C(kYellow, 0.85f * alpha), 16);
            dl->AddCircleFilled(V(q), p.size, C(kYellow, 0.85f * alpha), 24);
            for (int i = 0; i < 5; ++i) {
                const float k = p.seed + i * 1.3f;
                dl->AddCircleFilled(V(q + glm::vec2(std::cos(k), std::sin(k)) * p.size * 1.35f), p.size * 0.22f, C(kYellow, 0.85f * alpha), 12);
            }
            break;
        }
        case Particle::Stink: {
            std::vector<ImVec2> pts;
            for (int i = 0; i <= 12; ++i) {
                const float u = (float)i / 12.f;
                pts.push_back(V(q + glm::vec2(std::sin(u * 9.f + p.age * 6.f) * p.size * 0.12f, -u * p.size)));
            }
            dl->AddPolyline(pts.data(), (int)pts.size(), C(p.color, alpha), 0, std::max(2.f, p.size * 0.06f));
            break;
        }
        }
    }

    // Shocked telescope.
    if (m_place == Place::Telescope && m_fartT > 0.1f && m_fartT < 2.2f) {
        const glm::vec2 top = unit({0.f, -1.15f});
        for (int i = -1; i <= 1; ++i) {
            const float a = -1.5708f + i * 0.6f;
            const glm::vec2 d(std::cos(a), std::sin(a));
            dl->AddLine(V(top + d * m_scale * 0.12f), V(top + d * m_scale * 0.38f), C(glm::vec3(255, 240, 80), 1.f), m_scale * 0.035f);
        }
    }
    // Squeegee.
    if (m_place == Place::Telescope && m_peeHeld && m_head >= 1.f) {
        const float x = std::sin(t * 4.f) * 0.85f;
        const glm::vec2 a = unit({x, -0.75f}), b = unit({x, 0.75f});
        dl->AddLine(V((a + b) * 0.5f), V(unit({x + 0.6f, 1.3f})), C(glm::vec3(90, 90, 100), 1.f), m_scale * 0.06f);
        dl->AddRectFilled(V(a - glm::vec2(m_scale * 0.05f, 0)), V(b + glm::vec2(m_scale * 0.05f, 0)), C(glm::vec3(60, 120, 220), 1.f), 6.f);
        dl->AddRectFilled(V(a - glm::vec2(m_scale * 0.018f, 0)), V(b + glm::vec2(m_scale * 0.018f, 0)), C(glm::vec3(25), 1.f));
    }

    // The stream: from the bottom of the screen, arcing up and over onto the target.
    if (m_head > 0.f) {
        const glm::vec2 p0(width * 0.5f, height + 10.f);
        const glm::vec2 p2 = peeTarget() + glm::vec2(std::sin(t * 7.f), std::cos(t * 5.f)) * m_scale * 0.03f;
        glm::vec2 p1 = (p0 + p2) * 0.5f;
        p1.y = std::min(p0.y, p2.y) - height * 0.18f;
        p1.x += std::sin(t * 3.f) * width * 0.01f;
        std::vector<ImVec2> pts;
        const int n = 40;
        for (int i = (int)(m_tail * n); i <= (int)(m_head * n); ++i) {
            const float u = (float)i / n;
            pts.push_back(V((1 - u) * (1 - u) * p0 + 2 * (1 - u) * u * p1 + u * u * p2));
        }
        if (pts.size() > 1) {
            const float th = std::clamp(height * 0.011f, 5.f, 18.f);
            dl->AddPolyline(pts.data(), (int)pts.size(), C(glm::vec3(200, 150, 20), 0.95f), 0, th * 1.4f);
            dl->AddPolyline(pts.data(), (int)pts.size(), C(kYellow, 1.f), 0, th);
            dl->AddPolyline(pts.data(), (int)pts.size(), C(glm::vec3(255, 250, 190), 0.8f), 0, th * 0.3f);
        }
    }
}

} // namespace space
