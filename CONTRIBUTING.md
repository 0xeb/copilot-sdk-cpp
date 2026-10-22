# Contributing

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
```

## Validation

Build the library and examples, then check the generated protocol catalog:

```sh
cmake --build build --target copilot_codegen_check
```

The catalog check requires Python 3.

## Style

- Format C++ with `.clang-format` (4 spaces, Allman braces):
  - `clang-format -i src/*.cpp include/copilot/*.hpp examples/*.cpp`

## Pull Requests

- Use clear PR descriptions, include test output, and note any API/behavior changes.
- Prefer Conventional Commits-style messages (e.g., `feat(tests): ...`, `fix(example): ...`).
