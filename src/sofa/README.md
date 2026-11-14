# libmysofa's SOFA reader, in reAL

`hdf/` and `hrtf/` are the HDF5 and SOFA reading code of [libmysofa](https://github.com/hoene/libmysofa) 1.3.5
(commit 648eed0), under its BSD licence (`LICENSE`), for `hrtf=` with a `.sofa` file (`src/hrtf_sofa.c`). Only the
reader is here: libmysofa's lookup, interpolation, resampling and cache are not used.

What reAL changed or added:
- `inflate.c` (reAL's own) in place of `hdf/gunzip.c`, which needed zlib: `gunzip()` for HDF5's deflate filter.
- `hrtf/config.h` and `hrtf/mysofa_export.h`, which libmysofa's CMake generates.
- `hdf/btree.c`, `treeRead`: a chunk's element count is checked as it is multiplied out (it overflowed `int`).
- `hdf/btree.c`, `treeRead`: a chunk's bytes go to their places a row at a time instead of with two divisions
  per byte (the same places: every array of libmysofa's test files and of 450 damaged copies reads the same); half
  the reading time.
- `hrtf/reader.c`, `getHrtf`: a file refused after its attributes were read frees them too (they leaked).
- Built with `-fwrapv`: the reader counts on `int` arithmetic wrapping.
  Lines changed are marked `reAL:`.
