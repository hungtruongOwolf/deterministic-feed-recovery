// The book, which is the point of the message layer.
//
// The invariant, visible in the committed data rather than only in a test file. Acts I and II lose nothing, so
// they must end with the *same* book; act III loses data for good, so it must not. And it draws, and (the part
// that was missing) it *states the claim* rather than showing two numbers a reader has to interpret.

import type { Trace } from "../../src/model/trace";
import { check, frame, traces } from "./fixtures";

export function run(): void {
  console.log("\nthe book, which is the point of the message layer");

  const finalOf = (t: Trace) => {
    const events = t.events.filter((e) => e.bid_levels > 0 || e.ask_levels > 0);
    return events[events.length - 1];
  };
  const [one, two, three] = traces.map(finalOf);
  check(one !== undefined && two !== undefined && three !== undefined, "every act ends with a book");
  if (one !== undefined && two !== undefined && three !== undefined) {
    check(
      one.best_bid === two.best_bid && one.best_ask === two.best_ask &&
        one.traded_shares === two.traded_shares,
      `acts I and II end with the same book (${one.best_bid / 1e4}/${one.best_ask / 1e4}, ${one.traded_shares} shares)`,
    );
    check(
      three.traded_shares !== one.traded_shares,
      `act III ends with a different book, because data was lost for good (${three.traded_shares} shares against ${one.traded_shares})`,
    );
    check(one.best_bid < one.best_ask, "the book is not crossed at the end of act I");
    check(one.bid_levels > 1 && one.ask_levels > 0, "the book has real depth, not one level");
  }

  // And it draws, and (the part that was missing) it *states the claim* rather than showing two numbers a reader
  // has to interpret. A bid and an ask on a page tell nobody that they are looking at the project's central
  // argument.
  const drawn = frame(traces[0]!, 60, 0.5);
  check(drawn.includes('class="quote'), "the quote is drawn");
  check(/THE BOOK/.test(drawn), "and labelled, so a reader knows what they are looking at");
  check(!/NaN|undefined/.test(drawn), "no book figure rendered as NaN or undefined");

  // The verdict, at the end of each act, where the comparison is meaningful.
  for (const [index, trace] of traces.entries()) {
    const last = trace.events.length - 1;
    const shown = frame(trace, last, 0.9);
    check(/verdict/.test(shown), `act ${index + 1} states a verdict on its book`);
    const expectMatch = index < 2;
    check(
      expectMatch
        ? /THE BOOK IS RIGHT/.test(shown)
        : /THE BOOK IS INCOMPLETE/.test(shown),
      expectMatch
        ? `act ${index + 1} says the book is right, because it is`
        : `act ${index + 1} says the book is incomplete, because it is`,
    );
  }

  // Mid-run it must not claim either way: a difference halfway through is a film that is not over.
  const midway = frame(traces[2]!, 20, 0.5);
  check(/still playing/.test(midway), "mid-run it says the comparison is not due yet rather than reporting a failure");
}
