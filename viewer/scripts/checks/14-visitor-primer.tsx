// Whether somebody who does not know the subject can follow it.
//
// The failure this section exists to prevent: a page that describes what was built, to somebody who already
// knows why that was worth building. Measured before it existed: the first screen used nine technical terms
// before defining any of them, every heading was written from the builder's point of view, and nothing said why
// any of it mattered.

import { readFileSync } from "node:fs";
import { renderToStaticMarkup } from "react-dom/server";
import { plainly } from "../../src/model/plain";
import { Outcome } from "../../src/panels/Outcome";
import { Primer } from "../../src/panels/Primer";
import { check, traces } from "./fixtures";

export function run(): void {
  console.log("\nwhether somebody who does not know the subject can follow it");

  const primer = renderToStaticMarkup(<Primer onWatch={() => {}} />);
  const words = (html: string) => html.replace(/<[^>]*>/g, " ").replace(/\s+/g, " ");
  const visible = words(primer);

  // No jargon before it has been earned. Each of these is a term a reader would have to already know.
  const jargon = [
    "market-data", "packet", "multicast", "sequence number", "retransmit", "snapshot",
    "MoldUDP64", "SoupBinTCP", "OUCH", "DEEP", "order book", "allocation", "sanitiser",
    "lock-free", "WebAssembly", "C++",
  ];
  const used = jargon.filter((term) => new RegExp(term.replace("+", "\\+"), "i").test(visible));
  check(used.length === 0, `the opening explains itself with no jargon${used.length > 0 ? `; used: ${used.join(", ")}` : ""}`);

  // And it has to say why any of it matters. A mechanism with no stakes is a description.
  check(
    /confidently wrong|does not know|expensive/i.test(visible),
    "the opening says what goes wrong for somebody, not just what goes wrong",
  );
  check(/exchange/i.test(visible) && /prices/i.test(visible), "it names the subject in words anybody has");
  check(visible.split(" ").filter((w) => /[a-z]{2}/i.test(w)).length <= 200, "and stays short enough to read");
  check(/Break it/.test(primer), "there is one obvious thing to press");

  // The plain-language outcome, so the same run is legible without the drawing.
  const settled = traces[0]!.events[traces[0]!.events.length - 1]!;
  const plain = plainly(traces[0]!, settled);
  check(plain.settled, "a finished run reads as an outcome rather than a moment");
  check(plain.good, "and the first act's outcome is a good one, because it is");
  const outcome = words(renderToStaticMarkup(<Outcome outcome={plain} />));
  const jargonInOutcome = jargon.filter((t) => new RegExp(t.replace("+", "\\+"), "i").test(outcome));
  check(jargonInOutcome.length === 0, `the outcome is jargon-free${jargonInOutcome.length > 0 ? `; used: ${jargonInOutcome.join(", ")}` : ""}`);
  check(/identical/.test(outcome), "and states the claim in a word a reader already owns");

  // The failing act has to read as failing, in the same register.
  const broken = plainly(traces[2]!, traces[2]!.events[traces[2]!.events.length - 1]!);
  check(!broken.good, "the act that loses data does not read as a success");
  check(/knows/.test(broken.result), "and says the client knows, which is the point of that act");

  // Headings are questions a reader might have, not statements about what exists.
  const app = readFileSync("src/App.tsx", "utf8");
  const headings = [...app.matchAll(/act-section__number mono">\d<\/span>\s*\n\s*([^<\n]+)/g)].map((m) =>
    m[1]!.trim(),
  );
  check(headings.length === 5, `${headings.length} sections`);
  check(
    headings[0] !== undefined && /notice/.test(headings[0]),
    "the first section a visitor reaches is about what goes wrong, not about the author",
  );
  check(
    headings.findIndex((h) => /while I was building/.test(h)) >= 3,
    "the engineering section comes after the ones anybody can read",
  );
}
