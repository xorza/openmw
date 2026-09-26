# Review of the fork's diff against upstream

Whoever addresses an item deletes it.

Scope: everything `git diff 3ee798e988 HEAD` adds or changes, tests excluded. Each item was
re-checked against the code, and only items worth addressing are kept. The tag after a group's name
is the section of `.notes/fork-redesign.md` that addresses it.

## The crash monitor acts on a process it does not hold [R10]

- [ ] On macOS hang handling still acts on a PID number: the hang request and End are `kill` by id, so
      while the "not responding" box is up the game may exit and its id be reused, and End still
      kills that. `crashpadmonitor.cpp` (`Client::send`), where Linux holds a pidfd and Windows a
      handle.
- [ ] On macOS the hang signal handler allocates inside `crashpad::SimulateCrash`, so a thread hung on
      the malloc lock deadlocks in the handler. `crashpadclient.cpp` (`onHangSignal`,
      `reportAndContinue`).
