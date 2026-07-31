# Run viewer

A static page that reads a `dfr` trace and draws it. No server, no live connection, no build-time
code generation: it opens a JSONL file and renders what is in it.

```
npm install
npm run dev        # http://localhost:5173
npm run build      # static bundle in dist/, deployable anywhere
npm run typecheck
```

`public/traces/` holds the same fixtures as the repository's `traces/`, so a built bundle carries
its own data and works from a file server or a GitHub Pages subpath without configuration. Use
**open a trace…** to load one produced locally.

## The one rule

**No domain logic lives here.** Every number drawn is a field the trace already carries — the client
state, the delivered watermark, the messages missing, the unfillable range. Nothing is recomputed.

That is not tidiness. A viewer that reconstructed state from the event sequence would be a second
implementation of a C++ state machine, written in TypeScript by somebody reading the first. When the
two disagreed, the picture would be wrong and nothing would say so — which is the exact failure mode
the library exists to prevent, reintroduced in the tool built to display it.

So: if a number is not in the file, it is not drawn. When the viewer needs something new, the
trace format gains a field.

## Panels

| Panel | Question it answers |
|---|---|
| Run position | Where in the run am I, and what is the client's state here? |
| Client state over the run | When did it stop being live, and what happened just before? |
| Snapshot recovery | Did the snapshot arrive in time, and if not, how much is permanently gone? |
| Redundant lines | Is the second line earning its keep, or is it already dead? |
| Verdict | Was every message delivered exactly once? |
| At packet N | What happened here, and what fault was scheduled for it? |
| What this run measured | Which claims are measurements and which cannot be measured at all? |

## Design

Ink on paper, thin rules, no shadows, no rounded corners, everything numeric monospaced. A recovery
trace is a measurement, and a measurement drawn with gradients invites the reader to admire the
picture rather than read the numbers. Four colours only, each meaning exactly one thing: something
was done to the stream, something is being repaired, something is permanently lost, something is
healthy.
