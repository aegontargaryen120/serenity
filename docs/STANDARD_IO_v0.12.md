# Serenity standard I/O primitives v0.12

The runtime exposes three immutable built-in stream handles: `stdin`, `stdout`,
and `stderr`. They have the built-in type `stream`.

The low-level primitives are:

- `write(stream destination, any value) => void`: writes one printable primitive
  to `stdout` or `stderr` without adding a newline. Supported values are `int`,
  `float`, `double`, `bool`, and non-null `string`.
- `read(stream source) => string`: reads one line from `stdin`, excluding the
  line ending. It returns `null` at end-of-file.

`read` only accepts `stdin`; `write` accepts `stdout` or `stderr`. Using the
wrong stream is a runtime error. `any` is an explicit dynamically tagged value
type intended here to let the standard-library wrappers forward printable
primitive values while keeping the rest of the language statically typed.

The standard header `std/io.snh` defines:

- `print(any)`: writes to `stdout`, no newline.
- `println(any)`: writes to `stdout`, followed by a newline.
- `input()`: reads one line from `stdin`; returns `null` at EOF.
- `error(any)`: writes to `stderr`, no newline.

The old `print` and `println` compiler intrinsics remain as a compatibility
fallback for source files that do not include `std/io.snh`. When the header is
included, its Serenity functions take precedence.
