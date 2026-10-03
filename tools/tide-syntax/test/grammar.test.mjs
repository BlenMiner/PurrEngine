// Checks the TextMate grammar (../syntaxes/tide.tmLanguage.json) through
// Shiki, which the docs site highlights Tide with, as editors do: the scopes
// that matter in probe.tide's cases, and in every program of
// compiler/tests/e2e, that no text runs past its line and that declarations
// are scoped as declarations.
//
// It uses the docs site's Shiki: `npm ci`, then `npm run test:grammar` in docs/.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..', '..');
const require = createRequire(path.join(repo, 'docs', 'package.json'));
const { createHighlighter } = await import(pathToFileURL(require.resolve('shiki')).href);

const grammar = JSON.parse(fs.readFileSync(path.join(here, '..', 'syntaxes', 'tide.tmLanguage.json'), 'utf8'));
const theme = 'github-dark';
const highlighter = await createHighlighter({ themes: [theme], langs: [{ ...grammar, name: 'tide' }] });

// Each line of `code`: its text, and its pieces with their scopes, outermost first.
function tokenize(code) {
    const texts = code.split(/\r?\n/);
    const { tokens } = highlighter.codeToTokens(code, { lang: 'tide', theme, includeExplanation: 'scopeName' });
    return tokens.map((line, i) => {
        const pieces = [];
        let at = 0;
        for (const token of line) {
            for (const part of token.explanation) {
                pieces.push({ start: at, end: at + part.content.length, text: part.content, scopes: part.scopes.map(s => s.scopeName) });
                at += part.content.length;
            }
        }
        assert.equal(pieces.map(p => p.text).join(''), texts[i].replace(/\r$/, ''), `line ${i + 1}'s pieces are its text`);
        return { number: i + 1, text: texts[i], pieces };
    });
}

// Whether `scopes` has `scope`, or a scope inside it (string has string.quoted.double.tide).
function has(scopes, scope) {
    return scopes.some(s => s === scope || s.startsWith(scope + '.'));
}

// The pieces of `line` that make up the `nth` `text` in it: a whole word, when
// it's a word (the 'in' that isn't in 'int').
function piecesOf(line, text, nth) {
    const word = /^\w+$/.test(text);
    const isWord = i => !word || (!/\w/.test(line.text[i - 1] ?? '') && !/\w/.test(line.text[i + text.length] ?? ''));
    let index = -1;
    for (let i = 0; i < nth; i++) {
        do index = line.text.indexOf(text, index + 1);
        while (index !== -1 && !isWord(index));
        assert.notEqual(index, -1, `line ${line.number} has "${text}" ${nth} times: ${line.text}`);
    }
    const end = index + text.length;
    return line.pieces.filter(p => p.start < end && p.end > index);
}

// Checks [text, scope, nth?]: every piece of the text has the scope, or with
// '!' before the scope, none has it.
function check(line, [text, scope, nth = 1]) {
    const not = scope.startsWith('!');
    const wanted = not ? scope.slice(1) : scope;
    for (const piece of piecesOf(line, text, nth)) {
        const found = has(piece.scopes, wanted);
        assert.ok(not ? !found : found,
            `line ${line.number}: "${text}"${nth > 1 ? ` (${nth})` : ''} ${not ? 'has' : "doesn't have"} ${wanted}: "${piece.text}" is ${piece.scopes.join(' ')}\n  ${line.text}`);
    }
}

// Text is one line (lexer.c), so no line starts inside a string.
function checkLinesEndText(file, lines) {
    for (const line of lines) {
        const first = line.pieces[0];
        if (!first) continue;
        assert.ok(!has(first.scopes, 'string'), `${file}:${line.number} starts inside text: ${line.text}`);
    }
}

const probe = tokenize(fs.readFileSync(path.join(here, 'probe.tide'), 'utf8'));

// The probe's line that starts with `start`, after its indentation.
function probeLine(start) {
    const found = probe.filter(l => l.text.trimStart().startsWith(start));
    assert.equal(found.length, 1, `probe.tide has one line that starts with "${start}"`);
    return found[0];
}

// Lines of probe.tide, and what their pieces' scopes have to be.
const cases = {
    'unclosed text ends with its line': [
        ['return $"score {score} and more;', [['$"score', 'string.quoted.double.interpolated'], ['score', 'meta.interpolation', 2]]],
        ['component AfterUnclosed {', [['component', 'storage.type'], ['AfterUnclosed', 'entity.name.type'], ['int', '!string']]],
        ['return $"score {score;', [['score;', 'meta.interpolation']]],
        ['component AfterUnclosedValue {', [['component', 'storage.type'], ['AfterUnclosedValue', 'entity.name.type']]],
        ['return "no end;', [['no end;', 'string.quoted.double']]],
        ['component AfterUnclosedPlain {', [['component', 'storage.type'], ['AfterUnclosedPlain', 'entity.name.type']]],
    ],
    'functions that return generic types and T?': [
        ['List<int> Doubled(', [
            ['List', 'support.class'], ['<', 'punctuation.definition.typeparameters.begin'], ['int', 'storage.type.primitive'],
            ['>', 'punctuation.definition.typeparameters.end'], ['Doubled', 'entity.name.function'], ['Doubled', '!entity.name.function.call'],
            ['List', 'support.class', 2], ['<', 'punctuation.definition.typeparameters.begin', 2], ['<', '!keyword.operator', 2],
        ]],
        ['mut List<int> doubled', [['mut', 'storage.modifier'], ['<', '!keyword.operator'], ['>', '!keyword.operator']]],
        ['List<Combat.Stats>? MaybeStats(', [
            ['Combat', 'entity.name.type'], ['Stats', 'entity.name.type'], ['?', 'keyword.operator.optional'],
            ['MaybeStats', 'entity.name.function'], ['MaybeStats', '!entity.name.function.call'],
        ]],
        ['int? Find(', [['int', 'storage.type.primitive'], ['?', 'keyword.operator.optional'], ['Find', 'entity.name.function'], ['Find', '!entity.name.function.call']]],
        ['extern List<int> Primes(', [['extern', 'storage.modifier'], ['List', 'support.class'], ['Primes', 'entity.name.function'], ['Primes', '!entity.name.function.call']]],
        ['extern float Noise(', [['extern', 'storage.modifier'], ['float', 'storage.type.primitive'], ['Noise', 'entity.name.function']]],
        ['extern void Fill(', [['in', 'storage.modifier'], ['in', '!keyword.control'], ['Stats', 'entity.name.type'], ['mut', 'storage.modifier']]],
        ['const List<int> NONE', [['const', 'storage.modifier'], ['List', 'support.class'], ['NONE', 'variable.other.constant']]],
        ['const int MAX_HEALTH', [['const', 'storage.modifier'], ['MAX_HEALTH', 'variable.other.constant']]],
        ['Grid2<int> cells;', [['Grid2', 'support.class'], ['<', 'punctuation.definition.typeparameters.begin'], ['>', 'punctuation.definition.typeparameters.end']]],
        ['List<float> Weights()', [['Weights', 'entity.name.function'], ['Weights', '!entity.name.function.call']]],
        ['mut void Heal(', [['mut', 'storage.modifier'], ['Heal', 'entity.name.function']]],
        ['Stats operator +(', [['operator', 'keyword.other.operator'], ['+', 'keyword.operator']]],
    ],
    'declaration words are keywords only where a declaration starts': [
        ['input PlayerInput', [['input', 'storage.type'], ['PlayerInput', 'entity.name.type']]],
        ['system Steer(', [['system', 'storage.type'], ['Steer', 'entity.name.function'], ['input', '!storage'], ['events', '!storage']]],
        ['if (input is PlayerInput other)', [['input', '!storage'], ['input', '!keyword'], ['is', 'keyword.operator.expression.is'], ['is', '!entity.name.type']]],
        ['foreach (var event in events)', [['event', '!storage'], ['event', '!keyword'], ['in', 'keyword.control'], ['in', '!entity.name.type']]],
        ['var scene = 1;', [['scene', '!storage']]],
    ],
    'local and async, in either order': [
        ['local event Shown', [['local', 'storage.modifier'], ['event', 'storage.type'], ['Shown', 'entity.name.type']]],
        ['local component Toast', [['local', 'storage.modifier'], ['component', 'storage.type'], ['Toast', 'entity.name.type']]],
        ['async local event(Shown) Fade(', [
            ['async', 'storage.modifier'], ['local', 'storage.modifier'], ['event', 'storage.type'], ['Shown', 'entity.name.type'],
            ['Fade', 'entity.name.function'], ['Fade', '!entity.name.function.call'],
        ]],
        ['local async event(Shown) Glow(', [['local', 'storage.modifier'], ['async', 'storage.modifier'], ['event', 'storage.type'], ['Glow', 'entity.name.function']]],
        ['async void Countdown()', [['async', 'storage.modifier'], ['void', 'storage.type.primitive'], ['Countdown', 'entity.name.function'], ['Wait', 'support.class'], ['await', 'keyword.control']]],
    ],
    'fails after the parameters or on a line of its own': [
        ['int Parse(string text) fails', [['fails', 'storage.modifier.fails'], ['ParseError', 'entity.name.type']]],
        ['fails ParseError', [['fails', 'storage.modifier.fails'], ['ParseError', 'entity.name.type']]],
        ['int ParseLong(', [['ParseLong', 'entity.name.function']]],
    ],
    'attributes side by side': [
        ['[After(Steer)] [Before(Fade)]', [
            ['After', 'entity.name.function.attribute'], ['Steer', 'entity.name.function'],
            ['Before', 'entity.name.function.attribute'], ['Fade', 'entity.name.function'], ['[', 'punctuation.definition.attribute.begin', 2],
        ]],
        ['[After(Steer), Before(Fade)] system SameLine()', [
            ['After', 'entity.name.function.attribute'], ['Before', 'entity.name.function.attribute'],
            ['system', 'storage.type'], ['SameLine', 'entity.name.function'],
        ]],
        ['[Clamp(float2(0, -5), float2(10, 5))] float2 aim;', [
            ['Clamp', 'entity.name.function.attribute'], ['float2', 'storage.type.primitive'], ['float2', 'storage.type.primitive', 2],
            ['-', 'keyword.operator.arithmetic'], ['float2', 'storage.type.primitive', 3], ['aim', '!entity'],
        ]],
        ['[Min(1)] [Max(3)] int gear', [['Min', 'entity.name.function.attribute'], ['Max', 'entity.name.function.attribute'], ['int', 'storage.type.primitive']]],
    ],
    'built-in types and groups': [
        ['enum Voxel : byte', [['enum', 'storage.type'], ['Voxel', 'entity.name.type'], [':', 'punctuation.separator.colon'], [':', '!keyword.operator'], ['byte', 'storage.type.primitive']]],
        ['Keyboard keyboard', [['Keyboard', 'support.class']]],
        ['Mouse mouse', [['Mouse', 'support.class']]],
        ['Gamepad pad', [['Gamepad', 'support.class']]],
        ['Button jump', [['Button', 'support.class']]],
        ['Dpad dpad', [['Dpad', 'support.class']]],
        ['float4x4 world', [['float4x4', 'storage.type.primitive']]],
        ['float2x3 wrong', [['float2x3', '!storage.type.primitive']]],
        ['int2x2 alsoWrong', [['int2x2', '!storage.type.primitive']]],
        ['Draw.Rect(', [['Draw', 'support.class'], ['Rect', '!storage.type'], ['Rect', 'entity.name.function.call'], ['Color', 'storage.type.primitive']]],
        ['Vertex corner', [['Vertex', 'support.class']]],
        ['Vertex3 corner3', [['Vertex3', 'support.class']]],
        ['Draw.Mesh(', [['Mesh', 'entity.name.function.call'], ['Filter', 'support.class']]],
        ['extern void DrawUI(DrawList list);', [['DrawList', 'storage.type.primitive']]],
        ['if (GUI.Button("Copy")) Clipboard.Copy(', [['GUI', 'support.class'], ['Button', '!support.class'], ['Button', 'entity.name.function.call'], ['Clipboard', 'support.class']]],
    ],
    "operators that aren't what they look like": [
        ['int value = Parse(text)!;', [['!', 'keyword.operator.or-default'], ['!', '!keyword.operator.logical']]],
        ['bool not = !true;', [['!', 'keyword.operator.logical']]],
        ['int either = true ? 1 : 2;', [['?', 'keyword.operator.ternary'], [':', 'keyword.operator.ternary']]],
        ['case 1:', [['case', 'keyword.control'], [':', 'punctuation.separator.colon'], [':', '!keyword.operator']]],
        ['default:', [['default', 'keyword.control'], [':', 'punctuation.separator.colon']]],
    ],
    'escapes': [
        ['string good =', [['\\"', 'constant.character.escape'], ['\\\\', 'constant.character.escape'], ['\\n', 'constant.character.escape'], ['"', '!invalid', 3]]],
        ['string bad =', [['\\t', 'invalid.illegal']]],
        ['string braces =', [['{{', 'constant.character.escape'], ['}}', 'constant.character.escape'], [':F2', 'constant.other.format'], ['value', 'meta.interpolation']]],
        ['string lone =', [['}', 'invalid.illegal']]],
        ['string badValue =', [['\\t', 'invalid.illegal'], ['value', 'meta.interpolation']]],
    ],
    'directives': [
        ['#if TIDE_0_3_OR_NEWER', [
            ['#if', 'keyword.preprocessor'], ['TIDE_0_3_OR_NEWER', 'entity.name.variable.preprocessor.symbol'],
            ['&&', 'keyword.operator.logical'], ['!', 'keyword.operator.logical'], ['false', 'constant.language'],
            ['A newer tide', 'comment.line'],
        ]],
        ['#elif (TIDE_0_2_OR_NEWER)', [['#elif', 'keyword.preprocessor'], ['TIDE_0_2_OR_NEWER', 'entity.name.variable.preprocessor.symbol']]],
        ['#else', [['#else', 'keyword.preprocessor']]],
        ['#endif', [['#endif', 'keyword.preprocessor']]],
    ],
    "loops' headers": [
        ['parallel (var at in field.cells by 2 offset o)', [['parallel', 'keyword.control'], ['in', 'keyword.control'], ['by', 'keyword.control'], ['offset', 'keyword.control']]],
        ['foreach (var tick in Both(Ticks(), Ticks()))', [['in', 'keyword.control'], ['Both', 'entity.name.function.call'], ['in', '!keyword', 2]]],
    ],
};

for (const [name, lines] of Object.entries(cases)) {
    test(name, () => {
        for (const [start, checks] of lines) {
            const line = probeLine(start);
            for (const c of checks) check(line, c);
        }
    });
}

test('no text in probe.tide runs past its line', () => checkLinesEndText('probe.tide', probe));

// The editors indent after a line that opens a brace, but not one in text or a comment.
test('indenting ignores braces in text and comments', () => {
    const configuration = JSON.parse(fs.readFileSync(path.join(here, '..', 'language-configuration.json'), 'utf8'));
    const increase = new RegExp(configuration.indentationRules.increaseIndentPattern);
    const lines = [
        ['{', true],
        ['    if (x) {', true],
        ['    if (x) { // go', true],
        ['    if (x) { // the } closes it', true],
        ['    Foo("{") {', true],
        ['    Spawn(Body {', true],
        ['system Foo()', false],
        ['component A { int x; }', false],
        ['    Foo("{");', false],
        ['    string s = $"{x}";', false],
        ['    string s = $"{{";', false],
        ['    // see {', false],
        ['    x = 1; // {', false],
        ['    /* { */', false],
    ];
    for (const [line, indents] of lines) assert.equal(increase.test(line), indents, `indents after: ${line}`);
});

// Every program in compiler/tests/e2e, with its declarations found without the
// grammar: data declarations, systems, views, handlers, functions, constants
// and externs at the start of a line, and methods in a type's body, which the
// formatter indents by four spaces.
const e2e = path.join(repo, 'compiler', 'tests', 'e2e');
const programs = fs.readdirSync(e2e, { recursive: true }).filter(f => f.endsWith('.tide')).sort();

const modifiers = String.raw`(?:(?:local|async)\s+)*`;
const type = String.raw`[\w.]+(?:\s*<[\w.<>\s]*>)?\??`;
const keywords = new Set(['return', 'if', 'else', 'var', 'await', 'fail', 'try', 'case', 'switch', 'while', 'for', 'foreach', 'parallel']);
const declarations = [
    [new RegExp(String.raw`^${modifiers}(?:component|singleton|struct|input|event|enum|scene)\s+(\w+)`), 'entity.name.type'],
    [new RegExp(String.raw`^${modifiers}(?:system|view)\s+(\w+)`), 'entity.name.function'],
    [new RegExp(String.raw`^${modifiers}event\s*\([^)]*\)\s*(\w+)`), 'entity.name.function'],
    [new RegExp(String.raw`^const\s+${type}\s+(\w+)\s*=`), 'variable.other.constant'],
    [new RegExp(String.raw`^extern\s+${type}\s+(\w+)\s*\(`), 'entity.name.function'],
    [new RegExp(String.raw`^${modifiers}(?:mut\s+)?(${type})\s+(\w+)\s*\(`), 'entity.name.function'],
];
const method = new RegExp(String.raw`^    (?:mut\s+)?(${type})\s+(\w+)\s*\(`);

for (const program of programs) {
    test(`e2e/${program.replaceAll('\\', '/')}`, () => {
        const lines = tokenize(fs.readFileSync(path.join(e2e, program), 'utf8'));
        checkLinesEndText(program, lines);
        let inType = false; // In a component's, singleton's, struct's, input's or scene's body
        let found = 0;
        for (const line of lines) {
            const text = line.text.replace(/\r$/, '');
            for (const piece of line.pieces) {
                assert.ok(!has(piece.scopes, 'invalid'), `${program}:${line.number}: "${piece.text}" is ${piece.scopes.join(' ')}`);
            }
            if (/^(?:local\s+)?(?:component|singleton|struct|input|scene)\b/.test(text) && !text.includes('}')) inType = true;
            else if (text === '}') inType = false;
            let name = null;
            let scope = null;
            for (const [pattern, wanted] of declarations) {
                const match = pattern.exec(text);
                if (!match) continue;
                const last = match[match.length - 1];
                if (pattern === declarations[5][0] && keywords.has(match[1])) break;
                name = last;
                scope = wanted;
                break;
            }
            const inside = inType && method.exec(text);
            if (inside && !keywords.has(inside[1])) {
                name = inside[2];
                scope = 'entity.name.function';
            }
            if (!name) continue;
            found++;
            const index = text.search(new RegExp(String.raw`\b${name}\b`));
            const pieces = line.pieces.filter(p => p.start < index + name.length && p.end > index);
            for (const piece of pieces) {
                assert.ok(has(piece.scopes, scope) && !has(piece.scopes, 'entity.name.function.call'),
                    `${program}:${line.number}: ${name} is ${piece.scopes.join(' ')}, not ${scope}\n  ${text}`);
            }
        }
        assert.ok(found > 0 || program.includes('empty'), `${program} has declarations`);
    });
}
