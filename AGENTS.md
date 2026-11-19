# JC2 Agent Instructions

These instructions are loaded automatically for work in this repository. Follow them before using tools or changing files.

## Authorization gate

- A question, observation, report, or request for information is **not** an implementation instruction.
- Modify files only when the user uses an explicit imperative such as "implement", "fix", "write", "create", or "change".
- For any code change, present the plan first and wait for explicit approval.
- When a request is ambiguous, ask one focused question instead of guessing.
- After a correction or revert, stop. Do not re-implement the same thing, add variants, or continue with adjacent work.

Examples:

```text
"Can this be encapsulated?"       -> answer only; do not create a module
"Where should this file go?"      -> answer only; do not create the file
"Create the module"               -> plan first, then implement after approval
```

## Repository boundaries

- Never place personal scripts, credentials, API wrappers, or machine-specific configuration in `modules/`, `src/`, `test/`, or `data/`.
- `modules/` is the curated JC2 standard library. It is not a personal utility folder.
- Never commit `local/`; it is ignored local workspace data.
- Do not add a new public module, API, endpoint, or provider unless the user explicitly asks for it.
- Do not decide release contents, tags, assets, or version numbers on the user's behalf. Report observed facts and let the author decide.

## JC2 workflow

- For JC2 implementation work, read `local/LESSONS.md` (methodology and gates) before planning, if present.
- Read `docs/COMMIT_CONVENTION.md` verbatim before writing a commit message.
- One logical change per commit. Tests and user-facing help belong in the same commit as the behavior they describe.
- Do not create filler commits for documentation or records.
- Prefer the existing module and API style; inspect nearby code before inventing a new pattern.
- Keep user-visible help text in English and limited to the contract: signature, behavior, legal values, errors.

## Build and test

Run commands from the repository root, one at a time:

```powershell
cmake --build out\build\x64-release
out\build\x64-release\jc2.exe test test\tests
out\build\x64-release\jc2.exe --jit test test\tests
```

- Do not run concurrent builds or test suites.
- If a binary is locked by a running REPL, do not kill the user's process without permission.
- For compiler behavior changes, run both interpreter and JIT suites.

## Evidence and reporting

- Distinguish observation from inference. Do not report a guessed root cause as verified.
- Before claiming a bug, establish what correct behavior looks like.
- Do not change tests to make a failing change pass unless the user explicitly approves the specification change.
- If a change is not verified, say so explicitly.
