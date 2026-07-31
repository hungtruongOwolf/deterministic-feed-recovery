// Two books, and one of them filling up.
//
// The third defence was the one nobody could watch. In the film it is a plane the escalation marker falls onto, which
// shows the *consequence* of reaching a snapshot and not the snapshot itself. What a snapshot is — and the only thing
// that makes one believable — is that a client holding nothing but bytes ends up with the venue's book.
//
// So: the venue's book on the left, fixed. The client's on the right, empty at first, one level at a time, until the
// two are identical and the drawing says so. Then the resume sequence arrives, which is the one number a snapshot
// protocol exists to deliver.
//
// A list rather than a moving picture, deliberately. The film moves because a lost packet is an *event* and motion is
// the only way to show something failing to arrive. A rebuild is monotone — it only ever gains — so the whole of it
// fits on one sheet and a reader can see the convergence at a glance instead of waiting for it.

import type { SnapshotFrame, SnapshotTrace } from "../model/snapshot";
import { meaningOf } from "../model/snapshot";

const SCALE = 10_000;

function money(raw: number): string {
  return raw === 0 ? "—" : (raw / SCALE).toFixed(4);
}

interface Props {
  readonly trace: SnapshotTrace;
  readonly hover: number | undefined;
  readonly onHover: (step: number | undefined) => void;
}

export function Rebuild({ trace, hover, onHover }: Props) {
  const { header, frames } = trace;
  const last = frames[frames.length - 1];

  return (
    <div className="rebuild">
      <div className="rebuild__books">
        <Book
          title="THE VENUE'S BOOK"
          note="what the service is looking at"
          bid={header.venue_bid}
          bidSize={header.venue_bid_size}
          ask={header.venue_ask}
          askSize={header.venue_ask_size}
          bidLevels={header.venue_bid_levels}
          askLevels={header.venue_ask_levels}
        />
        <div className={`rebuild__join ${last?.matches === true ? "is-equal" : ""}`}>
          <span className="rebuild__join-mark mono">{last?.matches === true ? "=" : "≠"}</span>
          <span className="rebuild__join-note">
            {last?.matches === true ? "identical" : "not yet"}
          </span>
        </div>
        <Book
          title="THE CLIENT'S BOOK"
          note="rebuilt from the frames alone"
          bid={(hover === undefined ? last : frames[hover])?.bid ?? 0}
          bidSize={(hover === undefined ? last : frames[hover])?.bid_size ?? 0}
          ask={(hover === undefined ? last : frames[hover])?.ask ?? 0}
          askSize={(hover === undefined ? last : frames[hover])?.ask_size ?? 0}
          bidLevels={(hover === undefined ? last : frames[hover])?.bid_levels ?? 0}
          askLevels={(hover === undefined ? last : frames[hover])?.ask_levels ?? 0}
        />
      </div>

      <ol className="rebuild__frames">
        {frames.map((frame: SnapshotFrame) => (
          <li
            key={frame.step}
            className={`frame frame--${frame.type} ${hover === frame.step ? "is-on" : ""} ${
              frame.matches ? "is-complete" : ""
            }`}
            onMouseEnter={() => onHover(frame.step)}
            onMouseLeave={() => onHover(undefined)}
            onFocus={() => onHover(frame.step)}
            onBlur={() => onHover(undefined)}
            tabIndex={0}
          >
            <span className="frame__type mono">{frame.type}</span>
            <span className="frame__name">{frame.name}</span>
            <span className="frame__detail mono">{frame.detail}</span>
            <span className="frame__book mono">
              {frame.bid_levels + frame.ask_levels === 0
                ? "empty"
                : `${frame.bid_levels}+${frame.ask_levels} lvl`}
            </span>
          </li>
        ))}
      </ol>

      <p className="rebuild__caption">
        {hover === undefined
          ? `Hover a frame to see the book as it stood after it. Resume the live feed at ${header.resume_from.toLocaleString()} — the next message, not the last one included.`
          : meaningOf(frames[hover]!)}
      </p>
    </div>
  );
}

function Book({
  title,
  note,
  bid,
  bidSize,
  ask,
  askSize,
  bidLevels,
  askLevels,
}: {
  readonly title: string;
  readonly note: string;
  readonly bid: number;
  readonly bidSize: number;
  readonly ask: number;
  readonly askSize: number;
  readonly bidLevels: number;
  readonly askLevels: number;
}) {
  return (
    <div className="minibook">
      <span className="minibook__title mono">{title}</span>
      <span className="minibook__note">{note}</span>
      <div className="minibook__rows">
        <span className="minibook__label mono">ask</span>
        <span className="minibook__price minibook__price--ask mono">{money(ask)}</span>
        <span className="minibook__size mono">{askSize > 0 ? `×${askSize}` : ""}</span>
        <span className="minibook__depth mono">{askLevels > 0 ? `${askLevels} lvl` : ""}</span>

        <span className="minibook__label mono">bid</span>
        <span className="minibook__price minibook__price--bid mono">{money(bid)}</span>
        <span className="minibook__size mono">{bidSize > 0 ? `×${bidSize}` : ""}</span>
        <span className="minibook__depth mono">{bidLevels > 0 ? `${bidLevels} lvl` : ""}</span>
      </div>
    </div>
  );
}
