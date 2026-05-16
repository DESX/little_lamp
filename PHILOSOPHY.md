# Development Philosophy

## Build on an accurate dependency graph

Every operation — build, test, deploy, service startup — is a node in a
directed acyclic graph. Each node declares its prerequisites. Running
any node performs exactly the prerequisite steps needed to reach it, in
the right order, automatically. If a prerequisite can't be performed
automatically, the operation fails immediately with clear instructions:
what to do, how to verify it worked, and what to run next.

No implicit ordering. No "run this first." No tribal knowledge about
which steps must precede which. The graph is the contract — if the
graph is wrong, fix the graph.

## Idempotent by default

Every operation should be safe to re-run. Builds skip work when
sources haven't changed. Schema migrations are existence-checked.
Seed data inserts never clobber live content. Service startup is a
no-op if the service is already running. Deploys that create
resources check for existence first.

This isn't just convenience — it's a correctness property. An
idempotent pipeline can be retried after partial failure, interrupted
and resumed, or run from a clean state, and produce the same result.

## Responsiveness is structural

If something takes noticeable time by human standards, that's usually a
structural problem — not a performance problem. The fix is almost never
"optimize the slow thing." It's "stop doing work that doesn't need to
happen." A correct dependency graph that skips unchanged targets, a
build that doesn't re-fetch cached artifacts, a deploy that doesn't
rebuild what hasn't changed — these are fast because they're structured
correctly, not because they've been tuned.

Avoid hardcoded sleeps, polling delays, and retry loops as substitutes
for readiness signals. If you're waiting, wait for a condition — not
for a duration.

## Minimize managed complexity

The relevant measure of complexity is not total lines of code — it's
the lines you have to understand, audit, and maintain. A well-maintained
library with 10,000 lines is someone else's problem. Your 100 lines of
hand-rolled replacement are yours — every bug, every edge case, every
future change.

An airplane is vastly more complex than a car, but flying is simpler
than driving cross-country because the complexity of the aircraft is
actively managed by someone else. The same logic applies to
dependencies: 10 lines of code plus a proven library can be less managed
complexity than 150 lines of custom code that reimplements what the
library does.

That said, every dependency is a supply chain risk and a version to
track. The question is always: does this dependency reduce the
complexity I manage, or does it just move it somewhere harder to see?
Prefer one file over a directory of modules. Prefer no configuration
file over a flexible one. When in doubt, delete.

## Preserve cheap optionality

When an existing abstraction gives a natural extension point at
near-zero ongoing cost, keep it. A thin Function on a hot path is
cheap insurance for future logging, metrics, or auth; removing it
to reach "zero code" purity trades real optionality for a cosmetic
win. The test is whether the abstraction *already exists* and costs
*almost nothing* to keep.

This is not license to add abstractions for hypothetical futures —
that still violates managed-complexity minimization. The principle
applies only to abstractions already in the system under
consideration. New optionality has to earn its way in; existing
optionality only has to justify its ongoing cost.

## Don't prematurely de-optimize either

Removing working code in the name of purity is as premature as
adding it for unmeasured performance. If an existing layer imposes
no real cost, "it's cleaner without it" is an aesthetic claim, not
a technical one. Change when you have evidence — a measured
bottleneck, a real maintenance burden, a concrete risk — not when
you have a preference.

The flip side of "when in doubt, delete": don't delete something
working just because you could. The bias toward deletion applies to
code with no demonstrated value, not to code that earns its keep in
ways the current task doesn't exercise.

## Own your dependencies

Don't depend on an external service being alive when a user loads a
page or a developer runs a build. Download dependencies at build time,
cache them locally, and serve them from your own origin. If a CDN
disappears, a package registry goes down, or a project dies, your
cached copy still works.

When a dependency needs a small change, patch it — don't fork it. A
5-line patch on a vendored dependency is less surface area than a
10,000-line fork you now own. The patch is visible, auditable, and
clearly separated from the upstream source.

## Explicit over implicit

No hidden defaults. No magic variables. No framework conventions that
only work if you know the naming pattern. Every required parameter
should be declared and validated — missing values fail with an error,
not a silent default. `grep` should be sufficient to find where a value
is defined and where it's used.

## Traceability

Every running process should have a paper trail: what started it, when,
where its logs are, and how to stop it. Every deployed artifact should
trace back to the source that produced it. Every build step should
produce observable output that confirms it ran correctly or explains
why it didn't.

When something goes wrong, the question is always "what happened?" The
system should make that question easy to answer without attaching a
debugger or reading source code.

## Fail fast, recover by restart

When something goes wrong — bad input, unexpected state, a dependency
that isn't responding — stop immediately. Don't attempt to limp along
with partial results or degraded behavior. A crash with a clear error
is more useful than silent corruption.

## Architecture, tests, and implementation are redundant

Three artifacts describe the built system in different forms: the
architecture document in English, the tests in source code, and the
implementation in source code. Each should contain enough information to
substantially derive the other two. If the architecture says the system
does X, there should be a test that asserts X and an implementation that
performs X. If a test asserts Y, you should be able to find Y in the
architecture and trace it in the implementation.

The architecture document is the pillar. It's the one artifact written
for a human reader that captures every technical decision and its
rationale. You should be able to read the architecture and derive what
the tests must cover and roughly how the implementation is structured.

This redundancy is intentional — it's how you catch drift. When the
three disagree, at least one is wrong, and the disagreement is visible.

The most important tests are full end-to-end integration tests. They
exercise the system as a user would — real browser, real endpoints, real
data flow. Unit tests are fine where they help, but they don't prove the
system works. Integration tests do.

## One command interface

Every build, test, deploy, and service-management action goes through a
single orchestration layer. No raw shell commands, no remembering flags
or paths. The orchestrator is the source of truth for ordering,
environment variables, and dependency relationships. If there isn't a
target for something, that's a gap to fix — not a reason to run a
one-off command.

## The main file is the architecture, in code

The top-level source file (`main.c` here) is where a stranger reads the
system and understands what it does, **without help from comments,
diagrams, or external documentation**. The code itself is the
explanation. If you find yourself needing a comment to say what a
function or variable is for, the name is wrong or the abstraction is
wrong — fix that, don't paper over it.

### Persistent state lives in `main`, allocated by name

Every long-lived object the system owns — the log, the state machine,
the device records, the schedule controller, the network connection,
the queue between an interrupt handler and a worker task — is **declared
in `main` as a named variable**. The reader of `main.c` sees the full
list of "things the system has." There are no hidden file-static
singletons buried in helper modules; every piece of state has a visible
home with a name a human chose.

```c
static log_t          g_log;
static button_t       g_button;
static bulb_t         g_bulb;
static state_machine_t g_state;
```

A stranger reading those five lines knows exactly what the system
remembers. To find where any of them is touched, `grep` finds every
call site.

### Operations on state pass that state explicitly

Functions that mutate or inspect a struct take a pointer to it as their
first argument. `log_set_verbose(&g_log, true)` not `log_set_verbose(true)`.
The reader of any call site sees what data is being touched. The reader
of any function body sees what data it works on.

This is just object-oriented code without inheritance. The struct is
the object; the functions whose names start with the struct's prefix
are its methods; the first parameter is `self`. Anyone who's read good
C can follow it.

### Configuration is visible at the call site

The first thing a function does to an object should not be reaching
into globals to figure out how to behave. Whatever the caller wants
configured, the caller passes in.

```c
log_init(&g_log);
log_set_verbose(&g_log, persisted_verbose_from_nvs());
log_drainer_start(&g_log);
```

A reader of `main.c` learns the system's startup intent from main itself.
They don't have to open `log.c` and dig through `log_init` to discover
that verbosity is loaded from NVS — that's main's job to know and main's
job to show.

### Heavy lifting is in another file, but the call is in main

The body of `log_drainer_start` is in `log.c` — that's appropriate, it's
mechanical and long. But the **invocation** is in `main.c`. The
control flow is visible from main; only the implementation lives
elsewhere.

The exception is interrupt handlers and other low-level mechanisms that
have to be defined in their module to satisfy the linker or the SDK.
For those, main still shows the **wiring**: which module's handler
will fire which application callback, and where in main that callback
lives.

### Avoid comments by making the code obvious

Comments age out of sync with code. A function named
`button_register_press_callback` doesn't need a comment to say it
registers a press callback. A struct field named `last_press_tsn`
doesn't need a comment to say it stores the last press TSN. If a comment
seems necessary, the variable or function probably needs renaming.

The legitimate uses of comments are:

- **Why**: external constraints, hardware quirks, the rationale behind a
  non-obvious design decision. ("The 3RSB22BZ rejects this command in
  Echo mode; we send it anyway because we don't know the mode at boot.")
- **Sources**: a citation to a spec section, datasheet page, or
  upstream issue the reader would otherwise have to hunt for.

If a comment is just rephrasing the code, delete it.

### What does NOT belong in `main.c`

- Hex constants for protocols, cluster IDs, register offsets.
- Bit-level frame parsing.
- Vendor-specific quirks. `kick_off_enrollment(&g_button)` is fine;
  the body of `kick_off_enrollment` belongs in `enrollment.c`.
- Cluster registration tables, endpoint descriptors, network parameters.

### Acknowledged exception: third-party libraries

Some libraries — including most SDKs we depend on — use file-static
singletons internally and don't expose object handles. The ESP-Zigbee
SDK is one. The ESP-IDF console is another. For these, the helper
module holds the file-static state as a necessary evil; we still
**call its init from main** so the wiring is visible, and we still
**pass our own objects** to the helper so the helper has explicit
state to act on. Document the deviation in the helper's source — once,
at the top, plainly.

## Single-click direct manipulation

In the CMS UI, every action is a single click on a visible target. No
right-click menus for actions, no fly-out submenus, no `⋯`
catch-alls. If a cell needs two actions, two visible icons live in the
cell. The thing the user wants to do is the thing they can see, and
clicking it does it.

The discoverability cost of a hidden affordance is paid every time a
new user — or the same user a month later — sits down at the CMS. The
visual cost of a visible icon is paid once, by the renderer. Pay it
there.

Hover hints live in exactly one fixed location: a single-line status
bar at the bottom of the page describes what left-click will do for
whatever cell is under the cursor. No `title=` tooltips, no floating
popovers. There is one place to look for help and it is always in the
same place.

Configuration is the only thing the CMS uses popups for — a gear icon
opens a panel where the user shows, hides, and reorders columns.
These are infrequent meta-operations. Every other action is a direct
click on a visible target.

