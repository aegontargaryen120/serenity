# Serenity Grammar v0.1 (Parser Implementation Draft)

This document records the grammar implemented by the v0.4 recursive-descent
parser. It is intentionally a draft: language-design decisions not yet made are
not invented here.

EBNF conventions: `|` means alternative, `[ ... ]` means optional, and
`{ ... }` means zero or more repetitions. Terminals are written in quotes;
uppercase names refer to lexer token classes.

## Program and declarations

```ebnf
program                 = { top_level_declaration }, EOF ;

top_level_declaration   = global_variable_declaration
                        | function_prototype
                        | typed_function_definition
                        | signature_implementation
                        | struct_declaration
                        | enum_declaration ;

global_variable_declaration
                        = [ "mut" ], type, IDENTIFIER,
                          [ "=", initializer ], ";" ;

function_prototype      = "func", IDENTIFIER, "(",
                          [ prototype_parameters ], ")", "=>", type, ";" ;

prototype_parameters    = prototype_parameter,
                          { ",", prototype_parameter } ;
prototype_parameter     = type, [ IDENTIFIER ] ;

typed_function_definition
                        = "func", IDENTIFIER, "(",
                          [ typed_parameters ], ")", "=>", type, block ;

typed_parameters        = typed_parameter, { ",", typed_parameter } ;
typed_parameter         = type, IDENTIFIER ;

signature_implementation
                        = IDENTIFIER, "(", [ parameter_names ], ")",
                          "=>", type, block ;
parameter_names         = IDENTIFIER, { ",", IDENTIFIER } ;

struct_declaration      = "struct", "{", { field_declaration }, "}",
                          IDENTIFIER, ";" ;
field_declaration       = type, IDENTIFIER, [ "=", initializer ], ";" ;

enum_declaration        = "enum", "{", enum_member, { ",", enum_member },
                          [ "," ], "}", IDENTIFIER, ";" ;
enum_member             = IDENTIFIER ;
```

A program contains declarations only. Executable statements are permitted only
inside function bodies. The parser accepts user-defined type names syntactically
as `IDENTIFIER`; semantic analysis determines whether those names exist.

## Statements

```ebnf
block                   = "{", { statement }, "}" ;

statement               = variable_declaration
                        | assignment_statement
                        | function_call_statement
                        | return_statement
                        | if_statement
                        | while_statement
                        | for_statement
                        | break_statement
                        | continue_statement
                        | block ;

variable_declaration    = [ "mut" ], type, IDENTIFIER,
                          [ "=", initializer ], ";" ;

assignment_statement    = assignment_target, assignment_operator,
                          initializer, ";" ;
assignment_target       = IDENTIFIER, { ".", IDENTIFIER } ;
assignment_operator     = "=" | "+=" | "-=" | "*=" | "/=" | "%%=" ;

function_call_statement = function_call, ";" ;
return_statement        = "return", expression, ";" ;
if_statement            = "if", "(", expression, ")", block,
                          [ "else", ( block | if_statement ) ] ;
while_statement         = "while", "(", expression, ")", block ;
for_statement           = "for", "(", [ for_initializer ], ";",
                          [ expression ], ";", [ for_increment ], ")", block ;
for_initializer         = for_variable_declaration
                        | assignment_clause
                        | function_call ;
for_variable_declaration
                        = [ "mut" ], type, IDENTIFIER, [ "=", initializer ] ;
for_increment           = assignment_clause | function_call ;
assignment_clause       = assignment_target, assignment_operator, initializer ;
break_statement        = "break", ";" ;
continue_statement     = "continue", ";" ;
```

Variables may omit initializers. The AST preserves that omission; semantic and
runtime stages will apply the language-defined defaults. `return;` remains
unsupported. C-style `for` loops permit any of the three header clauses to be
omitted, so `for (;;) { ... }` is a valid infinite loop. A variable declared in
the initializer is scoped to the loop. `continue` proceeds to the increment
clause before reevaluating the condition; `break` exits the loop.

## Initializers and types

```ebnf
initializer             = struct_initializer | expression ;
struct_initializer      = "{", [ designated_initializer,
                          { ",", designated_initializer }, [ "," ] ], "}" ;
designated_initializer  = ".", IDENTIFIER, "=", initializer ;

type                    = "int" | "float" | "double" | "bool" | "string"
                        | "any" | "stream" | "void" (* void is return-only *)
                        | IDENTIFIER ;
```

## Expressions

The following precedence levels are listed from lowest to highest. Operators
within each level are left-associative.

```ebnf
expression              = logical_or_expression ;
logical_or_expression   = logical_and_expression, { "||", logical_and_expression } ;
logical_and_expression = bitwise_or_expression, { "&&", bitwise_or_expression } ;
bitwise_or_expression  = bitwise_xor_expression, { "|", bitwise_xor_expression } ;
bitwise_xor_expression = bitwise_and_expression, { "^", bitwise_and_expression } ;
bitwise_and_expression = equality_expression, { "&", equality_expression } ;
equality_expression    = relational_expression,
                          { ( "==" | "=/=" ), relational_expression } ;
relational_expression  = shift_expression,
                          { ( "<" | ">" | "<=" | ">=" ), shift_expression } ;
shift_expression       = additive_expression,
                          { ( "<<" | ">>" ), additive_expression } ;
additive_expression    = multiplicative_expression,
                          { ( "+" | "-" ), multiplicative_expression } ;
multiplicative_expression
                        = unary_expression,
                          { ( "*" | "/" | "%%" ), unary_expression } ;
unary_expression       = ( "+" | "-" | "!" | "~" ), unary_expression
                        | postfix_expression ;
postfix_expression     = primary_expression, { ".", IDENTIFIER } ;
primary_expression     = INTEGER_LITERAL | FLOATING_LITERAL | STRING_LITERAL
                        | "true" | "false" | "null" | IDENTIFIER
                        | function_call | "(", expression, ")" ;
function_call          = IDENTIFIER, "(", [ argument_list ], ")" ;
argument_list          = expression, { ",", expression } ;
```

String escape decoding and backslash-newline string continuation are handled by
the lexer. Both quote styles produce `STRING_LITERAL` tokens.


## v0.12 primitive I/O additions

The runtime provides immutable built-in stream identifiers `stdin`, `stdout`,
and `stderr`, all of type `stream`. Two built-in calls are available:

```ebnf
write_call              = "write", "(", expression, ",", expression, ")" ;
read_call               = "read", "(", expression, ")" ;
```

`write(stream, any)` returns `void`; it writes a printable primitive to
`stdout` or `stderr` without adding a newline. `read(stream)` returns a
nullable `string`, reading one line from `stdin` and returning `null` at EOF.
The `any` type is a tagged, type-erased value used by generic I/O wrappers;
it does not introduce implicit conversions to other declared types.

The standard header `std/io.snh` provides the Serenity-level wrappers
`print`, `println`, `input`, and `error`. See
`docs/STANDARD_IO_v0.12.md` for their exact behavior.
