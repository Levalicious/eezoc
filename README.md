# eezoc

The Eezo compiler: `.eezo` source to bytecode (`-f bcl|jot|jomplement|xbcl`) or a standalone ELF (`-e`; `-e -i` a
Lazy-K stream program, `-e -m` a program of the IO monad). Links
[libeezo](https://github.com/Levalicious/libeezo) from `../libeezo`; `#import name` resolves against `$EEZO_STDLIB`,
else the `$PREFIX/share/eezo/stdlib` compiled in (where `mk install` in [stdlib](https://github.com/Levalicious/stdlib)
puts the modules), else a `stdlib/` directory beside the importing file.

Build: `mk` (see [mk](https://github.com/Levalicious/mk), [mkroot](https://github.com/Levalicious/mkroot)).
Dependencies and their pinned commits: `deps.lock`; `ci/deps.sh` fetches them beside this checkout - the workspace
layout the [umbrella](https://github.com/Levalicious/umbrella) repository lays out. Tests (`tests/`: e2e, modes,
gc_regress, stream, io, xbcl; bench is not run by CI) need [eezo](https://github.com/Levalicious/eezo) beside it and the
stdlib installed; `EEZO_WS`, `EEZOC`, `EEZO` override where the scripts look.
