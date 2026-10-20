// HLE: controllers, via Aurora's PAD (SDL gamepads + keyboard bindings), plus
// an optional scripted input for automated testing (--input).
#include "recomp/ghost_lights.h"
#include "recomp/hle.h"

#include "port/options.h"
#include "recomp/controls.h"
#include "recomp/coop.h"
#include "recomp/coop_test.h"
#include "recomp/menu.h"
#include "recomp/skip.h"
#include "recomp/touch.h"
#include "recomp/unused2.h"

#include <dolphin/pad.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace recomp {
u32 viRetraceCount();  // hle_vi.cpp

namespace {

struct ScriptedPress {
    int port;  // 0-based; items prefixed "P2:".."P4:" drive the other ports
    u32 start, frames;
    u16 buttons;
    s8 stickX, stickY;
    s8 substickX = 0, substickY = 0;  // C stick: CUP/CDOWN/CLEFT/CRIGHT
};

std::vector<ScriptedPress> parseScript(const std::string& script) {
    std::vector<ScriptedPress> out;
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ',')) {
        int port = 0;
        if (item.size() > 3 && (item[0] == 'P' || item[0] == 'p') && item[1] >= '1' && item[1] <= '4' && item[2] == ':') {
            port = item[1] - '1';
            item.erase(0, 3);
        }
        const auto colon = item.find(':');
        if (colon == std::string::npos) continue;
        ScriptedPress p{port, u32(std::strtoul(item.c_str(), nullptr, 10)), 6, 0, 0, 0};
        std::string rest = item.substr(colon + 1);
        if (const auto star = rest.find('*'); star != std::string::npos) {
            p.frames = u32(std::strtoul(rest.c_str() + star + 1, nullptr, 10));
            rest.resize(star);
        }
        std::stringstream bs(rest);
        std::string b;
        while (std::getline(bs, b, '+')) {
            if (b == "A") p.buttons |= PAD_BUTTON_A;
            else if (b == "B") p.buttons |= PAD_BUTTON_B;
            else if (b == "X") p.buttons |= PAD_BUTTON_X;
            else if (b == "Y") p.buttons |= PAD_BUTTON_Y;
            else if (b == "Z") p.buttons |= PAD_TRIGGER_Z;
            else if (b == "L") p.buttons |= PAD_TRIGGER_L;
            else if (b == "R") p.buttons |= PAD_TRIGGER_R;
            else if (b == "START") p.buttons |= PAD_BUTTON_START;
            else if (b == "UP") p.buttons |= PAD_BUTTON_UP;
            else if (b == "DOWN") p.buttons |= PAD_BUTTON_DOWN;
            else if (b == "LEFT") p.buttons |= PAD_BUTTON_LEFT;
            else if (b == "RIGHT") p.buttons |= PAD_BUTTON_RIGHT;
            else if (b == "STICKUP") p.stickY = 100;
            else if (b == "STICKDOWN") p.stickY = -100;
            else if (b == "STICKLEFT") p.stickX = -100;
            else if (b == "STICKRIGHT") p.stickX = 100;
            else if (b == "CUP") p.substickY = 100;
            else if (b == "CDOWN") p.substickY = -100;
            else if (b == "CLEFT") p.substickX = -100;
            else if (b == "CRIGHT") p.substickX = 100;
        }
        out.push_back(p);
    }
    return out;
}

const std::vector<ScriptedPress>& script() {
    static const std::vector<ScriptedPress> s = parseScript(port::options().inputScript);
    return s;
}

// Ports other than 1 that the script drives: they read as connected for the whole run (a pad
// plugged in), so the game sees the controller even between presses.
bool scriptUsesPort(int port) {
    for (const auto& p : script())
        if (p.port == port) return true;
    return false;
}

void applyScript(PADStatus* pads) {
    // LM_INPUT_CLOCK=frames: time the script in guest frames, which do not depend on
    // fast-forward (cutscene skipping) the way retraces do.
    static const bool byFrames = [] {
        const char* v = std::getenv("LM_INPUT_CLOCK");
        return v && std::string(v) == "frames";
    }();
    const u32 now = byFrames ? skip::guestFrames() : viRetraceCount();
    for (int i = 1; i < PAD_MAX_CONTROLLERS; i++) {
        static const bool used[PAD_MAX_CONTROLLERS] = {true, scriptUsesPort(1), scriptUsesPort(2), scriptUsesPort(3)};
        if (used[i] && pads[i].err != PAD_ERR_NONE) {
            pads[i] = PADStatus{};
            pads[i].err = PAD_ERR_NONE;
        }
    }
    for (const auto& p : script()) {
        PADStatus& st = pads[p.port];
        if (now >= p.start && now < p.start + p.frames) {
            st.button |= p.buttons;
            // The game reads the analog trigger values (vacuum/blow strength), not just the bits.
            if (p.buttons & PAD_TRIGGER_L) st.triggerLeft = 0xFF;
            if (p.buttons & PAD_TRIGGER_R) st.triggerRight = 0xFF;
            if (p.stickX) st.stickX = p.stickX;
            if (p.stickY) st.stickY = p.stickY;
            if (p.substickX) st.substickX = p.substickX;
            if (p.substickY) st.substickY = p.substickY;
            st.err = PAD_ERR_NONE;
        }
    }
}

u16 sLastButtons = 0;  // port 1, as last handed to the game

// LM_PAD_LOG=1: log every change of a connected port's status (after bindings and shaping,
// before the menu's input suppression), for checking controller configuration.
void logPad(const PADStatus* st) {
    static const bool enabled = [] {
        const char* v = std::getenv("LM_PAD_LOG");
        return v && *v && *v != '0';
    }();
    if (!enabled) return;
    static PADStatus last[PAD_MAX_CONTROLLERS] = {};
    for (int i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        const PADStatus& s = st[i];
        const PADStatus& l = last[i];
        if (s.button == l.button && s.stickX == l.stickX && s.stickY == l.stickY && s.substickX == l.substickX &&
            s.substickY == l.substickY && s.triggerLeft == l.triggerLeft && s.triggerRight == l.triggerRight &&
            s.err == l.err)
            continue;
        last[i] = s;
        log("[pad] port %d retrace %u: err %d buttons %04X stick %d,%d C %d,%d L %u R %u", i + 1, viRetraceCount(),
            s.err, s.button, s.stickX, s.stickY, s.substickX, s.substickY, s.triggerLeft, s.triggerRight);
    }
}

}  // namespace

u16 padButtons() { return sLastButtons; }

HLE(PADInit) { c.r[3] = PADInit(); }
HLE(PADReset) { c.r[3] = PADReset(c.r[3]); }
HLE(PADRecalibrate) { c.r[3] = PADRecalibrate(c.r[3]); }
HLE(PADSetSpec) { PADSetSpec(c.r[3]); }
HLE(PADSetAnalogMode) { PADSetAnalogMode(c.r[3]); }
// The game's motor (JUTGamePad::CRumble: on Luigi's hits, vacuuming, ...): F1 > Controls
// rumble setting, sent to the port's controller (controls.cpp). LM_RUMBLE_LOG=1 logs the calls.
HLE(PADControlMotor) {
    static const bool sLog = [] {
        const char* v = std::getenv("LM_RUMBLE_LOG");
        return v && *v && *v != '0';
    }();
    if (sLog)
        log("[rumble] game PADControlMotor(chan %u, cmd %u) from %08X, retrace %u", c.r[3], c.r[4], c.lr,
            viRetraceCount());
    controls::controlMotor(c.r[3], c.r[4]);
}
HLE(PADSetSamplingRate) {}  // PAL release; sampling is the host's business

// PADStatus (12 bytes): button, stickX/Y, substickX/Y, triggerL/R, analogA/B, err.
HLE(PADRead) {
    PADStatus st[PAD_MAX_CONTROLLERS] = {};
    controls::beforeRead();  // port devices (hot-plug) and bindings from F1 > Controls
    const u32 motor = PADRead(st);
    controls::afterRead(st);  // dead zones, sensitivity; input test values
    touch::apply(st);         // Android on-screen pad on port 1 (touch.h)
    logPad(st);
    if (menu::isOpen()) {  // the overlay menu owns input while open
        for (auto& s : st) {
            const s8 err = s.err;
            s = PADStatus{};
            s.err = err;
        }
    }
    if (!port::options().inputScript.empty()) {
        applyScript(st);
        {  // LM_GHOST_HUNT test aid (LM_GHOST_HUNT_PLAYER=2: player 2 of the 2-player mode hunts, on port 2)
            PADStatus& h = st[ghostlights::huntPort()];
            ghostlights::testPad(h.button, h.stickX, h.stickY, h.substickY, h.triggerRight);
        }
        cooptest::pad(st);  // co-op test aids (coop_test.cpp)
        static u32 sLogged = 0;
        if (st[0].button && sLogged++ < 20)
            log("[pad] retrace %u frame %u: buttons %04X err %d", viRetraceCount(), skip::guestFrames(), st[0].button, st[0].err);
    }
    skip::filterPad(st, PAD_MAX_CONTROLLERS);
    // 2-player co-op improvements: player 2's Game Boy Horror buttons do nothing, and player 2
    // has no input while an event has set them aside (coop_fixes.cpp).
    if (coop::fixes()) {
        st[1].button &= u16(~(PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT));
        if (coop::player2SetAside()) {
            const s8 err = st[1].err;
            st[1] = PADStatus{};
            st[1].err = err;
        }
    }
    if (unused2::playerHeld()) {  // poisoned Luigi (luigi_states.cpp): no movement or actions
        st[0].button &= PAD_BUTTON_START;
        st[0].stickX = st[0].stickY = st[0].substickX = st[0].substickY = 0;
        st[0].triggerLeft = st[0].triggerRight = st[0].analogA = st[0].analogB = 0;
    }
    sLastButtons = st[0].err == PAD_ERR_NONE ? st[0].button : 0;
    for (int i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        const u32 p = c.r[3] + 12 * i;
        wr16(p, st[i].button);
        wr8(p + 2, u8(st[i].stickX));
        wr8(p + 3, u8(st[i].stickY));
        wr8(p + 4, u8(st[i].substickX));
        wr8(p + 5, u8(st[i].substickY));
        wr8(p + 6, st[i].triggerLeft);
        wr8(p + 7, st[i].triggerRight);
        wr8(p + 8, st[i].analogA);
        wr8(p + 9, st[i].analogB);
        wr8(p + 10, u8(st[i].err));
    }
    c.r[3] = motor;
}

}  // namespace recomp
