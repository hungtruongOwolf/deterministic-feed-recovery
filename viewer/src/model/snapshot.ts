// The Glimpse trace's shape, and nothing else.
//
// Same rule as every other model here: this parses and indexes. Both books come from the file: the venue's in the
// header, the client's on every frame, and the `matches` flag is the C++'s own comparison of the two. A viewer that
// compared them itself would be a third opinion on a question that has exactly two parties.

export type FrameKind = "A" | "g" | "S" | "G" | "Z" | string;

export interface SnapshotFrame {
  readonly kind: "frame";
  readonly step: number;
  /** The SoupBinTCP or Glimpse type byte. */
  readonly type: FrameKind;
  readonly name: string;
  readonly detail: string;
  /** The client's book after this frame. Prices are signed integers at four implied decimals. */
  readonly bid: number;
  readonly bid_size: number;
  readonly ask: number;
  readonly ask_size: number;
  readonly bid_levels: number;
  readonly ask_levels: number;
  /** Non-zero once End Of Snapshot has arrived. */
  readonly resume_from: number;
  /** The C++'s own comparison: does the client's book now equal the venue's? */
  readonly matches: boolean;
}

export interface SnapshotLimit {
  readonly claim: string;
  readonly status: "measured" | "not-measurable" | "simulated";
  readonly note: string;
}

export interface SnapshotHeader {
  readonly kind: "glimpse";
  readonly schema: string;
  readonly symbol: string;
  readonly session: number;
  readonly venue_bid: number;
  readonly venue_bid_size: number;
  readonly venue_ask: number;
  readonly venue_ask_size: number;
  readonly venue_bid_levels: number;
  readonly venue_ask_levels: number;
  readonly resume_from: number;
  readonly limits: readonly SnapshotLimit[];
}

export interface SnapshotTrace {
  readonly header: SnapshotHeader;
  readonly frames: readonly SnapshotFrame[];
}

export class SnapshotFormatError extends Error {}

export function parseSnapshot(text: string): SnapshotTrace {
  let header: SnapshotHeader | undefined;
  const frames: SnapshotFrame[] = [];

  for (const [at, line] of text.split("\n").entries()) {
    if (line.trim() === "") {
      continue;
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(line);
    } catch {
      throw new SnapshotFormatError(`line ${at + 1} is not JSON`);
    }
    const kind = (parsed as { kind?: unknown }).kind;
    if (kind === "glimpse") {
      header = parsed as SnapshotHeader;
    } else if (kind === "frame") {
      frames.push(parsed as SnapshotFrame);
    } else {
      throw new SnapshotFormatError(`line ${at + 1} has an unknown kind: ${String(kind)}`);
    }
  }
  if (header === undefined) {
    throw new SnapshotFormatError("the trace has no glimpse header");
  }
  if (frames.length === 0) {
    throw new SnapshotFormatError("the trace has no frames");
  }
  return { header, frames };
}

/** Plain language for one frame, because a type byte explains nothing to somebody who has not read the spec. */
export function meaningOf(frame: SnapshotFrame): string {
  switch (frame.type) {
    case "A":
      return "The service accepts the connection. A snapshot is its own numbered stream: nothing to do with the live feed's numbering, which is a tempting thing to conflate because both are sequence numbers on the same kind of packet.";
    case "g":
      return "Begin, before any state. A client that connected to the wrong service finds out here rather than after applying somebody else's book.";
    case "S":
      return "One price level. The service walks its book and sends one of these per level: O(depth), not O(messages), which is the whole reason a snapshot can be served to a client that cannot catch up by replay.";
    case "G":
      return "End Of Snapshot, carrying the sequence the state is valid as of. It is the *next* message, not the last one included: off by one either way looks like a working snapshot on a quiet feed and corrupts a book on a busy one.";
    case "Z":
      return "End Of Session, so the client knows the snapshot is complete rather than truncated by a dropped connection. Without it the two are the same thing on the wire.";
    default:
      return frame.detail;
  }
}
