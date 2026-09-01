# Open issues

- A view that holds emitters renders a different frame in `openmw-rtxtool verify` depending on which
  views the run rendered before it. Two runs of one binary over the same list agree exactly. A run of
  `--views=balmora-storm-night` against a reference written by
  `--views=addamasartus,vivec-canalworks,balmora-storm-night` reports
  `differs: worst 91 of 255 on 13.46% of the pixels`, and the same three-view list against that same
  reference reports every view the same. Views without emitters agree across every list.
