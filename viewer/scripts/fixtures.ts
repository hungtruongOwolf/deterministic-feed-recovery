// Minimal, hand-built Trace/TraceEvent values for testing pure logic in isolation.
//
// Different from scripts/checks/fixtures.ts, which reads the three *real* committed traces for measuring the
// drawing. These are synthetic, because a unit test for "what happens when the event is undefined" or "when
// nothing has ever gone missing" needs to construct the exact edge case rather than search a real run for one
// that might not occur in it.

import type { Layer, RunHeader, RunSummary, Trace, TraceEvent } from "../src/model/trace";

const header = (overrides: Partial<RunHeader> = {}): RunHeader => ({
  kind: "run",
  schema: "dfr-trace/2",
  seed: 1,
  messages: 100,
  packets: 100,
  session: 1,
  mode: "recovering",
  staleness_messages: 0,
  lines: 1,
  schedule: [],
  limits: [],
  ...overrides,
});

const summary = (overrides: Partial<RunSummary> = {}): RunSummary => ({
  kind: "summary",
  events: 0,
  events_dropped: 0,
  messages_delivered: 100,
  messages_delivered_twice: 0,
  messages_missing: 0,
  retransmit_requests: 0,
  retransmit_messages: 0,
  retransmits_served: 0,
  retransmit_refusals: 0,
  snapshot_requests: 0,
  unfillable_messages: 0,
  reference_bid: 100_000,
  reference_ask: 100_100,
  reference_traded: 1_000,
  final_state: "live",
  complete: true,
  ...overrides,
});

const event = (overrides: Partial<TraceEvent> = {}): TraceEvent => ({
  kind: "event",
  i: 0,
  t: 0,
  layer: "client" as Layer,
  event: "packet_accepted",
  line: 0,
  first: 0,
  end: 0,
  attempt: 0,
  detail: 0,
  reason: "",
  state: "live",
  delivered_before: 0,
  missing: 0,
  holes: 0,
  gaps: [],
  best_bid: 0,
  best_bid_size: 0,
  best_ask: 0,
  best_ask_size: 0,
  bid_levels: 0,
  ask_levels: 0,
  traded_shares: 0,
  ...overrides,
});

/** A trace with the given events, headed and summarised with sensible defaults, both overridable. */
export function trace(
  events: readonly TraceEvent[],
  headerOverrides: Partial<RunHeader> = {},
  summaryOverrides: Partial<RunSummary> = {},
): Trace {
  return {
    header: header(headerOverrides),
    events,
    summary: summary(summaryOverrides),
    lastIndex: events.reduce((most, e) => Math.max(most, e.i), 0),
  };
}

export { event };
