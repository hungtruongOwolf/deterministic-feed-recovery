// Detail that is reachable without being in the way.
//
// The page reached 1,504 words of visible prose across 13 stacked sections, because every criticism was
// answered by *adding* an explanation. The explanations were true and the page was unreadable: a six-minute
// read in front of a two-minute film.
//
// The material is not the problem — most of it belongs in `docs/`, where it already exists. What was wrong is
// that all of it was at the same visual weight, so nothing was. This is the fix: one line of the reason, and
// the rest one click away for whoever wants it.
//
// A native `<details>` rather than state and a chevron. It is keyboard-accessible, it survives with CSS off,
// and Ctrl-F finds text inside a closed one — which a div behind a `useState` does not.

import type { ReactNode } from "react";

interface Props {
  /** What is inside, in the reader's terms. Not "more" — that is a word that promises nothing. */
  readonly summary: string;
  readonly children: ReactNode;
  readonly open?: boolean;
}

export function Disclosure({ summary, children, open }: Props) {
  return (
    <details className="fold" open={open === true}>
      <summary className="fold__summary">{summary}</summary>
      <div className="fold__body">{children}</div>
    </details>
  );
}
