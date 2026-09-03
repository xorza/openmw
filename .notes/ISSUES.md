# Open issues

- The same binary draws one of two pictures, chosen per process and held for the life of it.
  `arkngthand`'s albedo differs between them by one of 255 on 277000 of 2073600 pixels, and its
  finished frame by up to 19 of 255 on 609. So `verify --against` reports a difference that no
  change made. The state is settled before the first frame: a second staging of the same view inside
  one process always lands on the same one of the two. With the trace pipelines compiled across
  `hardware_concurrency` threads it is about an even split, and with them compiled one at a time it
  was one run in twenty-seven — that one under a parallel build's load.

- The Vulkan pipeline cache under the temporary directory grows without bound. It reached 3.9 GiB on
  this machine, in tmpfs, because every build's pipelines join the blob that the next run loads and
  writes back.
