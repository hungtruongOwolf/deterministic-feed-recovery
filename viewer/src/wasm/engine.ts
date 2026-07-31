// The C++ library, loaded into the page.
//
// This is the difference between a viewer and a screenshot. Before this, everything on the page came from
// three JSONL files generated once and committed: correct, and not yours. Now the same header-only library
// the tests run is compiled to WebAssembly and asked directly, so a seed you type is a run that happens.
//
// It returns JSONL strings rather than objects, and that is the rule rather than laziness. The viewer's one
// rule is that it draws a trace and knows nothing about the state machine behind it; handing JavaScript a
// live object graph would have broken that under the excuse of efficiency, and the parsers in model/ would
// have quietly become a second definition of the format.
//
// The bytes are the same bytes. `scripts/check-wasm.sh` runs six shapes of run through both the native tool
// and this module and diffs them — two compilers, two targets, one seed. If they ever differ, something in
// the library depends on its platform and "deterministic" was a word rather than a property.

export interface TraceParameters {
  readonly seed: number;
  readonly messages: number;
  readonly faults: number;
  readonly lines: 1 | 2;
  readonly glimpse: boolean;
  readonly staleness: number;
}

export interface SessionParameters {
  readonly orders: number;
  readonly fill: number;
  readonly cancel: boolean;
}

export interface Engine {
  readonly runTrace: (parameters: TraceParameters) => string;
  readonly runSession: (parameters: SessionParameters) => string;
}

interface EmscriptenModule {
  cwrap: (name: string, returns: string, args: readonly string[]) => (...values: number[]) => string;
}

type Factory = (options?: Record<string, unknown>) => Promise<EmscriptenModule>;

let pending: Promise<Engine> | undefined;

/**
 * Loads the module once, and hands the same promise to every later caller.
 *
 * Once rather than per-component: the module is a megabyte of instantiation and two panels want it. A
 * second instance would also mean two heaps, which works and wastes a phone's memory for no reason.
 */
export function loadEngine(): Promise<Engine> {
  pending ??= instantiate();
  return pending;
}

async function instantiate(): Promise<Engine> {
  // Built into public/wasm by scripts/build-wasm.sh, so Vite copies it verbatim and the URL is relative to
  // wherever the page is served from — which on Pages is a subpath, not the root.
  const url = new URL("wasm/dfr.js", document.baseURI).href;
  const loaded = (await import(/* @vite-ignore */ url)) as { default: Factory };
  const module = await loaded.default({
    // Emscripten resolves the .wasm relative to the loader by default, which is correct here; naming it
    // explicitly means a change to the bundler's asset handling cannot silently break the fetch.
    locateFile: (path: string) => new URL(`wasm/${path}`, document.baseURI).href,
  });

  const trace = module.cwrap("dfr_run_trace", "string", [
    "number",
    "number",
    "number",
    "number",
    "number",
    "number",
  ]);
  const session = module.cwrap("dfr_run_session", "string", ["number", "number", "number"]);

  return {
    runTrace: (p) =>
      trace(p.seed, p.messages, p.faults, p.lines, p.glimpse ? 1 : 0, p.staleness),
    runSession: (p) => session(p.orders, p.fill, p.cancel ? 1 : 0),
  };
}
