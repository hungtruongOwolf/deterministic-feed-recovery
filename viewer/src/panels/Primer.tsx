// What this is about, before any of it is named.
//
// Measured: the old first screen used nine technical terms before explaining any of them — market-data, packet, feed,
// client, allocations, sanitiser, WebAssembly, C++20, diverging — and said nothing anywhere about why any of it
// mattered. It described what had been built, to somebody who already knew why that was worth building.
//
// The subject is genuinely explainable to anyone, which is what makes not explaining it a choice rather than a
// difficulty. A market is a stream of prices; a network loses some of them; a copy of the prices with a hole in it
// looks complete and is wrong. That is three sentences and no jargon, and everything technical on this page is
// downstream of it.
//
// The stakes are the part that was missing, not the mechanism. "Packets are lost" is a fact about networks. "You are
// pricing against a market that has moved and you do not know" is why anybody built this.

interface Props {
  readonly onWatch: () => void;
}

export function Primer({ onWatch }: Props) {
  return (
    <section className="primer">
      <ol className="primer__beats">
        <li className="beat">
          <span className="beat__number mono">1</span>
          <p className="beat__text">
            A stock exchange sends out prices constantly — every new bid, every new offer, every trade, tens of
            thousands a second. Everyone trading has their own copy, assembled from that stream as it arrives.
          </p>
        </li>
        <li className="beat">
          <span className="beat__number mono">2</span>
          <p className="beat__text">
            The network loses some of it. Not often, and not never. When it does, your copy of the prices has a hole
            in it — and the copy does not look broken. It looks like a market where nothing happened.
          </p>
        </li>
        <li className="beat beat--stake">
          <span className="beat__number mono">3</span>
          <p className="beat__text">
            So you are quoting prices into a market that has moved without you, and you do not know. Being{" "}
            <strong>confidently wrong</strong> is the expensive failure here. Knowing you are blind is survivable;
            believing a stale price is not.
          </p>
        </li>
      </ol>

      <p className="primer__thesis">
        This is the machine that notices, asks for the missing pieces, and then <strong>proves</strong> the copy it
        ended up with is the copy it would have had if nothing had been lost.
      </p>

      <button className="primer__go" onClick={onWatch}>
        ▶ &nbsp;Break it, and watch it repair
      </button>
      <span className="primer__go-note">
        runs in your browser, on your machine — nothing is pre-recorded
      </span>
    </section>
  );
}
