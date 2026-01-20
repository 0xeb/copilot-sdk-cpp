# Repro: ResumeSessionWithTools BYOK Issue

Standalone reproduction of the issue where resuming a session with new tools
fails when using BYOK (Bring Your Own Key) with OpenAI, but works with Copilot auth.

## Issue

When using BYOK with OpenAI:
- Resume session with new tool registration
- Ask AI to use the new tool
- **Tool is never invoked**

With Copilot auth: **PASS** (20s)
With BYOK/OpenAI: **FAIL** (tool never called)

## Build

```cmd
cd tests/repro
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Run

Without BYOK (uses Copilot auth):
```cmd
build\Release\repro_resume_with_tools.exe
```

With BYOK:
```cmd
copy byok.env.example byok.env
# Edit byok.env with your OpenAI credentials
build\Release\repro_resume_with_tools.exe
```

## Expected Output

With Copilot:
```
[!] Tool invoked with key: ALPHA
[PASS] Tool was invoked correctly!
```

With BYOK/OpenAI:
```
[FAIL] Tool was NOT invoked - this is the BYOK/OpenAI limitation
```
