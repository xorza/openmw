# Open issues

- `Rtx::BenchHeader::mPreset` (`components/rtxbench/benchrecord.hpp`) is never assigned: `Session::endStop` fills `mUpscale` off the renderer and leaves the preset at its default, so the JSON a `bench` writes reports preset `d` whatever the run pinned.
