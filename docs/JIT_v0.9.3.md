# Serenity JIT v0.9.3 hotfix

## Deoptimization accounting

The JIT now distinguishes three native trace return reasons:

- `SideExit`: a recorded branch condition no longer matches; execution resumes in the interpreter at the alternate bytecode path.
- `Budget`: the native trace reached the remaining instruction budget.
- `Deoptimization`: a runtime guard failed (for example, checked integer arithmetic overflow, division/remainder by zero or signed overflow, invalid shift counts, overflowing left shifts, or integer negation of `INT64_MIN`).

A guard deoptimization restores modified locals/globals and the complete operand stack as it existed before the guarded bytecode instruction, then sets the interpreter instruction pointer to that instruction. The interpreter therefore produces the language's canonical runtime diagnostic.

The `deoptimizations` statistic now counts these guard exits. `sideExits` remains reserved for changed control-flow paths, while `budgetExits` counts instruction-budget exits.
