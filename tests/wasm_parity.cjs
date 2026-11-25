// The browser playground's WebAssembly build (docs/wasm/) against the
// native binary: for every prompt in the test suites, `nls <tool> "..."`,
// `nls -- "..."` (tool inferred, as the playground and the Ctrl+G widget
// call it), `nls --explain ...` and `nls -v ...` must print the same
// stdout and stderr and exit with the same code in both.
//
//   node tests/wasm_parity.cjs ./nls [docs/wasm/nls.js]
//
// The wasm build runs in node on an in-memory copy of the repository (the
// lexer checks that grep paths exist, jq reads the input file), with the
// same working directory layout the native binary sees.
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const native = path.resolve(process.argv[2] || './nls');
const wasmJs = path.resolve(process.argv[3] || path.join(root, 'docs/wasm/nls.js'));
// The page's own runner, so its stack handling is tested too.
const createNlsRunner = require(path.join(root, 'docs/wasm/nls-run.js'));

const TOOLS = new Set(['ffmpeg', 'jq', 'grep', 'find', 'tar', 'git', 'curl', 'magick']);
// file -> the column holding the prompt (the tool is column 0)
const SUITES = {
    'cases.tsv': 1, 'infer_cases.tsv': 1, 'warn_cases.tsv': 1,
    'explain_cases.tsv': 1, 'paraphrase_cases.tsv': 2,
};
// Not copied into the in-memory repository.
const SKIP_DIRS = new Set(['.git', 'build', 'dist', 'midia_files', 'node_modules']);
const MAX_FILE = 4 << 20;

function argLists() {
    const seen = new Set(), lists = [];
    const add = args => {
        const k = JSON.stringify(args);
        if (!seen.has(k)) { seen.add(k); lists.push(args); }
    };
    // The options the playground passes through (not --version: the
    // native build stamps its commit, the wasm build doesn't).
    for (const a of [['--list'], ['--help'], ['--shell', 'zsh'], ['--shell', 'bash'], ['--list', 'nope']])
        add(a);
    for (const t of TOOLS) add(['--list', t]);
    for (const [file, col] of Object.entries(SUITES)) {
        const text = fs.readFileSync(path.join(root, 'tests', file), 'utf8');
        for (const line of text.split('\n')) {
            if (!line || line.startsWith('#')) continue;
            const f = line.split('\t');
            const prompt = f[col];
            if (!prompt) continue;
            add(['--', prompt]);
            add(['--explain', '--', prompt]);
            if (TOOLS.has(f[0])) {
                add([f[0], prompt]);
                add(['--explain', f[0], prompt]);
                add(['-v', f[0], prompt]);
            }
        }
    }
    return lists;
}

function mirror(FS, dir, to) {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        if (SKIP_DIRS.has(e.name)) continue;
        const from = path.join(dir, e.name), dest = to + '/' + e.name;
        if (e.isDirectory()) {
            FS.mkdir(dest);
            mirror(FS, from, dest);
        } else if (e.isFile()) {
            const size = fs.statSync(from).size;
            FS.writeFile(dest, size <= MAX_FILE ? fs.readFileSync(from) : new Uint8Array(0));
        }
    }
}

async function main() {
    if (!fs.existsSync(native)) throw new Error(`no native binary at ${native} (run make)`);
    if (!fs.existsSync(wasmJs)) throw new Error(`no wasm build at ${wasmJs} (run make wasm)`);
    const nls = await createNlsRunner(require(wasmJs));
    nls.FS.mkdirTree('/repo');
    mirror(nls.FS, root, '/repo');
    nls.FS.chdir('/repo');

    const lists = argLists();
    let pass = 0, fail = 0;
    for (const args of lists) {
        let w;
        try {
            w = nls.run(args);
        } catch (e) {
            // A trap leaves the instance unusable: stop here.
            throw new Error(`nls ${args.map(a => JSON.stringify(a)).join(' ')}: ${e.message}`);
        }
        const n = spawnSync(native, args, { cwd: root, stdio: ['ignore', 'pipe', 'pipe'], encoding: 'utf8' });
        if (n.status === w.rc && n.stdout === w.out && n.stderr === w.err) { pass++; continue; }
        fail++;
        if (fail <= 20) {
            console.log(`FAIL: nls ${args.map(a => JSON.stringify(a)).join(' ')}`);
            for (const [k, nv, wv] of [['exit', n.status, w.rc], ['stdout', n.stdout, w.out], ['stderr', n.stderr, w.err]])
                if (nv !== wv) console.log(`  ${k}\n    native: ${JSON.stringify(nv)}\n    wasm:   ${JSON.stringify(wv)}`);
        }
    }
    console.log(`${pass} passed, ${fail} failed (${path.relative(root, wasmJs)} vs ${path.relative(root, native) || native})`);
    if (fail) {
        console.log('If the native output is right, rebuild the playground: make wasm (or make wasm-docker)');
        process.exit(1);
    }
}

main().catch(e => { console.error(`wasm parity: ${e.message}`); process.exit(2); });
