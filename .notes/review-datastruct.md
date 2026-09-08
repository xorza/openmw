Delete an item when it is addressed. This file lists open findings only.

# Data-structure review: the whole RTX suite

Reviewed 2026-09-06 against a clean tree on `74d6702ce1`.

Scope: `components/rtx/`, `components/rtxvulkan/`, `components/rtxbackends/`,
`components/rtxbench/`, `components/surface/`, `components/myguirtx/`,
`apps/openmw/mwrender/rtx/` and `apps/rtxtool/`.

Static review of types, ownership and call graphs. Test structure and the API that tests use are out
of scope. No measurements were taken.

---

## Documentation blocks that describe the wrong member

Each of these reads as the member's contract and belongs to a different one. A reader who trusts them
is misled about ownership.

- [ ] **`sceneacceleration.hpp:304`.** The block describes the blocked position buffer — host
      writes, `SkinPass`, the refit's copy, the bind pose. The member under it is
      `Owned<VkQueryPool, vkDestroyQueryPool> mCompactable`. The real `mPositions` at line 340 has no
      comment. Two doc blocks are also merged: the compaction pool's own text starts mid-block at
      "One slot per mesh".

- [ ] **`texture.hpp:99`.** The `TextureArray` class doc runs into the `SetApart` doc, so `SetApart`
      keeps the last sentence and `class TextureArray` has none.

- [ ] **`instancerecord.hpp`.** `updateInstanceRecords` carries two summary sentences from two
      merged blocks. `InstanceRecord::mMask` has a blank comment line in the middle of a sentence.
