// The landing page's race (docs/index.html, #race-data): every "nls" line it
// shows must be what ./nls prints for that request today, and must pass
// shellcheck, since the page says so. The page shows live WebAssembly output
// (make test-wasm keeps that equal to ./nls); "nls" is the fallback when
// WebAssembly doesn't load, and the claims in the copy rest on it.
//
//   node tests/race_check.cjs ./nls
//
// shellcheck: the one on PATH, else the koalaman/shellcheck image if docker
// runs, else skipped with a note. NLS_REQUIRE_SHELLCHECK=1 (CI) makes a
// missing shellcheck a failure.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const native = path.resolve(process.argv[2] || './nls');
const html = fs.readFileSync(path.join(root, 'docs/index.html'), 'utf8');
const m = html.match(/<script type="application\/json" id="race-data">([\s\S]*?)<\/script>/);
if (!m) { console.error('race_check: no #race-data in docs/index.html'); process.exit(1); }
const race = JSON.parse(m[1]);

let fail = 0;
const bad = msg => { console.error('FAIL ' + msg); fail++; };

// The playground's pretend folder, as far as these requests look at it:
// jq reads users.json's fields, grep checks that src/ exists.
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nls-race-'));
fs.mkdirSync(path.join(dir, 'src'));
fs.writeFileSync(path.join(dir, 'users.json'), JSON.stringify([
    { name: 'Ada', email: 'ada@example.com', age: 36, status: 'active' },
    { name: 'Linus', email: null, age: 29, status: 'away' },
    { name: 'Grace', email: 'grace@example.com', age: 45, status: 'active' },
], null, 2));
for (const f of ['src/app.py', 'src/server.py', 'demo.mp4', 'hero.png', 'release-1.0.tgz'])
    fs.writeFileSync(path.join(dir, f), '');

const TOOLS = new Set(['ffmpeg', 'jq', 'grep', 'find', 'tar', 'git', 'curl', 'magick']);
const VERDICTS = new Set(['ok', 'warn', 'bad']);
if (race.length !== 10) bad(`race has ${race.length} requests; the page's copy says ten`);
for (const r of race) {
    const where = `${r.tool} "${r.prompt}"`;
    if (!TOOLS.has(r.tool)) bad(`${where}: unknown tool`);
    if (!VERDICTS.has(r.verdict)) bad(`${where}: verdict must be ok, warn or bad`);
    if (r.verdict !== 'ok' && !r.why) bad(`${where}: a ${r.verdict} verdict needs a "why"`);
    if (!(r.ms >= 500 && r.ms <= 4000)) bad(`${where}: ms ${r.ms} is outside the 1-4 s the page claims (give or take)`);
    const p = spawnSync(native, [r.tool, r.prompt], { cwd: dir, encoding: 'utf8' });
    if (p.status !== 0 || p.stdout !== r.nls + '\n')
        bad(`${where}\n  page:  ${r.nls}\n  nls:   ${p.stdout.trim()}${p.stderr ? '\n  stderr: ' + p.stderr.trim() : ''}`);
}
fs.rmSync(dir, { recursive: true, force: true });

function shellcheck(script) {
    const args = ['-s', 'sh', '-'];
    let p = spawnSync('shellcheck', args, { input: script, encoding: 'utf8' });
    if (p.error && p.error.code === 'ENOENT') {
        const d = spawnSync('docker', ['info'], { stdio: 'ignore' });
        if (d.error || d.status !== 0) return null;
        p = spawnSync('docker', ['run', '--rm', '-i', 'koalaman/shellcheck:stable', ...args],
                      { input: script, encoding: 'utf8' });
    }
    return p;
}
const script = '#!/bin/sh\n' + race.map(r => r.nls).join('\n') + '\n';
const sc = shellcheck(script);
if (!sc) {
    if (process.env.NLS_REQUIRE_SHELLCHECK) bad('shellcheck is not installed (and docker is not running)');
    else console.log('race_check: shellcheck not found (nor docker); skipped that part');
} else if (sc.status !== 0) {
    bad('shellcheck flags a command the page says passes:\n' + (sc.stdout || sc.stderr));
}

if (fail) { console.error(`race_check: ${fail} failure(s)`); process.exit(1); }
console.log(`race_check: ${race.length} requests match ./nls${sc ? ' and pass shellcheck' : ''}`);
