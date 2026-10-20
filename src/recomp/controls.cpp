// Controller configuration: F1 > Controls (docs/CONTROLS.md).
//
// Every GameCube input of every port can be bound to keyboard keys, mouse buttons and
// gamepad buttons/axes. Bindings live in profiles: one for the keyboard (and mouse) and
// one per gamepad model (SDL GUID), created from a per-type default layout. A port is
// driven by one gamepad (chosen, or "Auto" = the next free one) and optionally the
// keyboard.
//
// Aurora's PAD resolves a port's controller from the SDL player index, so device
// assignment sets player indices. Its own per-controller mappings and keyboard bindings
// are emptied and all our bindings go in through aurora::pad::set_action_bindings, which
// accepts any physical input for any PAD control (aurora/pad.hpp, aurora/binding.hpp).
// Stick dead zones and sensitivity are applied to the PADStatus afterwards (hle_pad.cpp).
// Everything is saved in settings.ini ([controls], [controls.keyboard],
// [controls.pad.<guid>]) through port::addSettingsExtension.
#include "recomp/menu.h"
#include "recomp/controls.h"
#include "recomp/ui_scale.h"
#include "recomp/touch.h"

#include "port/settings.h"

#include <aurora/binding.hpp>
#include <aurora/input.hpp>
#include <aurora/pad.hpp>
#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace recomp {
void log(const char* fmt, ...);  // runtime.cpp
}

namespace recomp::controls {
namespace {

using aurora::binding::Binding;
using aurora::binding::ControlId;
using aurora::binding::PhysicalInput;
using Clock = std::chrono::steady_clock;

// ---- GameCube inputs -----------------------------------------------------------------

enum Target : int {
    tA, tB, tX, tY, tZ, tStart, tL, tR,
    tDUp, tDDown, tDLeft, tDRight,
    tStickUp, tStickDown, tStickLeft, tStickRight,
    tCUp, tCDown, tCLeft, tCRight,
    tAnalogL, tAnalogR,
    kTargets
};

struct TargetInfo {
    const char* key;    // ini key
    const char* label;  // menu
    const char* group;  // menu section, printed when it changes
    u16 padButton;      // PAD_BUTTON_* for digital inputs, else 0
};

constexpr TargetInfo kTargetInfo[kTargets] = {
    {"a", "A", "Buttons", PAD_BUTTON_A},
    {"b", "B", "Buttons", PAD_BUTTON_B},
    {"x", "X", "Buttons", PAD_BUTTON_X},
    {"y", "Y", "Buttons", PAD_BUTTON_Y},
    {"z", "Z", "Buttons", PAD_TRIGGER_Z},
    {"start", "START", "Buttons", PAD_BUTTON_START},
    {"l", "L (click)", "Buttons", PAD_TRIGGER_L},
    {"r", "R (click)", "Buttons", PAD_TRIGGER_R},
    {"dpad_up", "Up", "D-pad", PAD_BUTTON_UP},
    {"dpad_down", "Down", "D-pad", PAD_BUTTON_DOWN},
    {"dpad_left", "Left", "D-pad", PAD_BUTTON_LEFT},
    {"dpad_right", "Right", "D-pad", PAD_BUTTON_RIGHT},
    {"stick_up", "Up", "Control stick", 0},
    {"stick_down", "Down", "Control stick", 0},
    {"stick_left", "Left", "Control stick", 0},
    {"stick_right", "Right", "Control stick", 0},
    {"c_up", "Up", "C-stick", 0},
    {"c_down", "Down", "C-stick", 0},
    {"c_left", "Left", "C-stick", 0},
    {"c_right", "Right", "C-stick", 0},
    {"analog_l", "L (analog)", "Analog triggers", 0},
    {"analog_r", "R (analog)", "Analog triggers", 0},
};

// ---- Physical inputs -----------------------------------------------------------------

struct Input {
    enum Kind : u8 { None, Key, Mouse, Button, Axis };
    Kind kind = None;
    s16 code = 0;  // SDL_Scancode, SDL_BUTTON_*, SDL_GamepadButton or SDL_GamepadAxis
    s8 dir = 0;    // Axis: +1 / -1 (SDL sticks: +x right, +y down; triggers 0..1)
    bool operator==(const Input&) const = default;
    explicit operator bool() const { return kind != None; }
};

Input key(SDL_Scancode s) { return {Input::Key, s16(s), 0}; }
Input mouse(int b) { return {Input::Mouse, s16(b), 0}; }
Input button(SDL_GamepadButton b) { return {Input::Button, s16(b), 0}; }
Input axis(SDL_GamepadAxis a, int dir) { return {Input::Axis, s16(a), s8(dir)}; }

constexpr const char* kButtonIds[SDL_GAMEPAD_BUTTON_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1",
    "rightpaddle1", "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad",
    "misc2", "misc3", "misc4", "misc5", "misc6"};
constexpr const char* kAxisIds[SDL_GAMEPAD_AXIS_COUNT] = {"leftx", "lefty", "rightx", "righty",
                                                          "lefttrigger", "righttrigger"};

bool isTrigger(int a) { return a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

// Ini text: key:<scancode name>, mouse:<1-5>, button:<id>, axis:<id>+ / axis:<id>-.
std::string toString(const Input& in) {
    switch (in.kind) {
    case Input::Key: {
        const char* n = SDL_GetScancodeName(SDL_Scancode(in.code));
        return n && *n ? std::string("key:") + n : "key:#" + std::to_string(in.code);
    }
    case Input::Mouse: return "mouse:" + std::to_string(in.code);
    case Input::Button:
        return in.code >= 0 && in.code < SDL_GAMEPAD_BUTTON_COUNT ? std::string("button:") + kButtonIds[in.code] : "";
    case Input::Axis:
        return in.code >= 0 && in.code < SDL_GAMEPAD_AXIS_COUNT
                   ? std::string("axis:") + kAxisIds[in.code] + (in.dir < 0 ? "-" : "+")
                   : "";
    default: return "";
    }
}

Input fromString(const std::string& s) {
    const auto colon = s.find(':');
    if (colon == std::string::npos) return {};
    const std::string kind = s.substr(0, colon), v = s.substr(colon + 1);
    if (kind == "key") {
        if (!v.empty() && v[0] == '#') return key(SDL_Scancode(std::atoi(v.c_str() + 1)));
        const SDL_Scancode sc = SDL_GetScancodeFromName(v.c_str());
        return sc != SDL_SCANCODE_UNKNOWN ? key(sc) : Input{};
    }
    if (kind == "mouse") {
        const int b = std::atoi(v.c_str());
        return b >= 1 && b <= 5 ? mouse(b) : Input{};
    }
    if (kind == "button") {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (v == kButtonIds[i]) return button(SDL_GamepadButton(i));
        return {};
    }
    if (kind == "axis" && v.size() > 1) {
        const int dir = v.back() == '-' ? -1 : 1;
        const std::string id = v.substr(0, v.size() - 1);
        for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
            if (id == kAxisIds[i]) return axis(SDL_GamepadAxis(i), dir);
    }
    return {};
}

enum class Family { Generic, Xbox, PlayStation, Nintendo, GameCube };

Family familyOf(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return Family::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return Family::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Family::Nintendo;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return Family::GameCube;
    default: return Family::Generic;
    }
}

const char* typeName(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_GAMEPAD_TYPE_PS3: return "PlayStation 3";
    case SDL_GAMEPAD_TYPE_PS4: return "PlayStation 4";
    case SDL_GAMEPAD_TYPE_PS5: return "PlayStation 5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (L)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (R)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
    case SDL_GAMEPAD_TYPE_GAMECUBE: return "GameCube";
    default: return "Gamepad";
    }
}

std::string buttonName(int b, SDL_GamepadType type) {
    const Family f = familyOf(type);
    if (b <= SDL_GAMEPAD_BUTTON_NORTH) {
        switch (SDL_GetGamepadButtonLabelForType(type, SDL_GamepadButton(b))) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: break;
        }
        static const char* kFace[] = {"South", "East", "West", "North"};
        return kFace[b];
    }
    const bool xb = f == Family::Xbox, ps = f == Family::PlayStation, nin = f == Family::Nintendo,
               gc = f == Family::GameCube;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_BACK: return xb ? "View" : ps ? "Share" : nin ? "-" : "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return xb ? "Xbox" : ps ? "PS" : nin ? "Home" : "Guide";
    case SDL_GAMEPAD_BUTTON_START: return xb ? "Menu" : ps ? "Options" : nin ? "+" : gc ? "START" : "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "Left stick press";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "Right stick press";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nin ? "L" : "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nin ? "R" : gc ? "Z" : "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad Right";
    case SDL_GAMEPAD_BUTTON_MISC1: return xb ? "Share" : ps ? "Mic" : nin ? "Capture" : "Misc 1";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "Paddle R1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "Paddle L1";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "Paddle R2";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "Paddle L2";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    case SDL_GAMEPAD_BUTTON_MISC3: return gc ? "L click" : "Misc 3";
    case SDL_GAMEPAD_BUTTON_MISC4: return gc ? "R click" : "Misc 4";
    default: return "Misc " + std::to_string(b - SDL_GAMEPAD_BUTTON_MISC2 + 2);
    }
}

std::string axisName(int a, int dir, SDL_GamepadType type) {
    const Family f = familyOf(type);
    if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
        return f == Family::PlayStation ? "L2" : f == Family::Nintendo ? "ZL" : f == Family::GameCube ? "L (analog)" : "LT";
    if (a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return f == Family::PlayStation ? "R2" : f == Family::Nintendo ? "ZR" : f == Family::GameCube ? "R (analog)" : "RT";
    const char* stick = a <= SDL_GAMEPAD_AXIS_LEFTY ? (f == Family::GameCube ? "Stick" : "Left stick")
                                                     : (f == Family::GameCube ? "C-stick" : "Right stick");
    const bool x = a == SDL_GAMEPAD_AXIS_LEFTX || a == SDL_GAMEPAD_AXIS_RIGHTX;
    const char* d = x ? (dir < 0 ? "Left" : "Right") : (dir < 0 ? "Up" : "Down");
    return std::string(stick) + " " + d;
}

std::string inputName(const Input& in, SDL_GamepadType type) {
    switch (in.kind) {
    case Input::Key: {
        const char* n = SDL_GetScancodeName(SDL_Scancode(in.code));
        return n && *n ? n : "Key " + std::to_string(in.code);
    }
    case Input::Mouse: {
        static const char* kMouse[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};
        return in.code >= 1 && in.code <= 5 ? kMouse[in.code] : "Mouse";
    }
    case Input::Button: return buttonName(in.code, type);
    case Input::Axis: return axisName(in.code, in.dir, type);
    default: return "";
    }
}

// ---- Profiles ------------------------------------------------------------------------

struct Profile {
    bool keyboard = false;
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_STANDARD;
    std::string name;  // last seen device name
    std::array<std::array<Input, 2>, kTargets> bind{};
    // Gamepads only. [0] control stick / L, [1] C-stick / R.
    float deadzone[2] = {0.08f, 0.08f};    // radial, fraction of full tilt; rescaled outside it
    float sensitivity[2] = {1.0f, 1.0f};   // tilt multiplier after the dead zone
    bool invertX[2] = {false, false};
    bool invertY[2] = {false, false};
    float threshold[2] = {0.85f, 0.85f};   // analog trigger pull that counts as the L/R click
    bool rumble = true;
    float rumbleStrength = 0.5f;           // 0..1 (controllers that support intensity)
    bool operator==(const Profile&) const = default;
};

// Keyboard: WASD = control stick, arrow keys = C-stick (aim), Space = R (vacuum),
// Left Shift = L, E = A, Q = B, R = X, F = Y, Left Ctrl = Z, Enter = START, 1-4 = D-pad;
// mouse left/right = R/L as alternates. (Tab, F1, F3, F4 and Esc are port hotkeys.)
Profile keyboardDefaults() {
    Profile p;
    p.keyboard = true;
    p.name = "Keyboard and mouse";
    auto& b = p.bind;
    b[tA][0] = key(SDL_SCANCODE_E);
    b[tB][0] = key(SDL_SCANCODE_Q);
    b[tX][0] = key(SDL_SCANCODE_R);
    b[tY][0] = key(SDL_SCANCODE_F);
    b[tZ][0] = key(SDL_SCANCODE_LCTRL);
    b[tZ][1] = key(SDL_SCANCODE_C);
    b[tStart][0] = key(SDL_SCANCODE_RETURN);
    b[tL][0] = key(SDL_SCANCODE_LSHIFT);
    b[tL][1] = mouse(SDL_BUTTON_RIGHT);
    b[tR][0] = key(SDL_SCANCODE_SPACE);
    b[tR][1] = mouse(SDL_BUTTON_LEFT);
    b[tDUp][0] = key(SDL_SCANCODE_1);
    b[tDDown][0] = key(SDL_SCANCODE_2);
    b[tDLeft][0] = key(SDL_SCANCODE_3);
    b[tDRight][0] = key(SDL_SCANCODE_4);
    b[tStickUp][0] = key(SDL_SCANCODE_W);
    b[tStickDown][0] = key(SDL_SCANCODE_S);
    b[tStickLeft][0] = key(SDL_SCANCODE_A);
    b[tStickRight][0] = key(SDL_SCANCODE_D);
    b[tCUp][0] = key(SDL_SCANCODE_UP);
    b[tCDown][0] = key(SDL_SCANCODE_DOWN);
    b[tCLeft][0] = key(SDL_SCANCODE_LEFT);
    b[tCRight][0] = key(SDL_SCANCODE_RIGHT);
    return p;
}

// Gamepads: the GameCube layout by position (A = bottom face button, B = left, X = right,
// Y = top), except Nintendo pads, which use their printed A/B/X/Y. Z = right shoulder,
// L/R = triggers (analog, plus the click past the threshold), sticks and D-pad as is.
// A GameCube controller (adapter or NSO) maps 1:1, L/R clicks being its real clicks.
Profile padDefaults(SDL_GamepadType type, const std::string& name) {
    Profile p;
    p.type = type;
    p.name = name;
    auto& b = p.bind;
    const Family f = familyOf(type);
    // Face buttons by printed label on Nintendo/GameCube pads, by position elsewhere.
    SDL_GamepadButton face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_EAST,
                                 SDL_GAMEPAD_BUTTON_NORTH};  // A, B, X, Y
    if (f == Family::Nintendo || f == Family::GameCube) {
        for (int i = 0; i <= SDL_GAMEPAD_BUTTON_NORTH; i++) {
            const auto btn = SDL_GamepadButton(i);
            switch (SDL_GetGamepadButtonLabelForType(type, btn)) {
            case SDL_GAMEPAD_BUTTON_LABEL_A: face[0] = btn; break;
            case SDL_GAMEPAD_BUTTON_LABEL_B: face[1] = btn; break;
            case SDL_GAMEPAD_BUTTON_LABEL_X: face[2] = btn; break;
            case SDL_GAMEPAD_BUTTON_LABEL_Y: face[3] = btn; break;
            default: break;
            }
        }
    }
    b[tA][0] = button(face[0]);
    b[tB][0] = button(face[1]);
    b[tX][0] = button(face[2]);
    b[tY][0] = button(face[3]);
    b[tZ][0] = button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    b[tStart][0] = button(SDL_GAMEPAD_BUTTON_START);
    if (f == Family::GameCube) {
        b[tL][0] = button(SDL_GAMEPAD_BUTTON_MISC3);
        b[tR][0] = button(SDL_GAMEPAD_BUTTON_MISC4);
        p.deadzone[0] = p.deadzone[1] = 0.0f;  // the game's PADClamp already has the GameCube dead zone
    } else {
        b[tZ][1] = button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        b[tL][0] = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1);
        b[tR][0] = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1);
        b[tStart][1] = button(SDL_GAMEPAD_BUTTON_BACK);
    }
    b[tDUp][0] = button(SDL_GAMEPAD_BUTTON_DPAD_UP);
    b[tDDown][0] = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    b[tDLeft][0] = button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    b[tDRight][0] = button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    b[tStickUp][0] = axis(SDL_GAMEPAD_AXIS_LEFTY, -1);
    b[tStickDown][0] = axis(SDL_GAMEPAD_AXIS_LEFTY, 1);
    b[tStickLeft][0] = axis(SDL_GAMEPAD_AXIS_LEFTX, -1);
    b[tStickRight][0] = axis(SDL_GAMEPAD_AXIS_LEFTX, 1);
    b[tCUp][0] = axis(SDL_GAMEPAD_AXIS_RIGHTY, -1);
    b[tCDown][0] = axis(SDL_GAMEPAD_AXIS_RIGHTY, 1);
    b[tCLeft][0] = axis(SDL_GAMEPAD_AXIS_RIGHTX, -1);
    b[tCRight][0] = axis(SDL_GAMEPAD_AXIS_RIGHTX, 1);
    b[tAnalogL][0] = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1);
    b[tAnalogR][0] = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1);
    return p;
}

// Restores a profile's bindings (keep=false: and its analog/rumble tuning) to defaults.
Profile defaultsFor(const Profile& p) { return p.keyboard ? keyboardDefaults() : padDefaults(p.type, p.name); }

// ---- Configuration -------------------------------------------------------------------

struct PortConfig {
    std::string device = "auto";  // auto | none | keyboard | pad:<guid>#<n>
    std::string deviceName;       // for a chosen pad that is not connected
    bool keyboard = false;        // keyboard (and mouse) also drive this port
};

struct Config {
    std::array<PortConfig, PAD_MAX_CONTROLLERS> ports;
    Profile keyboard = keyboardDefaults();
    std::map<std::string, Profile> pads;  // by SDL GUID
    Config() { ports[0].keyboard = true; }
};

std::recursive_mutex sMutex;  // menu (host thread) vs PADRead (guest thread)
Config sCfg;
u64 sGeneration = 1;   // bumped on every change; ports re-push when theirs is older
bool sUnsaved = false; // changed outside drawTab (capture callback)

// ---- Connected gamepads and port assignment ------------------------------------------

struct Pad {
    SDL_JoystickID id = 0;
    std::string guid, name;
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_STANDARD;
    int ordinal = 0;  // among connected pads of the same GUID (e.g. GameCube adapter ports)
    std::string deviceId() const { return "pad:" + guid + "#" + std::to_string(ordinal); }
};

std::vector<Pad> listPads() {
    std::vector<Pad> pads;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return pads;
    std::vector<SDL_JoystickID> sorted(ids, ids + count);
    SDL_free(ids);
    std::sort(sorted.begin(), sorted.end());
    for (const SDL_JoystickID id : sorted) {
        if (!SDL_GetGamepadFromID(id)) continue;  // not opened by Aurora (ignored device)
        Pad p;
        p.id = id;
        char guid[33] = {};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
        p.guid = guid;
        const char* n = SDL_GetGamepadNameForID(id);
        p.name = n ? n : "Gamepad";
        p.type = SDL_GetGamepadTypeForID(id);
        for (const auto& q : pads)
            if (q.guid == p.guid) p.ordinal++;
        pads.push_back(std::move(p));
    }
    return pads;
}

struct PortState {
    Pad pad;               // assigned gamepad (id 0: none)
    bool keyboard = false; // keyboard bindings active
    u64 pushedGen = 0;
    SDL_JoystickID pushedPad = -1;
    bool pushedKeyboard = false;
    PADStatus last{};      // last read, after shaping (for the input test)
    bool haveLast = false;
};
std::array<PortState, PAD_MAX_CONTROLLERS> sPorts;
std::vector<SDL_JoystickID> sPadIds;  // connected gamepads at the last assignment
u64 sAssignedGen = 0;
std::vector<Pad> sPads;

Profile& padProfile(const Pad& pad) {
    auto it = sCfg.pads.find(pad.guid);
    if (it == sCfg.pads.end()) it = sCfg.pads.emplace(pad.guid, padDefaults(pad.type, pad.name)).first;
    it->second.name = pad.name;
    it->second.type = pad.type;
    return it->second;
}

const bool sPadLog = [] {
    const char* v = std::getenv("LM_PAD_LOG");
    return v && *v && *v != '0';
}();

const char* connectionName(SDL_JoystickConnectionState s) {
    switch (s) {
    case SDL_JOYSTICK_CONNECTION_WIRED: return "wired";
    case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
    case SDL_JOYSTICK_CONNECTION_INVALID: return "connection ?";
    default: return "connection unknown";
    }
}

// Gives every port its device: chosen pads first, then "Auto" ports take the remaining
// pads in connection order. Sets SDL player indices, which is how Aurora's PAD maps ports.
void assign() {
    sPads = listPads();
    std::vector<SDL_JoystickID> ids;
    for (const auto& p : sPads) ids.push_back(p.id);
    if (ids == sPadIds && sAssignedGen == sGeneration) return;
    static bool sListed = false;
    if (sPadLog && (ids != sPadIds || !sListed)) {  // every joystick SDL sees, gamepad or not
        sListed = true;
        int n = 0;
        if (SDL_JoystickID* js = SDL_GetJoysticks(&n)) {
            for (int i = 0; i < n; i++)
                log("[controls] joystick %u: %s (vid %04x pid %04x)%s", unsigned(js[i]),
                    SDL_GetJoystickNameForID(js[i]) ? SDL_GetJoystickNameForID(js[i]) : "?",
                    SDL_GetJoystickVendorForID(js[i]), SDL_GetJoystickProductForID(js[i]),
                    SDL_IsGamepad(js[i]) ? " gamepad" : " (no gamepad mapping)");
            SDL_free(js);
        }
    }
    sPadIds = ids;
    sAssignedGen = sGeneration;

    std::vector<bool> claimed(sPads.size(), false);
    std::array<int, PAD_MAX_CONTROLLERS> chosen;
    chosen.fill(-1);
    for (int port = 0; port < PAD_MAX_CONTROLLERS; port++) {
        const std::string& dev = sCfg.ports[port].device;
        if (dev.rfind("pad:", 0) != 0) continue;
        const auto hash = dev.find('#');
        const std::string guid = dev.substr(4, hash == std::string::npos ? std::string::npos : hash - 4);
        const int ord = hash == std::string::npos ? 0 : std::atoi(dev.c_str() + hash + 1);
        int pick = -1;
        for (size_t i = 0; i < sPads.size(); i++) {
            if (claimed[i] || sPads[i].guid != guid) continue;
            if (sPads[i].ordinal == ord) {
                pick = int(i);
                break;
            }
            if (pick < 0) pick = int(i);  // same model, other slot: better than nothing
        }
        if (pick >= 0) {
            claimed[pick] = true;
            chosen[port] = pick;
        }
    }
    for (int port = 0; port < PAD_MAX_CONTROLLERS; port++) {
        if (sCfg.ports[port].device != "auto") continue;
        for (size_t i = 0; i < sPads.size(); i++)
            if (!claimed[i]) {
                claimed[i] = true;
                chosen[port] = int(i);
                break;
            }
    }
    for (size_t i = 0; i < sPads.size(); i++) {
        int want = -1;
        for (int port = 0; port < PAD_MAX_CONTROLLERS; port++)
            if (chosen[port] == int(i)) want = port;
        if (SDL_Gamepad* g = SDL_GetGamepadFromID(sPads[i].id); g && SDL_GetGamepadPlayerIndex(g) != want)
            SDL_SetGamepadPlayerIndex(g, want);
    }
    for (int port = 0; port < PAD_MAX_CONTROLLERS; port++) {
        auto& st = sPorts[port];
        const auto& pc = sCfg.ports[port];
        const Pad before = st.pad;
        st.pad = chosen[port] >= 0 ? sPads[chosen[port]] : Pad{};
        st.keyboard = pc.device == "keyboard" || (pc.keyboard && pc.device != "none");
        if (st.pad.id != before.id || st.pushedGen == 0) {
            if (st.pad.id) {
                SDL_Gamepad* g = SDL_GetGamepadFromID(st.pad.id);
                const SDL_PropertiesID props = g ? SDL_GetGamepadProperties(g) : 0;
                log("[controls] port %d: %s (%s, guid %s, vid %04x pid %04x, %s, rumble %s)%s", port + 1,
                    st.pad.name.c_str(), typeName(st.pad.type), st.pad.guid.c_str(), g ? SDL_GetGamepadVendor(g) : 0,
                    g ? SDL_GetGamepadProduct(g) : 0, g ? connectionName(SDL_GetGamepadConnectionState(g)) : "?",
                    props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "not reported",
                    st.keyboard ? " + keyboard" : "");
                if (const char* path = g ? SDL_GetGamepadPath(g) : nullptr)  // shows SDL's backend (XInput, raw input, HID)
                    log("[controls]   path %s", path);
            }
            else if (before.id || st.keyboard)
                log("[controls] port %d: %s", port + 1, st.keyboard ? "keyboard" : "no device");
            if (sPadLog && st.pad.id)
                if (SDL_Gamepad* g = SDL_GetGamepadFromID(st.pad.id))
                    if (char* m = SDL_GetGamepadMapping(g)) {
                        log("[controls]   SDL mapping: %s", m);
                        SDL_free(m);
                    }
        }
    }
}

ControlId stickControl(Target t, float& scale) {
    const auto& c = aurora::pad::controls();
    switch (t) {
    case tStickUp: scale = 1; return c.leftY;
    case tStickDown: scale = -1; return c.leftY;
    case tStickLeft: scale = -1; return c.leftX;
    case tStickRight: scale = 1; return c.leftX;
    case tCUp: scale = 1; return c.rightY;
    case tCDown: scale = -1; return c.rightY;
    case tCLeft: scale = -1; return c.rightX;
    case tCRight: scale = 1; return c.rightX;
    case tAnalogL: scale = 1; return c.triggerL;
    case tAnalogR: scale = 1; return c.triggerR;
    default: scale = 0; return aurora::binding::kInvalidControlId;
    }
}

void addBindings(std::vector<Binding>& out, const Profile& prof) {
    for (int t = 0; t < kTargets; t++) {
        for (const Input& in : prof.bind[t]) {
            if (!in) continue;
            Binding b;
            switch (in.kind) {
            case Input::Key: b.input.control = PhysicalInput::Key{SDL_Scancode(in.code)}; break;
            case Input::Mouse: b.input.control = PhysicalInput::MouseButton{u8(in.code)}; break;
            case Input::Button: b.input.control = PhysicalInput::GamepadButton{SDL_GamepadButton(in.code)}; break;
            case Input::Axis:
                b.input.control = PhysicalInput::GamepadAxis{
                    SDL_GamepadAxis(in.code), in.dir < 0 ? PhysicalInput::GamepadAxis::Direction::Negative
                                                         : PhysicalInput::GamepadAxis::Direction::Positive};
                break;
            default: continue;
            }
            if (const u16 bit = kTargetInfo[t].padButton) {
                b.target = aurora::pad::control_for_button(bit);
                if (in.kind == Input::Axis)
                    b.threshold = in.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER    ? prof.threshold[0]
                                  : in.code == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? prof.threshold[1]
                                                                              : 0.5f;
            } else {
                b.target = stickControl(Target(t), b.scale);
                if (!prof.keyboard) {
                    const int stick = t >= tCUp && t <= tCRight ? 1 : 0;
                    const bool horizontal = t == tStickLeft || t == tStickRight || t == tCLeft || t == tCRight;
                    const bool vertical = t == tStickUp || t == tStickDown || t == tCUp || t == tCDown;
                    if ((horizontal && prof.invertX[stick]) || (vertical && prof.invertY[stick])) b.scale = -b.scale;
                }
            }
            out.push_back(std::move(b));
        }
    }
}

// Hands a port's bindings to Aurora: its own controller mappings and keyboard bindings
// are emptied (in memory only; Aurora's mapping files are never written) and ours are
// added as action bindings.
void push(u32 port) {
    auto& st = sPorts[port];
    PADClearKeyBindings(port);
    PADSetKeyboardActive(port, st.keyboard ? TRUE : FALSE);
    std::vector<Binding> list;
    if (st.keyboard) addBindings(list, sCfg.keyboard);
    if (st.pad.id) {
        u32 n = 0;
        if (PADButtonMapping* m = PADGetButtonMappings(port, &n))
            for (u32 i = 0; i < n; i++) m[i].nativeButton = PAD_NATIVE_BUTTON_INVALID;
        if (PADAxisMapping* m = PADGetAxisMappings(port, &n))
            for (u32 i = 0; i < n; i++) {
                m[i].nativeAxis = {-1, AXIS_SIGN_POSITIVE};
                m[i].nativeButton = -1;
            }
        if (PADDeadZones* dz = PADGetDeadZones(port)) {
            dz->emulateTriggers = false;
            dz->useDeadzones = false;
        }
        const Profile& prof = padProfile(st.pad);
        addBindings(list, prof);
        if (PADSupportsRumbleIntensity(port)) {
            const u16 s = u16(std::clamp(prof.rumbleStrength, 0.0f, 1.0f) * 65535.0f);
            PADSetRumbleIntensity(port, s, s);
        }
        if (!prof.rumble) PADControlMotor(port, PAD_MOTOR_STOP_HARD);
    }
    aurora::pad::set_action_bindings(port, std::move(list));
    st.pushedGen = sGeneration;
    st.pushedPad = st.pad.id;
    st.pushedKeyboard = st.keyboard;
}

// ---- Rumble ---------------------------------------------------------------------------
// The game switches the GameCube motor on and off (PADControlMotor: JUTGamePad::CRumble
// start/stop and its bit patterns while vacuuming). A GameCube controller (adapter) keeps
// Aurora's on/off path. Every other gamepad is rumbled here with SDL_RumbleGamepad and a
// finite duration that is refreshed while the motor is on, instead of Aurora's single
// call with duration 0. Duration 0 means "until changed" with no resend: SDL then never
// repeats the request, so a first request the backend rejected (Windows raw input before
// the pad is matched to its XInput/WGI slot: "Controller isn't correlated yet") or one
// the controller's firmware times out (Xbox pads over Bluetooth/HIDAPI) was lost until
// the game changed the motor state. With a duration SDL resends on its own, and the
// refresh retries a rejected request. It also stops by itself if the game stops reading
// the pads (paused on focus loss) with the motor on. LM_RUMBLE_LOG=1 logs every request.
constexpr u32 kRumbleMs = 500;       // duration of each request
constexpr int kRumbleRefreshMs = 100;  // re-sent this often while the motor is on

struct Motor {
    bool on = false;      // the game's (or Test rumble's) motor state
    bool sdl = false;     // last request went through SDL_RumbleGamepad
    bool failed = false;  // last request was rejected
    Clock::time_point sent;
    u32 requests = 0, failures = 0;
    std::string error;    // SDL's reason for the last rejected request
};
std::array<Motor, PAD_MAX_CONTROLLERS> sMotor;

const bool sRumbleLog = [] {
    const char* v = std::getenv("LM_RUMBLE_LOG");
    return v && *v && *v != '0';
}();

bool isGameCubePad(SDL_Gamepad* g) {
    return SDL_GetGamepadVendor(g) == 0x057E && SDL_GetGamepadProduct(g) == 0x0337;
}

const char* motorName(u32 cmd) {
    return cmd == PAD_MOTOR_RUMBLE ? "rumble" : cmd == PAD_MOTOR_STOP_HARD ? "stop hard" : cmd == PAD_MOTOR_STOP ? "stop" : "?";
}

// Sends port's motor state to its gamepad. refresh: periodic re-send while on.
void sendMotor(u32 port, u32 cmd, bool refresh) {
    auto& m = sMotor[port];
    const auto& st = sPorts[port];
    const bool on = cmd == PAD_MOTOR_RUMBLE;
    m.on = on;
    SDL_Gamepad* g = st.pad.id ? SDL_GetGamepadFromID(st.pad.id) : nullptr;
    if (!g) {
        if (sRumbleLog && !refresh)
            log("[rumble] port %u %s: no gamepad on this port (%s)", port + 1, motorName(cmd),
                st.keyboard ? "keyboard" : "none");
        return;
    }
    const Profile& prof = padProfile(st.pad);
    if (isGameCubePad(g)) {  // adapter motor: on/off only, Aurora sends it with the adapter's output
        if (refresh) return;
        PADControlMotor(port, on && prof.rumble ? PAD_MOTOR_RUMBLE : cmd == PAD_MOTOR_RUMBLE ? PAD_MOTOR_STOP : cmd);
        if (sRumbleLog) log("[rumble] port %u %s: GameCube controller via Aurora", port + 1, motorName(cmd));
        return;
    }
    const u16 level = on && prof.rumble ? u16(std::clamp(prof.rumbleStrength, 0.0f, 1.0f) * 65535.0f + 0.5f) : 0;
    if (!refresh && !level && !m.sdl) return;  // nothing to stop
    const bool ok = SDL_RumbleGamepad(g, level, level, level ? kRumbleMs : 0);
    m.sdl = level != 0;
    m.sent = Clock::now();
    m.requests++;
    if (!ok) m.failures++;
    if (sRumbleLog && (!refresh || ok == m.failed))  // refreshes: only when the result changes
        log("[rumble] port %u %s%s -> '%s' low=high=%u for %u ms (request %u, profile rumble %s, strength %.2f, SDL cap %d): %s%s",
            port + 1, motorName(cmd), refresh ? " (refresh)" : "", st.pad.name.c_str(), unsigned(level),
            level ? kRumbleMs : 0u, m.requests, prof.rumble ? "on" : "off", prof.rumbleStrength,
            int(SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)),
            ok ? "ok" : "FAILED: ", ok ? "" : SDL_GetError());
    else if (!ok && !m.failed)  // always logged once per failure streak, so a user's log shows why
        log("[rumble] port %u: SDL_RumbleGamepad failed on '%s': %s (request %u, %u failed)", port + 1,
            st.pad.name.c_str(), SDL_GetError(), m.requests, m.failures);
    else if (ok && m.failed)
        log("[rumble] port %u: SDL_RumbleGamepad works again on '%s'", port + 1, st.pad.name.c_str());
    m.failed = !ok;
    if (!ok) m.error = SDL_GetError();
}

// Called on every PADRead and menu frame: keeps an "on" motor going (and retries a
// rejected request sooner).
void refreshMotors() {
    const auto now = Clock::now();
    for (u32 port = 0; port < PAD_MAX_CONTROLLERS; port++) {
        const auto& m = sMotor[port];
        if (m.on && now - m.sent >= std::chrono::milliseconds(m.failed ? kRumbleRefreshMs / 4 : kRumbleRefreshMs))
            sendMotor(port, PAD_MOTOR_RUMBLE, true);
    }
}

// Radial dead zone (rescaled so the edge stays reachable) and sensitivity, on PAD values.
void shape(s8& x, s8& y, float deadzone, float sensitivity) {
    if (deadzone <= 0.0f && sensitivity == 1.0f) return;
    float fx = x / 127.0f, fy = y / 127.0f;
    const float m = std::sqrt(fx * fx + fy * fy);
    if (m <= deadzone || m == 0.0f) {
        x = y = 0;
        return;
    }
    const float k = (m - deadzone) / (1.0f - std::min(deadzone, 0.95f)) / m * sensitivity;
    fx = std::clamp(fx * k, -1.0f, 1.0f);
    fy = std::clamp(fy * k, -1.0f, 1.0f);
    x = s8(std::lround(fx * 127.0f));
    y = s8(std::lround(fy * 127.0f));
}

// ---- Settings (settings.ini) ---------------------------------------------------------

void resetConfig() {
    std::lock_guard lk(sMutex);
    sCfg = Config{};
    sGeneration++;
}

bool parseBool(const std::string& v) { return v == "true" || v == "1"; }
float parseFloat(const std::string& v, float lo, float hi, float def) {
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end != v.c_str() && std::isfinite(f) ? std::clamp(f, lo, hi) : def;
}

// Profile keys shared by the keyboard and gamepad sections.
void loadProfileKey(Profile& p, const std::string& key, const std::string& value) {
    for (int t = 0; t < kTargets; t++) {
        const std::string k = kTargetInfo[t].key;
        if (key == k) p.bind[t][0] = fromString(value);
        else if (key == k + ".2") p.bind[t][1] = fromString(value);
    }
    if (p.keyboard) return;
    if (key == "name") p.name = value;
    else if (key == "type") p.type = SDL_GamepadType(std::clamp(std::atoi(value.c_str()), 0, int(SDL_GAMEPAD_TYPE_COUNT) - 1));
    else if (key == "stick_deadzone") p.deadzone[0] = parseFloat(value, 0, 0.9f, p.deadzone[0]);
    else if (key == "c_deadzone") p.deadzone[1] = parseFloat(value, 0, 0.9f, p.deadzone[1]);
    else if (key == "stick_sensitivity") p.sensitivity[0] = parseFloat(value, 0.25f, 3, 1);
    else if (key == "c_sensitivity") p.sensitivity[1] = parseFloat(value, 0.25f, 3, 1);
    else if (key == "stick_invert_x") p.invertX[0] = parseBool(value);
    else if (key == "stick_invert_y") p.invertY[0] = parseBool(value);
    else if (key == "c_invert_x") p.invertX[1] = parseBool(value);
    else if (key == "c_invert_y") p.invertY[1] = parseBool(value);
    else if (key == "l_threshold") p.threshold[0] = parseFloat(value, 0.05f, 1, p.threshold[0]);
    else if (key == "r_threshold") p.threshold[1] = parseFloat(value, 0.05f, 1, p.threshold[1]);
    else if (key == "rumble") p.rumble = parseBool(value);
    else if (key == "rumble_strength") p.rumbleStrength = parseFloat(value, 0, 1, p.rumbleStrength);
}

void loadKey(const std::string& section, const std::string& key, const std::string& value) {
    std::lock_guard lk(sMutex);
    if (section == "controls") {
        // port<N>_device / port<N>_device_name / port<N>_keyboard
        if (key.size() > 5 && key.rfind("port", 0) == 0 && key[4] >= '1' && key[4] <= '4' && key[5] == '_') {
            auto& pc = sCfg.ports[key[4] - '1'];
            const std::string k = key.substr(6);
            if (k == "device") pc.device = value.empty() ? "auto" : value;
            else if (k == "device_name") pc.deviceName = value;
            else if (k == "keyboard") pc.keyboard = parseBool(value);
        }
    } else if (section == "controls.keyboard") {
        loadProfileKey(sCfg.keyboard, key, value);
    } else if (section.rfind("controls.pad.", 0) == 0) {
        const std::string guid = section.substr(13);
        auto it = sCfg.pads.find(guid);
        // Keys left out keep the defaults of the pad's type (an empty value unbinds), so a
        // "type" key that comes before any other setting rebuilds those defaults.
        static std::vector<std::string> sTouched;  // sections with settings loaded
        if (it == sCfg.pads.end()) {
            it = sCfg.pads.emplace(guid, padDefaults(SDL_GAMEPAD_TYPE_STANDARD, "")).first;
            sTouched.erase(std::remove(sTouched.begin(), sTouched.end(), guid), sTouched.end());
        }
        Profile& prof = it->second;
        const bool touched = std::find(sTouched.begin(), sTouched.end(), guid) != sTouched.end();
        if (key == "type" && !touched) {
            const std::string name = prof.name;
            prof = padDefaults(SDL_GamepadType(std::clamp(std::atoi(value.c_str()), 0, int(SDL_GAMEPAD_TYPE_COUNT) - 1)),
                               name);
        } else {
            loadProfileKey(prof, key, value);
            if (key != "name" && !touched) sTouched.push_back(guid);
        }
    }
    sGeneration++;
}

void saveProfile(std::ostream& out, const Profile& p) {
    if (!p.keyboard) {
        out << "name=" << p.name << '\n' << "type=" << int(p.type) << '\n';
    }
    for (int t = 0; t < kTargets; t++) {
        out << kTargetInfo[t].key << '=' << toString(p.bind[t][0]) << '\n';
        if (p.bind[t][1]) out << kTargetInfo[t].key << ".2=" << toString(p.bind[t][1]) << '\n';
    }
    if (p.keyboard) return;
    out << "stick_deadzone=" << p.deadzone[0] << '\n' << "c_deadzone=" << p.deadzone[1] << '\n'
        << "stick_sensitivity=" << p.sensitivity[0] << '\n' << "c_sensitivity=" << p.sensitivity[1] << '\n'
        << "stick_invert_x=" << (p.invertX[0] ? "true" : "false") << '\n'
        << "stick_invert_y=" << (p.invertY[0] ? "true" : "false") << '\n'
        << "c_invert_x=" << (p.invertX[1] ? "true" : "false") << '\n'
        << "c_invert_y=" << (p.invertY[1] ? "true" : "false") << '\n'
        << "l_threshold=" << p.threshold[0] << '\n' << "r_threshold=" << p.threshold[1] << '\n'
        << "rumble=" << (p.rumble ? "true" : "false") << '\n' << "rumble_strength=" << p.rumbleStrength << '\n';
}

void saveConfig(std::ostream& out) {
    std::lock_guard lk(sMutex);
    out << "[controls]\n";
    for (int i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        const auto& pc = sCfg.ports[i];
        out << "port" << i + 1 << "_device=" << pc.device << '\n';
        if (!pc.deviceName.empty()) out << "port" << i + 1 << "_device_name=" << pc.deviceName << '\n';
        out << "port" << i + 1 << "_keyboard=" << (pc.keyboard ? "true" : "false") << '\n';
    }
    out << "[controls.keyboard]\n";
    saveProfile(out, sCfg.keyboard);
    for (const auto& [guid, p] : sCfg.pads) {
        if (p == padDefaults(p.type, p.name)) continue;  // untouched: defaults are rebuilt
        out << "[controls.pad." << guid << "]\n";
        saveProfile(out, p);
    }
}

const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

// ---- Binding capture -----------------------------------------------------------------

constexpr float kCaptureSeconds = 6.0f;

struct Capture {
    bool active = false;
    std::string profile;  // "keyboard" or a GUID
    int target = 0, slot = 0;
    Clock::time_point start;
    bool timeout = true;  // off for the LM_CONTROLS_CAPTURE test aid
} sCap;
SDL_Scancode sSwallowKey = SDL_SCANCODE_UNKNOWN;  // key that ended a capture: hidden from hotkeys until released

Profile* findProfile(const std::string& key) {
    if (key == "keyboard") return &sCfg.keyboard;
    const auto it = sCfg.pads.find(key);
    return it != sCfg.pads.end() ? &it->second : nullptr;
}

void endCapture() {
    sCap.active = false;
    aurora::binding::cancel_capture();
}

void onCaptured(const PhysicalInput& in);

void armCapture() { aurora::binding::capture_next(&onCaptured); }

// Host thread, from Aurora's input router (inside aurora_update).
void onCaptured(const PhysicalInput& in) {
    std::lock_guard lk(sMutex);
    if (!sCap.active) return;
    Profile* prof = findProfile(sCap.profile);
    if (!prof) {
        sCap.active = false;
        return;
    }
    Input got;
    if (const auto* k = in.control.get_if<PhysicalInput::Key>()) {
        sSwallowKey = k->scancode;
        if (k->scancode == SDL_SCANCODE_ESCAPE || k->scancode == SDL_SCANCODE_F1) {  // menu keys: cancel
            sCap.active = false;
            return;
        }
        if (prof->keyboard) got = key(k->scancode);
    } else if (const auto* m = in.control.get_if<PhysicalInput::MouseButton>()) {
        if (prof->keyboard) got = mouse(m->button);
    } else if (const auto* b = in.control.get_if<PhysicalInput::GamepadButton>()) {
        if (!prof->keyboard) got = button(b->button);
    } else if (const auto* a = in.control.get_if<PhysicalInput::GamepadAxis>()) {
        const int dir = a->direction == PhysicalInput::GamepadAxis::Direction::Negative ? -1 : 1;
        if (!prof->keyboard && !(isTrigger(a->axis) && dir < 0)) got = axis(a->axis, dir);
    }
    if (!got) {  // wrong kind of device for this profile: keep waiting
        armCapture();
        return;
    }
    prof->bind[sCap.target][sCap.slot] = got;
    sCap.active = false;
    sGeneration++;
    sUnsaved = true;
}

void beginCapture(const std::string& profile, int target, int slot) {
    sCap.active = true;
    sCap.profile = profile;
    sCap.target = target;
    sCap.slot = slot;
    sCap.start = Clock::now();
    sCap.timeout = true;
    armCapture();
}

// ---- Menu ----------------------------------------------------------------------------

std::string sSelProfile;  // profile shown in Bindings / Analog & rumble ("keyboard" or GUID)
bool sProfilePicked = false;
int sTestPort = 0;
Clock::time_point sRumbleTestEnd;
int sRumbleTestPort = -1;

std::string portDeviceLabel(const PortConfig& pc) {
    if (pc.device == "auto") return "Auto (next free controller)";
    if (pc.device == "none") return "None";
    if (pc.device == "keyboard") return "Keyboard only";
    for (const auto& p : sPads)
        if (p.deviceId() == pc.device) return p.name + (p.ordinal ? " #" + std::to_string(p.ordinal + 1) : "");
    return (pc.deviceName.empty() ? std::string("Controller") : pc.deviceName) + " (not connected)";
}

bool portsTab() {
    bool changed = false;
    ImGui::TextWrapped("Choose what drives each GameCube controller port. Luigi's Mansion is played on port 1. "
                       "\"Auto\" takes the next connected controller that no other port has chosen.");
    if (ImGui::BeginTable("ports", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 40 * recomp::ui::scale());
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("+ Keyboard", ImGuiTableColumnFlags_WidthFixed, 80 * recomp::ui::scale());
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();
        for (int port = 0; port < PAD_MAX_CONTROLLERS; port++) {
            auto& pc = sCfg.ports[port];
            const auto& st = sPorts[port];
            ImGui::PushID(port);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", port + 1);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##device", portDeviceLabel(pc).c_str())) {
                auto option = [&](const char* label, const std::string& dev, const std::string& name) {
                    if (ImGui::Selectable(label, pc.device == dev)) {
                        pc.device = dev;
                        pc.deviceName = name;
                        changed = true;
                    }
                };
                option("Auto (next free controller)", "auto", "");
                option("None", "none", "");
                option("Keyboard only", "keyboard", "");
                for (const auto& p : sPads) {
                    std::string label = p.name + (p.ordinal ? " #" + std::to_string(p.ordinal + 1) : "") + "##" +
                                        p.deviceId();
                    option(label.c_str(), p.deviceId(), p.name);
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("The controller (or the keyboard) that drives this GameCube port.");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(pc.device == "keyboard" || pc.device == "none");
            changed |= ImGui::Checkbox("##kb", &pc.keyboard);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Keyboard and mouse also control this port");
            ImGui::TableNextColumn();
            if (st.pad.id) {
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "%s%s", st.pad.name.c_str(), st.keyboard ? " + keys" : "");
                ImGui::SetItemTooltip("Connected: %s (%s)", st.pad.name.c_str(), typeName(st.pad.type));
            } else if (pc.device == "keyboard") {
                ImGui::TextUnformatted("Keyboard");
            } else if (pc.device == "none") {
                ImGui::TextDisabled("Unplugged");
            } else if (pc.device == "auto") {
                ImGui::TextDisabled(st.keyboard ? "No controller (keyboard)" : "No controller");
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "Waiting for %s",
                                   pc.deviceName.empty() ? "controller" : pc.deviceName.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Input test");
    for (int port = 0; port < PAD_MAX_CONTROLLERS; port++) {
        if (port) ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof(label), "Port %d", port + 1);
        ImGui::RadioButton(label, &sTestPort, port);
        ImGui::SetItemTooltip("Shows what the game reads from this port below.");
    }
    const auto& st = sPorts[sTestPort];
    if (!st.haveLast || st.last.err != PAD_ERR_NONE) {
        ImGui::TextDisabled("Nothing connected (or the game has not read the controllers yet).");
    } else {
        const PADStatus& s = st.last;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float k = recomp::ui::scale();  // menu size (ui_scale.cpp): the drawing scales with the text
        const float r = 34.0f * k;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImU32 rim = ImGui::GetColorU32(ImGuiCol_Border), dot = IM_COL32(120, 220, 120, 255);
        auto stick = [&](float cx, s8 x, s8 y, const char* label) {
            const ImVec2 c(origin.x + cx, origin.y + r + 2 * k);
            dl->AddCircle(c, r, rim, 32, 1.5f * k);
            dl->AddCircleFilled(ImVec2(c.x + x / 127.0f * r, c.y - y / 127.0f * r), 5 * k, dot);
            dl->AddText(ImVec2(c.x - ImGui::CalcTextSize(label).x / 2, c.y + r + 4 * k), ImGui::GetColorU32(ImGuiCol_Text), label);
        };
        stick(r + 4 * k, s.stickX, s.stickY, "Stick");
        stick(3 * r + 24 * k, s.substickX, s.substickY, "C");
        auto bar = [&](float x, float y, u8 v, const char* label) {
            const ImVec2 a(origin.x + x, origin.y + y), b(a.x + 90 * k, a.y + 12 * k);
            dl->AddRectFilled(a, ImVec2(a.x + 90 * k * v / 255.0f, b.y), dot);
            dl->AddRect(a, b, rim);
            dl->AddText(ImVec2(b.x + 6 * k, a.y - 2 * k), ImGui::GetColorU32(ImGuiCol_Text), label);
        };
        const float bx = 4 * r + 50 * k;
        bar(bx, 6 * k, s.triggerLeft, "L");
        bar(bx, 26 * k, s.triggerRight, "R");
        static const std::pair<u16, const char*> kBtns[] = {
            {PAD_BUTTON_A, "A"}, {PAD_BUTTON_B, "B"}, {PAD_BUTTON_X, "X"}, {PAD_BUTTON_Y, "Y"},
            {PAD_TRIGGER_Z, "Z"}, {PAD_TRIGGER_L, "L"}, {PAD_TRIGGER_R, "R"}, {PAD_BUTTON_START, "START"},
            {PAD_BUTTON_UP, "Up"}, {PAD_BUTTON_DOWN, "Down"}, {PAD_BUTTON_LEFT, "Left"}, {PAD_BUTTON_RIGHT, "Right"}};
        float x = bx;
        float y = 50 * k;
        for (const auto& [bit, name] : kBtns) {
            const ImVec2 sz = ImGui::CalcTextSize(name);
            const ImVec2 a(origin.x + x, origin.y + y), b(a.x + sz.x + 10 * k, a.y + sz.y + 4 * k);
            if (s.button & bit) dl->AddRectFilled(a, b, dot, 3 * k);
            dl->AddRect(a, b, rim, 3 * k);
            dl->AddText(ImVec2(a.x + 5 * k, a.y + 2 * k), ImGui::GetColorU32(ImGuiCol_Text), name);
            x += sz.x + 16 * k;
            if (bit == PAD_BUTTON_START) {
                x = bx;
                y += sz.y + 10 * k;
            }
        }
        ImGui::Dummy(ImVec2(0, 2 * r + 26 * k));
        ImGui::TextDisabled("buttons %04X  stick %d,%d  C %d,%d  L %u  R %u", s.button, s.stickX, s.stickY,
                            s.substickX, s.substickY, s.triggerLeft, s.triggerRight);
        ImGui::TextDisabled("Keyboard keys reach the game only while this menu does not have focus.");
    }
    return changed;
}

// Profile picker shared by the Bindings and Analog tabs: the keyboard, connected gamepad
// models, and saved profiles of models that are not connected.
void profilePicker(bool padsOnly) {
    std::vector<std::pair<std::string, std::string>> items;  // key, label
    if (!padsOnly) items.emplace_back("keyboard", "Keyboard and mouse");
    for (const auto& p : sPads) {
        if (std::any_of(items.begin(), items.end(), [&](const auto& i) { return i.first == p.guid; })) continue;
        padProfile(p);
        items.emplace_back(p.guid, p.name + " (" + typeName(p.type) + ")");
    }
    for (const auto& [guid, p] : sCfg.pads)
        if (std::none_of(items.begin(), items.end(), [&](const auto& i) { return i.first == guid; }))
            items.emplace_back(guid, (p.name.empty() ? std::string("Controller") : p.name) + " (not connected)");
    if (items.empty()) {
        sSelProfile.clear();
        return;
    }
    // Until one is picked, show port 1's controller (or the first in the list).
    if (!sProfilePicked || std::none_of(items.begin(), items.end(), [&](const auto& i) { return i.first == sSelProfile; }))
        sSelProfile = sPorts[0].pad.id ? sPorts[0].pad.guid : items[0].first;
    std::string current;
    for (const auto& [k, label] : items)
        if (k == sSelProfile) current = label;
    if (ImGui::BeginCombo("Profile", current.c_str())) {
        for (const auto& [k, label] : items)
            if (ImGui::Selectable((label + "##" + k).c_str(), k == sSelProfile)) {
                sSelProfile = k;
                sProfilePicked = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Settings are kept per device: the keyboard, and each controller model (all\n"
                          "controllers of one model share a profile).");
    std::string ports;
    for (int i = 0; i < PAD_MAX_CONTROLLERS; i++)
        if ((sSelProfile == "keyboard" && sPorts[i].keyboard) ||
            (sPorts[i].pad.id && sPorts[i].pad.guid == sSelProfile))
            ports += (ports.empty() ? "" : ", ") + std::to_string(i + 1);
    ImGui::SameLine();
    if (ports.empty()) ImGui::TextDisabled("(not on a port)");
    else ImGui::TextDisabled("(port %s)", ports.c_str());
}

// Inputs bound to more than one GameCube input (L click + L analog on the same trigger is
// the normal pairing and not a conflict).
bool conflicts(const Profile& p, int t, int slot, std::string& with) {
    const Input& in = p.bind[t][slot];
    if (!in) return false;
    for (int u = 0; u < kTargets; u++) {
        if (u == t) continue;
        if ((t == tL && u == tAnalogL) || (t == tAnalogL && u == tL) || (t == tR && u == tAnalogR) ||
            (t == tAnalogR && u == tR))
            continue;
        for (const Input& other : p.bind[u])
            if (other == in) {
                with = std::string(kTargetInfo[u].group) + " " + kTargetInfo[u].label;
                return true;
            }
    }
    return false;
}

bool bindingsTab() {
    bool changed = false;
    profilePicker(false);
    Profile* prof = findProfile(sSelProfile);
    if (!prof) return false;
    ImGui::TextDisabled("Click a slot, then press a %s. Esc cancels, right-click clears.",
                        prof->keyboard ? "key or mouse button" : "button or move a stick/trigger");
    const float secondsLeft =
        kCaptureSeconds - std::chrono::duration<float>(Clock::now() - sCap.start).count();
    if (ImGui::BeginChild("bindings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4))) {
        if (ImGui::BeginTable("bind", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("GameCube", ImGuiTableColumnFlags_WidthFixed, 150 * recomp::ui::scale());
            ImGui::TableSetupColumn("Primary");
            ImGui::TableSetupColumn("Alternate");
            ImGui::TableHeadersRow();
            const char* group = "";
            for (int t = 0; t < kTargets; t++) {
                if (std::strcmp(group, kTargetInfo[t].group) != 0) {
                    group = kTargetInfo[t].group;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", group);
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("  %s", kTargetInfo[t].label);
                for (int slot = 0; slot < 2; slot++) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(t * 2 + slot);
                    const bool capturing =
                        sCap.active && sCap.profile == sSelProfile && sCap.target == t && sCap.slot == slot;
                    std::string with;
                    const bool conflict = !capturing && conflicts(*prof, t, slot, with);
                    std::string label;
                    if (capturing) {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "Press %s... %.0f", prof->keyboard ? "a key" : "a button",
                                      std::max(secondsLeft, 0.0f));
                        label = buf;
                    } else {
                        label = prof->bind[t][slot] ? inputName(prof->bind[t][slot], prof->type) : "-";
                    }
                    if (conflict) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1));
                    if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    if (ImGui::Button(label.c_str(), ImVec2(-1, 0)) && !sCap.active) beginCapture(sSelProfile, t, slot);
                    if (capturing) ImGui::PopStyleColor();
                    if (conflict) {
                        ImGui::PopStyleColor();
                        ImGui::SetItemTooltip("Also bound to %s", with.c_str());
                    }
                    if (!sCap.active && ImGui::IsItemClicked(ImGuiMouseButton_Right) && prof->bind[t][slot]) {
                        prof->bind[t][slot] = {};
                        changed = true;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Reset bindings to defaults")) {
        prof->bind = defaultsFor(*prof).bind;
        changed = true;
    }
    ImGui::SetItemTooltip("Every binding of this profile back to its default.");
    ImGui::SameLine();
    if (ImGui::Button("Clear all")) {
        prof->bind = {};
        changed = true;
    }
    ImGui::SetItemTooltip("Removes every binding of this profile (then bind each input yourself).");
    return changed;
}

bool analogTab() {
    bool changed = false;
    profilePicker(true);
    Profile* prof = findProfile(sSelProfile);
    if (!prof || prof->keyboard) {
        ImGui::TextDisabled("No controller profile yet: connect a controller.");
        return false;
    }
    int port = -1;
    for (int i = 0; i < PAD_MAX_CONTROLLERS && port < 0; i++)
        if (sPorts[i].pad.id && sPorts[i].pad.guid == sSelProfile) port = i;
    auto pct = [&](const char* label, float& v, float lo, float hi) {
        float p = v * 100.0f;
        if (recomp::menu::SliderFloat(label, &p, lo * 100, hi * 100, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            v = p / 100.0f;
            changed = true;
        }
    };
    for (int s = 0; s < 2; s++) {
        ImGui::PushID(s);
        ImGui::SeparatorText(s == 0 ? "Control stick" : "C-stick");
        pct("Dead zone", prof->deadzone[s], 0.0f, 0.5f);
        ImGui::SetItemTooltip("Radial. The game adds the GameCube's own small dead zone (PADClamp) on top.");
        changed |= recomp::menu::SliderFloat("Sensitivity", &prof->sensitivity[s], 0.5f, 2.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Scales the stick's travel (1 = as the controller reports it). Above 1 the game's\n"
                              "full deflection is reached before the stick's edge.");
        changed |= ImGui::Checkbox("Invert X", &prof->invertX[s]);
        ImGui::SetItemTooltip("Left and right are swapped.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Invert Y", &prof->invertY[s]);
        ImGui::SetItemTooltip("Up and down are swapped.");
        ImGui::PopID();
    }
    ImGui::SeparatorText("Triggers");
    pct("L click at", prof->threshold[0], 0.05f, 1.0f);
    ImGui::SetItemTooltip("How far the analog L trigger must be pulled to press L (click).");
    pct("R click at", prof->threshold[1], 0.05f, 1.0f);
    ImGui::SetItemTooltip("How far the analog R trigger must be pulled to press R (click).");
    ImGui::TextDisabled("How far an analog trigger bound to L/R (click) must be pulled to press it.\n"
                        "Luigi's Mansion also reads the analog L/R values (Analog triggers bindings).");
    ImGui::SeparatorText("Rumble");
    changed |= recomp::menu::Checkbox("Rumble", &prof->rumble);
    ImGui::SetItemTooltip("The controller vibrates when the game turns the GameCube rumble motor on.");
    const bool intensity = port >= 0 ? PADSupportsRumbleIntensity(u32(port)) != FALSE : prof->type != SDL_GAMEPAD_TYPE_GAMECUBE;
    ImGui::BeginDisabled(!prof->rumble || !intensity);
    pct("Strength", prof->rumbleStrength, 0.05f, 1.0f);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", intensity ? "Vibration strength." : "This controller's rumble is only on or off.");
    if (!intensity) {
        ImGui::SameLine();
        ImGui::TextDisabled("(on/off only)");
    }
    ImGui::BeginDisabled(port < 0 || !prof->rumble);
    if (recomp::menu::Button("Test rumble")) {
        sRumbleTestPort = port;
        sRumbleTestEnd = Clock::now() + std::chrono::milliseconds(600);
        // Apply a changed strength first.
        sGeneration++;
        push(u32(port));
        sendMotor(u32(port), PAD_MOTOR_RUMBLE, false);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", port < 0 ? "Connect this controller and assign it to a port (Ports tab) to test."
                                         : "Vibrates the controller for 0.6 s.");
    if (port >= 0 && sMotor[port].requests) {
        ImGui::SameLine();
        if (sMotor[port].failed)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "The controller refused rumble: %s",
                               sMotor[port].error.c_str());
        else
            ImGui::TextDisabled("Sent to the controller.");
    }
    ImGui::Separator();
    if (ImGui::Button("Reset sticks, triggers and rumble")) {
        const Profile d = defaultsFor(*prof);
        const auto bind = prof->bind;
        *prof = d;
        prof->bind = bind;
        changed = true;
    }
    ImGui::SetItemTooltip("Dead zones, sensitivity, inversion, trigger points and rumble of this profile back\n"
                          "to their defaults (bindings are kept).");
    return changed;
}

// ---- Test aid: scripted virtual gamepad ----------------------------------------------
// LM_VIRTUAL_PAD="<read>:<action>,..." drives an SDL virtual Xbox 360 pad, timed in
// PADRead calls: attach, detach, motor=1|0 (game rumble on port 1), <button id>=0|1 (kButtonIds), <axis id>=-1..1 (kAxisIds;
// raw joystick values: +y is down, triggers go from -1 released to 1 pulled). Its rumble
// requests are logged. Exercises device
// assignment, hot-plug, gamepad bindings and rumble without touching a real controller.
struct VirtualStep {
    u32 at;
    std::string action;
};
std::vector<VirtualStep> sVirtualScript = [] {
    std::vector<VirtualStep> steps;
    const char* env = std::getenv("LM_VIRTUAL_PAD");
    if (!env) return steps;
    std::string s = env, item;
    for (size_t pos = 0; pos <= s.size();) {
        const size_t comma = std::min(s.find(',', pos), s.size());
        item = s.substr(pos, comma - pos);
        pos = comma + 1;
        if (const auto colon = item.find(':'); colon != std::string::npos)
            steps.push_back({u32(std::strtoul(item.c_str(), nullptr, 10)), item.substr(colon + 1)});
    }
    return steps;
}();
u32 sReads = 0;
SDL_JoystickID sVirtualId = 0;
SDL_Joystick* sVirtualJoy = nullptr;

// LM_VIRTUAL_PAD_RUMBLE_FAIL=<n>: the first n non-zero rumble requests are rejected, the way
// a backend rejects them before it is ready (Windows raw input before XInput correlation).
bool SDLCALL virtualRumble(void*, Uint16 low, Uint16 high) {
    static int sFail = [] {
        const char* v = std::getenv("LM_VIRTUAL_PAD_RUMBLE_FAIL");
        return v ? std::atoi(v) : 0;
    }();
    if ((low || high) && sFail > 0) {
        sFail--;
        log("[controls] virtual pad rumble low %u high %u: rejected (test)", low, high);
        return SDL_SetError("simulated rejection (LM_VIRTUAL_PAD_RUMBLE_FAIL)");
    }
    log("[controls] virtual pad rumble low %u high %u", low, high);
    return true;
}

void runVirtualScript() {
    if (sVirtualScript.empty()) return;
    sReads++;
    for (const auto& step : sVirtualScript) {
        if (step.at != sReads) continue;
        const std::string& a = step.action;
        if (a == "attach" && !sVirtualId) {
            SDL_VirtualJoystickDesc desc;
            SDL_INIT_INTERFACE(&desc);
            desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            desc.vendor_id = 0x045E;
            desc.product_id = 0x028E;  // Xbox 360 controller
            desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
            desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
            desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
            desc.name = "Virtual test pad";
            desc.Rumble = &virtualRumble;
            sVirtualId = SDL_AttachVirtualJoystick(&desc);
            sVirtualJoy = sVirtualId ? SDL_OpenJoystick(sVirtualId) : nullptr;
            if (sVirtualJoy)
                for (int t : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
                    SDL_SetJoystickVirtualAxis(sVirtualJoy, t, SDL_JOYSTICK_AXIS_MIN);  // released
            log("[controls] virtual pad attached (%u)", unsigned(sVirtualId));
        } else if (a == "detach" && sVirtualId) {
            if (sVirtualJoy) SDL_CloseJoystick(sVirtualJoy);
            SDL_DetachVirtualJoystick(sVirtualId);
            sVirtualJoy = nullptr;
            sVirtualId = 0;
            log("[controls] virtual pad detached");
        } else if (a == "motor=1" || a == "motor=0") {  // the game's PADControlMotor on port 1
            controlMotor(0, a == "motor=1" ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
        } else if (const auto eq = a.find('='); eq != std::string::npos && sVirtualJoy) {
            const std::string id = a.substr(0, eq);
            const float v = std::strtof(a.c_str() + eq + 1, nullptr);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
                if (id == kButtonIds[i]) SDL_SetJoystickVirtualButton(sVirtualJoy, i, v != 0.0f);
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
                if (id == kAxisIds[i])
                    SDL_SetJoystickVirtualAxis(sVirtualJoy, i, Sint16(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        }
    }
}

const char* sEnvSubTab = std::getenv("LM_CONTROLS_TAB");  // test aid: select a sub-tab once

ImGuiTabItemFlags subTabFlags(const char* name) {
    if (!sEnvSubTab || std::strcmp(sEnvSubTab, name) != 0) return 0;
    return ImGuiTabItemFlags_SetSelected;
}

}  // namespace

void selectSubTab(const char* name) { sEnvSubTab = name; }

bool drawTab() {
    std::lock_guard lk(sMutex);
    assign();  // hot-plug status even while the game is not reading the pads
    if (sCap.active && sCap.timeout &&
        std::chrono::duration<float>(Clock::now() - sCap.start).count() > kCaptureSeconds)
        endCapture();
    if (sRumbleTestPort >= 0 && Clock::now() >= sRumbleTestEnd) {
        sendMotor(u32(sRumbleTestPort), PAD_MOTOR_STOP, false);
        sRumbleTestPort = -1;
    }
    refreshMotors();  // the game may not be reading the pads while the menu is open
    // Test aid: LM_CONTROLS_CAPTURE=<keyboard|pad>:<input key, e.g. a>:<slot 1|2> starts a
    // binding capture when the tab first shows ("pad" = port 1's controller profile).
    static const char* sEnvCapture = std::getenv("LM_CONTROLS_CAPTURE");
    if (sEnvCapture) {
        const std::string spec = std::exchange(sEnvCapture, nullptr);
        const auto c1 = spec.find(':'), c2 = spec.rfind(':');
        if (c1 != std::string::npos && c2 > c1) {
            const std::string who = spec.substr(0, c1), what = spec.substr(c1 + 1, c2 - c1 - 1);
            const std::string profile = who == "pad" && sPorts[0].pad.id ? sPorts[0].pad.guid : "keyboard";
            if (profile != "keyboard") padProfile(sPorts[0].pad);
            sSelProfile = profile;
            sProfilePicked = true;
            for (int t = 0; t < kTargets; t++)
                if (what == kTargetInfo[t].key) {
                    beginCapture(profile, t, std::atoi(spec.c_str() + c2 + 1) == 2 ? 1 : 0);
                    sCap.timeout = false;
                }
        }
    }
    bool changed = std::exchange(sUnsaved, false);
    bool local = false;
    if (ImGui::BeginTabBar("controls")) {
        if (ImGui::BeginTabItem("Ports", nullptr, subTabFlags("Ports"))) {
            local |= portsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Bindings", nullptr, subTabFlags("Bindings"))) {
            local |= bindingsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sticks, triggers & rumble", nullptr, subTabFlags("Analog"))) {
            local |= analogTab();
            ImGui::EndTabItem();
        }
        if (touch::supported() && ImGui::BeginTabItem("Touch", nullptr, subTabFlags("Touch"))) {
            changed |= touch::drawSettings();  // a plain setting (settings.ini [enhancements])
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    sEnvSubTab = nullptr;
    if (local) sGeneration++;
    return changed || local;
}

bool handleEvent(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN && ev.type != SDL_EVENT_KEY_UP) return false;
    std::lock_guard lk(sMutex);
    if (sCap.active) return true;  // keys being captured never reach hotkeys
    if (sSwallowKey != SDL_SCANCODE_UNKNOWN && ev.key.scancode == sSwallowKey) {
        if (ev.type == SDL_EVENT_KEY_UP) sSwallowKey = SDL_SCANCODE_UNKNOWN;
        return true;
    }
    return false;
}

void beforeRead() {
    std::lock_guard lk(sMutex);
    runVirtualScript();
    assign();
    for (u32 port = 0; port < PAD_MAX_CONTROLLERS; port++) {
        const auto& st = sPorts[port];
        if (st.pushedGen != sGeneration || st.pushedPad != st.pad.id || st.pushedKeyboard != st.keyboard) push(port);
    }
    refreshMotors();
}

void afterRead(PADStatus* status) {
    std::lock_guard lk(sMutex);
    for (int port = 0; port < PAD_MAX_CONTROLLERS; port++) {
        auto& st = sPorts[port];
        PADStatus& s = status[port];
        if (s.err == PAD_ERR_NONE && st.pad.id) {
            const Profile& prof = padProfile(st.pad);
            shape(s.stickX, s.stickY, prof.deadzone[0], prof.sensitivity[0]);
            shape(s.substickX, s.substickY, prof.deadzone[1], prof.sensitivity[1]);
        }
        st.last = s;
        st.haveLast = true;
    }
}

void controlMotor(u32 chan, u32 cmd) {
    if (chan >= PAD_MAX_CONTROLLERS) return;
    std::lock_guard lk(sMutex);
    if (sRumbleTestPort == int(chan)) sRumbleTestPort = -1;  // the game takes the motor back
    sendMotor(chan, cmd, false);
}

}  // namespace recomp::controls
