# Data structure review — the whole diff against `upstream/master`

Scope: every file the fork adds or changes against `aaf769f511`. The review looks at data
structures, ownership and call graphs. It ignores test structure and the shapes tests use.

**Delete an item when you address it.** This file lists open findings only. Do not mark an item as
done, and do not keep a history section.

---

## Parallel arrays and nested collections stand in for one row

- [ ] `Rtx::DistantLights::mCells` is a `std::map<osg::Vec2i, osg::ref_ptr<osg::Group>>`
  (`distantlights.hpp:114`), which allocates a node for each cell it remembers.
