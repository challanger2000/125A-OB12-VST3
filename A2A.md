# A2A Protocol — Universal Agent-to-Agent Working Rules

*A project-agnostic protocol for robust multi-agent collaboration on a code project.*
*Designed so the work survives even when the chat app drops messages or crashes:*
**the repo + the STATE anchor are the memory; the chat is disposable.**

*Version 1 · last updated 2026-06-14*

---

## 0. Why this exists (read this first)

When two agents collaborate through a chat UI, the chat is **fragile** — messages vanish, the app
crashes on update, sessions can't always switch models. If the *state of the work* lives only in the
chat, a glitch destroys it.

The fix: **make the repository the single source of truth.** A living **STATE anchor** (committed,
referenced by commit hash) holds where things are; every round updates it **in-file**. A lost message
is then never lost work — the next session reads the STATE and continues.

> Core mantra: **the chat can be lost; the repo cannot.** Commit early, update STATE in-file,
> reference by `@hash`.

---

## 1. Roles

- **Orchestrator / Verifier.** Owns the **gates** (build / run / VCS). Treats the **compiler, tests,
  and evidence as the judge** — not claims. Writes and maintains the STATE anchor. Reconstructs
  missing contracts from evidence. Calls **STOP** when something smells like fiction.
- **Implementer.** Works **from STATE**. Makes focused, exact edits. Reports back **with evidence**
  (pasted gate output, grep proofs). Does **not** invent APIs, headers, symbols, or instructions.

The roles are a pattern, not specific products: any "driver + worker" agent pairing fits.

---

## 2. Core principle — the contract is the source of truth

**Never implement against a declaration that does not exist.** The header/interface is the contract;
the hardware/chip manual is the contract; the compiler is the judge.

- When a symbol is "undeclared" or "not a member," first ask whether the **contract is missing** —
  don't paper over it.
- **Fiction detector:** before claiming any API / flag / symbol / instruction exists, **grep for it.**
  Zero hits = it does not exist. Case closed.
- If a build "succeeds" but the **evidence** (a fresh run, a grep, a log) contradicts the claim, the
  **evidence wins.** STOP and re-check.

---

## 3. The STATE anchor

Each project keeps a **living STATE file** (e.g. `STATE.json`) at the repo:

- **Read it FIRST**, before any work. It carries the ritual, the rules, current status, audit queue,
  and the "next-session first question."
- **Reference it by commit:** `STATE.json@<short-hash>`.
- **Update it in-file every round** with the new commit + milestone. The STATE evolving in git **is**
  the durable memory.

This single discipline is what lets a worker resume cleanly after a chat-app failure.

---

## 4. A2A message format

```
== A2A v1 | proj:<name> | from:<X> to:<Y> ==
ctx: STATE.json@<hash> - read the ritual + rules first.
TASK (exact, with file:line where possible):
  1. ...
  2. ...
VERIFY: build_check [FRESH], run/smoke-test, vcs status, grep proof, STATE-UPD in-file.
REPORT: paste gate output verbatim; do NOT summarize away errors.
```

---

## 5. Tooling rules (Windows host / shell — hard-won)

These come from real failures. Violating them silently corrupts the workspace or fakes "success."

- **RULE-PATHS.** A file-writing tool must get an **ABSOLUTE Windows path WITH the drive colon**
  (`F:\...`, `C:\...`). **Never** pass POSIX (`/f/...`) or colon-less (`f\...`) — the tool resolves
  those **drive-relative** and creates a literal `\f\` **ghost tree** at the current drive root (seen
  on both `C:\f\...` and `F:\f\...`). **An `f` folder at a drive root is the signature of this bug.**
- **RULE-FAIL.** On `permission denied` / `ENOENT` / temp-file (`.hermes-tmp`) error: the path is
  likely already mangled, or the process can't see the drive. **STOP and REPORT.** Never POSIX-fallback
  — that is exactly what writes the ghost tree.
- **RULE-SHELLBOUNDARY.** A git-bash terminal uses POSIX (`/f/...`); the file tool uses Windows paths.
  Cross the boundary with `cygpath -w` (to the tool) / `cygpath -u` (to the shell). Always.
- **RULE-NOOUTPUT.** A shell result that returns in **~100 ms** with **only a Windows banner**
  (`Microsoft Windows [Version ...]` + a prompt) and **zero command output** means **the command DID
  NOT RUN** — usually broken nested quoting turned `cmd /c` into an interactive `cmd`. `cmd /c` never
  prints the banner; a banner = interactive shell. **Treat it like a fiction hit:** STOP, fix the
  invocation, do **not** read the banner as success.
- **GUI-subsystem binaries.** Some `.exe`s don't write to a piped stdout. If `> file` comes back
  empty, capture with `Start-Process -RedirectStandardOutput out.txt -RedirectStandardError err.txt
  -NoNewWindow -Wait`, then read the files.
- **Shell variable gotcha.** In `cmd`, `set X=...` then `"%X%"` on the **same line** fails (`%X%`
  expands at parse time, before `set` runs). Use the full value inline, or `setlocal enabledelayedexpansion`.

---

## 6. Verification gates (run every change; paste output)

1. **build_check** — compile/build fresh. Redirect large output to a log and grep `: error` / link errors.
2. **run / smoke-test** — actually run it; confirm it starts and stays alive (process present), not
   just that it compiled.
3. **vcs** — `git status` (exclude build artifacts); commit with a descriptive message.
4. **grep proof** — for any "removed / renamed / doesn't exist" claim, paste the zero-hit grep.
5. **STATE-UPD** — update the STATE file in-file with the new commit + milestone.

Paste output **verbatim**. Do not summarize errors away — the evidence is the point.

---

## 7. STOP reflexes (the detectors)

Treat any of these as an immediate **STOP + verify**:

- A claimed symbol/API/instruction with **zero grep hits** → fiction.
- An **`f` folder at a drive root** (`C:\f\`, `F:\f\`) → path-mangling bug.
- A **banner-only, ~100 ms shell return** → the command didn't run.
- **Evidence contradicting a "success" claim** (a fresh run/log/grep that disagrees) → the claim is wrong.
- A subtle, unverifiable assertion presented as fact → demand the grep / the run.

A good worker earns these reflexes; a single sharp "that word smells wrong → STOP and check" has
caught more bugs than any amount of confidence.

---

## 8. Why this survives a buggy chat app

The chat can drop a message mid-flight, crash on an update, or lose a session. The **repo, the STATE
file, and the commit history cannot.** So:

- **Commit early and often.** A landed commit is permanent; a chat bubble is not.
- **Update STATE in-file** so the *current* picture is in git, not in a message.
- **Reference everything by `@hash`.** "We are at `STATE.json@d2d0ed3`" is unambiguous and survivable.
- A **lost message ≠ lost work.** The next session reads STATE and resumes exactly.

This is the whole reason the protocol holds even when the front-end is flaky.

---

## 9. Quick-start: adding A2A to any new project

1. Create `STATE.json` at the repo root: status, rules (incl. the tooling rules above), audit queue,
   next-session question. Commit it.
2. Drop in a short `A2A.md` (or this file) so any agent reads the ritual first.
3. Establish the gates for *this* project (its build/run commands) under a `build_check` heading in STATE.
4. From then on: every round → work → gates → commit → STATE-UPD in-file → reference by `@hash`.

*Keep project-specific build commands and contracts in that project's STATE/A2A; keep these universal
rules here.*
