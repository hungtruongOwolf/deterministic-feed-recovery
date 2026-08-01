# Deterministic Feed Recovery: run viewer

The page a visitor lands on, and the reasoning behind its shape. Rewritten because the previous version of this
file described a page that no longer exists: it still had a section named after a heading that had been deleted,
and no mention of the two things the page now opens with.

## Who it is for, which is the decision everything else follows from

A hiring manager with thirty seconds, and an engineer with five minutes. Neither wants a lesson in MoldUDP64
recovery: that is what the *author* finds interesting, and organising the page around it is how it ended up at
1,504 words across thirteen blocks of identical visual weight: a six-minute read in front of a two-minute film.

Measured before the rewrite: **113 passages in the commit history describe a defect found and why it hid, and not
one appeared on the page outside a collapsed fold.** The test count appeared zero times. One GitHub link, buried in
a benchmark panel. The strongest material in the repository was the most hidden thing in it.

## The shape

```
hero    one sentence · four numbers · a link to the code · "running in your browser"
1       What broke, and what caught it
2       Watch a feed break and recover
3       What it costs
4       The same exchange, taking orders
```

**The hero** answers the thirty-second question without anybody pressing anything. The film used to be first,
behind a play button: a toll gate in front of everything, for a visitor who will not press it.

**Section 1 is the defect gallery** (`panels/Findings.tsx`, from `model/findings.ts`), because a hard bug found and correctly explained is the only real evidence of
judgement. Each card carries *how it hid*, *what caught it*, *what it changed*. Several are the author's own
mistakes, deliberately: a list of things that went right is a list of things nobody checked. The hardest one is
first, and `npm run check` asserts that it is.

**Section 2 is the film**, still the best thing here. It is just no longer the entrance.

## Three stacked planes, because the defences are layers

The drawing is an axonometric stack, back to front:

```
plane 1   two multicast paths, running in parallel across the depth of the plane
plane 2   the TCP retransmit service: reached only when both paths above missed
plane 3   the snapshot service: reached only when the retransmit came too late
```

A stack, because that is what they are: a ladder a packet *descends* when the layer above fails. A flat drawing has
to say that in a caption; a stack says it by being one. A column falls through all three at the receiver's edge,
and the marker on it is where the run currently is, so escalation is literal downward movement rather than a state
name changing somewhere.

Axonometric rather than perspective, because this is a drawing sheet: parallel lines stay parallel, and a length is
a length wherever it sits.

## One film, three acts, and nothing to choose

There is no dropdown and there are no chapter buttons. Both were tried and both were wrong for the same reason:
anything a reader picks between reads as an alternative, so three runs behind a control read as three competing
methods rather than three layers of one system.

So the three traces are joined into one film that plays straight through. An interlude card says what was taken
away, and the strip above the drawing is one bar whose fill crosses the act dividers as it plays: that crossing is
the argument. Clicking an act seeks; it does not select.

Pacing is part of it. Two thirds of a real feed is heartbeats, so an evenly ticked film is mostly watching nothing
happen. Nothing is skipped and the position axis still counts every step, but quiet stretches go past and the
moments that matter are held. About two minutes at 1×, and the checker asserts the runtime stays watchable.

## The library runs on the page

`wasm/api.cpp` compiles the C++ for the browser, so the pattern a reader picks is a run that happens. This works
for a reason rather than by luck: the library is header-only, reads no clock, opens no sockets, allocates nothing
after start-up, and every run is a function of its seed. Those were determinism constraints, and they turn out to be
exactly the constraints that make code portable to a sandbox with no operating system.

Two things keep it honest:

- **The output goes through the same writers the native tool uses.** A separate formatter for the browser would
  have been a second format. `scripts/check-wasm.sh` runs six shapes through both and diffs them byte for byte.
- **The verdict is measured on the run in front of the reader.** Turn the damage down and an act may never need a
  retransmit, so a claim stops holding, and the page says so instead of keeping a caption that has become false.

The controls change the data. They cannot change which defence each act removes, because that is the experiment. If
WebAssembly fails to load, the recorded traces are drawn, the controls are disabled, and the page says why: an inert
control that looks live is worse than no control.

## Nothing asks the reader for a "seed"

That is the right word inside the library and the wrong word on a page: a visitor cannot form an intention about the
difference between 4711 and 4712. The controls say **damage**(*a little, typical, a lot, brutal*) and **length**.
The seed survives as a footnote naming *this pattern*, with the one sentence that makes it matter: type it back in a
year and the same packets go missing at the same moments, on any machine.

`npm run check` asserts the absence of the word in visible text, which is an unusual thing to assert and the right
thing here.

## The one rule

**No domain logic lives here.** Every number drawn is a field a trace already carries. Nothing is recomputed. A
viewer that reconstructed state from the event sequence would be a second implementation of a C++ state machine,
written in TypeScript by somebody reading the first, and when the two disagreed the picture would be wrong with
nothing to say so.

So when the viewer needs something, **the trace format gains a field**. That has happened more than once: the book
grid needed the outstanding holes themselves rather than a count, and the page needed the *messages* behind a
retransmit rather than the number of requests. The rule working, not an exception to it.

The performance figures are the one place the page shows numbers it did not compute in the browser, and it says so
at the top of that section: WebAssembly cannot time a two-nanosecond operation, so a browser figure there would be
a different quantity wearing the same label.

## `npm run check`

I cannot look at this, so it is measured. An earlier version had a label inside the box it described and a title
block running past the frame: both invisible to a compiler, to a build, and to me.

- **geometry**: no two regions collide, nothing escapes the frame, every part sits inside its parent, every label
  fits its box, the book grid fits at 40/120/300/900 messages, and the ladder's columns sum to the sheet width;
- **rendering**: the planes, the path, the grid and a packet all draw, and two progress values of one step produce
  different markup, so the picture moves rather than being a still with a play button on it;
- **the argument**: the acts are contiguous and cover the film, nothing is delivered twice, nothing is lost until
  the last defence answers late, and a difference inside the noise floor is labelled as noise rather than reported
  as a win;
- **the reader**: no visible text asks for a "seed", no choice is a raw number, every fold names what is inside it,
  and the first screen stays under ninety words;
- **weight**: no panel over 80 words of prose before anything is opened, and no visible paragraph over 45. Prose is
  what this project over-produces, so it is budgeted rather than trusted to taste.

Nothing here can tell whether the drawing looks *good*. That limit is stated rather than implied.

## Design

An architectural drawing sheet: hairline frame, registration crosshairs, edge ticks, a drawn title block carrying
the run's headline figures. Ink on warm paper, four meaningful colours, typography doing the work colour is usually
asked to do. Every position is computed in `stage/layout.ts` and `session/layout.ts` from the sheet size: nothing is
placed by hand, which is the only reason the geometry can be checked at all.
