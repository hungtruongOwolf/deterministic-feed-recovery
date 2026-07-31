// The defects, first on the page after the evidence bar.
//
// This panel is the one place the word-count budget is deliberately relaxed, and the distinction is worth
// stating rather than quietly exempting: the prose the budget exists to suppress was prose *about the page* —
// explaining a control, justifying a design, describing a method. This prose *is* the substance. An engineer
// with five minutes came to find out whether the person who wrote this notices things, and the only evidence of
// that is a hard defect explained correctly.
//
// Four of the six are my own mistakes. That is the point rather than an admission: a portfolio of things that
// went right is a portfolio of things nobody checked.

import { FINDINGS, type Finding } from "../model/findings";

export function Findings() {
  return (
    <div className="findings">
      {FINDINGS.map((finding: Finding) => (
        <article key={finding.title} className="finding">
          <header className="finding__head">
            <span className="finding__kind mono">{finding.kind}</span>
            <h3 className="finding__title">{finding.title}</h3>
          </header>
          <dl className="finding__body">
            <dt className="mono">how it hid</dt>
            <dd>{finding.hid}</dd>
            <dt className="mono">what caught it</dt>
            <dd>{finding.caught}</dd>
            <dt className="mono">what it changed</dt>
            <dd>{finding.matters}</dd>
          </dl>
          <a className="finding__where" href={finding.where}>
            the code, and the reasoning next to it →
          </a>
        </article>
      ))}
    </div>
  );
}
