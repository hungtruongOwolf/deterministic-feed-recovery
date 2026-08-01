// What this thing is called, and what it is: in one place, so the page, the tab and the checker agree.
//
// The header used to read `deterministic feed recovery` in lower case, which is a style a repository slug can
// afford and a page cannot: somebody arriving cold reads it as three words rather than as the name of
// something. A name is capitalised so that it is legible *as a name*, and it is followed by a sentence saying
// what the thing does, because a name alone has never explained anything to anybody.

/** Title case, as a proper noun. */
export const PROJECT_NAME = "Deterministic Feed Recovery";

/** The short form, for the places a full name will not fit. Expanded on first use, never used alone. */
export const PROJECT_ABBREVIATION = "DFR";

/** One sentence, no jargon, present tense: what somebody is looking at. */
export const PROJECT_TAGLINE =
  "A market-data feed broken on purpose, and the C++ client that puts it back together.";

/** The second sentence: what this particular page is, as opposed to the project. */
export const PAGE_TAGLINE =
  "This page replays three recorded runs of it: no server, no live connection, every number read from the recording.";

/** For the browser tab, where the name has to carry the explanation on its own. */
export const PAGE_TITLE = `${PROJECT_NAME}: a market-data client under fault injection`;
