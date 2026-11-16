# Serenity Parser v0.4

The parser consumes the token vector returned by `Lexer::tokenize()` and
constructs the syntax-oriented AST defined in `serenity/ast/ast.hpp`.

## Public interface

```cpp
serenity::Lexer lexer(source);
auto lexed = lexer.tokenize();

serenity::parser::Parser parser(std::move(lexed.tokens));
auto parsed = parser.parse();

if (!parsed.ok()) {
    // Inspect parsed.errors.
}
```

`ParseResult` contains a `std::unique_ptr<ast::Program>` and a list of syntax
errors. The parser retains successfully parsed declarations where possible,
so the result can contain a partial AST when errors are reported.

Each `ParseError` contains a message and a `SourceLocation`. AST nodes carry
half-open source spans in the preprocessed source buffer. The preprocessor's
`PreprocessResult::originAt(span.begin.offset)` can map those offsets back to
the original `.sn` or `.snh` source.

## Parsing strategy

- Recursive descent is used for declarations and statements.
- Precedence climbing is used for binary expressions.
- Unary operators are parsed recursively.
- Binary operators are left-associative at each precedence level.
- The parser owns its token stream and ensures an EOF token is present.
- Error recovery synchronizes at declaration and statement boundaries.

## Supported top-level declarations

- Global variable declarations, with optional `mut` and optional initializer.
- Function prototypes.
- Typed function definitions.
- Signature-based function implementations.
- Struct declarations.
- Enum declarations.

Executable statements at top level are rejected. They must appear inside a
function body.

## Supported statements

- Local variable declarations.
- Assignments and compound assignments.
- Function-call statements.
- `return expression;`.
- `if` / `else if` / `else` with parenthesized conditions and braced bodies.
- `while` with a parenthesized condition and braced body.
- `break;` and `continue;`.
- Nested blocks.

`for` is tokenized as a reserved keyword but is not parsed in v0.4 because its
syntax has not yet been specified. `return;` is not supported; a return
expression is required by the current grammar.

## Functions

The parser recognizes these forms:

```serenity
func add(int, int) => int;

func add(int a, int b) => int {
    return a + b;
}

add(a, b) => int {
    return a + b;
}
```

Prototype parameters may omit names. Parameters in typed definitions and
signature-based implementations must have names. Signature-based implementations
store names without types; matching them to a prior prototype is semantic work.
The parser does not perform overload resolution or enforce declaration order.

## Expressions

The parser supports integer, floating-point, string, boolean, and `null`
literals; identifiers; function calls; member access; unary expressions; binary
expressions; and designated struct initializers.

Binary precedence, from lowest to highest:

1. `||`
2. `&&`
3. `|`
4. `^`
5. `&`
6. `==`, `=/=`
7. `<`, `>`, `<=`, `>=`
8. `<<`, `>>`
9. `+`, `-`
10. `*`, `/`, `%%`

Unary operators are `+`, `-`, `!`, and `~`.

Assignment is parsed as a statement rather than an expression. The assignment
target must syntactically be an identifier or member-access expression; name
resolution and mutability checks belong to semantic analysis.

## Deliberate non-goals

The parser does not perform name lookup, type checking, default-value
materialization, function signature matching, duplicate declaration checks,
control-flow validation, or overload resolution. It also does not yet parse
arrays, indexing, `for` loops, a `void` type, or a module system.
