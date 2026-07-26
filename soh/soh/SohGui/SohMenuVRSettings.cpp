#include "SohMenu.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <imgui.h>
#include <vr_interface.h>
#include <fast/vr_openxr.h>

namespace SohGui {

extern std::shared_ptr<SohMenu> mSohMenu;
using namespace UIWidgets;

static const std::map<int32_t, const char*> vrMirrorAxisOptions = {
    { 0, "X (finger axis)" },
    { 1, "Y" },
    { 2, "Z (thumb axis)" },
};

static const std::map<int32_t, const char*> vrViewModeOptions = {
    { 0, "Third Person" },
    { 1, "First Person" },
};

static const std::map<int32_t, const char*> vrHudAttachOptions = {
    { 0, "Head (Floating)" },
    { 1, "Left Hand" },
    { 2, "Right Hand" },
};

// --- VR Inputs: N64-button-first binding editor (styled after the base game's bindings window:
// colored N64 chip per row, removable chips for each bound VR input, "+" to add). All state lives
// in the gVrBind* mask CVars that padmgr.c reads.
struct VrInputDef {
    const char* label;
    const char* cvar;
    int32_t defaultMask;
};
static const VrInputDef sVrInputDefs[] = {
    { "L Trigger", "gVrBindLTrigger", BTN_Z },      { "L Grip", "gVrBindLGrip", BTN_R },
    { "X", "gVrBindLPrimary", BTN_CLEFT },          { "Y", "gVrBindLSecondary", BTN_CRIGHT },
    { "L Stick", "gVrBindLStickClick", 0 },         { "L Menu", "gVrBindLMenu", BTN_START },
    { "R Trigger", "gVrBindRTrigger", BTN_B },      { "R Grip", "gVrBindRGrip", 0 },
    { "A", "gVrBindRPrimary", BTN_A },              { "B", "gVrBindRSecondary", BTN_CDOWN },
    { "R Stick", "gVrBindRStickClick", 0 },         { "R Menu", "gVrBindRMenu", 0 },
};

struct VrN64RowDef {
    const char* label;
    uint16_t mask;
    ImVec4 color;
};

// Which N64 button row is currently listening for a physical VR press (0 = none), and the
// previous frame's controller state per hand for rising-edge detection (so a button already held
// when listening starts doesn't instantly bind).
static uint16_t sVrListenRowMask = 0;
static uint16_t sVrListenPrevBtn[2] = { 0, 0 };

static void VrInputBindingRow(const VrN64RowDef& row) {
    ImGui::PushID(row.label);

    // The N64 button chip (colored, inert — it's a label).
    ImGui::PushStyleColor(ImGuiCol_Button, row.color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, row.color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, row.color);
    ImGui::Button(row.label, ImVec2(76.0f, 0.0f));
    ImGui::PopStyleColor(3);

    // One removable chip per VR input currently bound to this button.
    for (const VrInputDef& input : sVrInputDefs) {
        int32_t cur = CVarGetInteger(input.cvar, input.defaultMask);
        if (cur & row.mask) {
            ImGui::SameLine();
            ImGui::PushID(input.cvar);
            char chip[48];
            snprintf(chip, sizeof(chip), "%s %s x", ICON_FA_GAMEPAD, input.label);
            if (ImGui::SmallButton(chip)) {
                CVarSetInteger(input.cvar, cur & ~row.mask);
                CVarSave();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove this binding");
            }
            ImGui::PopID();
        }
    }

    ImGui::SameLine();
    const bool listening = (sVrListenRowMask == row.mask);
    if (listening) {
        // Listening: press any input on either VR controller to bind it to this row.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.48f, 0.78f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.55f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.30f, 0.55f, 1.0f));
        if (ImGui::SmallButton("Press a VR input... (click or Esc to cancel)")) {
            sVrListenRowMask = 0;
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            sVrListenRowMask = 0;
        }
        static const uint16_t sVrBtnBits[6] = { VR_BTN_TRIGGER,   VR_BTN_GRIP,       VR_BTN_PRIMARY,
                                                VR_BTN_SECONDARY, VR_BTN_THUMBCLICK, VR_BTN_MENU };
        for (int hand = 0; hand < 2 && sVrListenRowMask != 0; hand++) {
            uint16_t curBtn = VR_GetControllerButton(hand);
            uint16_t pressed = curBtn & ~sVrListenPrevBtn[hand];
            sVrListenPrevBtn[hand] = curBtn;
            for (int b = 0; b < 6; b++) {
                if (pressed & sVrBtnBits[b]) {
                    const VrInputDef& input = sVrInputDefs[hand * 6 + b];
                    CVarSetInteger(input.cvar, CVarGetInteger(input.cvar, input.defaultMask) | row.mask);
                    CVarSave();
                    sVrListenRowMask = 0;
                    break;
                }
            }
        }
    } else {
        // Blue "+": in VR, listen for a physical press; outside VR, fall back to a picker list.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.38f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.48f, 0.78f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.30f, 0.55f, 1.0f));
        if (ImGui::SmallButton("+")) {
            if (VR_IsInitialized()) {
                sVrListenRowMask = row.mask;
                sVrListenPrevBtn[0] = VR_GetControllerButton(0);
                sVrListenPrevBtn[1] = VR_GetControllerButton(1);
            } else {
                ImGui::OpenPopup("VrAddBinding");
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(VR_IsInitialized() ? "Then press the VR controller input to bind"
                                                 : "Pick a VR input to bind (VR not active)");
        }
        if (ImGui::BeginPopup("VrAddBinding")) {
            for (const VrInputDef& input : sVrInputDefs) {
                int32_t cur = CVarGetInteger(input.cvar, input.defaultMask);
                if (!(cur & row.mask)) {
                    if (ImGui::MenuItem(input.label)) {
                        CVarSetInteger(input.cvar, cur | row.mask);
                        CVarSave();
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
}

static void VrInputBindings(WidgetInfo& info) {
    static const VrN64RowDef sButtonRows[] = {
        { "A", BTN_A, ImVec4(0.22f, 0.24f, 0.50f, 1.0f) },
        { "B", BTN_B, ImVec4(0.12f, 0.35f, 0.14f, 1.0f) },
        { "Start", BTN_START, ImVec4(0.48f, 0.14f, 0.14f, 1.0f) },
        { "L", BTN_L, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "R", BTN_R, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "Z", BTN_Z, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "C " ICON_FA_ARROW_UP, BTN_CUP, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "C " ICON_FA_ARROW_DOWN, BTN_CDOWN, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "C " ICON_FA_ARROW_LEFT, BTN_CLEFT, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
        { "C " ICON_FA_ARROW_RIGHT, BTN_CRIGHT, ImVec4(0.60f, 0.44f, 0.06f, 1.0f) },
    };
    static const VrN64RowDef sDpadRows[] = {
        { "D " ICON_FA_ARROW_UP, BTN_DUP, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "D " ICON_FA_ARROW_DOWN, BTN_DDOWN, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "D " ICON_FA_ARROW_LEFT, BTN_DLEFT, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
        { "D " ICON_FA_ARROW_RIGHT, BTN_DRIGHT, ImVec4(0.32f, 0.32f, 0.32f, 1.0f) },
    };

    if (ImGui::CollapsingHeader("Buttons##VrInputs", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const VrN64RowDef& row : sButtonRows) {
            VrInputBindingRow(row);
        }
    }
    if (ImGui::CollapsingHeader("D-Pad##VrInputs", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const VrN64RowDef& row : sDpadRows) {
            VrInputBindingRow(row);
        }
    }
}

// Live frame-cost breakdown. The interesting number is "XR wait": that is time spent blocked in
// xrWaitFrame, i.e. spare headroom. When it trends toward zero the frame no longer fits and the
// compositor starts reprojecting.
static void VrPerformanceReadout(WidgetInfo& info) {
    if (!VR_IsInitialized()) {
        ImGui::TextUnformatted("Not in VR.");
        return;
    }

    VrFrameStats s = {};
    vr_get_frame_stats(&s);

    ImGui::Text("XR frames submitted   %6.1f Hz", s.frame_hz);
    ImGui::Text("Stereo pairs rendered %6.1f Hz", s.eye_hz);
    ImGui::Separator();
    ImGui::Text("XR wait (headroom)  %6.2f ms", s.wait_ms);
    ImGui::Text("Both eye passes     %6.2f ms", s.eyes_ms);
    ImGui::Text("HUD quad pass       %6.2f ms", s.hud_ms);
    ImGui::Text("Companion window    %6.2f ms", s.desktop_ms);
    ImGui::Text("Whole frame         %6.2f ms", s.frame_ms);
    ImGui::Separator();
    ImGui::Text("Game logic tick     %6.2f ms", s.tick_ms);
    ImGui::TextUnformatted("(game logic runs once per 20 Hz tick, on this\n"
                           "same thread, so it comes out of the render budget)");
}

void SohMenu::AddMenuVRSettings() {
    AddMenuEntry("VR Settings", CVAR_SETTING("Menu.VRSettingsSidebarSection"));

    // ------------------------------------------------------------------ General
    AddSidebarEntry("VR Settings", "General", 1);
    WidgetPath generalPath = { "VR Settings", "General", SECTION_COLUMN_1 };

    AddWidget(generalPath, "VR Mode (F9)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrEnabled")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Switch between VR and regular flat-screen play at any time - F9 does "
                              "the same thing. The switch takes effect on the next game tick (up to "
                              "50 ms). Turning VR off leaves the headset idle and ready to resume "
                              "instantly; if the game was started with VR off, turning it on "
                              "connects to the headset on the spot."));
    AddWidget(generalPath, "Stay In VR When Headset Is Removed", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrStayOnDoff")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrEnabled", 1); })
        .Options(CheckboxOptions().Tooltip(
            "By default, taking the headset off automatically drops the game to flat-screen play, "
            "and putting it back on resumes VR right where you left it. Enable this to keep "
            "rendering in VR while the headset is off (useful if removal is being detected when "
            "you don't want it to). Needs a runtime that reports headset presence; if yours "
            "doesn't, doffing never switches regardless."));
    AddWidget(generalPath, "VR View", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrFirstPerson")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrEnabled", 1); })
        .Options(ComboboxOptions()
                     .DefaultIndex(1)
                     .ComboMap(vrViewModeOptions)
                     .Tooltip("First Person: you are Link - the camera sits at his head, movement "
                              "follows your gaze, motion-control hands, snap turning. Third "
                              "Person: the stock game in stereo 3D - the view rides the game "
                              "camera exactly (position and facing, cutscenes included), you add "
                              "head-look and lean on top, and the right stick is normal C-buttons "
                              "(no snap turn; the game owns all camera movement). The camera's "
                              "up/down tilt is not applied - the horizon stays level with the real "
                              "world and you tilt your own head instead. Switch any time."));
    AddWidget(generalPath, "Auto Director Camera When Far From Link", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrFpAutoDirectorCam")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1);
        })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Cutscenes stay first-person (you experience them from Link's eyes) - but when the "
            "game's camera goes far away from Link (a cutscene showing a distant place, or a "
            "scene still loading), first person would leave you staring at nothing, so the view "
            "automatically rides the game's camera until it comes back to Link. Disable to stay "
            "strictly in Link's head no matter what."));

    // ------------------------------------------------------- Comfort & Movement
    AddSidebarEntry("VR Settings", "Comfort & Movement", 1);
    WidgetPath comfortPath = { "VR Settings", "Comfort & Movement", SECTION_COLUMN_1 };

    AddWidget(comfortPath, "Locomotion", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Body Follows Head (VR Movement)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrBodyFollowsHead")
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !CVarGetInteger("gVrEnabled", 1) || !CVarGetInteger("gVrFirstPerson", 1);
        })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Standard VR locomotion: Link's body always faces where you're looking, and on the "
            "ground your velocity IS the stick - exact direction, speed proportional to "
            "deflection, applied the same frame. Push a little to creep, release to stop dead; no "
            "acceleration ramp, no start-step or turn-around animations gating movement (motion-"
            "sickness comfort: what your hand does is exactly what your body feels). Rolls, "
            "jumps, attacks and knockbacks keep their normal motion, and the game still "
            "choreographs Link in cutscenes, on Epona and while climbing. Off = classic OoT "
            "movement."));
    AddWidget(comfortPath, "Snap Turning (Right Stick)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrSnapTurnOn")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Flick the right thumbstick left/right to rotate the world in discrete "
                              "steps. While enabled, the stick's X axis no longer triggers C-Left/"
                              "C-Right (C-Up/C-Down on the Y axis still work)."));
    AddWidget(comfortPath, "Snap Turn Angle: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrSnapTurnDegrees")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrSnapTurnOn", 1); })
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(180.0f)
                     .DefaultValue(45.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("Degrees rotated per flick of the stick."));

    AddWidget(comfortPath, "World Scale", WIDGET_SEPARATOR_TEXT);
    AddWidget(comfortPath, "Match Scale To My Height (Be Link-Sized)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrAutoWorldScale")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Derive world scale from your real standing eye height so you are exactly Link-sized: "
            "the ground meets your physical floor and age swaps rescale automatically. "
            "Re-measured when you recenter or re-enter first person - stand normally when you do. "
            "Disable to use the fixed World Scale slider instead (one true world size regardless "
            "of who is playing). Needs a runtime with floor calibration; without one the slider "
            "applies either way."));
    AddWidget(comfortPath, "World Scale: %.1f units/m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrWorldScale")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrAutoWorldScale", 1); })
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(100.0f)
                     .DefaultValue(35.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Game units per real-world meter. Higher makes the world feel smaller "
                              "(and physical movements cover more in-game distance); lower makes "
                              "everything tower over you. Applies live."));
    AddWidget(comfortPath, "Eye Height Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadHeightOffset")
        .Options(FloatSliderOptions()
                     .Min(-100.0f)
                     .Max(100.0f)
                     .DefaultValue(-9.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Raise/lower the eye anchor relative to Link's eye height, in game "
                              "units. Lowering it brings the ground closer by exactly offset / world "
                              "scale meters."));

    // ------------------------------------------------------------------ Gameplay
    AddSidebarEntry("VR Settings", "Gameplay", 1);
    WidgetPath gameplayPath = { "VR Settings", "Gameplay", SECTION_COLUMN_1 };

    AddWidget(gameplayPath, "Motion-Control Hands", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrMotionHands")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Detach Link's hands from his body and pin them to the VR controllers."));
    AddWidget(gameplayPath, "Motion Weapon Aim", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrWeaponAim")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrMotionHands", 1); })
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Slingshot seeds, arrows and the hookshot launch from your weapon hand and fly where "
            "that controller points (its aim ray - the same ray runtimes use for menu pointing). "
            "The weapon rides the hand holding the bow/slingshot model. Even while Z-targeted, "
            "your hand decides the shot; lock-on only steers the camera. Off = the stock "
            "stick-aiming behavior."));
    AddWidget(gameplayPath, "Left-Handed Mode", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLeftHanded")
        .Options(CheckboxOptions().Tooltip(
            "Swap which controller drives the sword hand: the LEFT controller holds the sword (matching "
            "Link's own left-handedness) instead of the right."));
    AddWidget(gameplayPath, "Hide Link's Body", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHideBody")
        .Options(CheckboxOptions().Tooltip(
            "First person only: don't draw Link's body - just the floating hands and whatever "
            "they hold (the classic VR style). Some players prefer it because the body can block "
            "the view when looking down, and its animations don't always match what you're "
            "doing. Third person and cutscenes always show the full body."));
    AddWidget(gameplayPath, "Lock-On Reticle Size: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrReticleScale")
        .Options(FloatSliderOptions()
                     .Min(0.3f)
                     .Max(3.0f)
                     .DefaultValue(1.0f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Size of the in-world Z-target reticle (the converging triangles "
                              "that wrap whatever you lock onto). In VR the reticle is drawn in "
                              "the 3D scene at the target, not on the flat HUD."));
    AddWidget(gameplayPath, "Show Letterbox Bars", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLetterbox")
        .Options(CheckboxOptions().Tooltip(
            "Show the cinematic black bars during Z-targeting and cutscenes, like the original "
            "game. Off by default in VR: the bars just float on the head-locked overlay and "
            "shrink your view. Flat-screen play is unaffected by this setting (see Enhancements "
            "> Graphics for the flat equivalent)."));

    // --------------------------------------------------------------- HUD & Menus
    AddSidebarEntry("VR Settings", "HUD & Menus", 1);
    WidgetPath hudPath = { "VR Settings", "HUD & Menus", SECTION_COLUMN_1 };

    AddWidget(hudPath, "HUD", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "HUD Attachment", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrHudAttach")
        .Options(ComboboxOptions()
                     .DefaultIndex(0)
                     .ComboMap(vrHudAttachOptions)
                     .Tooltip("Where the HUD (hearts, rupees, C-button items) lives: floating in "
                              "front of your face, or pinned to a controller like a wrist panel - "
                              "glance at your hand to check your status. Falls back to head-locked "
                              "while that controller isn't tracked."));
    AddWidget(hudPath, "HUD Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(0.5f).Max(5.0f).DefaultValue(2.0f).Step(0.1f).Format("%.1f"));
    AddWidget(hudPath, "HUD Size: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudSize")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(0.2f).Max(3.0f).DefaultValue(1.5f).Step(0.05f).Format("%.2f"));
    AddWidget(hudPath, "HUD Horizontal: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudOffX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).DefaultValue(0.0f).Step(0.02f).Format("%.2f"));
    AddWidget(hudPath, "HUD Vertical: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).DefaultValue(0.0f).Step(0.02f).Format("%.2f"));
    AddWidget(hudPath, "Hand HUD Size: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandSize")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions().Min(0.1f).Max(1.0f).DefaultValue(0.35f).Step(0.01f).Format("%.2f"));
    AddWidget(hudPath, "Hand HUD Sideways: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandOffX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions()
                     .Min(-0.5f)
                     .Max(0.5f)
                     .DefaultValue(0.0f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Offset along the grip's sideways axis (mirrored automatically for "
                              "the right hand, so one tuning fits both)."));
    AddWidget(hudPath, "Hand HUD Up: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions().Min(-0.5f).Max(0.5f).DefaultValue(0.10f).Step(0.01f).Format("%.2f"));
    AddWidget(hudPath, "Hand HUD Forward: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandOffZ")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions().Min(-0.5f).Max(0.5f).DefaultValue(-0.08f).Step(0.01f).Format("%.2f"));
    AddWidget(hudPath, "Hand HUD Tilt: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandPitch")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions()
                     .Min(-90.0f)
                     .Max(90.0f)
                     .DefaultValue(-40.0f)
                     .Step(1.0f)
                     .Format("%.0f")
                     .Tooltip("Tilt about the grip so the panel faces your eyes at a natural "
                              "wrist-watch angle."));

    AddWidget(hudPath, "Menu Screen", WIDGET_SEPARATOR_TEXT);
    AddWidget(hudPath, "Menu Screen Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrScreenDistance")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(5.0f)
                     .DefaultValue(2.2f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How far in front of you the floating menu panel (file select, pause) "
                              "appears. Applies the next time a menu opens."));
    AddWidget(hudPath, "Menu Screen Size: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrScreenSize")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(5.0f)
                     .DefaultValue(2.4f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Width of the floating menu panel in meters (height follows 4:3). "
                              "Applies live."));

    // ---------------------------------------------------------------- Performance
    AddSidebarEntry("VR Settings", "Performance", 2);
    WidgetPath perfPath = { "VR Settings", "Performance", SECTION_COLUMN_1 };

    AddWidget(perfPath, "Render Cost", WIDGET_SEPARATOR_TEXT);
    AddWidget(perfPath, "The game's display list is walked once per eye, every frame. At 120 Hz "
                       "that is 240 full traversals a second on one thread, plus the HUD. These "
                       "settings trade world-update rate for headroom; head tracking always stays "
                       "at the headset's full rate because skipped frames are reprojected by the "
                       "compositor.",
              WIDGET_TEXT);
    AddWidget(perfPath, "Stereo Render Divisor: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar("gVrStereoDivisor")
        .Options(IntSliderOptions()
                     .Min(1)
                     .Max(4)
                     .DefaultValue(1)
                     .Format("%d")
                     .Tooltip("Redraw the stereo pair every Nth frame; in between, the previous "
                              "images are resubmitted with the pose they were drawn from and the "
                              "compositor reprojects them onto your live head pose. 2 roughly "
                              "halves render cost. The source animation is 20 fps, so the drop "
                              "from 120 to 60 world updates is hard to see; head tracking is "
                              "unaffected."));
    AddWidget(perfPath, "Draw HUD Once Per Game Tick", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHudPerTick")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("The overlay display list is rebuilt once per 20 Hz game tick, so "
                              "redrawing it on every interpolated sub-frame renders identical "
                              "content up to six times. Off = redraw every frame (only useful if "
                              "something in the HUD looks like it is updating too slowly)."));
    AddWidget(perfPath, "Companion Window Divisor: %d", WIDGET_CVAR_SLIDER_INT)
        .CVar("gVrDesktopViewDivisor")
        .Options(IntSliderOptions()
                     .Min(1)
                     .Max(16)
                     .DefaultValue(4)
                     .Format("%d")
                     .Tooltip("How often the desktop window is updated, in frames. Each update "
                              "costs an ImGui frame, a full-eye-resolution mirror copy and a "
                              "Present, all on the critical path. 4 gives roughly 30 fps on the "
                              "monitor at a 120 Hz headset. This menu updates at that rate too."));
    AddWidget(perfPath, "Render Resolution Scale: %.2f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrResolutionScale")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(1.5f)
                     .DefaultValue(1.0f)
                     .Step(0.05f)
                     .Format("%.2f")
                     .Tooltip("Multiplier on the runtime's recommended per-eye resolution. This is "
                              "the GPU-side knob. Applied when the OpenXR session is created, so "
                              "it takes effect on the next restart."));

    perfPath.column = SECTION_COLUMN_2;
    AddWidget(perfPath, "Live Frame Cost", WIDGET_SEPARATOR_TEXT);
    AddWidget(perfPath, "VRPerformanceReadout", WIDGET_CUSTOM).CustomFunction(VrPerformanceReadout).HideInSearch(true);

    AddWidget(perfPath, "Settings That Do Nothing In VR", WIDGET_SEPARATOR_TEXT);
    AddWidget(perfPath, "MSAA and Internal Resolution (under Settings > Graphics) have no effect "
                       "while VR is active: the eyes render into OpenXR swapchains, which are "
                       "created single-sampled and at the size set by Render Resolution Scale "
                       "above. Turning them up costs memory and changes nothing you can see.",
              WIDGET_TEXT);
    AddWidget(perfPath, "V-Sync is bypassed in VR: the headset's compositor paces frames, and a "
                       "second pacer running off the monitor's refresh rate would fight it.",
              WIDGET_TEXT);

    // ------------------------------------------------------------------ VR Inputs
    AddSidebarEntry("VR Settings", "VR Inputs", 1);
    WidgetPath buttonsPath = { "VR Settings", "VR Inputs", SECTION_COLUMN_1 };

    AddWidget(buttonsPath, "VrInputBindings", WIDGET_CUSTOM).CustomFunction(VrInputBindings).HideInSearch(true);
    AddWidget(buttonsPath, "Click a chip to remove a binding, + to add one. A VR input may press "
                           "several buttons at once. Sticks are fixed: left = movement, right = "
                           "C-buttons/snap turn.",
              WIDGET_TEXT);

    // ---------------------------------------------------------------- Calibration
    AddSidebarEntry("VR Settings", "Calibration", 2);
    WidgetPath calPath = { "VR Settings", "Calibration", SECTION_COLUMN_1 };

    AddWidget(calPath, "One-time tuning. The defaults were calibrated in-headset; you should not "
                       "need anything here unless the hands, weapon aim or camera look off on "
                       "your setup. Tune, then use the copy button to share your values.",
              WIDGET_TEXT);

    AddWidget(calPath, "Hand Mirroring", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Mirror Sword Hand", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandMirrorSword")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Reflect the sword hand's mesh so it reads as a right hand on the right "
                              "controller. Only applies in right-handed mode."));
    AddWidget(calPath, "Mirror Shield Hand", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandMirrorShield")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Reflect the shield hand's mesh so it reads as a left hand on the left "
                              "controller. Note the reflection also mirrors the shield's face design; "
                              "pair with the Left Hand Override values to orient it correctly."));
    AddWidget(calPath, "Mirror Axis", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrHandMirrorAxis")
        .Options(ComboboxOptions()
                     .DefaultIndex(2)
                     .ComboMap(vrMirrorAxisOptions)
                     .Tooltip("Which model-local axis the mirror reflection negates. Should be the thumb "
                              "axis: it must keep the finger direction and flip the thumb so the mesh reads "
                              "as the opposite hand. Try each if the hands look inside-out."));

    AddWidget(calPath, "Hand Rotation", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Tune while looking at the SWORD hand - the other hand mirrors automatically.", WIDGET_TEXT);
    AddWidget(calPath, "Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalPitch")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(88.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip X axis (wrist tilt up/down)."));
    AddWidget(calPath, "Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalYaw")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(-100.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip Y axis. If the sword points backward or sideways "
                              "out of your fist, adjust this first."));
    AddWidget(calPath, "Roll: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalRoll")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(80.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip Z axis (twist around the handle - use to line up "
                              "the blade edge and palm)."));

    AddWidget(calPath, "Hand Position", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Offset X: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffX")
        .Options(FloatSliderOptions()
                     .Min(-30.0f)
                     .Max(30.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip X axis (game units, tuned for the left "
                              "controller; the right controller mirrors)."));
    AddWidget(calPath, "Offset Y: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffY")
        .Options(FloatSliderOptions()
                     .Min(-30.0f)
                     .Max(30.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip Y axis."));
    AddWidget(calPath, "Offset Z: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffZ")
        .Options(FloatSliderOptions()
                     .Min(-30.0f)
                     .Max(30.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip Z axis (roughly along the handle)."));

    calPath.column = SECTION_COLUMN_2;
    AddWidget(calPath, "Left Hand Override", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Tune Left Hand Separately", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandLOverride")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "By default the left controller's hand is derived from the values above by mirror symmetry. "
            "If it doesn't look right, enable this and dial it in with its own values below."));
    AddWidget(calPath, "L Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalPitch")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(-149.0f).Step(1.0f).Format("%.1f"));
    AddWidget(calPath, "L Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalYaw")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(76.0f).Step(1.0f).Format("%.1f"));
    AddWidget(calPath, "L Roll: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalRoll")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(30.0f).Step(1.0f).Format("%.1f"));
    AddWidget(calPath, "L Offset X: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-30.0f).Max(30.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));
    AddWidget(calPath, "L Offset Y: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-30.0f).Max(30.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));
    AddWidget(calPath, "L Offset Z: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffZ")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-30.0f).Max(30.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));

    AddWidget(calPath, "Weapon Aim Trim", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Aim Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimCalPitch")
        .Options(FloatSliderOptions()
                     .Min(-45.0f)
                     .Max(45.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Tilt the aim ray up/down relative to the controller. If shots "
                              "consistently land high or low of where you point, trim it here."));
    AddWidget(calPath, "Aim Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimCalYaw")
        .Options(FloatSliderOptions()
                     .Min(-45.0f)
                     .Max(45.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Skew the aim ray left/right relative to the controller."));
    AddWidget(calPath, "Aim Origin Right: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimOffX")
        .Options(FloatSliderOptions()
                     .Min(-0.3f)
                     .Max(0.3f)
                     .DefaultValue(0.0f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Slide the projectile's launch point sideways along the aim frame "
                              "(meters), e.g. to sit in the slingshot pouch."));
    AddWidget(calPath, "Aim Origin Up: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimOffY")
        .Options(FloatSliderOptions().Min(-0.3f).Max(0.3f).DefaultValue(0.0f).Step(0.01f).Format("%.2f"));
    AddWidget(calPath, "Aim Origin Forward: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrAimOffZ")
        .Options(FloatSliderOptions()
                     .Min(-0.3f)
                     .Max(0.3f)
                     .DefaultValue(0.0f)
                     .Step(0.01f)
                     .Format("%.2f")
                     .Tooltip("Push the launch point forward along the ray (negative = toward "
                              "you). Note OpenXR aim forward is -Z, so forward here is negative Z "
                              "in the raw frame - this slider already accounts for that."));

    AddWidget(calPath, "Head Position (relative to Link's body)", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Forward Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadOffsetForward")
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(6.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Move the eye anchor along Link's facing (game units). Positive pushes "
                              "the camera forward out of his head; negative pulls it back."));
    AddWidget(calPath, "Side Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadOffsetSide")
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Move the eye anchor sideways relative to Link's facing (game units)."));

    AddWidget(calPath, "Export", WIDGET_SEPARATOR_TEXT);
    AddWidget(calPath, "Copy All Calibration Values", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Copy every VR tuning value (hands, weapon aim, camera, "
                                         "world scale, HUD placement) so they can be handed to a "
                                         "developer to become the defaults."))
        .Callback([](WidgetInfo& info) {
            char buf[2048];
            snprintf(buf, sizeof(buf),
                     "gVrMotionHands=%d\n"
                     "gVrLeftHanded=%d\n"
                     "gVrHandMirrorSword=%d\n"
                     "gVrHandMirrorShield=%d\n"
                     "gVrHandMirrorAxis=%d\n"
                     "gVrHandCalPitch=%.1f\n"
                     "gVrHandCalYaw=%.1f\n"
                     "gVrHandCalRoll=%.1f\n"
                     "gVrHandOffX=%.1f\n"
                     "gVrHandOffY=%.1f\n"
                     "gVrHandOffZ=%.1f\n"
                     "gVrHandLOverride=%d\n"
                     "gVrHandLCalPitch=%.1f\n"
                     "gVrHandLCalYaw=%.1f\n"
                     "gVrHandLCalRoll=%.1f\n"
                     "gVrHandLOffX=%.1f\n"
                     "gVrHandLOffY=%.1f\n"
                     "gVrHandLOffZ=%.1f\n"
                     "gVrAimCalPitch=%.1f\n"
                     "gVrAimCalYaw=%.1f\n"
                     "gVrAimOffX=%.2f\n"
                     "gVrAimOffY=%.2f\n"
                     "gVrAimOffZ=%.2f\n"
                     "gVrHeadHeightOffset=%.1f\n"
                     "gVrHeadOffsetForward=%.1f\n"
                     "gVrHeadOffsetSide=%.1f\n"
                     "gVrWorldScale=%.1f\n"
                     "gVrScreenDistance=%.1f\n"
                     "gVrScreenSize=%.1f\n"
                     "gVrHudAttach=%d\n"
                     "gVrHudDistance=%.1f\n"
                     "gVrHudSize=%.2f\n"
                     "gVrHudOffX=%.2f\n"
                     "gVrHudOffY=%.2f\n"
                     "gVrHudHandSize=%.2f\n"
                     "gVrHudHandOffX=%.2f\n"
                     "gVrHudHandOffY=%.2f\n"
                     "gVrHudHandOffZ=%.2f\n"
                     "gVrHudHandPitch=%.0f\n",
                     CVarGetInteger("gVrMotionHands", 1), CVarGetInteger("gVrLeftHanded", 0),
                     CVarGetInteger("gVrHandMirrorSword", 1), CVarGetInteger("gVrHandMirrorShield", 1),
                     CVarGetInteger("gVrHandMirrorAxis", 2),
                     CVarGetFloat("gVrHandCalPitch", 88.0f), CVarGetFloat("gVrHandCalYaw", -100.0f),
                     CVarGetFloat("gVrHandCalRoll", 80.0f), CVarGetFloat("gVrHandOffX", 0.0f),
                     CVarGetFloat("gVrHandOffY", 0.0f), CVarGetFloat("gVrHandOffZ", 0.0f),
                     CVarGetInteger("gVrHandLOverride", 1), CVarGetFloat("gVrHandLCalPitch", -149.0f),
                     CVarGetFloat("gVrHandLCalYaw", 76.0f), CVarGetFloat("gVrHandLCalRoll", 30.0f),
                     CVarGetFloat("gVrHandLOffX", 0.0f), CVarGetFloat("gVrHandLOffY", 0.0f),
                     CVarGetFloat("gVrHandLOffZ", 0.0f),
                     CVarGetFloat("gVrAimCalPitch", 0.0f), CVarGetFloat("gVrAimCalYaw", 0.0f),
                     CVarGetFloat("gVrAimOffX", 0.0f), CVarGetFloat("gVrAimOffY", 0.0f),
                     CVarGetFloat("gVrAimOffZ", 0.0f),
                     CVarGetFloat("gVrHeadHeightOffset", -9.0f), CVarGetFloat("gVrHeadOffsetForward", 6.0f),
                     CVarGetFloat("gVrHeadOffsetSide", 0.0f), CVarGetFloat("gVrWorldScale", 35.0f),
                     CVarGetFloat("gVrScreenDistance", 2.2f), CVarGetFloat("gVrScreenSize", 2.4f),
                     CVarGetInteger("gVrHudAttach", 0), CVarGetFloat("gVrHudDistance", 2.0f),
                     CVarGetFloat("gVrHudSize", 1.5f), CVarGetFloat("gVrHudOffX", 0.0f),
                     CVarGetFloat("gVrHudOffY", 0.0f), CVarGetFloat("gVrHudHandSize", 0.35f),
                     CVarGetFloat("gVrHudHandOffX", 0.0f), CVarGetFloat("gVrHudHandOffY", 0.10f),
                     CVarGetFloat("gVrHudHandOffZ", -0.08f), CVarGetFloat("gVrHudHandPitch", -40.0f));
            ImGui::SetClipboardText(buf);
        });
}

} // namespace SohGui
