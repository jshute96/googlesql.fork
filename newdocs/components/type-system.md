# Subcomponent: Type System & Values (`googlesql/public/types/`, `public/value.*`)

**Role:** The in-memory model of SQL **types** and **values**. Every resolved
expression has a `Type`; every literal/result is a `Value`. This subsystem is
used by *every* other component, so it's worth understanding on its own.

Parent: [public-api.md](public-api.md).

## Type hierarchy

`Type` (abstract, `types/type.{h,cc}`) is the base; concrete subclasses:

```
Type
├── SimpleType        INT32/64, UINT32/64, FLOAT/DOUBLE, BOOL, STRING, BYTES,
│                     DATE, TIME, DATETIME, TIMESTAMP, INTERVAL, NUMERIC,
│                     BIGNUMERIC, JSON, UUID, GEOGRAPHY, TOKENLIST, …
├── ArrayType         ARRAY<T>
├── StructType        STRUCT<name T, …>  (case-insensitive field lookup)
├── ProtoType         a protobuf message type
├── EnumType          a protobuf enum type
├── MapType           MAP<K, V>
├── RangeType         RANGE<T>
├── RowType           a row/tuple type (+ RowTypeWithCatalog)
├── GraphElementType  property-graph node/edge
├── GraphPathType     property-graph path
├── MeasureType       a measure/metric type
└── ExtendedType      engine-defined custom types
```

Files: `simple_type.*`, `array_type.*`, `struct_type.*`, `proto_type.*`,
`enum_type.*`, `map_type.*`, `range_type.*`, `row_type.*`,
`row_type_with_catalog.*`, `graph_element_type.*`, `graph_path_type.*`,
`measure_type.*`, `extended_type.*`.

## TypeFactory

`types/type_factory.{h,cc}` — **`TypeFactory`** creates and **interns** all
`Type` instances and owns their lifetime. Two `INT64`s, or two identical
`ARRAY<STRUCT<...>>`s, return the **same `const Type*`**, so type equality is
pointer equality and types are cheap to pass around. A `Type*` is only valid as
long as its `TypeFactory` lives — engines keep a long-lived factory.

Related: `type_deserializer.*` (rebuild types from proto), `type_modifiers.*`,
`type_parameters.*` (e.g. `STRING(10)`, parameterized NUMERIC),
`annotation.*`/`collation.*` (annotations on types — see
[public-api.md](public-api.md)), `timestamp_util.*`, `internal_proto_utils.*`.

## Values

`public/value.{h,cc}` — **`Value`**, an immutable, typed runtime value:
- Holds a `Type*` plus content. Scalars are stored inline; arrays/structs are
  **reference-counted** so copies are cheap.
- **NULLs are typed** — a NULL has a `Type` (`Value::NullInt64()`, etc.).
- Construction/access via typed helpers (`Value::Int64(...)`, `.int64_value()`,
  `.is_null()`, `.type()`).

Specialized scalar value representations (used inside `Value` and by functions):
- `numeric_value.*` — fixed-precision `NUMERIC`/`BIGNUMERIC`.
- `json_value.*` — JSON documents.
- `uuid_value.*`, `interval_value.*`, `timestamp_picos_value.*`.
- `civil_time.*`, `pico_time.*`, `time_zone_util.*`, `numeric_parser.*` —
  date/time and number parsing helpers.
- `common/internal_value.*` provides internal Value helpers;
  `common/multiprecision_int*.*` backs big-integer math.

## Why it matters when changing code

- Adding a **type** touches: `types/` (new `Type` subclass + factory method),
  `Value` support, serialization (`type.proto` / `type_deserializer`), coercion
  (`public/coercer`, `cast`), the [analyzer](analyzer.md) (type-syntax resolution
  in `resolver_stmt.cc`), and the [reference impl](reference-impl.md) for
  evaluation.
- Type **equality, nullability, and coercion** rules are central to analyzer
  behavior; the `Coercer` (`public/coercer.*`) and `cast` catalog encode them.

## Connections

- Created/owned via `TypeFactory`, threaded through `AnalyzerOptions`.
- Referenced by every [Resolved AST](resolved-ast.md) expression node and
  `ResolvedColumn`.
- Produced/consumed as `Value`s by the [reference impl](reference-impl.md) and
  the [evaluator](public-api.md).
