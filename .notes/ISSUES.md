# Open issues

- `build-debug-asan/` no longer links `components-tests`: `mold: undefined symbol:
  testing::internal::PrintStringTo` and `GetWithoutMatchers`. The directory was configured against
  an older gtest than the installed 1.18, so the sanitizer run `debug-asan.sh` promises is not
  available until the directory is deleted and made again.
