# Open issues

- `components-tests` does not compile in `build-release`: `apps/components_tests/rtx/visibility/fog.cpp:377`
  fails `-Werror=null-dereference`.
- `RtxFramingTest.theClockAndTheWeatherMoveTheSkyAndLeaveTheCellAlone` passes only in a full run. Under
  a filter that skips whichever test seeds `Fallback::Map` it throws for a missing
  `Weather_Clear_Sky_Sunrise_Color`.
- `CI/check_clang_format.sh` fails on `components/rtx/shaders/camera.h` with clang-format 14.
