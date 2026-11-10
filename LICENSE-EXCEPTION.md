# BSI-Quest licence

Copyright (C) 2026 the BSI-Quest contributors.

BSI-Quest is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, version 3 of the License (the full text is in `LICENSE`), with the
additional permission below.

BSI-Quest is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

## Additional permission under GNU GPL version 3 section 7

BSI-Quest exists to run a game that its users own. A working build therefore combines BSI-Quest with code that is not,
and cannot be, under the GPL. This permission makes that combination allowed.

"Translated Game Code" means code, in source or object form, produced by mechanically translating the executable files
of a lawfully obtained copy of the game BioShock Infinite (including its bundled libraries) with BSI-Quest's translator,
together with the tables and stubs the translator generates for it.

If you modify BSI-Quest, or any covered work, by linking or combining it with Translated Game Code, the copyright
holders of BSI-Quest grant you additional permission to convey the resulting work. The Corresponding Source for a
non-source form of such a combination shall include the source code of the parts of BSI-Quest used, as well as that of
the covered work, but need not include the Translated Game Code.

This permission does not extend to any other code. Third-party code under the GPL without this permission must not be
combined into a build that contains Translated Game Code.

As section 7 of the GPL provides, you may remove this additional permission from a copy of BSI-Quest that you convey.

## What this licence does not cover

- **The game.** BioShock Infinite, its code, its data and the Translated Game Code belong to their owners. This licence
  grants no rights in them. You need your own copy of the game.
- **Files under other licences**, which keep those licences:
  - `patches/dxvk/` : changes to DXVK, under DXVK's zlib/libpng licence, so they can be offered upstream.
  - `patches/mesa/` : changes to Mesa, under Mesa's MIT licence, for the same reason.
  - SMAA (`kit/src_b1n/linux/smaa_*`, `SMAA_LICENSE.txt`, `runtime/linux/aa76/smaa/`, and the SMAA parts of the generated
    `*_spv.h` headers): MIT, copyright the SMAA authors.

## Contributions

By contributing to this repository you agree that your contribution is licensed under the same terms: GPL version 3
with the additional permission above (or the upstream licence, for files in `patches/dxvk/` and `patches/mesa/`).
