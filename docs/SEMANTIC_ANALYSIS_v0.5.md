# Serenity Semantic Analysis v0.5

This document describes the semantic rules implemented by the v0.5 analyzer.
It supplements the parser grammar; it does not change the source syntax.

## Responsibilities

The semantic analyzer runs after preprocessing, lexing, and parsing. It performs:

- User-defined type registration and resolution.
- Struct field and enum member validation.
- Global and local variable declaration checks.
- Lexical scope and identifier resolution.
- Function prototype/definition matching.
- Function call argument-count and argument-type checks.
- Expression type analysis.
- Assignment and mutability checks.
- Return-type checks.
- Boolean condition checks.
- `break` and `continue` loop-context checks.

The analyzer does not modify the AST. It returns a `SemanticModel` containing an
expression-to-type map, which later bytecode and IR generation can consume.

## Types

Primitive types:

| Type | Semantic kind |
|---|---|
| `int` | Integer category; exact storage width is to be specified |
| `float` | Single-precision floating-point category |
| `double` | Double-precision floating-point category |
| `bool` | Boolean |
| `string` | Nullable string reference |

Struct and enum names introduce additional nominal types.

There are no implicit conversions between `int`, `float`, and `double`. Types must
match exactly. A floating literal adopts an expected `float` or `double` type in
a typed context; otherwise, an unconstrained floating literal defaults to
`double`. An integer literal has type `int`.

`null` is compatible with `string`, struct, and enum types. It is not compatible
with `int`, `float`, `double`, or `bool`.

## Default initialization

A declaration without an initializer remains initializer-free in the AST. The
language/runtime default is determined from its declared type:

| Type | Default |
|---|---|
| `int` | `0` |
| `float` | `0.0` |
| `double` | `0.0` |
| `bool` | `false` |
| `string` | `null` |
| Struct | `null` |
| Enum | `null` |

Omitted fields in a designated struct initializer receive the same type-based
defaults. Explicit field defaults are checked against the field's declared type.

## Variables and scopes

Variables are immutable by default. `mut` permits assigning a new value to the
variable itself. Local scopes may shadow names from outer scopes, but duplicate
names in the same scope are rejected.

Current v0.5 object rule: struct variables are treated as reference-like values.
`mut` controls rebinding the variable; fields of a referenced struct may be
updated even when the variable binding itself is immutable. This is a deliberate
initial semantic rule and can be revised if Serenity adopts value semantics or
field-level mutability later.

Global variables are visible inside functions. Executable statements remain
restricted to function bodies.

## Functions

Function overloading is rejected. A function name has one signature.

A prototype may be followed by a matching typed definition or signature-based
implementation. A signature-based implementation must follow a prototype:

```serenity
func add(int, int) => int;

add(a, b) => int {
    return a + b;
}
```

Parameter types and return types must match. Function calls must supply the
correct number and types of arguments. Parameter names must be unique within a
function.

The initial analyzer does not yet diagnose prototypes that have no definition;
that check is deferred until linkage/runtime boundaries and external functions
are specified.

## Expressions and operators

- `+`, `-`, `*`, `/`: operands must have the same numeric type.
- `%%`: requires two `int` operands.
- `<`, `>`, `<=`, `>=`: require matching numeric operand types and return `bool`.
- `==`, `=/=`: require compatible operand types and return `bool`.
- `&&`, `||`, `!`: operate on `bool`.
- `&`, `|`, `^`, `~`, `<<`, `>>`: operate on `int`.
- Unary `+` and `-` require numeric operands.

Mixed numeric arithmetic is rejected. There are no implicit promotions.

## Structs and enums

Struct field names must be unique. Field initializers must match the declared
field types. Designated initializers may omit fields because omitted fields
receive their type-based defaults. Unknown and duplicate designated fields are
errors.

Enum member names must be unique within their enum. Enum values are referenced
using qualified syntax such as `Direction.North`.

## Built-in output functions

For initial development, `print(value)` and `println(value)` are recognized as
built-in procedures. Each accepts exactly one `int`, `float`, `double`, `bool`,
or `string` argument and has an internal `void` result. `void` is not currently
a user-declarable Serenity type. These built-ins can be replaced by declarations
from a future standard library/runtime design.

## Not implemented yet

- `for` loops and arrays/indexing.
- Generic types and function overloading.
- Explicit casts and numeric promotions.
- Definite-return / unreachable-code analysis.
- Missing function-definition diagnostics.
- Function types, pointers, and user-defined nullable annotations.
- Runtime emission of default values (the analyzer validates types; the future
  interpreter/code generator must materialize the defaults).
