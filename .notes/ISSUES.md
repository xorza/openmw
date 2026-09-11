# Open issues

- A view's `look` is ignored while its `pos` is honoured, at least in an interior: two opposite
  `look` values for `mournhold-arrival` give byte-identical `shot.png`, and moving `pos` changes it.
  The camera comes out facing whatever the player faces.
