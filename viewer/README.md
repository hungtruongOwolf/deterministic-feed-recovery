# Run viewer

Press play and watch a market-data feed be damaged, and watch what the receiver does or cannot do about it.
It reads a `dfr` trace file and draws it — no server, no live connection.

```
npm install
npm run dev        # http://localhost:5173
npm run build
npm run typecheck
npm run check      # measures the drawing; see below
```

Space plays and pauses, ← and → step.

## Two spaces, because the system has two

**The path**, on the left. Where a packet physically travels: the matching engine, two multicast networks
that bow apart through separate switches and rejoin at the receiver's two NICs, and — drawn apart, because it
is a different network — the pair of TCP services you can ask questions of. Redundancy explains itself in
this drawing: the same packet is on both arcs, so losing one arc costs nothing.

**The book**, on the right. What the receiver ends up *knowing*, one cell per message. Cells fill with ink as
data arrives; a hole stays cut out in red. This is what turns "six messages missing" from a number into a
shape, and it separates the two kinds of empty cell that matter — one ahead of the frontier is ordinary, one
behind it is a hole.

Damage happens on the left and shows up on the right a few beats later. Watching the second follow the first
is the argument the whole project is making.

## The three runs are one experiment, not three demos

Above the plan is a ladder of the three defences a receiver has against a lost packet, cheapest first:

| | defence | costs | catches |
|---|---|---|---|
| ① | **two lines** | bandwidth, continuously | loss on one path — the common case — and costs no time at all |
| ② | **ask for it back** | a round trip, and it expires | what both lines missed, if you ask in time |
| ③ | **rebuild from a snapshot** | seconds, blind while it runs | everything, but you lose your place |

Each bundled run **removes one more rung on purpose**, so the one underneath can be watched doing its job. A
run with redundancy working never exercises retransmission, because there is nothing left for it to do — so
to see retransmission you take the second line away. The removed rung is drawn struck out, and so is the part
of the plan it corresponds to.

The third run removes both, and the snapshot arrives reflecting an older moment than the oldest message the
receiver kept. Twenty messages then exist in neither place. Nothing knows they are gone — and a receiver that
carried on would publish a book that looks complete and is permanently wrong. This one refuses, which is the
point of the exercise.

## The one rule

**No domain logic lives here.** Every number drawn is a field the trace already carries — the client state,
the delivered watermark, the outstanding holes. Nothing is recomputed. A viewer that reconstructed state from
the event sequence would be a second implementation of a C++ state machine written in TypeScript by somebody
reading the first, and when the two disagreed the picture would be wrong with nothing to say so.

So when the viewer needs something, **the trace format gains a field**. That has happened once: the book grid
needed the outstanding holes themselves rather than a count, so `dfr::trace::event` grew a bounded `gaps`
array. The rule working, not an exception to it.

What *is* added here is presentation: where a packet is at a given instant, and what a step means in words.

## `npm run check`

I cannot look at this, so it is measured instead. An earlier version had a label sitting inside the box it
described and a title block running past the frame — both invisible to a compiler, to a build, and to me.

The check asserts:

- **geometry** — no two regions collide, nothing escapes the frame, every part sits inside its parent, every
  label fits the box it is drawn in, and the book grid fits its region at 40, 120, 300 and 900 messages;
- **rendering** — the ladder, the path, the grid and a packet all draw, and **two progress values of the same
  step produce different markup**, so the picture moves rather than being a still with a play button on it;
- **the experiment is visible** — a run missing a defence draws it struck out, and a run with all three does
  not;
- **language** — every step is a sentence, no `snake_case` leaked through, and the run that loses data
  explains the loss in words.

Nothing here can tell whether the drawing looks *good*. That limit is stated rather than implied.

## Design

An architectural drawing sheet: hairline frame, registration crosshairs, edge ticks, a drawn title block
carrying the run's four headline figures. Ink on warm paper, one weight of rule, typography doing the work
colour is usually asked to do. Every position is computed in `stage/layout.ts` from the sheet size — nothing
is placed by eye, which is why the geometry can be checked at all.

Four colours, each meaning one thing: something crossed the wire, something was damaged, something is being
repaired, something is gone for good. Nothing decorative is coloured.
