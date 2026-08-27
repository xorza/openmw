# Open issues

- Close to the camera the surf line renders as a solid pure-white ribbon with a hard edge along
  both sides, rather than as foam.

- The sea sheet ends at a straight diagonal edge visible from a high camera, with a different water
  colour beyond it.

- The analytic cone in `waterTooFineToResolveWidensTheConeItRefractsThrough` and what the frame
  measures disagree by 0.088 of a mip level, against 0.025 before the surface became an FFT field.
  Where the rest of it goes is not established.

- The water is too green near the surface, and so are the surfaces seen through a short path of it.
  Real water reads that green only after tens of metres.

- The caustics do not change with the distance from the surface. They look the same at every depth.
