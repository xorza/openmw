# Review: whole diff against `upstream/master` — what remains

Delete an item when you address it. Delete a heading when its items are gone. Everything else the
review found is landed.

## Comments narrate history and restate code

A third of every line in the RTX places is a comment line (`components/rtx` 8194 of 23975,
`rtxvulkan` 5468 of 19646), ~1900 open with a bolded thesis, and about a hundred still tell what the
code used to be or quote a number measured once. The seam and the cited passages are swept; the rest
is not.

- [ ] A file-by-file pass over `components/rtx`, `components/rtxvulkan`, `components/rtxbench`,
      `apps/rtxtool`: keep an invariant, a workaround and its cause, or a trade-off; delete what the
      code used to be, a number measured once, or what the line under it does.
- [ ] `myguirtx/rendermanager.hpp`: a `/*internal:*/` marker comment splits the public section.
