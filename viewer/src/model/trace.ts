// The trace file's shape, and nothing else.
//
// This module parses and indexes. It derives no state, classifies no event and computes no
// recovery arithmetic, because the trace already carries its own conclusions: every event records
// the resulting client state and headline numbers. A viewer that recomputed them would be a second
// implementation of a C++ state machine, written in TypeScript by someone reading the first — and
// when the two disagreed the picture would be wrong with nothing to say so.
//
// So the rule for this whole app: if a number is not in the file, it is not drawn.

export type Layer = "venue" | "chaos" | "wire" | "client";

export interface TraceEvent {
  readonly kind: "event";
  /** Which source packet the run had reached. The scrubber's x-axis. */
  readonly i: number;
  /** Nanoseconds on the run's manual clock. Simulated, and the ledger says so. */
  readonly t: number;
  readonly layer: Layer;
  readonly event: string;
  readonly line: number;
  /** Half-open sequence range the event is about, matching recovery::sequence_range. */
  readonly first: number;
  readonly end: number;
  readonly attempt: number;
  readonly detail: number;
  readonly reason: string;
  /** The client state *after* this event. Read, never inferred. */
  readonly state: string;
  readonly delivered_through: number;
  readonly missing: number;
  readonly holes: number;
  /**
   * The outstanding holes themselves, as `[first, end)` pairs — bounded at four by the writer.
   *
   * Present because the viewer needed to *draw* them and the alternative was accumulating them from the
   * gap events, which is reconstructing recovery state in TypeScript. The format grew instead. When
   * `holes` exceeds this array's length, say "and N more" rather than drawing a lie.
   */
  readonly gaps: ReadonlyArray<readonly [number, number]>;
}

export interface ScheduledFault {
  readonly op: string;
  readonly first_packet: number;
  readonly packet_count: number;
  readonly parameter: number;
  readonly detail: number;
}

export type LimitStatus = "measured" | "not-measurable" | "simulated";

export interface Limit {
  readonly claim: string;
  readonly status: LimitStatus;
  readonly note: string;
}

export interface RunHeader {
  readonly kind: "run";
  readonly schema: string;
  readonly seed: number;
  readonly messages: number;
  readonly packets: number;
  readonly session: number;
  readonly mode: "recovering" | "glimpse";
  readonly staleness_messages: number;
  readonly lines: number;
  readonly schedule: readonly ScheduledFault[];
  readonly limits: readonly Limit[];
}

export interface RunSummary {
  readonly kind: "summary";
  readonly events: number;
  readonly events_dropped: number;
  readonly messages_delivered: number;
  readonly messages_delivered_twice: number;
  readonly messages_missing: number;
  readonly retransmit_requests: number;
  /**
   * How many messages those requests covered, as distinct from how many requests there were.
   *
   * The two answer different questions and the difference is interesting: a second line that fills the
   * middle of a hole splits one gap into two, so it can produce more requests while asking for fewer
   * messages. The format grew a field rather than have the viewer sum event ranges.
   */
  readonly retransmit_messages: number;
  readonly retransmits_served: number;
  readonly retransmit_refusals: number;
  readonly snapshot_requests: number;
  readonly unfillable_messages: number;
  readonly final_state: string;
  /** False when the recorder filled: the trace is a prefix, and the UI must say so. */
  readonly complete: boolean;
}

export interface Trace {
  readonly header: RunHeader;
  readonly events: readonly TraceEvent[];
  readonly summary: RunSummary;
  /** Highest packet index seen, so the scrubber has a range without scanning twice. */
  readonly lastIndex: number;
}

export class TraceFormatError extends Error {}

/**
 * Parses JSONL. Strict on purpose: a viewer that silently skipped a line it did not understand
 * would draw an incomplete run and look like a complete one, which is the failure this whole
 * project is about.
 */
export function parseTrace(text: string): Trace {
  const lines = text.split("\n").filter((line) => line.trim().length > 0);
  if (lines.length < 2) {
    throw new TraceFormatError("a trace needs at least a header and a summary");
  }

  let header: RunHeader | undefined;
  let summary: RunSummary | undefined;
  const events: TraceEvent[] = [];
  let lastIndex = 0;

  for (const [at, line] of lines.entries()) {
    const parsed: unknown = JSON.parse(line);
    if (typeof parsed !== "object" || parsed === null || !("kind" in parsed)) {
      throw new TraceFormatError(`line ${at + 1} has no kind`);
    }
    const record = parsed as { kind: string };
    switch (record.kind) {
      case "run":
        header = parsed as RunHeader;
        break;
      case "summary":
        summary = parsed as RunSummary;
        break;
      case "event": {
        const event = parsed as TraceEvent;
        events.push(event);
        lastIndex = Math.max(lastIndex, event.i);
        break;
      }
      default:
        throw new TraceFormatError(`line ${at + 1}: unknown kind "${record.kind}"`);
    }
  }

  if (header === undefined) {
    throw new TraceFormatError("no run header");
  }
  if (summary === undefined) {
    throw new TraceFormatError("no summary: the trace was truncated");
  }
  if (!header.schema.startsWith("dfr-trace/")) {
    throw new TraceFormatError(`unsupported schema "${header.schema}"`);
  }
  return { header, events, summary, lastIndex };
}
