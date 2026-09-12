# Open issues

- `components/terrain/objectstorage.cpp:140` and `components/terrain/objectpaging.cpp:552` build with `-Wmissing-field-initializers` warnings on designated initialisers, and `objectpaging.cpp` with a `-Wmaybe-uninitialized` on `InstanceList`.
