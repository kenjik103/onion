# Working rules

The full project spec is imported below. Read it before starting any milestone.

@SPEC.md

## Design principles

- Don't overengineer. Simple beats complex.
- No fallbacks. One correct path, no alternatives.
- One way to do each thing, not many.
- Clarity over backward compatibility.
- Throw errors. Fail fast when preconditions aren't met.
- No backups. Trust the primary mechanism.
- Separation of concerns. Each function has a single responsibility.
- Let the compiler and type system catch errors instead of adding excessive runtime checks.

## Code style: procedural, not object-oriented

- **Plain data, free functions.** Data lives in plain structs with public fields and no behavior. Logic lives in free functions that take data as parameters and return results. No member functions; initialize structs with aggregate initialization.
- **No OOP machinery.** No inheritance, no virtual functions, no abstract base classes, no getters/setters, no design patterns (factories, strategies, visitors, builders-as-classes, singletons).
- **Explicit data flow.** A function's inputs are its parameters and its outputs are its return value or explicitly passed output arguments. No hidden state, no globals. The one exception is process-wide library configuration set once at startup (e.g. the GDAL config option in SPEC.md 8.1).
- **Pipeline stages are functions called in sequence** from each subcommand's entry point. Group related functions with namespaces and files, not classes.
- **"Interface" in SPEC.md means a data or file boundary,** not a C++ type. The scene spec and the scene geometry contract are structs and files. Scanner swappability comes from the contract files; there is no `Scanner` base class.
- **RAII for resources only.** Wrapping a C handle so it's released (e.g. `GDALDatasetUniquePtr`, `std::unique_ptr` with a custom deleter) is fine; that's resource cleanup, not object design.
- **Third-party APIs are used as they come.** DuckDB, GDAL, and Polyscope have object-based APIs. Call them directly from free functions. Don't wrap them in classes of your own.

## Abstraction

- **Abstract only what already repeats.** Extract a shared function only when the same logic exists in two or more places in the code today. One use is not a pattern.
- **Never abstract preemptively.** No generic code, templates, configuration knobs, or extension points for uses that don't exist yet, including uses listed in SPEC.md section 12. Write the concrete version; generalize when the second real use arrives.
- **Duplication beats the wrong abstraction.** Two similar blocks that might diverge stay separate until they are proven identical.

## How to work

- **Surgical changes only.** Make minimal, focused edits.
- **Detective workflow for every bug:**
  1. State a theory of the cause.
  2. Collect evidence with minimal, targeted logging or a measurement.
  3. Fix only once the evidence proves the theory.
  Never apply a fix backed only by a plausible theory.
- **Fix root causes,** not symptoms.
- **Gates are hard stops.** At every GATE in SPEC.md section 9, present the evidence and wait for the user's approval.
- **Never guess a data format.** Inspect it, record what you found, then code against it.
- **Collaborate.** When there's a real choice to make, lay out the options with evidence and let the user pick. Don't decide open questions in SPEC.md section 11 yourself.
- **No new dependencies** beyond GDAL, DuckDB, and Polyscope without asking.
- **No sensor physics.** The scanner stays exactly as specified in SPEC.md section 6.9. The user's proprietary simulator owns everything beyond it.
