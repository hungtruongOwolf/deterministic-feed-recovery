// The three runs, joined into one continuous film.
//
// Every earlier design offered the three as a *choice*(a dropdown, then three buttons) and a choice is
// read as three alternatives being compared. They are not alternatives. They are the same system watched
// three times while a defence is taken away each time, and the only honest way to show that is to make them
// one thing that plays through without asking anybody anything.
//
// So there is no selector in this app. There is a position in a film. Act two follows act one the way the
// second half of a sentence follows the first, and between them is an interlude that says what changed,
// which is the sentence the old chapter buttons were trying to be, in the place where it actually lands.
//
// Nothing here computes recovery state. An act is a trace plus a span of film indices; a moment is a beat
// plus which act it belongs to. The arithmetic is index arithmetic.

import type { Trace } from "./trace";
import { buildStory, type Beat } from "./story";

/**
 * What each act is, written down once, including the parameters that make it that act.
 *
 * The parameters are here rather than in the controls because they are what the act *means*: act II is "the
 * second line taken away", which is `lines: 1`. A reader changing the seed changes the data; they cannot
 * change the argument, because the argument is the three removals and those are fixed.
 */
export interface ActSpec {
  /** The committed trace, used when WebAssembly is unavailable. */
  readonly file: string;
  /** How many redundant lines carry the feed in this act. */
  readonly lines: 1 | 2;
  /** Whether the retransmit facility refuses, forcing a snapshot. */
  readonly glimpse: boolean;
  /** How far behind the snapshot reply is, in messages. Only meaningful when `glimpse`. */
  readonly staleness: number;
  readonly ordinal: string;
  readonly title: string;
  /** What is different from the act before: the reason this act exists. */
  readonly change: string;
  /** How far down the stack of defences this act is forced to reach, in words. */
  readonly reaches: string;
}

export const ACTS: readonly ActSpec[] = [
  {
    file: "redundant-ab.jsonl",
    lines: 2,
    glimpse: false,
    staleness: 0,
    ordinal: "I",
    title: "Two lines carry the feed",
    change:
      "The venue sends every message down two paths that lose different packets. Watch how rarely the client has to ask anybody for anything.",
    reaches: "stays on the first defence",
  },
  {
    file: "recovering-seed4711.jsonl",
    lines: 1,
    glimpse: false,
    staleness: 0,
    ordinal: "II",
    title: "Now take the second line away",
    change:
      "Same venue, same kind of faults: one fewer defence. Every hole that the other line used to cover now costs a round trip to the venue and back.",
    reaches: "falls to the second defence",
  },
  {
    file: "glimpse-race.jsonl",
    lines: 1,
    glimpse: true,
    staleness: 20,
    ordinal: "III",
    title: "And close the retransmit window",
    change:
      "The round trip now comes back too late: those messages have aged out. Only the last defence is left, and it answers with an older moment than the client needs.",
    reaches: "falls to the third defence, and still loses data",
  },
];

/**
 * What a reader may change: the data, never the argument.
 *
 * The seed and the fault count are theirs. Which defence each act removes is not, because that is the
 * experiment(three parts in order, each with one fewer layer) and a control that could break it would
 * turn the page back into three unrelated runs.
 */
export interface Settings {
  readonly seed: number;
  readonly messages: number;
  readonly faults: number;
}

export const DEFAULT_SETTINGS: Settings = { seed: 4711, messages: 300, faults: 6 };

/** The committed traces were generated with exactly these, which is why the page looks the same on arrival. */
export function isDefault(settings: Settings): boolean {
  return (
    settings.seed === DEFAULT_SETTINGS.seed &&
    settings.messages === DEFAULT_SETTINGS.messages &&
    settings.faults === DEFAULT_SETTINGS.faults
  );
}

/** How many beats the interlude card stays up for. Long enough to read, short enough not to be a wait. */
export const INTERLUDE_BEATS = 9;

export interface Act extends ActSpec {
  readonly index: number;
  readonly trace: Trace;
  /** First film index of this act. */
  readonly from: number;
  /** One past the last film index of this act. */
  readonly to: number;
}

export interface Moment {
  /** Index into the film. */
  readonly at: number;
  readonly act: number;
  /** Index within this act's own story, so per-act panels still read correctly. */
  readonly local: number;
  readonly beat: Beat;
  /** True while the act's interlude card is up. */
  readonly opening: boolean;
}

export interface Film {
  readonly acts: readonly Act[];
  readonly moments: readonly Moment[];
}

/**
 * Joins the traces, in the order of `ACTS`, into one film.
 *
 * Throws rather than skipping a missing act: a film with a hole in it would argue the opposite of what it
 * exists to argue, and would do so silently.
 */
export function buildFilm(traces: readonly Trace[]): Film {
  if (traces.length !== ACTS.length) {
    throw new Error(`the film has ${ACTS.length} acts but ${traces.length} traces were given`);
  }

  const acts: Act[] = [];
  const moments: Moment[] = [];

  for (const [index, spec] of ACTS.entries()) {
    const trace = traces[index]!;
    const story = buildStory(trace);
    const from = moments.length;
    for (const beat of story) {
      moments.push({
        at: moments.length,
        act: index,
        local: beat.at,
        beat,
        opening: beat.at < INTERLUDE_BEATS,
      });
    }
    acts.push({ ...spec, index, trace, from, to: moments.length });
  }

  return { acts, moments };
}

/** Which act a film position is in. Clamped, so a position past the end is the last act rather than a crash. */
export function actAt(film: Film, at: number): Act {
  const last = film.acts[film.acts.length - 1]!;
  for (const act of film.acts) {
    if (at < act.to) {
      return act;
    }
  }
  return last;
}

/**
 * How long to hold each moment, relative to the others.
 *
 * Two thirds of a real feed is heartbeats, so two thirds of an evenly paced film is watching nothing happen.
 * Nothing is skipped(the quiet stretches still play, and the position axis still counts every step) but
 * they go past quickly, and the moments that carry the argument are held long enough to read.
 */
export function dwellFor(film: Film, at: number): number {
  const moment = film.moments[at];
  if (moment === undefined) {
    return 1;
  }
  if (moment.opening) {
    return 1.7; // the interlude card is up; give somebody time to read what changed
  }
  return moment.beat.notable ? 1.4 : 0.28;
}

/** How long the whole film runs at 1×, in seconds. Reported by the checker so pacing cannot drift unnoticed. */
export function runtimeSeconds(film: Film, beatsPerSecond: number): number {
  let total = 0;
  for (let at = 0; at < film.moments.length; at += 1) {
    total += dwellFor(film, at) / beatsPerSecond;
  }
  return total;
}

/**
 * What this particular run turned out to be true of.
 *
 * Read from the runs rather than from the story, so the page reports what happened instead of what it
 * intended. Which numbers to report here was settled by measurement, not by taste, and the first two
 * attempts were both wrong:
 *
 *   - "each act reaches one layer deeper than the last" fails often. At seed 7 the second act never needs a
 *     retransmit at all, so it reaches no deeper than the first.
 *   - "two lines mean fewer round trips" fails too, and more interestingly. A second line that fills the
 *     *middle* of a hole splits one gap into two, so it can produce more requests while asking for fewer
 *     messages (seed 114). And because the injector damages each line separately, two lines are not
 *     strictly better off either (seed 186). It is a strong tendency and not a law.
 *
 * Swept over 400 seeds × 3 acts, exactly two properties held every time, and those are what this reports:
 * nothing is delivered twice, and nothing is lost until the last defence answers too late.
 */
export interface Verdict {
  readonly lost: readonly number[];
  readonly duplicated: readonly number[];
  readonly roundTripMessages: readonly number[];
  /** Acts I and II lose nothing and act III does. The point of the whole film. */
  readonly recoveryHeld: boolean;
  /** No act delivered anything twice. The other half of correctness, and easy to forget. */
  readonly exactlyOnce: boolean;
}

export function verdictOf(film: Film): Verdict {
  const lost = film.acts.map((a) => a.trace.summary.unfillable_messages);
  const duplicated = film.acts.map((a) => a.trace.summary.messages_delivered_twice);
  return {
    lost,
    duplicated,
    roundTripMessages: film.acts.map((a) => a.trace.summary.retransmit_messages),
    recoveryHeld: lost.length === 3 && lost[0] === 0 && lost[1] === 0 && lost[2]! > 0,
    exactlyOnce: duplicated.every((n) => n === 0),
  };
}

/** One or two sentences on the whole film, shown before anything moves. */
export function prologue(film: Film): { readonly title: string; readonly body: string } {
  const messages = film.acts.reduce((sum, a) => sum + a.trace.summary.messages_delivered, 0);
  const lost = film.acts.reduce((sum, a) => sum + a.trace.summary.unfillable_messages, 0);
  return {
    title: "One feed, three times, with a defence taken away each time",
    body:
      "A market-data client has three defences against a lost packet, and they are layers, not options: a second line, " +
      "then asking the venue to resend, then a snapshot of the whole book. This runs the same system three times and " +
      `removes one defence each time, so you can watch it fall a layer deeper. ${messages.toLocaleString()} messages ` +
      `are delivered across the three acts and ${lost} are lost for good: all of them in the last act, and the client ` +
      "says so rather than publishing a book it knows to be wrong.",
  };
}
