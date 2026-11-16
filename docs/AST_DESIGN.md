# Serenity AST design, milestone 0.3

The AST is a syntax-oriented representation of Serenity programs. It does not
perform name lookup, type checking, overload resolution, or default-value
materialization. Those belong to semantic analysis and the runtime.

## Top-level nodes

A `Program` contains declarations only:

- `GlobalVariableDecl`
- `FunctionDecl` (prototype or definition)
- `StructDecl`
- `EnumDecl`

Executable statements are therefore represented only beneath function bodies.
There is no top-level statement node in `Program`.

## Statements

Function bodies contain `BlockStmt` nodes. Blocks can contain variable
declarations, assignments, expression statements (the parser will initially
restrict these to function calls), returns, conditionals, loops, `break`,
`continue`, and nested blocks.

`for` remains a reserved keyword. No `ForStmt` exists until its syntax is
specified.

## Expressions

The current expression nodes cover integer/floating/string/bool/null literals,
identifiers, unary and binary operations, member access, function calls, and
designated struct initializers.

Numeric literals retain their source spelling. In particular, the parser does
not decide whether a floating literal is an `float` or `double`; semantic
analysis will use the surrounding context and Serenity's eventual literal
conversion rules.

String literals store the decoded value supplied by `Token::stringValue`.
Both quote styles therefore produce the same AST node.

## Function declarations and implementations

`FunctionDecl` represents all three currently designed forms:

- `Prototype`: `func add(int, int) => int;`
- `TypedDefinition`: `func add(int a, int b) => int { ... }`
- `SignatureImplementation`: `add(a, b) => int { ... }`

The last form deliberately stores parameter names without types. Semantic
analysis must find a matching earlier prototype and transfer/check its
signature. The AST does not implement that lookup itself.

## Default initialization and null

A missing initializer is represented by a null `VariableSpec::initializer`
pointer. It does not mean the Serenity value is the `null` literal.

The semantic/runtime layers will apply the language rules:

- `int`, `float`, `double` -> zero
- `bool` -> `false`
- `string`, structs, enums, and other reference-like/user-defined values -> `null`

An explicit `null` expression is represented by `NullLiteralExpr`.

## Source locations

Every node carries a `SourceSpan` with begin and exclusive-end locations in the
preprocessed source buffer. Use the preprocessor's `PreprocessResult::originAt`
with the span's expanded-source offset to recover the original `.sn` or `.snh`
file and location.

## Ownership

Tree children are owned through `std::unique_ptr`. This gives the AST a single,
clear owner for every expression, statement, and declaration and avoids shared
mutable subtrees. Types and small metadata records are stored by value.
