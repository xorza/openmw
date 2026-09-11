# Open issues

`apps/openmw/mwrender/rtx/session.cpp:104` builds `Rtx::Route` from one designated initializer
and warns `-Wmissing-field-initializers` for `mTo` and `mLookTo`.

`openmw-rtxtool map` ended with `*** Fatal Error ***` and wrote no file on one run, immediately
after an `openmw-rtxtool scene` process had exited. It has not happened again.
