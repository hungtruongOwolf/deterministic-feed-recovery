// Turning a trace into something watchable: one beat per event, each with a picture and a sentence.
//
// This is presentation, not domain logic. Every number a beat shows is read from the event: the state, the
// watermark, the holes. What is added here is *how it moves* and *what it means in plain words*, which are
// the two things a trace file cannot carry and a reader cannot get from a table of field names.
//
// The sentences are the point. A panel that says `snapshot_rejected · snapshot_behind_buffer` is only
// legible to somebody who already knows the codebase. "The snapshot is older than the oldest message the
// client kept, so twenty messages exist in neither" is legible to anybody.

import type { Trace, TraceEvent } from "./trace";

/** Which track a glyph travels on. Three, because they mean three different things. */
export type Lane = "data" | "request" | "snapshot";

/** Left to right is the feed. Right to left is the client asking for something. */
export type Direction = "out" | "back";

/** What becomes of the glyph when it gets there. */
export type Fate =
  | "arrive" // absorbed by the client
  | "vanish" // lost in the network; nothing tells the client
  | "reject" // arrives corrupted and is refused at the client's edge
  | "twin" // a duplicate: two glyphs, one absorbed and one discarded
  | "lag" // delayed: crosses slowly and arrives out of position
  | "settle"; // a snapshot or a reply that lands and stays

export type Tone = "normal" | "quiet" | "fault" | "repair" | "fatal";

export interface Beat {
  readonly at: number; // index into the beat list
  readonly event: TraceEvent;
  readonly lane: Lane;
  readonly direction: Direction;
  readonly fate: Fate;
  readonly tone: Tone;
  /** Plain language, present tense, no field names. */
  readonly caption: string;
  /** True for the beats worth stopping to read. Drives the act card. */
  readonly notable: boolean;
  readonly label: string;
}

function range(event: TraceEvent): string {
  return event.end > event.first ? `${event.first}–${event.end - 1}` : "";
}

function count(event: TraceEvent): number {
  return Math.max(0, event.end - event.first);
}

function describe(event: TraceEvent): Omit<Beat, "at" | "event"> {
  const r = range(event);
  const n = count(event);

  switch (event.event) {
    case "published":
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: "normal",
        notable: false,
        label: r,
        caption: `The venue publishes messages ${r}.`,
      };

    case "heartbeat_sent":
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: "quiet",
        notable: false,
        label: "♥",
        caption:
          "A heartbeat. It carries no messages, but it tells the client where the feed has reached, which is how a gap is noticed during a quiet period.",
      };

    case "fault_applied":
      return {
        lane: "data",
        direction: "out",
        fate: "vanish",
        tone: "fault",
        notable: true,
        label: "✕",
        caption:
          "The injector damages this packet. Nothing sends a warning: from here on the client can only find out by noticing what is missing.",
      };

    case "packet_dropped":
      return {
        lane: "data",
        direction: "out",
        fate: "vanish",
        tone: "fault",
        notable: true,
        label: "✕",
        caption: "The network loses this packet outright. Nothing tells the client.",
      };

    case "packet_duplicated":
      return {
        lane: "data",
        direction: "out",
        fate: "twin",
        tone: "fault",
        notable: false,
        label: "×2",
        caption:
          "This packet arrives twice. The client must recognise the second copy and not deliver it again: a duplicate corrupts a book as surely as a loss.",
      };

    case "packet_delayed":
      return {
        lane: "data",
        direction: "out",
        fate: "lag",
        tone: "fault",
        notable: false,
        label: "…",
        caption:
          "This packet is held back and arrives out of position. Most gaps on a real feed are this, not loss, which is why the client waits before asking for anything.",
      };

    case "packet_discarded":
      return {
        lane: "data",
        direction: "out",
        fate: "reject",
        tone: "fault",
        notable: false,
        label: "⌀",
        caption:
          "The packet arrives, but its framing does not add up, so the client refuses it: exactly as a real receiver would. From the client's side this is indistinguishable from a loss.",
      };

    case "packet_accepted":
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: "normal",
        notable: false,
        label: r,
        caption: `Messages ${r} cross to the client, in order.`,
      };

    case "packet_duplicate":
      return {
        lane: "data",
        direction: "out",
        fate: "twin",
        tone: "quiet",
        notable: false,
        label: "dup",
        caption:
          "A copy of something already delivered. The client drops it and carries on: routine, and the reason the duplicate count is not an error rate.",
      };

    case "gap_opened":
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: "fault",
        notable: true,
        label: r,
        caption: `The client notices that messages ${r} never arrived, ${n} of them. It keeps delivering what does arrive; stalling on a hole would make one loss into an outage.`,
      };

    case "gap_filled":
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: "repair",
        notable: true,
        label: r,
        caption: `The hole closes: ${event.detail} messages recovered. Nothing was lost.`,
      };

    case "session_reset":
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: "fatal",
        notable: true,
        label: "new session",
        caption:
          "The session identifier changed. Every sequence number the client holds now refers to a stream that no longer exists, so it starts again from here.",
      };

    case "retransmit_requested":
      return {
        lane: "request",
        direction: "back",
        fate: "settle",
        tone: "repair",
        notable: true,
        label: `${r} · try ${event.attempt}`,
        caption: `The client asks the venue to resend messages ${r}. Attempt ${event.attempt}: it waited first, because a packet that is merely late needs no help.`,
      };

    case "retransmit_served":
      return {
        lane: "request",
        direction: "out",
        fate: "settle",
        tone: "repair",
        notable: true,
        label: r,
        caption: `The venue resends messages ${r} from its retention window.`,
      };

    case "retransmit_refused":
      return {
        lane: "request",
        direction: "out",
        fate: "settle",
        tone: "fatal",
        notable: true,
        label: "refused",
        caption:
          "Too late. Those messages have aged out of the venue's retention window, so no amount of asking will bring them back. The client is told, rather than left to time out.",
      };

    case "range_abandoned":
      return {
        lane: "request",
        direction: "out",
        fate: "settle",
        tone: "fatal",
        notable: true,
        label: r,
        caption: `The client gives up on retransmission for messages ${r}. A snapshot is the only repair left.`,
      };

    case "snapshot_requested":
      return {
        lane: "snapshot",
        direction: "back",
        fate: "settle",
        tone: "repair",
        notable: true,
        label: "snapshot?",
        caption:
          "The client asks for a snapshot of the whole book. Meanwhile it keeps buffering the live feed, because it cannot yet know which of those messages the snapshot will already contain.",
      };

    case "snapshot_replied":
      return {
        lane: "snapshot",
        direction: "out",
        fate: "settle",
        tone: "repair",
        notable: true,
        label: `frozen at ${event.first}`,
        caption: `The snapshot arrives. It shows the book as it was at message ${event.first}: the moment the request reached the venue, not the moment the reply left it.`,
      };

    case "snapshot_applied":
      return {
        lane: "snapshot",
        direction: "out",
        fate: "settle",
        tone: "repair",
        notable: true,
        label: r,
        caption:
          "The snapshot is new enough: it meets the buffered messages with nothing between them, so the buffer replays on top of it and the stream continues.",
      };

    case "snapshot_rejected":
      return {
        lane: "snapshot",
        direction: "out",
        fate: "settle",
        tone: "fatal",
        notable: true,
        label: `${n} lost`,
        caption: `The snapshot is older than the oldest message the client managed to keep. Messages ${r}(${n} of them) exist in neither the snapshot nor the buffer, and nothing knows they are gone. A client that carried on here would publish a book that looks complete and is permanently wrong. This one refuses.`,
      };

    case "replay_started":
      return {
        lane: "snapshot",
        direction: "out",
        fate: "settle",
        tone: "repair",
        notable: true,
        label: r,
        caption: "The buffered messages are replayed on top of the snapshot.",
      };

    case "replay_finished":
      return {
        lane: "snapshot",
        direction: "out",
        fate: "settle",
        tone: "repair",
        notable: true,
        label: "live",
        caption: "The replay is done. The client is live again, and nothing is missing.",
      };

    case "state_changed":
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: event.state === "failed" ? "fatal" : "normal",
        notable: true,
        label: event.state,
        caption:
          event.state === "failed"
            ? "The client is in a state it cannot honestly continue from, and it says so on every poll rather than producing a book it knows to be wrong."
            : `The client is now ${event.state}.`,
      };

    default:
      return {
        lane: "data",
        direction: "out",
        fate: "arrive",
        tone: "normal",
        notable: false,
        label: event.event,
        caption: event.event.replace(/_/g, " "),
      };
  }
}

export function buildStory(trace: Trace): readonly Beat[] {
  return trace.events.map((event, at) => ({ at, event, ...describe(event) }));
}
