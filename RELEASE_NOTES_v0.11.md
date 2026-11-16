# Serenity v0.11.0 — Standard Library Foundation

- Adds `stdlib/std/math.snh` with integer, float, and double helpers.
- Adds standard-library fallback lookup after including-file-relative lookup.
- Supports explicit `PreprocessorOptions::standardLibraryDirectory` and the
  `SERENITY_STDLIB_PATH` environment override.
- Keeps standard-library functions as ordinary Serenity source, shared by
  `sninterp`, `snjit`, and `serenity` without backend-specific built-ins.
- Adds an interpreter standard-library integration test and a macOS LLVM AOT
  smoke test that compares AOT, interpreter, and JIT output.

Configure/build/test as usual. No LLVM API or backend unification changes are
included in this release.
