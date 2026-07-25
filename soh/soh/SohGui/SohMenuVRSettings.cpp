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

// Must match the value encoding read by padmgr.c's VR button mapping.
static const std::map<int32_t, const char*> vrN64ButtonOptions = {
    { 0, "None" },     { 1, "A" },        { 2, "B (Sword)" }, { 3, "Z (Target)" },
    { 4, "R (Shield)" }, { 5, "L" },      { 6, "Start (Pause)" }, { 7, "C-Up" },
    { 8, "C-Down" },   { 9, "C-Left" },   { 10, "C-Right" },
};

void SohMenu::AddMenuVRSettings() {
    AddMenuEntry("VR Settings", CVAR_SETTING("Menu.VRSettingsSidebarSection"));

    AddSidebarEntry("VR Settings", "Movement", 1);
    WidgetPath movementPath = { "VR Settings", "Movement", SECTION_COLUMN_1 };

    AddWidget(movementPath, "Mode", WIDGET_SEPARATOR_TEXT);
    AddWidget(movementPath, "VR Mode (F9)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrEnabled")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Switch between VR and regular flat-screen play at any time - F9 does "
                              "the same thing. The switch takes effect on the next game tick (up to "
                              "50 ms). Turning VR off leaves the headset idle and ready to resume "
                              "instantly; if the game was started with VR off, turning it on "
                              "connects to the headset on the spot."));
    AddWidget(movementPath, "Stay In VR When Headset Is Removed", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrStayOnDoff")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrEnabled", 1); })
        .Options(CheckboxOptions().Tooltip(
            "By default, taking the headset off automatically drops the game to flat-screen play, "
            "and putting it back on resumes VR right where you left it. Enable this to keep "
            "rendering in VR while the headset is off (useful if removal is being detected when "
            "you don't want it to). Needs a runtime that reports headset presence; if yours "
            "doesn't, doffing never switches regardless."));
    AddWidget(movementPath, "VR View", WIDGET_CVAR_COMBOBOX)
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

    AddWidget(movementPath, "Turning", WIDGET_SEPARATOR_TEXT);
    AddWidget(movementPath, "Snap Turning (Right Stick)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrSnapTurnOn")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Flick the right thumbstick left/right to rotate the world in discrete "
                              "steps. While enabled, the stick's X axis no longer triggers C-Left/"
                              "C-Right (C-Up/C-Down on the Y axis still work)."));
    AddWidget(movementPath, "Snap Turn Angle: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrSnapTurnDegrees")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrSnapTurnOn", 1); })
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(180.0f)
                     .DefaultValue(45.0f)
                     .Step(5.0f)
                     .Format("%.0f")
                     .Tooltip("Degrees rotated per flick of the stick."));

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

    AddSidebarEntry("VR Settings", "Buttons", 2);
    WidgetPath buttonsPath = { "VR Settings", "Buttons", SECTION_COLUMN_1 };

    AddWidget(buttonsPath, "Left Controller", WIDGET_SEPARATOR_TEXT);
    AddWidget(buttonsPath, "Left Trigger", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnLTrigger")
        .Options(ComboboxOptions().DefaultIndex(3).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Left Grip", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnLGrip")
        .Options(ComboboxOptions().DefaultIndex(4).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "X Button", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnLPrimary")
        .Options(ComboboxOptions().DefaultIndex(9).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Y Button", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnLSecondary")
        .Options(ComboboxOptions().DefaultIndex(10).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Left Stick Click", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnLStickClick")
        .Options(ComboboxOptions().DefaultIndex(0).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Left Menu Button", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnLMenu")
        .Options(ComboboxOptions()
                     .DefaultIndex(6)
                     .ComboMap(vrN64ButtonOptions)
                     .Tooltip("Make sure SOMETHING maps to Start, or you won't be able to pause."));

    buttonsPath.column = SECTION_COLUMN_2;
    AddWidget(buttonsPath, "Right Controller", WIDGET_SEPARATOR_TEXT);
    AddWidget(buttonsPath, "Right Trigger", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnRTrigger")
        .Options(ComboboxOptions().DefaultIndex(2).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Right Grip", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnRGrip")
        .Options(ComboboxOptions().DefaultIndex(0).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "A Button", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnRPrimary")
        .Options(ComboboxOptions().DefaultIndex(1).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "B Button", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnRSecondary")
        .Options(ComboboxOptions().DefaultIndex(8).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Right Stick Click", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnRStickClick")
        .Options(ComboboxOptions().DefaultIndex(0).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Right Menu Button", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrBtnRMenu")
        .Options(ComboboxOptions().DefaultIndex(0).ComboMap(vrN64ButtonOptions));
    AddWidget(buttonsPath, "Sticks are fixed: left = movement, right = C-buttons/snap turn.", WIDGET_TEXT);

    AddSidebarEntry("VR Settings", "Camera", 1);
    WidgetPath cameraPath = { "VR Settings", "Camera", SECTION_COLUMN_1 };

    AddWidget(cameraPath, "Head Position (relative to Link's body)", WIDGET_SEPARATOR_TEXT);
    AddWidget(cameraPath, "Height Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
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
    AddWidget(cameraPath, "Forward Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadOffsetForward")
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(6.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Move the eye anchor along Link's facing (game units). Positive pushes "
                              "the camera forward out of his head; negative pulls it back."));
    AddWidget(cameraPath, "Side Offset: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHeadOffsetSide")
        .Options(FloatSliderOptions()
                     .Min(-60.0f)
                     .Max(60.0f)
                     .DefaultValue(0.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Move the eye anchor sideways relative to Link's facing (game units)."));
    AddWidget(cameraPath, "World Scale", WIDGET_SEPARATOR_TEXT);
    AddWidget(cameraPath, "World Scale: %.1f units/m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrWorldScale")
        .Options(FloatSliderOptions()
                     .Min(10.0f)
                     .Max(100.0f)
                     .DefaultValue(35.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Game units per real-world meter. Higher makes the world feel smaller "
                              "(and physical movements cover more in-game distance); lower makes "
                              "everything tower over you. Applies live."));
    AddWidget(cameraPath, "Menu Screen", WIDGET_SEPARATOR_TEXT);
    AddWidget(cameraPath, "Menu Screen Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrScreenDistance")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(5.0f)
                     .DefaultValue(2.2f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("How far in front of you the floating menu panel (file select, pause) "
                              "appears. Applies the next time a menu opens."));
    AddWidget(cameraPath, "Menu Screen Size: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrScreenSize")
        .Options(FloatSliderOptions()
                     .Min(0.5f)
                     .Max(5.0f)
                     .DefaultValue(2.4f)
                     .Step(0.1f)
                     .Format("%.1f")
                     .Tooltip("Width of the floating menu panel in meters (height follows 4:3). "
                              "Applies live."));
    AddWidget(cameraPath, "HUD", WIDGET_SEPARATOR_TEXT);
    AddWidget(cameraPath, "HUD Attachment", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrHudAttach")
        .Options(ComboboxOptions()
                     .DefaultIndex(0)
                     .ComboMap(vrHudAttachOptions)
                     .Tooltip("Where the HUD (hearts, rupees, C-button items) lives: floating in "
                              "front of your face, or pinned to a controller like a wrist panel - "
                              "glance at your hand to check your status. Falls back to head-locked "
                              "while that controller isn't tracked."));
    AddWidget(cameraPath, "HUD Distance: %.1f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudDistance")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(0.5f).Max(5.0f).DefaultValue(2.0f).Step(0.1f).Format("%.1f"));
    AddWidget(cameraPath, "HUD Size: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudSize")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(0.2f).Max(3.0f).DefaultValue(1.5f).Step(0.05f).Format("%.2f"));
    AddWidget(cameraPath, "HUD Horizontal: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudOffX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).DefaultValue(0.0f).Step(0.02f).Format("%.2f"));
    AddWidget(cameraPath, "HUD Vertical: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) != 0; })
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).DefaultValue(0.0f).Step(0.02f).Format("%.2f"));
    AddWidget(cameraPath, "Hand HUD Size: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandSize")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions().Min(0.1f).Max(1.0f).DefaultValue(0.35f).Step(0.01f).Format("%.2f"));
    AddWidget(cameraPath, "Hand HUD Sideways: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
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
    AddWidget(cameraPath, "Hand HUD Up: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions().Min(-0.5f).Max(0.5f).DefaultValue(0.10f).Step(0.01f).Format("%.2f"));
    AddWidget(cameraPath, "Hand HUD Forward: %.2f m", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHudHandOffZ")
        .PreFunc([](WidgetInfo& info) { info.isHidden = CVarGetInteger("gVrHudAttach", 0) == 0; })
        .Options(FloatSliderOptions().Min(-0.5f).Max(0.5f).DefaultValue(-0.08f).Step(0.01f).Format("%.2f"));
    AddWidget(cameraPath, "Hand HUD Tilt: %.0f deg", WIDGET_CVAR_SLIDER_FLOAT)
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
    AddWidget(cameraPath, "Show Letterbox Bars", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLetterbox")
        .Options(CheckboxOptions().Tooltip(
            "Show the cinematic black bars during Z-targeting and cutscenes, like the original "
            "game. Off by default in VR: the bars just float on the head-locked overlay and "
            "shrink your view. Flat-screen play is unaffected by this setting (see Enhancements "
            "> Graphics for the flat equivalent)."));

    AddWidget(cameraPath, "Export", WIDGET_SEPARATOR_TEXT);
    AddWidget(cameraPath, "Copy Camera Values to Clipboard", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Copy the head-position and world-scale values so they can "
                                         "be handed to a developer to become the defaults."))
        .Callback([](WidgetInfo& info) {
            char buf[640];
            snprintf(buf, sizeof(buf),
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

    AddSidebarEntry("VR Settings", "Hands", 3);
    WidgetPath path = { "VR Settings", "Hands", SECTION_COLUMN_1 };

    AddWidget(path, "General", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Motion-Control Hands", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrMotionHands")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Detach Link's hands from his body and pin them to the VR controllers."));
    AddWidget(path, "Left-Handed Mode", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrLeftHanded")
        .Options(CheckboxOptions().Tooltip(
            "Swap which controller drives the sword hand: the LEFT controller holds the sword (matching "
            "Link's own left-handedness) instead of the right."));
    AddWidget(path, "Mirror Sword Hand", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandMirrorSword")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Reflect the sword hand's mesh so it reads as a right hand on the right "
                              "controller. Only applies in right-handed mode."));
    AddWidget(path, "Mirror Shield Hand", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandMirrorShield")
        .Options(CheckboxOptions()
                     .DefaultValue(true)
                     .Tooltip("Reflect the shield hand's mesh so it reads as a left hand on the left "
                              "controller. Note the reflection also mirrors the shield's face design; "
                              "pair with the Left Hand Override values to orient it correctly."));
    AddWidget(path, "Mirror Axis", WIDGET_CVAR_COMBOBOX)
        .CVar("gVrHandMirrorAxis")
        .Options(ComboboxOptions()
                     .DefaultIndex(2)
                     .ComboMap(vrMirrorAxisOptions)
                     .Tooltip("Which model-local axis the mirror reflection negates. Should be the thumb "
                              "axis: it must keep the finger direction and flip the thumb so the mesh reads "
                              "as the opposite hand. Try each if the hands look inside-out."));

    path.column = SECTION_COLUMN_2;
    AddWidget(path, "Hand Rotation", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Tune while looking at the SWORD hand - the other hand mirrors automatically.", WIDGET_TEXT);
    AddWidget(path, "Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalPitch")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(88.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip X axis (wrist tilt up/down)."));
    AddWidget(path, "Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalYaw")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(-100.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip Y axis. If the sword points backward or sideways "
                              "out of your fist, adjust this first."));
    AddWidget(path, "Roll: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandCalRoll")
        .Options(FloatSliderOptions()
                     .Min(-180.0f)
                     .Max(180.0f)
                     .DefaultValue(80.0f)
                     .Step(1.0f)
                     .Format("%.1f")
                     .Tooltip("Rotation about the grip Z axis (twist around the handle - use to line up "
                              "the blade edge and palm)."));

    AddWidget(path, "Left Hand Override", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Tune Left Hand Separately", WIDGET_CVAR_CHECKBOX)
        .CVar("gVrHandLOverride")
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "By default the left controller's hand is derived from the values above by mirror symmetry. "
            "If it doesn't look right, enable this and dial it in with its own values below."));
    AddWidget(path, "L Pitch: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalPitch")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(-149.0f).Step(1.0f).Format("%.1f"));
    AddWidget(path, "L Yaw: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalYaw")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(76.0f).Step(1.0f).Format("%.1f"));
    AddWidget(path, "L Roll: %.1f deg", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLCalRoll")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-180.0f).Max(180.0f).DefaultValue(30.0f).Step(1.0f).Format("%.1f"));
    AddWidget(path, "L Offset X: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffX")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-30.0f).Max(30.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));
    AddWidget(path, "L Offset Y: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffY")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-30.0f).Max(30.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));
    AddWidget(path, "L Offset Z: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandLOffZ")
        .PreFunc([](WidgetInfo& info) { info.isHidden = !CVarGetInteger("gVrHandLOverride", 1); })
        .Options(FloatSliderOptions().Min(-30.0f).Max(30.0f).DefaultValue(0.0f).Step(0.5f).Format("%.1f"));

    path.column = SECTION_COLUMN_3;
    AddWidget(path, "Hand Position", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Offset X: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffX")
        .Options(FloatSliderOptions()
                     .Min(-30.0f)
                     .Max(30.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip X axis (game units, tuned for the left "
                              "controller; the right controller mirrors)."));
    AddWidget(path, "Offset Y: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffY")
        .Options(FloatSliderOptions()
                     .Min(-30.0f)
                     .Max(30.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip Y axis."));
    AddWidget(path, "Offset Z: %.1f", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVrHandOffZ")
        .Options(FloatSliderOptions()
                     .Min(-30.0f)
                     .Max(30.0f)
                     .DefaultValue(0.0f)
                     .Step(0.5f)
                     .Format("%.1f")
                     .Tooltip("Slide the hand along the grip Z axis (roughly along the handle)."));

    AddWidget(path, "Export", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Copy Values to Clipboard", WIDGET_BUTTON)
        .Options(ButtonOptions().Tooltip("Copy every VR hand-tuning value to the clipboard so it can be "
                                         "pasted into a bug report or handed to a developer."))
        .Callback([](WidgetInfo& info) {
            char buf[1024];
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
                     "gVrHandLOffZ=%.1f\n",
                     CVarGetInteger("gVrMotionHands", 1), CVarGetInteger("gVrLeftHanded", 0),
                     CVarGetInteger("gVrHandMirrorSword", 1), CVarGetInteger("gVrHandMirrorShield", 1),
                     CVarGetInteger("gVrHandMirrorAxis", 2),
                     CVarGetFloat("gVrHandCalPitch", 88.0f), CVarGetFloat("gVrHandCalYaw", -100.0f),
                     CVarGetFloat("gVrHandCalRoll", 80.0f), CVarGetFloat("gVrHandOffX", 0.0f),
                     CVarGetFloat("gVrHandOffY", 0.0f), CVarGetFloat("gVrHandOffZ", 0.0f),
                     CVarGetInteger("gVrHandLOverride", 1), CVarGetFloat("gVrHandLCalPitch", -149.0f),
                     CVarGetFloat("gVrHandLCalYaw", 76.0f), CVarGetFloat("gVrHandLCalRoll", 30.0f),
                     CVarGetFloat("gVrHandLOffX", 0.0f), CVarGetFloat("gVrHandLOffY", 0.0f),
                     CVarGetFloat("gVrHandLOffZ", 0.0f));
            ImGui::SetClipboardText(buf);
        });
}

} // namespace SohGui
