# Open issues

- A reference the game moves, rotates or scales at run time, or one `World::getAnimation` takes
  over, reaches `RenderingManager::pagingBlacklistObject` and the paging's blacklist, but not
  `MWRender::Renderer`: once its cell leaves the active grid, the cell ring stands it again at the
  content file's position, where the paging stands nothing. `pagingEnableObject` already tells the
  renderer by `enableReference`; the blacklist has no such line.

- `Rtx::descendInWorld` says a ray is owed an LOD's finest child, and hands the visitor every
  child: `osg::LOD::traverse` under `TRAVERSE_ALL_CHILDREN` visits all of them, so a `NiLODNode`
  with more than one level is traced with every level standing at once.
