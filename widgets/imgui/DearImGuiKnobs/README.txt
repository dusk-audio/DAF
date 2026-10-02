Taken from https://github.com/altschuler/imgui-knobs main branch with commit 1126a5f8c71dea5d8b228144db4957a8a265b02b.

Files are used as-is except for these local changes:

- adjusted include paths
- AddBezierCurve renamed to AddBezierCubic
- the knob position is clamped to [0, 1] when drawing, so a value outside [v_min, v_max]
  (typed into the value box, or set by the caller) no longer over-rotates or breaks the drawing;
  it is also computed after the value box, so the knob shows the current value
- new flag ImGuiKnobFlags_AlwaysClamp (1 << 4): passes ImGuiSliderFlags_AlwaysClamp to dragging and
  the value box, and clamps the value to [v_min, v_max] afterwards
- new flag ImGuiKnobFlags_Logarithmic (1 << 5): passes ImGuiSliderFlags_Logarithmic to dragging and
  the value box, and draws the knob position in log space when v_min and v_max are both > 0
  (linearly otherwise)

The values of the existing flags are unchanged. tests/ImGuiKnobs.cpp covers the local changes.
