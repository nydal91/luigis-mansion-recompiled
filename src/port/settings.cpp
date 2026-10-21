#include "port/settings.h"

#include "port/install.h"
#include "port/recovery.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace port {
namespace {

Settings sSettings;

std::vector<SettingsExtension>& extensions() {
    static std::vector<SettingsExtension> sExtensions;
    return sExtensions;
}

// One ini key bound to a setting, with text conversion both ways.
struct Field {
    const char* section;
    const char* key;
    std::function<std::string()> get;
    std::function<void(const std::string&)> set;
};

template <typename T>
Field num(const char* section, const char* key, T& v) {
    return {section, key, [&v] { std::ostringstream s; s << v; return s.str(); },
            [&v](const std::string& s) {
                std::istringstream in(s);
                T t{};
                if (in >> t) v = t;
            }};
}
Field flag(const char* section, const char* key, bool& v) {
    return {section, key, [&v] { return std::string(v ? "true" : "false"); },
            [&v](const std::string& s) { v = s == "true" || s == "1"; }};
}
Field str(const char* section, const char* key, std::string& v) {
    return {section, key, [&v] { return v; }, [&v](const std::string& s) { v = s; }};
}
template <typename E>
Field enm(const char* section, const char* key, E& v) {
    return {section, key, [&v] { return std::to_string(int(v)); },
            [&v](const std::string& s) { v = E(std::atoi(s.c_str())); }};
}

std::vector<Field> fields() {
    auto& v = sSettings.video;
    auto& a = sSettings.audio;
    auto& e = sSettings.enhancements;
    auto& c = sSettings.crt;
    auto& g = sSettings.gameplay;
    auto& r = sSettings.rendering;
    auto& l = sSettings.enhancedLighting;
    auto& u = sSettings.unused2;
    return {
        num("interface", "menu_scale", sSettings.ui.menuScale),
        num("video", "target_fps", v.targetFps),
        flag("video", "unlimited_fps", v.unlimitedFps),
        flag("video", "match_display_refresh", v.matchDisplay),
        enm("video", "aspect", v.aspect),
        num("video", "resolution_scale", v.resolutionScale),
        num("video", "fov_scale", v.fovScale),
        flag("video", "fullscreen", v.fullscreen),
        flag("video", "vsync", v.vsync),
        num("video", "msaa", v.msaa),
        flag("video", "hud_keep_4x3", v.hudKeep4x3),
        flag("video", "pal60", v.pal60),
        num("video", "vrr", v.vrr),
        flag("rendering", "integer_tev", r.integerTev),
        flag("rendering", "efb_color_depth", r.efbColorDepth),
        flag("rendering", "efb_blend_6bit", r.efbBlend6),
        flag("rendering", "smooth_banding", r.smoothBanding),
        flag("rendering", "early_z_alpha_depth", r.earlyZAlphaDepth),
        flag("rendering", "display_copy_filter", r.displayCopyFilter),
        flag("rendering", "texture_copy_filter", r.textureCopyFilter),
        flag("rendering", "efb_copy_to_ram", sSettings.rendering.efbCopyToRam),
        flag("rendering", "vertex_snapshot", r.vtxSnapshot),
        flag("rendering", "gpu_keep_warm", r.gpuKeepWarm),
        flag("rendering", "shadow_render_resolution", r.shadowRenderResolution),
        flag("rendering", "shadow_widescreen", r.shadowWidescreen),
        flag("rendering", "shadow_any_distance", r.shadowAnyDistance),
        // Enhanced lighting > Ghost lights
        flag("ghost_lights", "enabled", sSettings.ghostLights.enabled),
        num("ghost_lights", "intensity", sSettings.ghostLights.intensity),
        num("ghost_lights", "radius_scale", sSettings.ghostLights.radiusScale),
        flag("ghost_lights", "dramatic", sSettings.ghostLights.dramatic),
        num("ghost_lights", "max_lights_64", sSettings.ghostLights.maxLights),
        flag("ghost_lights", "shadows", sSettings.ghostLights.shadows),
        // Enhanced lighting > World lights
        flag("world_lights", "enabled", sSettings.worldLights.enabled),
        num("world_lights", "intensity", sSettings.worldLights.masterIntensity),
        flag("world_lights", "flicker", sSettings.worldLights.flicker),
        flag("world_lights", "merge_nearby", sSettings.worldLights.mergeNearby),
        flag("world_lights", "candles", sSettings.worldLights.on[port::kWlCandles]),
        num("world_lights", "candles_intensity", sSettings.worldLights.intensity[port::kWlCandles]),
        flag("world_lights", "fires", sSettings.worldLights.on[port::kWlFires]),
        num("world_lights", "fires_intensity", sSettings.worldLights.intensity[port::kWlFires]),
        flag("world_lights", "lamps", sSettings.worldLights.on[port::kWlLamps]),
        num("world_lights", "lamps_intensity", sSettings.worldLights.intensity[port::kWlLamps]),
        flag("world_lights", "sprays", sSettings.worldLights.on[port::kWlSprays]),
        num("world_lights", "sprays_intensity", sSettings.worldLights.intensity[port::kWlSprays]),
        flag("world_lights", "fire_effects", sSettings.worldLights.on[port::kWlFireFx]),
        num("world_lights", "fire_effects_intensity", sSettings.worldLights.intensity[port::kWlFireFx]),
        flag("world_lights", "lightning", sSettings.worldLights.on[port::kWlLightning]),
        num("world_lights", "lightning_intensity", sSettings.worldLights.intensity[port::kWlLightning]),
        flag("world_lights", "sparkles", sSettings.worldLights.on[port::kWlSparkles]),
        num("world_lights", "sparkles_intensity", sSettings.worldLights.intensity[port::kWlSparkles]),
        flag("enhanced_lighting", "enabled", l.enabled),
        enm("enhanced_lighting", "preset", l.preset),
        flag("enhanced_lighting", "per_pixel_lighting", l.perPixelLighting),
        flag("enhanced_lighting", "flashlight_quality", l.flashlightQuality),
        num("enhanced_lighting", "flashlight_softness", l.flashlightSoftness),
        num("enhanced_lighting", "flashlight_specular", l.flashlightSpecular),
        flag("enhanced_lighting", "soft_shadows", l.softShadows),
        num("enhanced_lighting", "soft_shadow_quality", l.softShadowQuality),
        num("enhanced_lighting", "soft_shadow_radius", l.softShadowRadius),
        num("enhanced_lighting", "soft_shadow_penumbra", l.softShadowPenumbra),
        num("enhanced_lighting", "shadow_darkness", l.shadowDarkness),
        num("enhanced_lighting", "shadow_ambient_keep", l.shadowAmbientKeep),
        num("enhanced_lighting", "shadow_map_size", l.shadowMapSize),
        flag("enhanced_lighting", "extra_lights", l.extraLights),
        num("enhanced_lighting", "extra_light_intensity", l.extraLightIntensity),
        num("enhanced_lighting", "extra_light_saturation", l.extraLightSaturation),
        flag("enhanced_lighting", "extra_light_shadows", l.extraLightShadows),
        num("enhanced_lighting", "extra_shadow_quality", l.extraShadowQuality),
        num("enhanced_lighting", "extra_shadow_mode", l.extraShadowMode),
        num("enhanced_lighting", "extra_shadow_lights", l.extraShadowLights),
        num("enhanced_lighting", "extra_shadow_map_size", l.extraShadowMapSize),
        num("enhanced_lighting", "extra_shadow_softness", l.extraShadowSoftness),
        flag("enhanced_lighting", "volumetric_beam", l.volumetricBeam),
        num("enhanced_lighting", "volumetric_intensity", l.volumetricIntensity),
        num("enhanced_lighting", "volumetric_quality", l.volumetricQuality),
        num("enhanced_lighting", "realistic_flashlight", l.realisticFlashlight),
        num("enhanced_lighting", "beam_density", l.beamDensity),
        num("enhanced_lighting", "beam_dust", l.beamDust),
        flag("enhanced_lighting", "bloom", l.bloom),
        num("enhanced_lighting", "bloom_intensity", l.bloomIntensity),
        num("enhanced_lighting", "bloom_threshold", l.bloomThreshold),
        num("enhanced_lighting", "bloom_radius", l.bloomRadius),
        num("enhanced_lighting", "bloom_flashlight_suppress", l.bloomFlashlightSuppress),
        num("enhanced_lighting", "bloom_scene", l.bloomScene),
        num("enhanced_lighting", "bloom_emissive", l.bloomEmissive),
        flag("enhanced_lighting", "ssao", l.ssao),
        num("enhanced_lighting", "ssao_intensity", l.ssaoIntensity),
        num("enhanced_lighting", "ssao_radius", l.ssaoRadius),
        flag("crt", "enabled", c.enabled),
        enm("crt", "preset", c.preset),
        enm("crt", "scanlines", c.scanlines),
        num("crt", "beam_min_sigma", c.beamMinSigma),
        num("crt", "beam_max_sigma", c.beamMaxSigma),
        num("crt", "beam_spot_power", c.beamSpotPower),
        num("crt", "beam_min_shape", c.beamMinShape),
        num("crt", "beam_max_shape", c.beamMaxShape),
        num("crt", "beam_shape_power", c.beamShapePower),
        num("crt", "horizontal_softness", c.horizontalSoftness),
        enm("crt", "mask", c.mask),
        num("crt", "mask_size", c.maskSize),
        num("crt", "mask_strength", c.maskStrength),
        num("crt", "halation", c.halation),
        num("crt", "diffusion", c.diffusion),
        num("crt", "bloom", c.bloom),
        num("crt", "convergence_red_x", c.convergence[0]),
        num("crt", "convergence_red_y", c.convergence[1]),
        num("crt", "convergence_green_x", c.convergence[2]),
        num("crt", "convergence_green_y", c.convergence[3]),
        num("crt", "convergence_blue_x", c.convergence[4]),
        num("crt", "convergence_blue_y", c.convergence[5]),
        num("crt", "gamma_in", c.gammaIn),
        num("crt", "gamma_out", c.gammaOut),
        num("crt", "curvature", c.curvature),
        num("crt", "corner_radius", c.cornerRadius),
        num("crt", "brightness", c.brightness),
        enm("crt", "signal", c.signal),
        enm("crt", "separation", c.separation),
        num("crt", "luma_bandwidth", c.lumaBandwidth),
        num("crt", "chroma_bandwidth", c.chromaBandwidth),
        num("crt", "sharpening", c.sharpening),
        num("crt", "artifacts", c.artifacts),
        num("crt", "saturation", c.saturation),
        num("crt", "noise", c.noise),
        num("crt", "ghost", c.ghost),
        num("crt", "ghost_delay", c.ghostDelay),
        num("crt", "jitter", c.jitter),
        num("crt", "hum", c.hum),
        num("crt", "interference", c.interference),
        num("audio", "master_volume", a.masterVolume),
        num("audio", "music_volume", a.musicVolume),
        num("audio", "sfx_volume", a.sfxVolume),
        enm("audio", "output", a.output),
        flag("audio", "swap_channels", a.swapChannels),
        flag("audio", "reverb", a.reverb),
        flag("audio", "voice_filters", a.voiceFilters),
        flag("audio", "sinc_resampler", a.sincResampler),
        flag("enhancements", "show_fps", e.showFps),
        num("enhancements", "touch_controls", e.touchControls),
        num("enhancements", "touch_scale", e.touchScale),
        num("enhancements", "touch_opacity", e.touchOpacity),
        flag("enhancements", "pause_on_focus_loss", e.pauseOnFocusLoss),
        flag("enhancements", "hidden_mansion_pal", e.hiddenMansionPal),
        num("enhancements", "pal_language", e.palLanguage),
        flag("enhancements", "gbh_camera", e.gbhCamera),
        flag("enhancements", "gbh_tracker", e.gbhTracker),
        flag("enhancements", "gbh_clock", e.gbhClock),
        flag("enhancements", "early_ghost_lighting", e.earlyGhostLighting),
        flag("enhancements", "beta_skybox", e.betaSkybox),
        flag("enhancements", "heart_door", e.heartDoor),
        flag("enhancements", "star_door", e.starDoor),
        flag("enhancements", "beta_map_luigi", e.betaMapLuigi),
        flag("enhancements", "early_luigi", e.earlyLuigi),
        flag("enhancements", "air_water_nozzles", e.airWaterNozzles),
        flag("enhancements", "early_poltergust", e.earlyPoltergust),
        flag("enhancements", "early_bogmire", e.earlyBogmire),
        flag("enhancements", "green_toad", e.greenToad),
        flag("enhancements", "two_player", e.twoPlayer),
        flag("enhancements", "coop_improvements", e.coopImprovements),
        flag("enhancements", "unused_portrait_models", e.unusedPortraitModels),
        flag("enhancements", "skip_cutscenes", e.skipCutscenes),
        flag("enhancements", "fast_forward_key", e.fastForwardKey),
        flag("enhancements", "portal_clip_fix", e.portalClipFix),
        flag("gameplay", "pal_darkness_everywhere", g.palDarknessEverywhere),
        num("gameplay", "ambient_scale", g.ambientScale),
        num("gameplay", "damage_scale", g.damageScale),
        num("gameplay", "health_scale", g.healthScale),
        num("gameplay", "ghost_health_scale", g.ghostHealthScale),
        flag("gameplay", "no_heart_drops", g.noHeartDrops),
        num("gameplay", "vacuum_strength", g.vacuumStrength),
        flag("gameplay", "always_ride", g.alwaysRide),
        flag("debug", "particle_report", e.dbgParticleReport),
        flag("debug", "depth_blur", e.dbgDepthBlur),
        flag("debug", "luigi_debug_flags", e.dbgLuigiFlags),
        flag("debug", "info_display", e.dbgInfoDisplay),
        flag("debug", "scan_boxes", e.dbgScanBoxes),
        flag("debug", "axes", e.dbgAxes),
        num("debug", "hud_test", e.dbgHudTest),
        flag("debug", "font_test", e.dbgFontTest),
        flag("debug", "font_test_latin", e.dbgFontTestLatin),
        flag("debug", "warp_skip_events", e.dbgWarpSkipEvents),
        flag("unused2", "play_as_mario", u.playAsMario),
        flag("unused2", "mario_p2", u.marioP2),
        flag("unused2", "bordered_view", u.borderedView),
        flag("unused2", "poison_state", u.poisonState),
        num("unused2", "poison_damage", u.poisonDamage),
        num("unused2", "poison_seconds", u.poisonSeconds),
        flag("unused2", "crawl_fear", u.crawlFear),
        flag("unused2", "overheat", u.overheat),
        num("unused2", "overheat_fill_seconds", u.overheatFillSeconds),
        num("unused2", "overheat_cool_seconds", u.overheatCoolSeconds),
        num("unused2", "overheat_blow_cool", u.overheatBlowCool),
        num("unused2", "overheat_damage", u.overheatDamage),
        flag("unused2", "ending_pictures", u.endingPictures),
        flag("unused2", "ghost_placements", u.ghostPlacements),
        flag("unused2", "pink_mice", u.pinkMice),
        flag("unused2", "blue_ghost_guys", u.blueGhostGuys),
        flag("unused2", "extra_speedy_spirits", u.extraSpeedySpirits),
        flag("updates", "check_at_startup", sSettings.updates.checkAtStartup),
        str("updates", "skip_version", sSettings.updates.skipVersion),
    };
}

void writeSettings(std::ostream& out);
void logSettings(const char* when);

template <typename T>
void clampTo(T& v, T lo, T hi) {
    if (!(v >= lo)) v = lo;  // (also catches NaN)
    if (v > hi) v = hi;
}
template <typename E>
void clampEnum(E& v, E last, E def = E(0)) {
    if (int(v) < 0 || int(v) > int(last)) v = def;
}

// Values from a hand-edited or damaged settings.ini are brought back into the ranges the menu
// offers: out-of-range enums would select nothing (or index past a table), and e.g. a huge render
// scale or MSAA count fails to create the GPU targets at startup.
void sanitize() {
    auto& v = sSettings.video;
    // Menu size: 0 (Auto) or a percentage (the UI snaps it to its nearest step).
    if (sSettings.ui.menuScale != 0) clampTo(sSettings.ui.menuScale, 75, 300);
    clampTo(v.targetFps, kMinFps, kMaxFps);
    clampEnum(v.aspect, AspectMode::Ultra32x9, AspectMode::Auto);
    clampTo(v.resolutionScale, 0, 8);
    clampTo(v.fovScale, 0.75f, 1.5f);
    v.msaa = v.msaa >= 8 ? 8 : v.msaa >= 4 ? 4 : v.msaa >= 2 ? 2 : 1;
    auto& c = sSettings.crt;
    clampEnum(c.preset, CrtPreset::Custom);
    clampEnum(c.scanlines, CrtScanlines::Interlaced480i);
    clampEnum(c.mask, CrtMask::None);
    clampEnum(c.signal, CrtSignal::Rf);
    clampEnum(c.separation, CrtSeparation::Comb3Line);
    clampTo(c.lumaBandwidth, 1.5f, 6.0f);
    clampTo(c.chromaBandwidth, 0.3f, 2.0f);
    clampTo(c.sharpening, 0.0f, 1.0f);
    clampTo(c.artifacts, 0.0f, 1.0f);
    clampTo(c.saturation, 0.0f, 1.5f);
    clampTo(c.noise, 0.0f, 1.0f);
    clampTo(c.ghost, 0.0f, 0.5f);
    clampTo(c.ghostDelay, 0.2f, 4.0f);
    clampTo(c.jitter, 0.0f, 2.0f);
    clampTo(c.hum, 0.0f, 1.0f);
    clampTo(c.interference, 0.0f, 1.0f);
    auto& a = sSettings.audio;
    clampTo(a.masterVolume, 0.0f, 1.0f);
    clampTo(a.musicVolume, 0.0f, 1.0f);
    clampTo(a.sfxVolume, 0.0f, 1.0f);
    clampEnum(a.output, AudioOutput::Surround71);
    auto& e = sSettings.enhancements;
    clampTo(e.palLanguage, 1, 5);
    clampTo(e.dbgHudTest, 0, 3);
    auto& l = sSettings.enhancedLighting;
    clampEnum(l.preset, EnhancedPreset::Custom, EnhancedPreset::High);
    clampTo(l.softShadowQuality, 0, 3);
    clampTo(l.extraShadowQuality, 0, 2);
    clampTo(l.extraShadowMode, 0, 1);
    clampTo(l.extraShadowLights, 0, 5);
    clampTo(l.extraShadowMapSize, 0, 3);
    clampTo(l.extraShadowSoftness, 0.0f, 60.0f);
    clampTo(l.volumetricQuality, 0, 2);
    clampTo(l.realisticFlashlight, 0, 3);
    clampTo(l.beamDensity, 0.0f, 3.0f);
    clampTo(l.beamDust, 0.0f, 2.0f);
    clampTo(l.shadowMapSize, 0, 2);
    // The ranges of the F1 sliders (Visuals > Enhanced lighting).
    clampTo(l.flashlightSoftness, 0.0f, 1.0f);
    clampTo(l.flashlightSpecular, 0.0f, 2.0f);
    clampTo(l.softShadowRadius, 0.25f, 6.0f);
    clampTo(l.softShadowPenumbra, 0.0f, 20.0f);
    clampTo(l.shadowDarkness, 0.0f, 1.5f);
    clampTo(l.shadowAmbientKeep, 0.0f, 1.0f);
    clampTo(l.extraLightIntensity, 0.0f, 3.0f);
    clampTo(l.extraLightSaturation, 0.0f, 2.5f);
    clampTo(l.volumetricIntensity, 0.0f, 3.0f);
    clampTo(l.bloomIntensity, 0.0f, 2.0f);
    clampTo(l.bloomThreshold, 0.0f, 1.0f);
    clampTo(l.bloomRadius, 0.25f, 2.0f);
    clampTo(l.bloomFlashlightSuppress, 0.0f, 1.0f);
    clampTo(l.bloomScene, 0.0f, 1.0f);
    clampTo(l.bloomEmissive, 0.0f, 2.0f);
    clampTo(l.ssaoIntensity, 0.0f, 2.0f);
    clampTo(l.ssaoRadius, 10.0f, 200.0f);
    auto& gl = sSettings.ghostLights;
    clampTo(gl.intensity, 0.0f, 4.0f);
    clampTo(gl.radiusScale, 0.25f, 3.0f);
    clampTo(gl.maxLights, 1, 64);
    auto& wl = sSettings.worldLights;
    clampTo(wl.masterIntensity, 0.0f, 3.0f);
    for (float& i : wl.intensity) clampTo(i, 0.0f, 3.0f);
    auto& g = sSettings.gameplay;
    clampTo(g.ambientScale, 0.05f, 1.5f);
    clampTo(g.damageScale, 0.0f, 5.0f);
    clampTo(g.healthScale, 0.25f, 5.0f);
    clampTo(g.ghostHealthScale, 0.25f, 5.0f);
    clampTo(g.vacuumStrength, 0.25f, 4.0f);
    auto& u = sSettings.unused2;
    clampTo(u.poisonDamage, 1.0f, 100.0f);
    clampTo(u.poisonSeconds, 1.0f, 20.0f);
    clampTo(u.overheatFillSeconds, 5.0f, 120.0f);
    clampTo(u.overheatCoolSeconds, 5.0f, 120.0f);
    clampTo(u.overheatBlowCool, 1.0f, 10.0f);
    clampTo(u.overheatDamage, 0, 50);
}

}  // namespace

Settings& settings() { return sSettings; }

void addSettingsExtension(const SettingsExtension& ext) { extensions().push_back(ext); }

std::string settingsPath() { return displayPath(configDir() / "settings.ini"); }

namespace {
// The file itself: an extended path, so a deep %APPDATA% works without the long path policy.
std::filesystem::path settingsFile() { return longPath(configDir() / "settings.ini"); }
}  // namespace

bool setSetting(const std::string& section, const std::string& key, const std::string& value) {
    bool known = false;
    for (auto& f : fields())
        if (section == f.section && key == f.key) {
            f.set(value);
            known = true;
        }
    if (known) sanitize();
    return known;
}

void loadSettings() {
    for (auto& ext : extensions()) ext.reset();
    // A settings.ini that is not text (NUL bytes: a write cut off by a crash or power loss on a
    // file system that leaves zero-filled blocks) is put aside as settings.ini.corrupt and the
    // defaults are used (docs/SETUP.md "When the game does not start"). Unknown keys and bad
    // values in a text file are ignored and clamped (sanitize) as before.
    {
        std::ifstream probe(settingsFile(), std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(probe)), std::istreambuf_iterator<char>());
        if (raw.find('\0') != std::string::npos) {
            probe.close();
            std::error_code ec;
            const std::string bad = settingsPath() + ".corrupt";
            std::filesystem::rename(settingsFile(), std::filesystem::path(settingsFile()) += ".corrupt", ec);
            const std::string msg = "[settings] settings.ini is damaged (binary data); moved to " + bad + ", using the defaults";
            std::fprintf(stderr, "%s\n", msg.c_str());
            recovery::logLine(msg.c_str(), msg.size());
            sanitize();
            return;
        }
    }
    std::ifstream in(settingsFile());
    if (!in) return;
    struct LogAfter {
        ~LogAfter() { logSettings("loaded"); }
    } logAfter;
    auto table = fields();
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // UTF-8 BOM (file saved by an editor)
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (section == "video" && key == "frame_rate") {  // before target_fps: 0 = 30, 1 = 60, 2 = unlimited
            const int old = std::atoi(value.c_str());
            sSettings.video.targetFps = old == 1 ? 60 : 30;
            sSettings.video.matchDisplay = old == 2;
            continue;
        }
        bool known = false;
        for (auto& f : table)
            if (section == f.section && key == f.key) {
                f.set(value);
                known = true;
            }
        if (!known)
            for (auto& ext : extensions()) ext.load(section, key, value);
    }
    sanitize();
}

void resetSettingsToDefaults() {
    sSettings = Settings{};
    for (auto& ext : extensions()) ext.reset();
    sanitize();
    saveSettings();
}

namespace {
// Shared logs (Android "Share log") should say which settings a run used: the video and
// rendering sections and the enhanced-lighting master switch, as written to settings.ini.
void logSettings(const char* when) {
    std::ostringstream all;
    writeSettings(all);
    std::istringstream in(all.str());
    std::string line, section, out;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
            section = line;
            continue;
        }
        if (section == "[video]" || section == "[rendering]" ||
            (section == "[enhanced_lighting]" && line.rfind("enabled=", 0) == 0))
            out += " " + section.substr(1, section.size() - 2) + "." + line;
    }
    std::printf("[settings] %s:%s\n", when, out.c_str());
    std::fflush(stdout);
}
}  // namespace

void saveSettings() {
    logSettings("saved");
    // Written to a temporary file and renamed over settings.ini, so a crash or power loss while
    // saving never leaves a truncated file (which would reset every setting on the next launch).
    const std::filesystem::path path = settingsFile(), tmp = std::filesystem::path(path) += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        writeSettings(out);
        out.flush();
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {  // e.g. settings.ini held open by another program: write it in place instead
        std::filesystem::remove(tmp, ec);
        std::ofstream out(path, std::ios::trunc);
        if (out) writeSettings(out);
    }
}

namespace {
void writeSettings(std::ostream& out) {
    std::string section;
    for (auto& f : fields()) {
        if (section != f.section) {
            section = f.section;
            out << '[' << section << "]\n";
        }
        out << f.key << '=' << f.get() << '\n';
    }
    for (auto& ext : extensions()) ext.save(out);
}
}  // namespace

}  // namespace port
