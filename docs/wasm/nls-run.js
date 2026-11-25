// Runs the WebAssembly build of nls (nls.js / nls.wasm, from `make wasm`)
// as if from a shell: run(["--", "undo last commit"]) calls main() and
// returns { rc, out, err }, the exit code and what it printed.
//
// main() may be called any number of times: Emscripten's callMain() puts
// argv on the wasm stack and never pops it, so each call is wrapped in
// stackSave() / stackRestore(); without that the stack runs out after a few
// thousand calls. Used by the landing page and by tests/wasm_parity.cjs.
(function (root) {
    'use strict';

    // createNls: the factory nls.js defines. Resolves to { run, FS }.
    async function createNlsRunner(createNls, options) {
        let out = [], err = [];
        const m = await createNls(Object.assign({
            print: s => out.push(s),
            printErr: s => err.push(s),
        }, options || {}));
        function run(args) {
            out = []; err = [];
            const sp = m.stackSave();
            let rc;
            try {
                rc = m.callMain(args.slice());   // callMain prepends argv[0] in place
                m._fflush(0);
            } finally {
                m.stackRestore(sp);
            }
            const text = lines => lines.map(l => l + '\n').join('');
            return { rc, out: text(out), err: text(err) };
        }
        return { run, FS: m.FS };
    }

    if (typeof module === 'object' && module.exports) module.exports = createNlsRunner;
    else root.createNlsRunner = createNlsRunner;
})(this);
