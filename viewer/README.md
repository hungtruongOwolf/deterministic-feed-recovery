# Deterministic Feed Recovery — run viewer

Press play and watch a market-data feed be damaged, and watch what the receiver does or cannot do about it.

The name is written out in full and in title case wherever a reader meets it. A lower-case slug is a fine
thing for a repository and a poor thing for a page: somebody arriving cold reads `deterministic feed
recovery` as three ordinary words rather than as the name of something. `src/model/brand.ts` holds the name,
the abbreviation and the two taglines in one place, and `npm run check` asserts the tab title and the header
still agree.
It reads a `dfr` trace file and draws it — no server, no live connection.

```
npm install
npm run dev        # http://localhost:5173
npm run build
npm run typecheck
npm run check      # measures the drawing; see below
```

Space plays and pauses, ← and → step. Under the transport is a rail of every moment worth going back to —
click one and the run jumps there. A scrubber can reach any instant, which sounds better and is worse: the
instants that matter are a handful out of two hundred, and finding them by dragging is a search.

## Three stacked planes, because the defences are layers

The left of the sheet is an axonometric stack, drawn back to front:

```
   plane 1   two multicast paths, running in parallel across the depth of the plane
   plane 2   the TCP retransmit service — reached only when both paths above missed
   plane 3   the snapshot service — reached only when the retransmit came too late
```

They are drawn as a stack because that is what they are: a ladder a packet *descends* when the layer above
fails. A flat drawing has to say that in words; a stack says it by being one. A column falls through all
three at the receiver's edge, and the marker on it is where the run currently is — so escalation is literal
downward movement rather than a state name changing somewhere.

Axonometric rather than perspective, because this is a drawing sheet: parallel lines stay parallel and a
length is a length wherever it sits.

**The book**, on the right. What the receiver ends up *knowing*, one cell per message. Cells fill with ink as
data arrives; a hole stays cut out in red. This is what turns "six messages missing" from a number into a
shape, and it separates the two kinds of empty cell that matter — one ahead of the frontier is ordinary, one
behind it is a hole.

Damage happens on the left and shows up on the right a few beats later. Watching the second follow the first
is the argument the whole project is making.

## One film, three acts — and nothing to choose

There is no dropdown and there are no chapter buttons. Both were tried and both were wrong for the same
reason: anything you pick between reads as an alternative, so three runs behind a control read as three
competing methods rather than three layers of one system.

So the three traces are fetched together and joined into one film that plays straight through. Act II starts
where act I ends, an interlude card says what was taken away, and the run continues under worse conditions.
The strip above the drawing is one bar whose fill crosses the act dividers as it plays — that crossing is the
argument. Clicking an act seeks; it does not select.

The three acts, each with one more defence removed:

| | act | what is taken away | how deep it falls |
|---|---|---|---|
| **I** | Two lines carry the feed | nothing | stays on the first defence |
| **II** | Now take the second line away | the second line | falls to the second defence |
| **III** | And close the retransmit window | the retransmit window too | falls to the third, and still loses data |

Pacing is part of the argument. Two thirds of a real feed is heartbeats, so an evenly ticked film of 623
steps is mostly watching nothing happen. Nothing is skipped — every step still plays and the position axis
still counts them all — but quiet stretches go past quickly and the moments that matter are held. The whole
film runs about 114 seconds at 1×, and the checker asserts that.

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
- **the experiment is visible** — an act missing a defence draws it struck out, an act with all three does
  not, and the acts **fall strictly deeper as the film goes on** (`0 → 1 → 2`). That is the claim the whole
  page exists to make, so it is asserted rather than hoped for: if it ever stops holding, the page is drawing
  three unrelated runs and calling them an argument;
- **there is nothing to choose** — no `<select>` renders anywhere, the acts share one bar rather than having
  one each, the acts are contiguous and cover the film exactly, and an act already played is marked as
  *behind* rather than as *unchosen*;
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
