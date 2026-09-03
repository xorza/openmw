# Open issues

- The first `verify` run after a rebuild draws two interiors differently from every run after it.
  `andrano-tomb` moves 4 pixels and `arkngthand` 609 of 2073600, worst 19 of 255, scattered over the
  whole frame. A second run of the same binary reproduces the previous build's frames byte for byte,
  so `verify --against` reports a difference that no code change made.
