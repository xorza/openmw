# Open issues

- `RtxTool::parseVec3` (`apps/rtxtool/options.cpp`) quotes only the part of the text it has not
  read yet when it refuses a later field: `--pos=1,2` is reported as `"2"`, not `"1,2"`.
- The `--warmup` help line (`apps/rtxtool/options.cpp`) says a process that compiled its launches
  draws the first stop until the driver's threads are quiet, then starts the run again warm, or
  gives up at forty-five seconds. Nothing in the tree does that.
