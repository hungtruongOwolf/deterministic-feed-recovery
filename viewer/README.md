# Run viewer

Press play and watch a market-data feed be damaged and repaired. It reads a `dfr` trace file and draws it —
no server, no live connection, no build-time code generation.

```
npm install
npm run dev        # http://localhost:5173
npm run build      # static bundle in dist/
npm run typecheck
npm run check      # renders the scene in node; see below
```

Space plays and pauses, ← and → step one beat at a time.

## What it shows

A venue on the left, a client on the right, and three tracks between them: market data going out,
retransmit requests coming back, snapshots coming back. Packets are objects that cross the space. A lost one
dies part way over. A corrupted one reaches the far side and is refused. A duplicate arrives as two. Under
all of it, a message axis: solid where messages have been delivered, cut out in red where they are missing.

Every step carries a sentence in plain language, and the steps that change the story interrupt with a card.
That is deliberate: a panel reading `snapshot_rejected · snapshot_behind_buffer` is legible only to somebody
who already knows the codebase. *"The snapshot is older than the oldest message the client kept, so twenty
messages exist in neither"* is legible to anybody.

Three runs ship with it: faults injected and repaired; two redundant lines where nearly every hole closes
without asking anyone; and the Glimpse race, where the snapshot arrives too old and some messages are gone
for good.

## The one rule

**No domain logic lives here.** Every number drawn is a field the trace already carries — the client state,
the delivered watermark, the outstanding holes. Nothing is recomputed.

That is not tidiness. A viewer that reconstructed state from the event sequence would be a second
implementation of a C++ state machine, written in TypeScript by somebody reading the first. When the two
disagreed the picture would be wrong and nothing would say so — which is the exact failure the library exists
to prevent, reintroduced in the tool built to display it.

So when the viewer needs something new, **the trace format gains a field**. It has happened once already: the
message axis needed the outstanding holes themselves rather than a count of them, and the alternative was
accumulating them here from the gap events. `dfr::trace::event` grew a bounded `gaps` array instead. That is
the rule working, not an exception to it.

What *is* added here is presentation: how a packet moves, and what a step means in words. Neither is in the
file, and neither can be got from a table of field names.

## `npm run check`

Renders the scene in node with `react-dom/server` and asserts the three things a build cannot:

- the sheet, a packet, the message axis and the client's lit state all draw;
- **two progress values of the same beat produce different markup** — otherwise nothing is moving and the
  page is a still picture with a play button on it;
- every caption is a sentence, no `snake_case` leaked through, and the Glimpse run reaches its rejection with
  a non-empty unfillable range.

It cannot tell whether the drawing looks good. Nothing without eyes can, and that limit is stated rather than
implied.

## Design

An architectural drawing sheet: hairline frame, registration crosshairs, edge ticks, a drawn title block
carrying the legend instead of a floating overlay. Ink on warm paper, one weight of rule, typography doing
the work colour usually gets asked to do.

Four colours, each meaning exactly one thing — something crossed the wire, something was damaged, something is
being repaired, something is gone for good. Nothing decorative is coloured at all. A recovery trace is a
measurement, and a measurement drawn with gradients invites the reader to admire the picture instead of
reading it.
