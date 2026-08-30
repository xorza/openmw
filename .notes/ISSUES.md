# Open issues

- `openmw-rtxtool doll` and `openmw-rtxtool map` write different bytes on two runs of one binary,
  with the same arguments. `shot` writes the same bytes twice.
- `openmw-rtxtool bench` reports a different `structureBytes` and `tableBytes` for an exterior place
  on two runs of one binary with the same arguments. Every other figure of that place agrees.
