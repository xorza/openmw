# Open issues

`Rtx::DistantLights::build` reads `mOffDefault` off a `SceneUtil::LightCommon` to decide whether a
record burns, which is the rule `Rtx::castsWherePlaced` states for an `ESM::Light`. One rule, two
spellings, and only one of them has a test.
