# Component: Scripting (`googlesql/scripting/`)

**Role:** Execute **procedural SQL scripts** — multiple statements with variables
and control flow (`DECLARE`, `SET`, `IF/ELSEIF/ELSE`, `WHILE`/`LOOP`/`REPEAT`,
`FOR`, `BEGIN…EXCEPTION…END`, `CALL`, `RAISE`, etc.). This is the layer that turns
GoogleSQL from a single-statement analyzer into something that can run a stored
procedure or a multi-statement script.

It is built **on top of** the [parser](parser.md) (which already parses scripts
into an `ASTScript`) and the [analyzer](analyzer.md) + a statement executor
(typically [`reference_impl/statement_evaluator`](reference-impl.md)).

## How it works

```
 script text
   │  parser: ParseScript()  →  ASTScript
   ▼
 ParsedScript::Create()        validate variable scoping; build control-flow graph
   │
   ▼
 ControlFlowGraph              nodes = statements, edges = normal / true / false / exception
   │
   ▼
 ScriptExecutor.ExecuteNext()  step through segments, maintaining variable state,
                               analyzing & running each statement, handling
                               loops / exceptions / CALL
```

The executor is **resumable**: it advances one segment at a time and its state
can be serialized, so an engine can checkpoint a long-running script.

## Files

- **`parsed_script.{h,cc}`** — `ParsedScript`: the validated script model. Builds
  on the AST, extracts variable declarations, and ties statements to their
  control-flow positions.
- **`script_segment.{h,cc}`** — a unit of script (a statement or a control-flow
  boundary) with its source range.
- **`parse_helpers.{h,cc}`** — traverse the AST to pull out variables/statements.
- **`control_flow_graph.{h,cc}`** — the **`ControlFlowGraph`**: models execution
  paths including branch conditions, loop back-edges, exception edges, and where
  variable scopes begin/end.
- **`script_executor.h`** — public interface: `ScriptExecutor`,
  `ProcedureDefinition` (native C++ or SQL body), execution options. The host
  supplies callbacks for analyzing/executing individual statements.
- **`script_executor_impl.{h,cc}`** — the execution loop: variable map
  management, evaluating conditions, loop/exception unwinding, `CALL` into
  procedures.
- **`stack_frame.h`** — variable scoping for nested blocks/loops/handlers.
- **`type_aliases.h`** — `VariableMap` and related aliases.
- **`serialization_helpers.{h,cc}`** — serialize/restore executor state
  (checkpoint & resume).
- **`error_helpers.{h,cc}`** — script-specific error construction (exception
  locations, variable lookup failures); related proto `proto/script_exception.proto`.

## Connections

- **Parser:** `Parser::ParseScript` produces the `ASTScript` that `ParsedScript`
  wraps.
- **Analyzer:** each statement is analyzed (with the current script variables in
  scope as query parameters/system variables) before execution.
- **Execution:** delegated to a statement evaluator — in the reference setup,
  [`reference_impl/statement_evaluator`](reference-impl.md).
- **Errors:** uses [`common/errors`](common-and-base.md) conventions.
