# Open issues

- The same binary draws one of two pictures, and which one is settled before the first frame.
  `arkngthand`'s albedo differs between them by one of 255 on 277000 of 2073600 pixels, and its
  finished frame by up to 19 of 255 on 609 — so `verify --against` reports a difference that no
  change made. Forty-seven `shot --albedo` runs of that view fell into exactly two pictures, and
  both of the builds tried drew both. The mixture drifts: six runs one hour split three and three,
  and twenty later ones agreed, idle and under a full processor load alike. Inside one process a
  second staging of the same view always draws the same one of the two.
