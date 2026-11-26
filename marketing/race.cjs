#!/usr/bin/env node
// The landing page's race, in a terminal, for recording (marketing/race.tape):
// two panes side by side.
//
//   left   the real nls: `nls <tool> "<request>"` as ten separate processes
//          on this machine, timed end to end (process start included)
//   right  a simulated LLM CLI helper: canned answers after canned waits,
//          nothing sent anywhere, labelled as a simulation on screen
//
// The ten requests, the simulated answers and the waits come from the page
// (docs/index.html, #race-data), so the GIF and the page tell the same story;
// `make test-race` checks the page's nls lines against ./nls and shellcheck.
//
//   node marketing/race.cjs [path/to/nls]      run it from marketing/demo/; any key quits
//                                              (sh marketing/make-demo-media.sh)
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const NLS = process.argv[2] || 'nls';
const html = fs.readFileSync(path.join(root, 'docs/index.html'), 'utf8');
const RACE = JSON.parse(html.match(/<script type="application\/json" id="race-data">([\s\S]*?)<\/script>/)[1]);

const out = process.stdout;
const COLS = out.columns || Number(process.env.COLUMNS) || 140;
const ROWS = out.rows || Number(process.env.LINES) || 46;
const PANE = Math.floor((COLS - 3) / 2);   // " │ " between the panes
const BODY = ROWS - 5;                     // title, rule, body, rule, footer, caption

const C = { reset: '\x1b[0m', p: '\x1b[33m', s: '\x1b[34m', o: '\x1b[32m', e: '\x1b[31m',
            w: '\x1b[33m', c: '\x1b[90m', x: '\x1b[37m', b: '\x1b[1m', '': '' };
const SPIN = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const secs = ms => `${(ms / 1000).toFixed(1)} s`;
const quoted = p => p.includes('$') ? `'${p}'` : `"${p}"`;

// A line is a list of [text, colour]; wrap() cuts it into pane-wide rows.
function wrap(parts) {
    const cells = [];
    for (const [t, c] of parts) for (const ch of t) cells.push([ch, c]);
    // Break after the last space that fits, or mid-word when there is none.
    const rows = [];
    let i = 0;
    do {
        let end = Math.min(i + PANE, cells.length);
        if (end < cells.length) {
            let sp = end;
            while (sp > i && cells[sp - 1][0] !== ' ') sp--;
            if (sp > i + PANE / 3) end = sp;
        }
        rows.push(cells.slice(i, end));
        i = end;
    } while (i < cells.length);
    return rows;
}
function paint(cells) {
    let s = '', cur = null;
    for (const [ch, c] of cells) { if (c !== cur) { s += C.reset + C[c]; cur = c; } s += ch; }
    return s + C.reset + ' '.repeat(PANE - cells.length);
}
class Pane {
    constructor(title) { this.title = title; this.rows = []; this.foot = [['', '']]; }
    add(parts) { this.rows.push(...wrap(parts)); }
    gap() { this.rows.push([]); }
    drop(n) { this.rows.length -= n; }
    view() {
        const v = this.rows.slice(-BODY);
        while (v.length < BODY) v.push([]);
        return v;
    }
}

const left = new Pane([['nls', 'b'], [' · real processes on this machine', 'c']]);
const right = new Pane([['a typical LLM CLI helper', 'b'], [' · SIMULATED', 'w']]);
const caption = 'Right pane: simulated answers and 1-4 s waits, not a recording of any product. '
              + 'Ten requests picked because they are easy to get wrong.';

function draw() {
    const rule = '─'.repeat(PANE);
    const fit = parts => paint(wrap(parts)[0]);
    const lines = [fit(left.title) + ' │ ' + fit(right.title), C.c + rule + '─┼─' + rule + C.reset];
    const l = left.view(), r = right.view();
    for (let i = 0; i < BODY; i++) lines.push(paint(l[i]) + ' │ ' + paint(r[i]));
    lines.push(C.c + rule + '─┼─' + rule + C.reset);
    lines.push(fit(left.foot) + ' │ ' + fit(right.foot));
    lines.push(C.c + caption.slice(0, COLS) + C.reset);
    out.write('\x1b[H' + lines.map(s => s + '\x1b[K').join('\n'));
}

function nlsSide() {
    const runs = RACE.map(r => {
        const t0 = process.hrtime.bigint();
        const p = spawnSync(NLS, [r.tool, r.prompt], { encoding: 'utf8' });
        const ns = Number(process.hrtime.bigint() - t0);
        if (p.error) { console.error(`race: can't run ${NLS}: ${p.error.message}`); process.exit(1); }
        return { p, ns };
    });
    const total = runs.reduce((a, x) => a + x.ns, 0) / 1e6;
    let same = 0;
    RACE.forEach((r, i) => {
        const { p } = runs[i];
        if (i) left.gap();
        left.add([['$ ', 'p'], [`nls ${r.tool} `, 'x'], [quoted(r.prompt), 's']]);
        if (p.status !== 0) { left.add([[p.stderr.trim(), 'e']]); return; }
        const cmd = p.stdout.replace(/\n$/, '');
        if (cmd === r.nls) same++;
        left.add([[cmd, 'o']]);
        // What -x adds before a destructive command runs (-v prints it too).
        const v = spawnSync(NLS, ['-v', r.tool, r.prompt], { encoding: 'utf8' }).stderr.split('\n');
        const danger = v.find(l => l.startsWith('destructive: '));
        const dry = v.find(l => l.startsWith('dry run: '));
        if (danger) left.add([['with -x: ', 'c'], [danger, 'e']]);
        if (dry) left.add([['         ' + dry, 'c']]);
    });
    const ok = runs.filter(x => x.p.status === 0).length;
    // shellcheck is checked on the page's lines (make test-race): claim it
    // only when every line here is one of them.
    left.foot = [[`✓ ${ok}/${RACE.length} in ${total.toFixed(0)} ms (start-up included)`, 'o'],
                 [` · 0 network${same === RACE.length ? ' · shellcheck clean' : ''}`, 'x']];
}

async function llmSide() {
    let total = 0, wrong = 0, unsafe = 0, f = 0;
    for (const [i, r] of RACE.entries()) {
        if (i) right.gap();
        right.add([['$ ', 'p'], ['ai ', 'x'], [quoted(r.prompt), 's']]);
        right.add([['', 'c']]);
        const t0 = Date.now();
        for (;;) {
            const el = Math.min(r.ms, Date.now() - t0);
            right.drop(1);
            right.add([[`${SPIN[f++ % SPIN.length]} waiting for the model ${secs(el)}`, 'c']]);
            right.foot = [[`request ${i + 1} of ${RACE.length} · ${secs(total + el)}`, 'x']];
            draw();
            if (el >= r.ms) break;
            await sleep(80);
        }
        total += r.ms;
        right.drop(1);
        right.add([[r.llm, 'x']]);
        if (r.verdict === 'bad') { wrong++; right.add([['✗ ' + r.why, 'e']]); }
        else if (r.verdict === 'warn') { unsafe++; right.add([['! ' + r.why, 'w']]); }
        else right.add([['✓ ok', 'c']]);
    }
    right.foot = [[`${RACE.length} answers in ${secs(total)}`, 'x'],
                  [` · ${wrong} wrong · ${unsafe} delete without a preview`, 'e']];
    draw();
}

async function main() {
    const restore = () => out.write('\x1b[?25h\n');
    process.on('SIGINT', () => { restore(); process.exit(130); });
    out.write('\x1b[?25l\x1b[2J');
    left.foot = [['…', 'c']];
    draw();
    await sleep(400);   // a beat, so the eye sees both start
    nlsSide();
    draw();
    await llmSide();
    // Hold the result on screen (and in the recording) until a key is
    // pressed: exiting would print the prompt and scroll the title away.
    if (process.stdin.isTTY) {
        process.stdin.setRawMode(true);
        await new Promise(r => process.stdin.once('data', r));
        process.stdin.setRawMode(false);
        process.stdin.pause();
        out.write('\x1b[2J\x1b[H');
    }
    restore();
}
main();
