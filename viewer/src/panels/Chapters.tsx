// The three runs, as three visible steps rather than a dropdown.
//
// A dropdown hides the most important thing about them: they are one experiment in three parts, meant to be
// watched in order, each removing a defence the one before it had. Somebody arriving at the page has no way
// to know that from a select element — so the three are laid out as chapters, numbered, with what each one
// takes away, and the current one filled in.

export interface Chapter {
  readonly file: string;
  readonly ordinal: string;
  readonly title: string;
  readonly removes: string;
}

export const CHAPTERS: readonly Chapter[] = [
  {
    file: "redundant-ab.jsonl",
    ordinal: "1",
    title: "Two lines carry the feed",
    removes: "nothing removed — packets are lost and nobody has to ask anyone for anything",
  },
  {
    file: "recovering-seed4711.jsonl",
    ordinal: "2",
    title: "The second line is taken away",
    removes: "the same faults now cost a round trip each, asking the venue to resend",
  },
  {
    file: "glimpse-race.jsonl",
    ordinal: "3",
    title: "The retransmit window has closed too",
    removes: "the last resort arrives reflecting an older moment — and some data is gone for good",
  },
];

interface Props {
  readonly current: string;
  readonly onChoose: (file: string) => void;
}

export function Chapters({ current, onChoose }: Props) {
  return (
    <nav className="chapters" aria-label="the three parts of the experiment">
      <div className="chapters__head mono">ONE EXPERIMENT, THREE PARTS — WATCH THEM IN ORDER</div>
      <div className="chapters__row">
        {CHAPTERS.map((chapter) => (
          <button
            key={chapter.file}
            className={`chapter ${chapter.file === current ? "is-on" : ""}`}
            onClick={() => onChoose(chapter.file)}
          >
            <span className="chapter__ordinal">{chapter.ordinal}</span>
            <span className="chapter__text">
              <span className="chapter__title">{chapter.title}</span>
              <span className="chapter__removes">{chapter.removes}</span>
            </span>
          </button>
        ))}
      </div>
    </nav>
  );
}
