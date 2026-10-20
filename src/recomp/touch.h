#pragma once
// On-screen touch controls (Android; docs/ANDROID.md): a GameCube pad drawn over the game
// (floating main stick, C-stick, A/B/X/Y, L/R/Z, START, D-pad) and a menu button, since a
// phone has no F1 key. Their input is merged into controller port 1 in PADRead. Shown while
// no physical controller is in use ("Auto"), always, or never (F1 > Controls > Touch).
union SDL_Event;
struct PADStatus;

namespace recomp::touch {

// Host event loop: finger events. True when the event was used by the touch controls.
bool handleEvent(const SDL_Event& ev);
// Host thread, inside an Aurora frame, after the menu: draws the controls.
void draw();
// PADRead (guest thread): adds the touch pad to port 1.
void apply(PADStatus* status);
// F1 > Controls > Touch.
bool drawSettings();
// Built for a touch platform (Android) at all.
bool supported();

}  // namespace recomp::touch
