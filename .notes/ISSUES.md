# Open issues

`apps/openmw/mwrender/rtx/session.cpp:104` builds `Rtx::Route` from one designated initializer
and warns `-Wmissing-field-initializers` for `mTo` and `mLookTo`.
