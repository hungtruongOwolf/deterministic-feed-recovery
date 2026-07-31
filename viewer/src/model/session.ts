// The order-entry session trace's shape, and nothing else.
//
// Same rule as model/trace.ts and for the same reason: this parses and indexes, it derives nothing. Both
// sequence counters are read from the file. A viewer that recomputed either would be doing a third count,
// and the whole point of the session is that two independent counts agree — which a third count could not
// demonstrate, only muddy.

export type Party = "client" | "server" | "venue";

export interface WireStep {
  readonly kind: "wire";
  readonly step: number;
  readonly from: Party;
  /** The SoupBinTCP type byte, so a reader can check it against the specification. */
  readonly type: string;
  readonly name: string;
  readonly detail: string;
  /** The sequence this packet carried. Zero for the types that have none — not unknown, none. */
  readonly sequence: number;
  readonly server_next: number;
  /** What the client's own cursor counted to. Must match `server_next`, and never travels. */
  readonly client_next: number;
  readonly live_orders: number;
  readonly shares_open: number;
  readonly phase: string;
}

export interface SessionLimit {
  readonly claim: string;
  readonly status: "measured" | "not-measurable" | "simulated";
  readonly note: string;
}

export interface SessionHeader {
  readonly kind: "session";
  readonly schema: string;
  readonly session: string;
  readonly user: string;
  readonly orders: number;
  readonly fill: number;
  readonly cancel: boolean;
  readonly first_sequence: number;
  readonly limits: readonly SessionLimit[];
}

export interface SessionSummary {
  readonly kind: "session_summary";
  readonly ending: string;
  readonly packets_in: number;
  readonly packets_out: number;
  readonly orders_in: number;
  readonly acknowledgements_out: number;
  readonly live_orders: number;
  readonly accounts: boolean;
  readonly server_next: number;
  readonly client_next: number;
  readonly agreed: boolean;
}

export interface SessionTrace {
  readonly header: SessionHeader;
  readonly steps: readonly WireStep[];
  readonly summary: SessionSummary;
}

export class SessionFormatError extends Error {}

/**
 * Parses the JSONL `tools/session --trace` writes.
 *
 * Strict on purpose, like the market-data parser: a viewer that skipped lines it did not understand would
 * draw a conversation with a hole in it and look like a complete one.
 */
export function parseSession(text: string): SessionTrace {
  let header: SessionHeader | undefined;
  let summary: SessionSummary | undefined;
  const steps: WireStep[] = [];

  const lines = text.split("\n");
  for (const [at, line] of lines.entries()) {
    if (line.trim() === "") {
      continue;
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(line);
    } catch {
      throw new SessionFormatError(`line ${at + 1} is not JSON`);
    }
    if (typeof parsed !== "object" || parsed === null || !("kind" in parsed)) {
      throw new SessionFormatError(`line ${at + 1} has no kind`);
    }
    const kind = (parsed as { kind: unknown }).kind;
    if (kind === "session") {
      header = parsed as SessionHeader;
    } else if (kind === "wire") {
      steps.push(parsed as WireStep);
    } else if (kind === "session_summary") {
      summary = parsed as SessionSummary;
    } else {
      throw new SessionFormatError(`line ${at + 1} has an unknown kind: ${String(kind)}`);
    }
  }

  if (header === undefined) {
    throw new SessionFormatError("the trace has no session header");
  }
  if (summary === undefined) {
    throw new SessionFormatError("the trace has no summary; the session did not finish");
  }
  if (steps.length === 0) {
    throw new SessionFormatError("the trace has no wire steps");
  }
  return { header, steps, summary };
}

/** Whether a step carries a number in the server's stream. */
export function isNumbered(step: WireStep): boolean {
  return step.sequence > 0;
}

/**
 * The plain-language reason each step is worth looking at.
 *
 * Presentation, like story.ts: the trace carries what happened, and this carries why it matters. Somebody
 * who does not know SoupBinTCP cannot get "Unsequenced Data Packet" from a type byte.
 */
export function meaningOf(step: WireStep): string {
  switch (step.name) {
    case "Login Request":
      return "The client identifies itself. Nothing else is legal until this is answered.";
    case "Login Accepted":
      return "The server names the session and the sequence of the next packet it will number — the next one, not this one. From here both sides count for themselves.";
    case "Enter Order":
      return "An order, in an Unsequenced Data Packet: messages to the exchange carry no position, because they are not part of a numbered stream.";
    case "Cancel Order":
      return "A cancel. Zero shares means everything still open, and the only answer is the Canceled message that results.";
    case "Logout Request":
      return "The client leaves. The specification gives no acknowledgement, so none is sent.";
    case "Accepted":
      return "The exchange accepted the order, and numbered the reply. Note that the order token is the client's identifier and the reference number is the exchange's.";
    case "Executed":
      return "A fill. It is numbered in the same stream as the acknowledgements, because there is one stream out — whoever the message was caused by.";
    case "Canceled":
      return "The cancel took effect, and the message says how many shares it removed rather than leaving the client to subtract.";
    case "a fill lands":
      return "Matching happens here, and this project deliberately does not implement it: the caller drives executions so that the protocol behaviour around matching is what gets tested.";
    case "End Of Session":
      return "The server is finished with the connection.";
    default:
      return step.detail;
  }
}
