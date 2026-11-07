# MFL — binary package

This archive contains the `mfl` command, the example programs, the C
header and static libraries for embedding, and the documentation of
MFL (Materials & Manufacturing Language), version given by
`bin/mfl --version`.

```
bin/        mfl (the interpreter), mfl_heat_host (C host example, built)
examples/   .mfl examples (thermal/, welding/, basic/) and C sources (embedding/)
include/    mfl/mfl.h  (public C API)
lib/        static libraries mfl_core and mfl_numerical (link both, and libm on Linux/macOS)
docs/       language specification, heat conduction model, embedding guide, welding thermal models
```

## Run the examples

From the directory where this archive was extracted:

```
bin/mfl --version
bin/mfl examples/welding/heat_input.mfl                  # prints 480 J/mm
bin/mfl examples/thermal/bar_heated_end.mfl --csv bar_heated_end.csv
```

The last command runs the 1D heat conduction example (a steel bar heated
at one end) and writes `bar_heated_end.csv` (columns `time_s`,
`position_m`, `temperature_K`) and `bar_heated_end.json` (metadata). The
metadata file is written last: if it is missing, the export did not
complete. On Windows use `bin\mfl.exe`.

The C host example runs the same case through the C API and compares it
with the CSV written above:

```
bin/mfl_heat_host examples/thermal/bar_heated_end.mfl bar_heated_end.csv
```

## Exit codes of `mfl`

0 success · 1 errors in the program (compile-time or run-time, including
solver errors such as an unstable time step) or bad usage · 3 internal
failure · 4 the export files could not be written.

## Scope

The heat solver is a 1D model of a homogeneous bar (explicit finite
differences). It is a verification case of that model, not a welding
simulation: moving heat sources and 2D/3D models are planned for a later
version. See `docs/heat-conduction.md`.

Source code, issues and full documentation:
https://github.com/BeathAzreal/MFLanguage (MIT licence, see LICENSE).
