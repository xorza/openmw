# Open issues

- The analytic cone in `waterTooFineToResolveWidensTheConeItRefractsThrough` and what the frame
  measures disagree by 0.082 of a mip level. It is not the chain's band limit: asking the cone's own
  footprint rather than the whole spectrum moves the prediction by 0.009. The mip ladder the frame is
  read off carries bytes that are linear in level, and a sampler blends their *linear* values, so a
  fractional level decodes with a bias of its own — that is the next thing to rule out.

- An underwater sun shaft is not blocked by geometry above the water. A rock over the surface
  casts no gap in it.

- The surf line draws as a thin scatter of white flecks along one depth contour rather than as a
  band of foam.
