// On-screen touch controls (touch.h, docs/ANDROID.md).
#include "recomp/touch.h"

#include "port/settings.h"
#include "recomp/menu.h"

#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace recomp::touch {
namespace {

#ifdef __ANDROID__
constexpr bool kSupported = true;
#else
constexpr bool kSupported = false;  // desktops: keyboard/controller (could be enabled for touchscreens)
#endif

enum class Ctl { None, Stick, CStick, A, B, X, Y, L, R, Z, Start, DUp, DDown, DLeft, DRight, Menu };

struct Circle {
    Ctl ctl;
    float x, y, r;
    const char* label;
};

struct Layout {
    float w = 0, h = 0, u = 0;
    float stickX = 0, stickY = 0, stickR = 0;  // main stick resting place
    float cX = 0, cY = 0, cR = 0;              // C-stick
    float dX = 0, dY = 0, dS = 0;              // D-pad centre, arm size
    Circle buttons[9];
    int nButtons = 0;
};

struct Finger {
    Ctl ctl = Ctl::None;
    float ox = 0, oy = 0;  // stick origin (where the finger landed, or the C-stick centre)
    float x = 0, y = 0;    // current position
};

std::mutex sM;
std::map<SDL_FingerID, Finger> sFingers;
Layout sLayout;
bool sMenuButtonDown = false;

bool enabledSetting() { return port::settings().enhancements.touchControls != 0; }

// Shown: "Always", or "Auto" while no gamepad is connected (a Bluetooth/USB controller takes over).
bool visible() {
    if (!kSupported || !enabledSetting()) return false;
    if (port::settings().enhancements.touchControls == 2) return true;
    return !SDL_HasGamepad();
}

Layout makeLayout(float w, float h) {
    Layout l;
    l.w = w;
    l.h = h;
    const float scale = std::clamp(port::settings().enhancements.touchScale, 0.6f, 1.6f);
    const float u = std::min(w, h) / 10.0f * scale;
    l.u = u;
    l.stickX = 2.4f * u;
    l.stickY = h - 2.6f * u;
    l.stickR = 1.5f * u;
    l.cX = w - 6.4f * u;
    l.cY = h - 3.0f * u;
    l.cR = 0.95f * u;
    l.dX = 1.7f * u;
    l.dY = h - 6.1f * u;
    l.dS = 0.62f * u;
    auto add = [&](Ctl c, float x, float y, float r, const char* label) { l.buttons[l.nButtons++] = {c, x, y, r, label}; };
    // GameCube face-button arrangement: big A, B low-left of it, X to the right, Y above.
    add(Ctl::A, w - 2.7f * u, h - 2.4f * u, 1.0f * u, "A");
    add(Ctl::B, w - 4.5f * u, h - 1.3f * u, 0.68f * u, "B");
    add(Ctl::X, w - 1.0f * u, h - 3.9f * u, 0.66f * u, "X");
    add(Ctl::Y, w - 3.1f * u, h - 4.7f * u, 0.62f * u, "Y");
    add(Ctl::L, 1.5f * u, 1.1f * u, 0.85f * u, "L");
    add(Ctl::R, w - 1.5f * u, 1.1f * u, 0.85f * u, "R");
    add(Ctl::Z, w - 1.5f * u, 2.85f * u, 0.6f * u, "Z");
    add(Ctl::Start, w * 0.5f + 1.0f * u, 0.8f * u, 0.5f * u, "START");
    add(Ctl::Menu, w * 0.5f - 1.0f * u, 0.8f * u, 0.5f * u, "MENU");
    return l;
}

Ctl dpadAt(const Layout& l, float x, float y) {
    const float dx = x - l.dX, dy = y - l.dY;
    const float reach = l.dS * 2.2f;
    if (std::fabs(dx) > reach || std::fabs(dy) > reach) return Ctl::None;
    if (std::fabs(dx) < l.dS * 0.35f && std::fabs(dy) < l.dS * 0.35f) return Ctl::None;  // dead centre
    if (std::fabs(dx) > std::fabs(dy)) return dx < 0 ? Ctl::DLeft : Ctl::DRight;
    return dy < 0 ? Ctl::DUp : Ctl::DDown;
}

Ctl hit(const Layout& l, float x, float y, bool menuOpen) {
    for (int i = 0; i < l.nButtons; i++) {
        const Circle& c = l.buttons[i];
        if (menuOpen && c.ctl != Ctl::Menu) continue;
        const float dx = x - c.x, dy = y - c.y;
        if (dx * dx + dy * dy <= (c.r * 1.2f) * (c.r * 1.2f)) return c.ctl;
    }
    if (menuOpen) return Ctl::None;
    {
        const float dx = x - l.cX, dy = y - l.cY;
        if (dx * dx + dy * dy <= (l.cR * 1.4f) * (l.cR * 1.4f)) return Ctl::CStick;
    }
    if (Ctl d = dpadAt(l, x, y); d != Ctl::None) return d;
    if (x < l.w * 0.42f && y > l.h * 0.3f) return Ctl::Stick;  // floating stick: anywhere lower-left
    return Ctl::None;
}

// Unit vector-ish stick value from a finger: -1..1 each axis, magnitude clamped to 1.
void stickValue(const Finger& f, float radius, float& vx, float& vy) {
    vx = (f.x - f.ox) / radius;
    vy = (f.y - f.oy) / radius;
    const float m = std::sqrt(vx * vx + vy * vy);
    if (m > 1.0f) {
        vx /= m;
        vy /= m;
    }
}

s8 axis(float v) { return s8(std::lround(std::clamp(v, -1.0f, 1.0f) * 105.0f)); }  // GameCube stick range ~±100

}  // namespace

bool supported() { return kSupported; }

bool handleEvent(const SDL_Event& ev) {
    if (!kSupported) return false;
    if (ev.type != SDL_EVENT_FINGER_DOWN && ev.type != SDL_EVENT_FINGER_MOTION && ev.type != SDL_EVENT_FINGER_UP &&
        ev.type != SDL_EVENT_FINGER_CANCELED)
        return false;
    std::lock_guard lk(sM);
    if (!visible() && ev.type == SDL_EVENT_FINGER_DOWN) return false;
    Layout& l = sLayout;
    if (l.w <= 0) return false;  // not drawn yet
    const float x = ev.tfinger.x * l.w, y = ev.tfinger.y * l.h;
    const SDL_FingerID id = ev.tfinger.fingerID;
    if (ev.type == SDL_EVENT_FINGER_DOWN) {
        const Ctl c = hit(l, x, y, menu::isOpen());
        if (c == Ctl::None) return false;
        Finger f;
        f.ctl = c;
        f.x = x;
        f.y = y;
        if (c == Ctl::CStick) {
            f.ox = l.cX;
            f.oy = l.cY;
        } else {
            f.ox = x;
            f.oy = y;
        }
        if (c == Ctl::Menu) sMenuButtonDown = true;
        sFingers[id] = f;
        return true;
    }
    auto it = sFingers.find(id);
    if (it == sFingers.end()) return false;
    Finger& f = it->second;
    f.x = x;
    f.y = y;
    // The D-pad follows a sliding finger between its arms.
    if (f.ctl == Ctl::DUp || f.ctl == Ctl::DDown || f.ctl == Ctl::DLeft || f.ctl == Ctl::DRight)
        if (Ctl d = dpadAt(l, x, y); d != Ctl::None) f.ctl = d;
    if (ev.type == SDL_EVENT_FINGER_UP || ev.type == SDL_EVENT_FINGER_CANCELED) {
        if (f.ctl == Ctl::Menu) {
            sMenuButtonDown = false;
            // Released on the button: open or close the F1 menu (Android has no F1 key).
            const float dx = x - l.buttons[8].x, dy = y - l.buttons[8].y;
            if (ev.type == SDL_EVENT_FINGER_UP && dx * dx + dy * dy <= l.buttons[8].r * l.buttons[8].r * 2.0f)
                menu::toggle();
        }
        sFingers.erase(it);
    }
    return true;
}

void apply(PADStatus* status) {
    if (!kSupported) return;
    std::lock_guard lk(sM);
    if (!visible()) return;
    PADStatus& s = status[0];
    // The touch pad is a controller in port 1 even while idle (no "connect a controller").
    if (s.err != PAD_ERR_NONE) {
        s = PADStatus{};
        s.err = PAD_ERR_NONE;
    }
    const Layout& l = sLayout;
    for (const auto& [id, f] : sFingers) {
        float vx, vy;
        switch (f.ctl) {
        case Ctl::Stick:
            stickValue(f, l.stickR, vx, vy);
            if (std::abs(axis(vx)) + std::abs(axis(-vy)) > std::abs(s.stickX) + std::abs(s.stickY)) {
                s.stickX = axis(vx);
                s.stickY = axis(-vy);
            }
            break;
        case Ctl::CStick:
            stickValue(f, l.cR, vx, vy);
            s.substickX = axis(vx);
            s.substickY = axis(-vy);
            break;
        case Ctl::A: s.button |= PAD_BUTTON_A; s.analogA = 255; break;
        case Ctl::B: s.button |= PAD_BUTTON_B; s.analogB = 255; break;
        case Ctl::X: s.button |= PAD_BUTTON_X; break;
        case Ctl::Y: s.button |= PAD_BUTTON_Y; break;
        case Ctl::L: s.button |= PAD_TRIGGER_L; s.triggerLeft = 255; break;
        case Ctl::R: s.button |= PAD_TRIGGER_R; s.triggerRight = 255; break;
        case Ctl::Z: s.button |= PAD_TRIGGER_Z; break;
        case Ctl::Start: s.button |= PAD_BUTTON_START; break;
        case Ctl::DUp: s.button |= PAD_BUTTON_UP; break;
        case Ctl::DDown: s.button |= PAD_BUTTON_DOWN; break;
        case Ctl::DLeft: s.button |= PAD_BUTTON_LEFT; break;
        case Ctl::DRight: s.button |= PAD_BUTTON_RIGHT; break;
        default: break;
        }
    }
}

void draw() {
    if (!kSupported) return;
    std::lock_guard lk(sM);
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    sLayout = makeLayout(size.x, size.y);
    if (!visible()) {
        sFingers.clear();
        return;
    }
    const Layout& l = sLayout;
    const bool menuOpen = menu::isOpen();
    const float alpha = std::clamp(port::settings().enhancements.touchOpacity, 0.1f, 1.0f);
    auto col = [&](int r, int g, int b, float a) { return IM_COL32(r, g, b, int(255.0f * a * alpha)); };
    auto pressed = [&](Ctl c) {
        for (const auto& [id, f] : sFingers)
            if (f.ctl == c) return true;
        return false;
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    auto label = [&](const char* text, float cx, float cy, float px) {
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
        dl->AddText(font, px, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), col(255, 255, 255, 0.95f), text);
    };
    for (int i = 0; i < l.nButtons; i++) {
        const Circle& c = l.buttons[i];
        if (menuOpen && c.ctl != Ctl::Menu) continue;
        const bool down = pressed(c.ctl);
        ImU32 fill = col(30, 30, 40, down ? 0.75f : 0.4f);
        if (c.ctl == Ctl::A) fill = col(40, 150, 120, down ? 0.85f : 0.5f);  // GameCube green A
        if (c.ctl == Ctl::B) fill = col(200, 50, 50, down ? 0.85f : 0.5f);   // red B
        dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, fill, 40);
        dl->AddCircle(ImVec2(c.x, c.y), c.r, col(255, 255, 255, 0.6f), 40, std::max(1.5f, l.u * 0.04f));
        const float px = c.r * (std::strlen(c.label) > 2 ? 0.5f : 0.95f);
        label(c.label, c.x, c.y, px);
    }
    if (menuOpen) return;
    // Main stick: base where the finger landed (or its resting place), knob under the finger.
    {
        float bx = l.stickX, by = l.stickY, kx = bx, ky = by;
        for (const auto& [id, f] : sFingers)
            if (f.ctl == Ctl::Stick) {
                float vx, vy;
                stickValue(f, l.stickR, vx, vy);
                bx = f.ox;
                by = f.oy;
                kx = bx + vx * l.stickR;
                ky = by + vy * l.stickR;
            }
        dl->AddCircleFilled(ImVec2(bx, by), l.stickR, col(30, 30, 40, 0.3f), 48);
        dl->AddCircle(ImVec2(bx, by), l.stickR, col(255, 255, 255, 0.5f), 48, std::max(1.5f, l.u * 0.04f));
        dl->AddCircleFilled(ImVec2(kx, ky), l.stickR * 0.45f, col(200, 200, 210, 0.6f), 36);
    }
    // C-stick (yellow on the GameCube pad).
    {
        float kx = l.cX, ky = l.cY;
        for (const auto& [id, f] : sFingers)
            if (f.ctl == Ctl::CStick) {
                float vx, vy;
                stickValue(f, l.cR, vx, vy);
                kx = l.cX + vx * l.cR;
                ky = l.cY + vy * l.cR;
            }
        dl->AddCircleFilled(ImVec2(l.cX, l.cY), l.cR, col(30, 30, 40, 0.3f), 40);
        dl->AddCircle(ImVec2(l.cX, l.cY), l.cR, col(240, 200, 40, 0.7f), 40, std::max(1.5f, l.u * 0.04f));
        dl->AddCircleFilled(ImVec2(kx, ky), l.cR * 0.5f, col(240, 200, 40, 0.6f), 32);
        label("C", kx, ky, l.cR * 0.6f);
    }
    // D-pad: four arms.
    {
        const struct {
            Ctl c;
            float dx, dy;
        } arms[] = {{Ctl::DUp, 0, -1}, {Ctl::DDown, 0, 1}, {Ctl::DLeft, -1, 0}, {Ctl::DRight, 1, 0}};
        const float s = l.dS;
        for (const auto& a : arms) {
            const float cx = l.dX + a.dx * s * 1.05f, cy = l.dY + a.dy * s * 1.05f;
            dl->AddRectFilled(ImVec2(cx - s * 0.5f, cy - s * 0.5f), ImVec2(cx + s * 0.5f, cy + s * 0.5f),
                              col(30, 30, 40, pressed(a.c) ? 0.8f : 0.4f), s * 0.15f);
            dl->AddRect(ImVec2(cx - s * 0.5f, cy - s * 0.5f), ImVec2(cx + s * 0.5f, cy + s * 0.5f),
                        col(255, 255, 255, 0.5f), s * 0.15f, 0, std::max(1.0f, l.u * 0.03f));
        }
    }
}

bool drawSettings() {
    if (!kSupported) return false;
    auto& e = port::settings().enhancements;
    bool changed = false;
    ImGui::SeparatorText("Touch controls");
    const char* modes[] = {"Off", "Auto (hidden while a controller is connected)", "Always"};
    int mode = std::clamp(e.touchControls, 0, 2);
    if (ImGui::Combo("Show##touch", &mode, modes, 3)) {
        e.touchControls = mode;
        changed = true;
    }
    changed |= ImGui::SliderFloat("Size##touch", &e.touchScale, 0.6f, 1.6f, "%.2f");
    changed |= ImGui::SliderFloat("Opacity##touch", &e.touchOpacity, 0.1f, 1.0f, "%.2f");
    ImGui::TextDisabled("Left side: move (the stick appears where you touch). Yellow circle: C-stick.\n"
                        "MENU opens this menu; the Back button does too.");
    return changed;
}

}  // namespace recomp::touch
