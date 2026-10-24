#pragma once
// User settings, edited in the in-game overlay menu (F1) and saved to
// %APPDATA%\Luigi's Mansion Recompiled\settings.ini. Read them wherever they apply;
// changes take effect immediately unless noted.
#include <iosfwd>
#include <string>

namespace port {

enum class AspectMode { Original4x3, Auto, Wide16x9, Wide16x10, Ultra21x9, Ultra32x9 };
// Sound output. Stereo / Mono / Surround set the game's own sound mode (its options menu
// shows and changes the same thing); Surround plays the console's matrix-encoded stereo.
// Surround51 / Surround71 run the game in surround mode and play its centre and surround
// buses on their own speakers (when the device has them; else like Surround).
enum class AudioOutput { Stereo, Mono, Surround, Surround51, Surround71 };

constexpr int kMinFps = 30, kMaxFps = 320;

struct VideoSettings {
    // Presentation frame rate. 30 = the game's own rate, drawn directly (no interpolation);
    // above 30 the frames in between are interpolated (src/recomp/interp.*). matchDisplay
    // uses the refresh rate of the display the window is on instead of targetFps.
    int targetFps = 30;         // kMinFps..kMaxFps
    bool unlimitedFps = false;  // interpolated, as fast as the PC can (no frame-rate cap or schedule)
    bool matchDisplay = false;
    AspectMode aspect = AspectMode::Auto;
#ifdef __ANDROID__
    int resolutionScale = 2;    // phones: 2 x 480p by default (a phone's full resolution costs battery and heat)
#else
    int resolutionScale = 0;    // internal render resolution: 0 = match window, N = N x 480p (aspect-widened)
#endif
    float fovScale = 1.0f;      // multiplies the game's vertical field of view
    bool fullscreen = false;
    bool vsync = true;          // applied at next launch
    int msaa = 1;               // 1, 2, 4, 8; applied at next launch
    bool hudKeep4x3 = true;     // keep 2D/HUD at 4:3 (centered) in widescreen
    bool pal60 = false;         // PAL game only: 59.94 Hz retrace (PAL60) instead of 50 Hz (game speed unchanged)
    // Interpolated presentation on a variable refresh rate display (G-SYNC / FreeSync): 0 auto
    // (detected from the display's refresh timing), 1 on, 2 off (frame.cpp, docs/INTERPOLATION.md).
    int vrr = 0;
};

// CRT post-process filter (src/recomp/crt.cpp). Beam sigmas are fractions of the
// scanline spacing, convergence offsets are in game pixels / lines, mask size in
// output pixels, corner radius a fraction of the screen height.
enum class CrtPreset { ApertureGrille, SlotMask, ShadowMask, ConsumerTv, Custom };
enum class CrtScanlines { Progressive480, DoubleStrike240, Interlaced480i };
enum class CrtMask { ApertureGrille, SlotMask, ShadowMask, None };
// Video signal between the console and the tube (crt.cpp "signal path"): RGB is the clean
// picture; the others model the NTSC encoder, the cable / RF channel and the TV's decoder.
enum class CrtSignal { Rgb, SVideo, Composite, Rf };
// How the TV splits a composite signal into luma and chroma.
enum class CrtSeparation { Notch, Comb2Line, Comb3Line };

struct CrtSettings {
    bool enabled = false;
    CrtPreset preset = CrtPreset::ApertureGrille;
    CrtScanlines scanlines = CrtScanlines::Progressive480;
    float beamMinSigma = 0.20f, beamMaxSigma = 0.36f, beamSpotPower = 0.6f;
    float beamMinShape = 2.0f, beamMaxShape = 3.0f, beamShapePower = 0.5f;
    float horizontalSoftness = 0.45f;
    CrtMask mask = CrtMask::ApertureGrille;
    float maskSize = 3.0f, maskStrength = 0.75f;
    float halation = 0.04f, diffusion = 0.06f, bloom = 0.15f;
    float convergence[6] = {0, 0, 0, 0, 0, 0};  // R x,y  G x,y  B x,y
    float gammaIn = 2.4f, gammaOut = 2.2f;
    float curvature = 0.0f, cornerRadius = 0.02f;
    float brightness = 1.2f;
    // Signal path. Bandwidths in MHz of the NTSC baseband; crt::applySignal sets the signal's
    // defaults when a signal is chosen (applying a tube preset keeps them).
    CrtSignal signal = CrtSignal::Rgb;
    CrtSeparation separation = CrtSeparation::Notch;
    float lumaBandwidth = 4.2f;    // TV luma channel
    float chromaBandwidth = 1.0f;  // TV chroma demodulator low-pass
    float sharpening = 0.25f;      // TV luma peaking
    float artifacts = 1.0f;        // Y/C crosstalk (dot crawl, cross-colour): 1 = as modelled, 0 = none
    float saturation = 0.92f;      // TV colour control
    float noise = 0.0f;            // RF snow
    float ghost = 0.0f, ghostDelay = 1.0f;  // multipath echo: gain, delay in microseconds
    float jitter = 0.0f;           // RF line jitter, game pixels
    float hum = 0.0f;              // rolling hum bar depth
    float interference = 0.0f;     // beat with an interfering carrier (diagonal bars)
};

struct AudioSettings {
    float masterVolume = 1.0f;  // 0..1
    float musicVolume = 1.0f;   // streamed/sequenced music
    float sfxVolume = 1.0f;     // everything else
    AudioOutput output = AudioOutput::Stereo;
    bool swapChannels = false;
    bool reverb = true;        // DSP effect lines (echo/reverb) and the chorus bus
    bool voiceFilters = true;  // per-voice FIR / IIR / distortion stages of the DSP microcode
    bool sincResampler = true; // windowed-sinc 32 kHz -> device rate (off: SDL's resampler)
};

struct EnhancementSettings {
    bool showFps = false;
    int touchControls = 1;      // Android on-screen pad: 0 off, 1 auto (hidden with a controller), 2 always
    float touchScale = 1.0f;    // its size, 0.6..1.6
    float touchOpacity = 0.5f;  // 0.1..1
    bool pauseOnFocusLoss = true;
    bool hiddenMansionPal = true;  // default Hidden Mansion rules for saves that have not chosen in E. Gadd's lab
    int palLanguage = 1;           // PAL game only: 1 English, 2 German, 3 French, 4 Spanish, 5 Italian (next launch)
    bool gbhClock = false;         // cut E3 GBH clock/radar screen, toggled in game with D-pad Left
    bool gbhTracker = false;       // unused GBH ghost tracker, cycled in game with D-pad Down (off/far/close)
    bool gbhCamera = false;        // unused Game Boy Horror live camera, toggled in game with D-pad Up (docs/UNUSED_CONTENT.md)
    bool betaSkybox = false;            // unused sky in the game archive (iwamoto/vrbox) on map 2
    bool heartDoor = false;             // unused heart door (type 07) on Chauncey's room
    bool starDoor = false;              // unused star door (type 08) on the Twins' room
    bool betaMapLuigi = false;          // unused full Luigi model on the Game Boy Horror map
    bool unusedPortraitModels = false;  // each ghost's own model in the Portrificationizer scene
    bool earlyLuigi = false;            // unused early Luigi model /model/luige.szp (src/recomp/early_luigi.cpp)
    bool airWaterNozzles = false;       // unused wpair/wpwater Poltergust nozzles (src/recomp/nozzles.cpp)
    bool earlyPoltergust = false;       // E3 2001 Poltergust body /model/vbody.szp (src/recomp/unused_models.cpp)
    bool earlyBogmire = false;          // early Bogmire model /model/shadow.szp (src/recomp/unused_models.cpp)
    bool greenToad = false;             // Toads use kinopio2's green texture set (src/recomp/unused_models.cpp)
    bool twoPlayer = false;             // unused 2-player mode, P2 on controller port 2 (src/recomp/coop.cpp)
    bool coopImprovements = false;      // co-op fixes on top of the 2-player mode (src/recomp/coop_fixes.cpp)
    bool earlyGhostLighting = false;  // "Ghost shading: Beta" = the known beta-shading cheat (fn_800581E8 channel setup)
    bool skipCutscenes = false;  // START skips cutscenes (src/recomp/skip.cpp)
    bool fastForwardKey = true;  // holding Tab runs the game at high speed (src/recomp/skip.cpp)
    bool portalClipFix = true;   // rooms behind a doorway near the camera don't vanish (src/recomp/portal_fix.cpp)
    // Debug: the game's own compiled-in debug switches (src/recomp/dev_tools.cpp)
    bool dbgParticleReport = false;  // particle manager JUTReport text
    bool dbgDepthBlur = false;       // unused depth blur screen pass
    bool dbgLuigiFlags = false;      // "Debug Mode" AR code: Luigi debug flags 0xCB
    bool dbgInfoDisplay = false;     // Luigi's debug text (dust, aura, water, heart)
    bool dbgScanBoxes = false;       // Game Boy Horror scan boxes
    bool dbgAxes = false;            // XYZ axes at Luigi
    int dbgHudTest = 0;              // 0 off, 1 icons, 2 numbers, 3 element meter
    bool dbgFontTest = false;        // font test screen text
    bool dbgFontTestLatin = false;   // PAL fix: last font-test line shows "alphabet 123"
    bool dbgWarpSkipEvents = true;   // warps don't start the destination's entry cutscene/event
};

// Gameplay / difficulty options (F1 > Enhancements > Gameplay). Defaults are neutral: the
// original game is unchanged until one is moved. They apply in every mansion and variant
// (docs/GAMEPLAY_OPTIONS.md).
struct GameplaySettings {
    bool palDarknessEverywhere = false;  // PAL Hidden Mansion lighting (ambient x0.2) in every mansion
    float ambientScale = 1.0f;           // room ambient light multiplier, 0.05..1.5 (composes with the above)
    float damageScale = 1.0f;            // damage Luigi takes, 0..5
    float healthScale = 1.0f;            // Luigi's starting and maximum health, 0.25..5
    float ghostHealthScale = 1.0f;       // ghost HP, 0.25..5
    bool noHeartDrops = false;           // recovery hearts never appear (and any that do heal nothing)
    float vacuumStrength = 1.0f;         // Poltergust drain rate on ghost HP, 0.25..4
    bool alwaysRide = false;             // Luigi always uses the Poltergust-riding movement (PAL Boolossus mode)
    bool mirrorWorld = false;            // the European Hidden Mansion's left/right mirror in every mode (hm::mirrorWorld)
};

// Hardware-accuracy rendering features (F1 > Visuals > Rendering). Every costly accuracy
// feature gets a flag here, ON by default (best quality); weaker PCs can turn them off.
// Costly accuracy features are on by default on PC and off on phones (docs/ANDROID.md: on a
// phone GPU they cost tens of milliseconds per frame); F1 > Visuals > Rendering turns them on.
#ifdef __ANDROID__
constexpr bool kPcDefault = false;
#else
constexpr bool kPcDefault = true;
#endif
struct RenderingSettings {
    // Integer TEV: the color combiner computes with the hardware's 8-bit inputs, signed 11-bit
    // registers and per-stage rounding instead of floats (Aurora GX_AURORA_ACCURACY_INT_TEV).
    bool integerTev = kPcDefault;
    // EFB color depth: pixels are stored the way the game's RGBA6 framebuffer stores them,
    // 6 bits per channel with the ordered dither the game enables (GX_AURORA_ACCURACY_EFB_DEPTH).
    bool efbColorDepth = kPcDefault;
    // 6-bit blending: with efbColorDepth, blended draws (the flashlight beam, glows, HUD edges)
    // are stored like the console's RGBA6 framebuffer stores them, the blend result dithered and
    // truncated to 6 bits (GX_AURORA_ACCURACY_EFB_BLEND, Aurora 0176).
    bool efbBlend6 = kPcDefault;
    // Smooth color banding (enhancement, OFF = vanilla-accurate): with efbColorDepth, the 6-bit
    // framebuffer and the game's RGB565 screen copy store the local average of the console's
    // 6/5-bit result instead of its steps, so gradients keep the same brightness and tint without
    // contour bands or dither grain, plus a sub-step noise so the 8-bit target does not band either
    // (Aurora 0190, GX_AURORA_ACCURACY_EFB_SMOOTH + _NOISE). Forced on while enhanced lighting is on
    // (its per-pixel gradients and bloom band badly in 6 bits).
    bool smoothBanding = false;
    // Early Z writes depth for alpha-tested pixels: draws that compare Z before texturing
    // (GXSetZCompLoc(GX_TRUE)) update Z even where the alpha test then rejects the pixel, as the
    // hardware does (GX_AURORA_ACCURACY_EARLY_Z, Aurora 0170/0179: depth-only pass + colour pass).
    bool earlyZAlphaDepth = kPcDefault;
    // Display copy filter: the vertical filter (and gamma) GXCopyDisp applies on the way to the TV.
    bool displayCopyFilter = true;
    // Texture copy filter: the same vertical filter applied to the game's GXCopyTex copies (the
    // full-screen RGB565 copy it redraws every frame, the box-filtered half-size copies...), as on
    // the console (GX_AURORA_ACCURACY_COPY_FILTER).
    bool textureCopyFilter = true;
    // GXCopyTex results also exist as GX-format bytes in main memory, so textures that read a
    // copy with another format/size (the shadow depth map) and CPU reads see what the
    // console sees (docs/RENDERING.md). Off: copies stay GPU-only textures.
    bool efbCopyToRam = kPcDefault;
    // Vertex data each draw indexes is copied when the game issues the draw (as the console's GPU
    // reads it then), not when Aurora's FIFO thread gets to it; fixes Luigi's rare mouth/hose
    // flicker (CPU/GPU timing race; docs/RENDERING.md "Vertex snapshots"). Interpolated frame
    // rates snapshot anyway.
    bool vtxSnapshot = true;
    // Keep the GPU clocks up when a frame is much lighter than play (menus over a still picture):
    // filler GPU work so the first frames after it are not 5-7x slower (pacing.cpp, Aurora 0215).
    bool gpuKeepWarm = kPcDefault;
    // Shadows (docs/RENDERING.md, "Shadows"). The game's shadow map (light's-view depth, copied
    // as Z16 and compared in the TEV) at the internal render resolution instead of 640x480.
    bool shadowRenderResolution = kPcDefault;
    // Widescreen: the shadow light's frustum is widened by the same factor as the view, so
    // shadows reach the extra side areas (the game only covers what a 4:3 screen shows).
    bool shadowWidescreen = true;
    // The game's shadow light only covers a depth band around Luigi (e.g. 619..2322 units from the
    // room light), so shadows vanish on things farther away. On: the band is extended to what the
    // player's view shows (enhancement). Off: the original range.
    bool shadowAnyDistance = kPcDefault;
    // Custom texture packs (Dolphin tex1_ names) from <data>/textures (src/recomp/texture_pack.cpp).
    bool customTextures = true;
};

// ---- Enhanced lighting > Ghost lights (src/recomp/ghost_lights.cpp; docs/RENDERING.md) ----
// Every visible ghost lights the room around it in its own glow colour. Part of the optional
// "Enhanced lighting" group: on by default, so it comes on with the Enhanced lighting master
// switch (itself OFF by default: the original look stays one click away).
struct GhostLightSettings {
    bool enabled = true;
    float intensity = 1.0f;     // 0..4, multiplies every ghost light
    float radiusScale = 1.0f;   // 0.25..3, multiplies the per-ghost-size radius
    bool dramatic = true;       // pulse, flare on appearing/attacking, flicker when stunned, vacuum struggle, capture burst
    int maxLights = 48;         // 1..64, strongest/nearest visible ghosts first (shared 64-light budget)
    bool shadows = true;        // the 1-2 strongest ghost lights cast shadows (if the renderer supports it)
};

// ---- Enhanced lighting > World lights (src/recomp/world_lights.cpp; docs/RENDERING.md) ----
// Candles, fireplaces, lamps, element sprays, fire, lightning and sparkles as coloured lights.
// Only with the Enhanced lighting master switch (and its "Dynamic colored lights" part).
enum WorldLightCategory {
    kWlCandles,     // candles and candelabras
    kWlFires,       // fireplaces, torches
    kWlLamps,       // lamps, chandeliers, lit-room lights
    kWlSprays,      // Poltergust fire/water/ice spray
    kWlFireFx,      // fireballs, burning objects/characters, fire ghosts
    kWlLightning,   // lightning through the windows
    kWlSparkles,    // treasure, coins, keys, Boo and pickup glows
    kWlCount
};
struct WorldLightSettings {
    bool enabled = true;
    bool on[kWlCount] = {true, true, true, true, true, true, true};
    float intensity[kWlCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};  // 0..3 each
    float masterIntensity = 1.0f;  // 0..3
    bool flicker = true;           // natural flame flicker (off: steady flames)
    bool mergeNearby = true;       // nearby flames of one kind become one light (a candelabra = one light)
};

// Enhanced lighting (F1 > Visuals > Enhanced lighting; docs/RENDERING.md "Enhanced lighting"):
// effects layered on top of the accurate GX rendering of world geometry. Everything is gated by
// `enabled` (OFF by default: the original look stays one click away; off renders exactly what
// the plain renderer does). Each part has its own switch and strength/quality controls; the
// preset fills in the quality fields (Custom keeps them).
enum class EnhancedPreset { Low, Medium, High, Ultra, Custom };

struct EnhancedLightingSettings {
    bool enabled = false;
    EnhancedPreset preset = EnhancedPreset::High;
    // The game's own GX lights (room lights, flashlight, specular) evaluated per pixel.
    bool perPixelLighting = true;
    // Flashlight cone per pixel with a smooth penumbra.
    bool flashlightQuality = true;
    float flashlightSoftness = 0.6f;  // 0..1
    float flashlightSpecular = 0.6f;  // 0..2: glossy highlight where the beam hits (per pixel)
    // Soft shadows for the game's shadow map (PCF / PCSS contact hardening).
    bool softShadows = true;
    int softShadowQuality = 2;        // 0 PCF 8, 1 PCF 16, 2 PCSS 16, 3 PCSS 32
    float softShadowRadius = 1.0f;    // filter radius (640x480 shadow-map texels)
    float softShadowPenumbra = 6.0f;  // PCSS penumbra growth
    // Shadow look: the game darkens shadowed pixels toward black whatever lights them. With these
    // (enhanced shaders only) colored lights keep lighting the shadow, part of the ambient light is
    // kept (shadows pick up the room's color), and the darkness is scaled. 1 / 0 = the game's look.
    float shadowDarkness = 0.9f;      // 0..1.5
    float shadowAmbientKeep = 0.25f;  // 0..1 ("Colored shadow tint")
    int shadowMapSize = 0;            // 0 render resolution, 1 2048x1536, 2 4096x3072 (offscreen)
    // Dynamic colored lights from game objects (ghosts) and test lights.
    bool extraLights = true;
    float extraLightIntensity = 1.0f;  // 0..3
    float extraLightSaturation = 1.6f; // 0..2.5: color vibrance of all colored lights (1 = as the source gives it)
    bool extraLightShadows = true;     // shadows of lights flagged to cast them (ghost lights: the strongest two)
    int extraShadowQuality = 1;        // 0 low, 1 medium, 2 high (ray-march steps / shadow-map filter taps)
    int extraShadowMode = 1;           // 0 screen-space (cheap), 1 shadow maps (cube maps per light)
    int extraShadowLights = 2;         // 0..5: 0 / 2 / 4 / 8 / 12 / 16 shadow-casting lights per frame
    int extraShadowMapSize = 1;        // 0..3: 256 / 512 / 1024 / 2048 texels per cube face
    float extraShadowSoftness = 14.0f; // light size (world units) for soft contact-hardening edges; 0 = hard
    // Volumetric haze in the flashlight beam.
    bool volumetricBeam = true;
    float volumetricIntensity = 2.0f;  // 0..3
    int volumetricQuality = 1;         // 0 low, 1 medium, 2 high
    // Realistic flashlight (Aurora 0196; replaces the volumetric beam while on): light scattering
    // through the air against the flashlight's shadow map (shadow shafts), reflector-shaped
    // footprint with inverse-square falloff, dust motes, the game's cone mesh hidden.
    int realisticFlashlight = 2;       // 0 off, 1 medium, 2 high, 3 ultra
    float beamDensity = 1.0f;          // 0..3: haze in the air
    float beamDust = 1.0f;             // 0..2: dust motes in the beam
    // Glow around bright light sources (HUD excluded).
    bool bloom = true;
    float bloomIntensity = 0.5f;       // 0..2
    float bloomThreshold = 0.8f;       // 0..1
    float bloomRadius = 1.0f;          // 0.25..2
    float bloomFlashlightSuppress = 0.6f;  // 0..1: less glow from walls lit by the flashlight
    float bloomScene = 0.12f;          // 0..1: glow of ordinary bright surfaces (above the threshold)
    float bloomEmissive = 0.6f;        // 0..2: glow of light-emitting effects (glows, fire, beams, sparks)
    // Screen-space ambient occlusion (contact darkening in corners and under furniture;
    // horizon-based since Aurora 0224). On by default, so it comes on with the master switch.
    bool ssao = true;
    float ssaoIntensity = 0.4f;        // 0..2
    float ssaoRadius = 60.0f;          // world units
};

// Second batch of restored unused content (src/recomp/unused2.*, docs/UNUSED_CONTENT.md
// "Second batch"). Everything OFF by default: retail is unchanged until an option is turned on.
struct Unused2Settings {
    bool playAsMario = false;      // the player uses the unused bmario model (play_as_mario.cpp)
    bool marioP2 = false;          // 2-player mode: player 2 is Mario
    bool borderedView = false;     // fixed 560x430 bordered 3D view (hidden code at 0x80010FD4)
    bool poisonState = false;      // poison mushroom poisons (doku_* animations, slow health loss) instead of shrinking
    float poisonDamage = 25.0f;    // total health lost, 1..100
    float poisonSeconds = 5.0f;    // over this many seconds, 1..20
    bool crawlFear = false;        // scared from behind -> the unused crawling-fear state
    bool overheat = false;         // vacuum overheat meter (cut feature)
    float overheatFillSeconds = 20.0f;  // continuous vacuuming until the Poltergust overheats, 5..120
    float overheatCoolSeconds = 30.0f;  // full meter back to empty while idle, 5..120
    float overheatBlowCool = 3.0f;      // cooling speed multiplier while blowing air (L), 1..10
    int overheatDamage = 10;            // health lost when it catches fire, 0..50
    bool endingPictures = false;   // unused Luigi pictures (luigi_V/g/b) on the ending rank screen
    bool ghostPlacements = false;  // unused ghost placements (Slim in the Hidden Room, Mr. Bones in the Courtyard, ...)
    bool pinkMice = false;         // mice use the unused pink variant (rat2)
    bool blueGhostGuys = false;    // Ghost Guys use the unused blue/cyan colour sets (heypo9/heypo10 .prm)
    bool extraSpeedySpirits = false;  // Speedy Spirits 16-20 at invented spots (NOT original placements)
};

// F1 > Enhancements > Silly: world options (src/recomp/silly_world.cpp; docs/ENHANCEMENTS.md
// "Silly options: world"). Cosmetic fun; every default is the retail game.
struct SillyWorldSettings {
    float ghostSize = 1.0f;        // ghost size (whole model, visual only), 0.5..3 (1 = off)
    int ghostSizeTargets = 0;      // 0 all ghosts, 1 Boos only
    bool ghostColorShuffle = false;  // every ghost gets its own random body/glow colour
    bool disco = false;            // room lights cycle through colours
    float discoSpeed = 1.0f;       // colour cycles per 10 s, 0.1..5
    float luigiVoicePitch = 1.0f;  // Luigi's voice pitch, 0.5..2 (1 = off)
    bool moonGravity = false;      // spilled treasure falls slowly, bounces higher
    float moonGravityScale = 0.25f;  // gravity multiplier while on, 0.05..1
    bool confettiCoins = false;    // money bursts throw confetti sparkles
};

// F1 > Enhancements > Silly: Luigi options (src/recomp/silly.cpp, docs/ENHANCEMENTS.md "Silly").
// Visual only; the defaults are the retail look.
enum SillyPiece { kSillyHat, kSillyShirt, kSillyOveralls, kSillyShoes, kSillyPoltergust, kSillyPieceCount };
struct SillyLuigiSettings {
    float headScale = 1.0f;       // Luigi's head (and hat, face) about the neck, 0.25..3
    float bodyScale = 1.0f;       // whole model about his feet, 0.25..3 (camera/collision unchanged)
    bool wobble = false;          // head on a spring driven by his movement
    float wobbleStrength = 1.0f;  // 0.25..3
    bool paper = false;           // flat along his facing axis (Paper Mario)
    bool colorOn[kSillyPieceCount] = {};
    unsigned colorRgb[kSillyPieceCount] = {0x3CA03C, 0x3CA03C, 0x2C3C8C, 0x6A3A1E, 0xD82020};  // 0xRRGGBB
};

// Self-updater (F1 > Game > Updates; src/port/update.cpp, docs/SETUP.md "Updates").
struct UpdateSettings {
    bool checkAtStartup = true;  // look for a newer GitHub release in the background at startup
    std::string skipVersion;     // release tag the player chose "Skip this version" for ("v1.2")
};

// Size of the F1 menu and the port's other overlays (src/recomp/ui_scale.cpp; issue #20).
struct InterfaceSettings {
    int menuScale = 0;  // percent (75..300, ui::kSteps); 0 = Auto (window height / display scaling)
    bool startHint = true;  // "Press F1 for options" notice for the first seconds of play (menu.cpp)
};

struct Settings {
    InterfaceSettings ui;  // (not "interface": a macro in the Windows headers)
    VideoSettings video;
    RenderingSettings rendering;
    GhostLightSettings ghostLights;
    WorldLightSettings worldLights;
    EnhancedLightingSettings enhancedLighting;
    CrtSettings crt;
    AudioSettings audio;
    EnhancementSettings enhancements;
    GameplaySettings gameplay;
    Unused2Settings unused2;
    SillyWorldSettings sillyWorld;
    SillyLuigiSettings sillyLuigi;
    UpdateSettings updates;
};

Settings& settings();
void loadSettings();
void saveSettings();
// Every setting (the table and all extensions, e.g. controller bindings) back to its first-install
// default, then saved.
void resetSettingsToDefaults();
std::string settingsPath();
// Sets one ini key of the table (as if read from settings.ini, then sanitized); false when the
// key is unknown. Not saved. Used by the LM_SETTINGS_AT test aid (menu.cpp).
bool setSetting(const std::string& section, const std::string& key, const std::string& value);

// Ini sections owned by another module (e.g. controller bindings, src/recomp/controls.cpp).
// reset() runs before every load, load() gets each key the table above does not know,
// save() appends the module's own sections. Register from a static initializer.
struct SettingsExtension {
    void (*reset)();
    void (*load)(const std::string& section, const std::string& key, const std::string& value);
    void (*save)(std::ostream& out);
};
void addSettingsExtension(const SettingsExtension& ext);

}  // namespace port
