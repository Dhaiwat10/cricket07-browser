Error.stackTraceLimit = 400;
// --pre-js for the Node test build: pass RC_* environment variables to the game's getenv().
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  ENV.RC_HEADLESS = '1';   // no page to render to
  for (const [k, v] of Object.entries(process.env)) if (k.startsWith('RC_')) ENV[k] = v;
});
