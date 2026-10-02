/*
 * Dusk console widgets for DAF
 * Copyright (C) 2026 Dusk Audio
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with
 * or without fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// The mixing-console widget set shared by Dusk Studio and the Dusk plug-in UIs: the
// knob with its pre-rendered dome, the full-travel fader, the segmented meter, the
// gain-reduction column, the module header pill, buttons, the drag value bubble and
// the text field.
//
// The set depends on Dear ImGui only, not on DGL, so a host that already has an ImGui
// context can use it whatever windowing it runs on. Every widget draws through the
// caller's ImDrawList and takes its colours from a Theme the caller owns, so nothing
// here carries application state.
//
// Values go in and come back out: a widget never writes through a pointer, so the
// caller decides whether a parameter lives in an atomic, in a host parameter or in a
// plain float.

#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS

#include "DearImGui/imgui.h"

#include <cstddef>

namespace DuskWidgets {

// --------------------------------------------------------------------------------------------------------------------
// colour helpers, in ImGui's own channel order

ImU32 withAlpha(ImU32 colour, float alpha) noexcept;
// JUCE's Colour::brighter / ::darker, so a theme ported from a JUCE look-and-feel lands
// on the same shades rather than something merely similar.
ImU32 brighter(ImU32 colour, float amount) noexcept;
ImU32 darker(ImU32 colour, float amount) noexcept;
ImU32 lerpColour(ImU32 a, ImU32 b, float t) noexcept;

// --------------------------------------------------------------------------------------------------------------------

/**
   A skewed value range with the same shape as a JUCE NormalisableRange, so a knob
   sweep lands on the value the same control would produce in a JUCE UI.
 */
struct Range {
    float start;
    float end;
    float skew;

    // Spelled out rather than left to aggregate initialisation, which a C++11 consumer
    // cannot use on a struct carrying default member initialisers.
    constexpr Range(float rangeStart = 0.0f, float rangeEnd = 1.0f, float rangeSkew = 1.0f)
        : start(rangeStart), end(rangeEnd), skew(rangeSkew) {}

    static Range withMidPoint(float start, float end, float valueAtCentre) noexcept;

    float toNorm(float value) const noexcept;
    float fromNorm(float norm) const noexcept;
};

/**
   Peak-meter ballistics for the UI side: instant attack, exponential release and a
   peak hold that decays. Driven per frame, told how many frames make up one tick of
   the rate the meter source actually writes at.
 */
struct MeterBallistics {
    float displayed = -100.0f;
    float peakHold = -100.0f;
    int peakHoldFrames = 0;

    void tick(float incomingDb, float framesPerSourceTick) noexcept;
    void reset() noexcept;
};

// --------------------------------------------------------------------------------------------------------------------

/**
   Every colour the set draws with. The defaults are the Dusk console palette; a
   consumer overwrites the members it wants and passes the whole struct per frame.
 */
struct Theme {
    ImU32 panelFill = IM_COL32(0x1a, 0x1a, 0x1c, 0xff);
    ImU32 panelBorder = IM_COL32(0x2a, 0x2a, 0x2e, 0xff);
    ImU32 background = IM_COL32(0x12, 0x12, 0x14, 0xff);

    ImU32 knobFill = IM_COL32(0xe0, 0xe0, 0xe4, 0xff);
    ImU32 knobOutline = IM_COL32(0x40, 0x40, 0x48, 0xff);
    ImU32 knobTick = IM_COL32(0xc8, 0xc8, 0xd2, 0xff);
    ImU32 knobPointer = IM_COL32(0xff, 0xff, 0xff, 0xff);
    ImU32 knobPointerShadow = IM_COL32(0x0c, 0x0c, 0x0e, 0xff);

    ImU32 textDim = IM_COL32(0x8e, 0x92, 0x98, 0xff);
    ImU32 textValue = IM_COL32(0xd8, 0xd8, 0xd8, 0xff);
    ImU32 textBright = IM_COL32(0xff, 0xff, 0xff, 0xff);
    ImU32 textOn = IM_COL32(0x12, 0x12, 0x14, 0xff);
    ImU32 textBypassed = IM_COL32(0x77, 0x77, 0x7f, 0xff);

    ImU32 buttonOff = IM_COL32(0x20, 0x20, 0x24, 0xff);
    ImU32 buttonPanel = IM_COL32(0x22, 0x22, 0x26, 0xff);

    ImU32 pillFill = IM_COL32(0x20, 0x20, 0x24, 0xff);
    ImU32 pillBorder = IM_COL32(0x55, 0x55, 0x5c, 0xff);
    ImU32 pillDivider = IM_COL32(0x4a, 0x4a, 0x50, 0xff);
    ImU32 ledRing = IM_COL32(0x09, 0x09, 0x0b, 0xff);
    ImU32 ledOff = IM_COL32(0x29, 0x29, 0x2e, 0xff);

    ImU32 meterBack = IM_COL32(0x06, 0x06, 0x08, 0xff);
    ImU32 meterBorder = IM_COL32(0x2a, 0x2a, 0x30, 0xff);
    ImU32 meterSegment = IM_COL32(0x02, 0x02, 0x03, 0xff);
    ImU32 meterLow = IM_COL32(0x20, 0xd0, 0x40, 0xff);
    ImU32 meterMid = IM_COL32(0xf0, 0xe0, 0x20, 0xff);
    ImU32 meterHigh = IM_COL32(0xff, 0x20, 0x20, 0xff);
    ImU32 peakTick = IM_COL32(0xf0, 0xf0, 0xf0, 0xff);
    ImU32 peakTickHot = IM_COL32(0xff, 0x80, 0x80, 0xff);

    ImU32 trackFill = IM_COL32(0x0a, 0x0a, 0x0c, 0xff);
    ImU32 trackBorder = IM_COL32(0x2a, 0x2a, 0x2e, 0xff);
    ImU32 capTop = IM_COL32(0xe2, 0xdc, 0xcb, 0xff);
    ImU32 capUpper = IM_COL32(0xcf, 0xc8, 0xb8, 0xff);
    ImU32 capMid = IM_COL32(0x9d, 0x95, 0x8a, 0xff);
    ImU32 capBottom = IM_COL32(0xb8, 0xb0, 0xa0, 0xff);
    ImU32 capGroove = IM_COL32(0x20, 0x20, 0x18, 0xff);
    ImU32 capRim = IM_COL32(0x0a, 0x0a, 0x0a, 0xff);
    ImU32 tickLabel = IM_COL32(0xb8, 0xb8, 0xc0, 0xff);

    ImU32 grBack = IM_COL32(0x14, 0x14, 0x18, 0xff);
    ImU32 grLow = IM_COL32(0xd9, 0xa8, 0x7d, 0xff);
    ImU32 grHigh = IM_COL32(0xc9, 0x53, 0x53, 0xff);
    ImU32 grHandle = IM_COL32(0xd0, 0x90, 0x60, 0xff);

    ImU32 bubbleFill = IM_COL32(0x0e, 0x0e, 0x10, 0xf2);
    ImU32 bubbleBorder = IM_COL32(0x80, 0x80, 0x8c, 0xff);
    ImU32 bubbleText = IM_COL32(0xff, 0xff, 0xff, 0xff);

    ImU32 fieldFill = IM_COL32(0x20, 0x20, 0x24, 0xff);
    ImU32 fieldText = IM_COL32(0xff, 0xff, 0xff, 0xff);
};

/**
   One face per design size rather than one scaled face: an 8 pt column header is
   unreadable when it is a scaled 13 pt atlas entry.
 */
struct Fonts {
    ImFont* caption = nullptr;   // column headers
    ImFont* label = nullptr;     // control captions and value readouts
    ImFont* pill = nullptr;      // module headers and button labels
    ImFont* band = nullptr;      // section labels and fader ticks
    ImFont* title = nullptr;     // strip and preset names
    ImFont* value = nullptr;     // numeric readouts
    ImFont* valueLarge = nullptr; // the primary readout of a view
    ImFont* textEntry = nullptr; // anything a user types into
};

struct FontSizes {
    float caption = 8.0f;
    float label = 9.0f;
    float pill = 10.5f;
    float band = 12.0f;
    float title = 13.0f;
    float value = 11.0f;
    float valueLarge = 14.0f;
    float textEntry = 13.0f;
};

/**
   The glyph set the widgets themselves draw: Latin-1 plus the marks a console shows by
   name - the fader's infinity, the phase symbol, the degree sign, arrows, triangles and
   the accidentals. Dear ImGui bakes glyphs at atlas build time, so a mark that is not
   declared is silently dropped rather than substituted, and a face pays for every mark
   it bakes whether or not anything draws it.
 */
const ImWchar* consoleGlyphRanges() noexcept;

/**
   The glyph set for anything a user types into: the console set plus Greek, Cyrillic,
   Hebrew, Arabic, Devanagari, currency, CJK punctuation and kana. Baked for the text
   entry face alone, because the cost is per face and the console faces never show a
   name a user typed.
 */
const ImWchar* textEntryGlyphRanges() noexcept;

/**
   Bake one face per design size from a single TTF. The data is not copied, so it must
   outlive the atlas. Call before ImFontAtlas::Build().
 */
Fonts buildFonts(ImFontAtlas& atlas, const void* ttfData, int ttfDataSize,
                 float scale, const FontSizes& sizes = FontSizes());

// --------------------------------------------------------------------------------------------------------------------

/**
   The pre-rendered knob dome.

   A dome drawn as concentric circles costs about a thousand vertices, and a console
   strip carries twenty-one knobs. Three tinted quads carry the same picture in twelve:
   the body (multiplied by the knob's own colour, so its rim and drop shadow bake in as
   black), the sheen (white) and the pointer ticks. Only the pointer itself is still
   drawn per frame, because it turns.

   The layers live in three custom rectangles inside the font atlas, so they share the
   font texture and add no draw call of their own.
 */
class KnobAtlas
{
public:
    /**
       Reserve the three rectangles. Call before ImFontAtlas::Build().
       @param pixels the baked dome size; 128 is enough for a knob drawn at 64 px.
     */
    void reserve(ImFontAtlas& atlas, int pixels = 128);

    /**
       Rasterise into the built atlas. Call after ImFontAtlas::Build() and before the
       renderer uploads the texture, since it writes into the atlas pixel buffer.
       @note An atlas rebuilt for a new scale needs both calls again: ImFontAtlas::Clear()
             drops the custom rectangles along with the fonts, and until they are back the
             knob quietly falls back to drawing its dome.
     */
    void rasterise(ImFontAtlas& atlas);

    enum Layer {
        body = 0,  // tinted by the knob colour; rim and drop shadow bake in as black
        sheen = 1, // white
        ticks = 2, // tinted by Theme::knobTick
        layerCount = 3
    };

    bool ready() const noexcept;
    ImTextureID textureId() const noexcept;
    ImVec2 uvMin(Layer layer) const noexcept { return uv[layer][0]; }
    ImVec2 uvMax(Layer layer) const noexcept { return uv[layer][1]; }

    // How far past the dome radius the baked image reaches, drop shadow included.
    static constexpr float kExtent = 1.35f;

private:
    const ImFontAtlas* source = nullptr;
    ImVec2 uv[layerCount][2] = {};
    int rects[layerCount] = { -1, -1, -1 };
    int size = 0;
};

// --------------------------------------------------------------------------------------------------------------------

/**
   Which control owns the pointer. One per view: two knobs in different views can be
   dragged by two pointers, two knobs in the same view cannot.
 */
struct DragState {
    ImGuiID active = 0;
    float startValue = 0.0f;
    ImVec2 bubbleAt {};
    char bubbleText[32] = {};
};

/**
   Everything a widget needs that is not its own geometry. Rebuilt per frame; cheap to
   copy, and `emit` is the only member a caller flips mid-frame.
 */
struct Context {
    ImDrawList* dl = nullptr;
    const Theme* theme = nullptr;
    const Fonts* fonts = nullptr;
    const KnobAtlas* knobAtlas = nullptr; // null falls back to the vector dome
    DragState* drag = nullptr;
    float scale = 1.0f;

    // Counts what the frame submitted, so a view can report its own cost.
    int widgets = 0;
    // Set by textField(); read by shortcutsAvailable().
    bool textFieldOpen = false;

    float s(float value) const noexcept { return value * scale; }
};

/**
   The shortcut routing rule, settled once for every Dusk view.

   `io.WantCaptureKeyboard` cannot answer this: the ImGui bridge turns keyboard
   navigation on unconditionally, which pins that flag true whether or not anything
   would consume the key. A shortcut layer may take a key when no text field is open,
   no item is active, and no modal is up.

   The window's keyboard focus follows the narrower io.WantTextInput instead, through
   ImGuiWidget::wantsKeyboardFocus(): textField() is an ImGui InputText, so the editor
   holds the keyboard exactly while one is being edited, and hands it back to the host
   (CLAP and VST2 on Windows) otherwise.
 */
bool shortcutsAvailable(const Context& ctx);

// --------------------------------------------------------------------------------------------------------------------
// primitives

enum class Align { left, centre, right };

void text(const Context& ctx, ImFont* font, float size, ImVec2 at, float width,
          ImU32 colour, const char* str, Align align = Align::centre);

void panel(const Context& ctx, ImVec2 tl, ImVec2 br, ImU32 fill, ImU32 border,
           float borderAlpha, float rounding, float thickness);

// The interaction half of a widget the caller draws itself: submits the rectangle to
// ImGui and answers whether the pointer is over it. Everything ImGui knows about the
// item - active, clicked, double-clicked - is available through the usual IsItemXXX
// calls until the next widget is submitted.
bool hitArea(Context& ctx, const char* id, ImVec2 tl, ImVec2 br);

// The dome on its own, for a view that wants the picture without the interaction.
void drawKnobDome(const Context& ctx, ImVec2 centre, float radius, float norm, ImU32 fill);

// --------------------------------------------------------------------------------------------------------------------
// widgets

struct KnobStyle {
    ImU32 fill = 0;    // 0 takes Theme::knobFill
    ImU32 outline = 0; // 0 draws no extra ring
    const char* caption = nullptr;
    const char* value = nullptr;
    // Travel in design pixels for a full sweep; shift-drag divides it by four.
    float dragTravel = 140.0f;
    float wheelStep = 0.02f;
    bool bubble = true; // float the value beside the pointer while dragging
};

struct KnobResult {
    float value = 0.0f;
    bool changed = false;
    bool dragging = false;
    bool hovered = false;
};

KnobResult knob(Context& ctx, const char* id, ImVec2 centre, float radius, float value,
                const Range& range, float defaultValue, const KnobStyle& style = KnobStyle());

struct FaderTick {
    float value;
    const char* label;
};

// +6 dB to -inf, the scale a Dusk channel fader is printed with.
const FaderTick* decibelFaderTicks(int& count) noexcept;

struct FaderStyle {
    const FaderTick* ticks = nullptr; // null takes decibelFaderTicks()
    int tickCount = 0;
    // Absolute x the gutter labels are right-aligned from; ignored when off.
    float gutterLeft = 0.0f;
    bool gutter = true;
    float trackInset = 18.0f; // design pixels of travel trimmed at each end
    float capWidth = 20.0f;
    float capHeight = 36.0f;
    bool bubble = true;
    const char* valueText = nullptr; // null formats the value as decibels
};

struct FaderResult {
    float value = 0.0f;
    bool changed = false;
    bool dragging = false;
};

FaderResult fader(Context& ctx, const char* id, ImVec2 tl, ImVec2 br, float value,
                  const Range& range, float defaultValue,
                  const FaderStyle& style = FaderStyle());

struct MeterStyle {
    const Range* scale = nullptr; // null takes the fader range
    float midDb = -5.0f;          // green above this
    float hotDb = 5.0f;           // red above this
    bool segments = true;
    bool glow = true;
};

// Split so a view that caches its static geometry can keep the well and its segment
// grid in the cache and redraw only the bar, which is the only part that moves.
void meterBackground(const Context& ctx, ImVec2 tl, ImVec2 br,
                     const MeterStyle& style = MeterStyle());
void meterBar(const Context& ctx, ImVec2 tl, ImVec2 br, float db, float peakDb,
              const MeterStyle& style = MeterStyle());
void meter(const Context& ctx, ImVec2 tl, ImVec2 br, float db, float peakDb,
           const MeterStyle& style = MeterStyle());

struct GainReductionStyle {
    const Range* scale = nullptr; // for the threshold handle; null takes the fader range
    float floorDb = -24.0f;
    int segments = 24;
    bool handle = false;
    float handleWidth = 8.0f;
    float handleMinDb = -60.0f;
    float handleMaxDb = 0.0f;
};

struct GainReductionResult {
    float threshold = 0.0f;
    bool changed = false;
};

GainReductionResult gainReduction(Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                                  float reductionDb, float threshold,
                                  const GainReductionStyle& style = GainReductionStyle());

// --------------------------------------------------------------------------------------------------------------------
// the analogue needle meter

/**
   How a value maps to needle deflection, which is the one thing a printed meter face
   never states and every meter gets wrong differently.
 */
enum class NeedleLaw {
    // Deflection is linear in the value. A gain-reduction meter, a percentage scale, a
    // VU face whose endpoints are already expressed as deflection.
    linear,
    // Deflection is linear in signal amplitude while the scale is printed in decibels,
    // which is what gives a broadcast VU its crowded left end and open right end:
    // deflection = 10^((value - maxValue) / 20), so maxValue is full-scale deflection.
    amplitude,
};

/**
   One mark on the face. A tick with no label draws the line alone.
 */
struct NeedleTick {
    float value;
    const char* label;
    bool major;
};

/**
   A second row of numbers nearer the pivot, the way a VU face carries a percentage row
   under its decibel row. Positions are deflections, 0 at the left stop and 1 at the
   right, because the second row is a different unit spread over the same travel.
 */
struct NeedleInnerTick {
    float deflection;
    const char* label;
};

/**
   The face as data: where the needle can point, what is printed on it, and where the
   red zone starts. Everything a caller has to change to turn a VU into a
   gain-reduction meter lives here rather than in the drawing code.

   The endpoints may be inverted -- minValue 20, maxValue 0 gives the gain-reduction
   meter that rests at full right and swings left as it works.
 */
struct NeedleScale {
    const NeedleTick* ticks = nullptr;
    int tickCount = 0;
    const NeedleInnerTick* innerTicks = nullptr;
    int innerTickCount = 0;

    float minValue = 0.0f; // deflection 0 for the linear law
    float maxValue = 1.0f; // deflection 1, and the amplitude law's full-scale reference
    // Where the red zone begins, in scale units. Outside the endpoints draws none.
    float redFrom = 0.0f;
    bool red = false;

    NeedleLaw law = NeedleLaw::linear;
    // Takes over from `law` when set: value in, deflection 0..1 out.
    float (*customLaw)(float value, const NeedleScale& scale) = nullptr;
};

// Where the needle sits for a value, clamped to the travel. Public because the
// ballistics are the caller's: a meter is smoothed in deflection, not in its own units,
// or the smoothing changes shape along the scale.
float needleDeflection(const NeedleScale& scale, float value) noexcept;

/**
   The cosmetic pole in front of a needle. Not a meter ballistic: the 300 ms VU
   integration belongs in the DSP, and this only stops the needle stepping once per
   frame when the source updates more slowly than the display.
 */
struct NeedleBallistics {
    float deflection = 0.0f;

    void tick(float targetDeflection, float deltaSeconds, float tauSeconds = 0.025f) noexcept;
    void reset() noexcept;
};

struct NeedleMeterStyle {
    ImU32 face = 0;       // 0 takes the aged-cream face
    ImU32 faceShade = 0;  // the bottom of the face gradient; 0 derives it from `face`
    ImU32 ink = 0;        // ticks, numbers, legends
    ImU32 accent = 0;     // the red zone, its ticks and its numbers
    ImU32 needle = 0;     // 0 takes `ink`
    ImU32 bezel = 0;      // the housing; 0 draws the face alone
    ImU32 innerInk = 0;   // the second tick row; 0 fades `ink`

    const char* legend = nullptr;    // "VU"
    const char* sublegend = nullptr; // channel tag, "GAIN REDUCTION"
    const char* leftMark = nullptr;  // the corner marks a VU face carries
    const char* rightMark = nullptr;

    ImFont* font = nullptr;   // null takes Fonts::caption
    float labelSize = 10.0f;
    float innerLabelSize = 8.5f;
    float legendSize = 11.0f;

    // Needle travel, in degrees either side of straight up.
    float startAngle = -50.0f;
    float endAngle = 50.0f;

    float bezelInset = 7.0f;
    float rounding = 3.0f;
    bool arc = true;   // the hairline the ticks stand on
    bool glass = true; // the reflection sweep across the face
};

// The face, the scale and the needle. Display only, so it takes no id and no Context
// mutation: pass the deflection, which is what the ballistics produce.
void needleMeter(const Context& ctx, ImVec2 tl, ImVec2 br, float deflection,
                 const NeedleScale& scale,
                 const NeedleMeterStyle& style = NeedleMeterStyle());

// -20 to +3 VU with the red zone from 0, the broadcast face and its amplitude law.
const NeedleScale& broadcastVuScale() noexcept;
// The percentage row a broadcast VU carries under its decibel row, 0 to 100.
const NeedleInnerTick* broadcastVuPercentTicks(int& count) noexcept;
// 20 dB of gain reduction down to 0, resting at full right, linear in decibels.
const NeedleScale& gainReductionScale() noexcept;

struct PillResult {
    bool toggled = false;      // the LED half was clicked
    bool labelClicked = false; // the label half was clicked
    bool hovered = false;
    ImVec2 labelAt {};         // where a menu opened by the label half belongs
};

// The module header: an LED that engages the section, and a label that opens its menu.
PillResult modulePill(Context& ctx, const char* id, ImVec2 tl, ImVec2 br, const char* label,
                      bool engaged, ImU32 accent);

struct ButtonStyle {
    ImU32 offFill = 0; // 0 takes Theme::buttonOff
    ImU32 onFill = 0;  // 0 takes Theme::textBright
    ImU32 offText = 0; // 0 takes Theme::textDim
    ImU32 onText = 0;  // 0 takes Theme::textOn
    ImFont* font = nullptr; // null takes Fonts::pill
    float fontSize = 10.0f;
    float rounding = 2.0f;
    bool border = true;
};

struct ButtonResult {
    bool clicked = false;
    bool rightClicked = false;
    bool hovered = false;
};

ButtonResult textButton(Context& ctx, const char* id, ImVec2 tl, ImVec2 br, const char* label,
                        bool on, const ButtonStyle& style = ButtonStyle());

/**
   A hardware latching button bank: a row or column of rectangular buttons of which
   exactly one is down, which is the control every Dusk faceplate had been
   re-implementing by hand.
 */
enum class BankOrientation { vertical, horizontal };

// Where each button's label sits, on the cross axis of the bank. `before` is left of a
// column and above a row; `after` is right of a column and below a row. Both carve
// `labelGutter` design pixels off that side of the box, so the buttons keep the rest.
enum class BankLabelSide { inside, before, after };

// How the selected button reads. `tab` lights a strip along its leading edge, the way a
// backlit ratio bank does; `pressed` inverts its bevel so the button looks pushed in.
enum class BankActiveStyle { tab, pressed };

struct ButtonBankStyle {
    BankOrientation orientation = BankOrientation::vertical;
    BankLabelSide labelSide = BankLabelSide::before;
    BankActiveStyle activeStyle = BankActiveStyle::tab;

    ImU32 face = 0;        // 0 takes Theme::buttonOff
    ImU32 faceActive = 0;  // 0 darkens `face`
    ImU32 border = 0;      // 0 takes Theme::pillBorder
    ImU32 tab = 0;         // the lit strip; 0 takes Theme::grHandle
    ImU32 label = 0;       // 0 takes Theme::textDim
    ImU32 labelActive = 0; // 0 takes Theme::textBright
    ImU32 caption = 0;     // 0 takes Theme::textDim

    ImFont* font = nullptr; // null takes Fonts::caption
    float fontSize = 8.0f;
    float captionSize = 8.5f;
    const char* captionText = nullptr; // drawn centred above the bank

    float gap = 3.0f;         // between buttons, along the bank axis
    float labelGutter = 26.0f;
    float labelGap = 4.0f;
    float rounding = 1.5f;
    bool shadow = true;

    // Visual position to option index, for a bank printed in an order the parameter is
    // not: a ratio column reading 20, 12, 8, 4, ALL over choices 0..4. Null is identity,
    // and the result is always an option index whichever way the face is printed.
    const int* order = nullptr;
};

struct ButtonBankResult {
    int clicked = -1; // option index, -1 when nothing was clicked this frame
    int hovered = -1;
    bool changed = false; // clicked, and it was not already the selected option
};

ButtonBankResult buttonBank(Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                            const char* const* labels, int count, int selected,
                            const ButtonBankStyle& style = ButtonBankStyle());

// A floating readout beside the pointer. Drawn on the foreground list so a later strip
// cannot cover it.
void valueBubble(const Context& ctx, ImVec2 anchor, const char* str);

// Draws the bubble the frame's live drag asked for, if any. Call once, after the last
// widget of the frame.
void drawDragBubble(Context& ctx);

struct TextFieldResult {
    bool committed = false; // Enter, or focus left the field
    bool cancelled = false; // Escape
    bool active = false;
};

// An in-place editable label. `buffer` is edited directly and stays the caller's.
TextFieldResult textField(Context& ctx, const char* id, ImVec2 tl, ImVec2 br, char* buffer,
                          std::size_t bufferSize, bool takeFocus);

// --------------------------------------------------------------------------------------------------------------------
// formatting, shared so two views cannot disagree about what 8000 Hz reads as

void formatFrequency(char* out, std::size_t size, float hz);
void formatGain(char* out, std::size_t size, float db);
void formatDecibels(char* out, std::size_t size, float db, float minusInfinityAt = -89.95f);

} // namespace DuskWidgets
