// PurrLang for VS Code (and Cursor, VSCodium, Windsurf): the grammar colors
// the text, and purrls, the language server that comes with purr, does the rest.
// PurrLang: Run plays the game in a window, and Run on the Web beside the code.

const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

const suffix = process.platform === 'win32' ? '.exe' : '';
let client = null;

function isFile(file) {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

function openFolders() {
    return (vscode.workspace.workspaceFolders ?? []).map(f => f.uri.fsPath);
}

// A program a setting names; `${workspaceFolder}` is the open folder. null
// when there's none there.
function settingProgram(setting) {
    const file = setting.replaceAll('${workspaceFolder}', openFolders()[0] ?? '');
    if (process.platform === 'win32' && !isFile(file) && isFile(file + '.exe')) return file + '.exe';
    return isFile(file) ? file : null;
}

// A program that comes with purr: on PATH, else where the installers put
// purr, as an editor started before purr was installed doesn't have it on its
// PATH yet.
function installedProgram(name) {
    const exe = name + suffix;
    for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
        if (dir && isFile(path.join(dir, exe))) return path.join(dir, exe);
    }
    const installed = process.platform === 'win32'
        ? path.join(process.env.LOCALAPPDATA ?? '', 'Purr', 'bin', exe)
        : path.join(os.homedir(), '.purr', 'bin', exe);
    return isFile(installed) ? installed : null;
}

// Says a program is missing, offering to install purr or to change `setting`.
async function missing(message, setting) {
    const choice = await vscode.window.showErrorMessage(message, 'Install purr', 'Settings');
    if (choice === 'Install purr') {
        vscode.env.openExternal(vscode.Uri.parse('https://github.com/BlenMiner/PurrEngine#install'));
    } else if (choice === 'Settings') {
        vscode.commands.executeCommand('workbench.action.openSettings', setting);
    }
}

// The setting; else the open folder's own build/tools/purrls, when it's
// PurrEngine itself; else the one that comes with purr.
function findServer() {
    const setting = vscode.workspace.getConfiguration('purrlang').get('server.path');
    if (setting) return settingProgram(setting);
    for (const folder of openFolders()) {
        const built = path.join(folder, 'build', 'tools', 'purrls' + suffix);
        if (isFile(built)) return built;
    }
    return installedProgram('purrls');
}

async function start() {
    const server = findServer();
    if (!server) {
        const setting = vscode.workspace.getConfiguration('purrlang').get('server.path');
        await missing(setting
            ? `PurrLang: purrlang.server.path is set to ${setting}, and there's no language server there.`
            : "PurrLang: couldn't find purrls, the language server that comes with purr. Install purr, or set purrlang.server.path.",
            'purrlang.server.path');
        return;
    }
    client = new LanguageClient('purrlang', 'PurrLang', { command: server }, {
        documentSelector: [
            { scheme: 'file', language: 'purrlang' },
            { scheme: 'untitled', language: 'purrlang' },
        ],
    });
    await client.start();
}

async function stop() {
    if (!client) return;
    const running = client;
    client = null;
    await running.stop();
}

async function restart() {
    await stop();
    await start();
}

// ---------------------------------------------------------------------------
// Run, and Run on the Web

// Whether `file` is `folder` or in it.
function inFolder(file, folder) {
    const relative = path.relative(folder, file);
    return relative !== '..' && !relative.startsWith('..' + path.sep) && !path.isAbsolute(relative);
}

// The games of the open folders that build them with CMake, like PurrEngine
// itself: purr_add_game lists them in build/tools/games.txt, a game and one of
// its files per line, or a folder ending in '/' (see cmake/PurrLang.cmake).
function manifestGames() {
    const games = [];
    for (const folder of openFolders()) {
        let text;
        try {
            text = fs.readFileSync(path.join(folder, 'build', 'tools', 'games.txt'), 'utf8');
        } catch {
            continue;
        }
        for (const line of text.split(/\r?\n/)) {
            const tab = line.indexOf('\t');
            if (tab > 0) games.push({ name: line.slice(0, tab), path: line.slice(tab + 1) });
        }
    }
    return games;
}

// The folder `purr run` plays for the .purr file `file`, found as purrls finds
// the game a file is in (game_of in compiler/lsp/server.c): the folder a
// manifest lists it in; else the innermost open folder it's in, unless a
// manifest lists games there, whose other files stand alone; else its own
// folder. { error } when purr run can't play it.
function gameFolder(file) {
    const games = manifestGames();
    const game = games.find(g => g.path.endsWith('/') ? inFolder(file, g.path) : path.relative(g.path, file) === '');
    if (game) {
        if (game.path.endsWith('/')) return { folder: path.resolve(game.path) };
        return { error: `${path.basename(file)} is in ${game.name}, which CMake builds from a list of files, and purr run plays a whole folder.` };
    }
    const root = openFolders().filter(f => inFolder(file, f)).sort((a, b) => b.length - a.length)[0];
    if (!root) return { folder: path.dirname(file) };
    if (games.some(g => inFolder(g.path, root))) {
        return { error: `${path.basename(file)} is in none of the games build/tools/games.txt lists.` };
    }
    return { folder: root };
}

// The game to run: the active .purr file's; else the last one run; else the
// one game there is, or the one picked from the open folders' games.
async function pickGame(last) {
    const document = vscode.window.activeTextEditor?.document;
    if (document?.languageId === 'purrlang' && document.uri.scheme === 'file') {
        const found = gameFolder(document.uri.fsPath);
        if (found.error) vscode.window.showErrorMessage(`PurrLang: ${found.error}`);
        return found.folder;
    }
    if (last) return last;
    const games = manifestGames();
    const folders = [
        ...games.filter(g => g.path.endsWith('/')).map(g => path.resolve(g.path)),
        ...openFolders().filter(f => !games.some(g => inFolder(g.path, f))),
    ];
    if (folders.length <= 1) {
        if (!folders.length) vscode.window.showErrorMessage('PurrLang: open a .purr file of the game to run.');
        return folders[0];
    }
    const picked = await vscode.window.showQuickPick(
        folders.map(f => ({ label: path.basename(f), description: f, folder: f })),
        { placeHolder: 'The game to run' });
    return picked?.folder;
}

// `purr run` in a terminal of its own, which shows purr's output and the
// game's, and sends purr what's typed there (r and Enter starts the game
// over). On the web, purr serves the page without opening a browser, and
// `onPage` gets its address once purr says where it is.
class Run {
    constructor(purr, folder, web, onPage) {
        this.output = new vscode.EventEmitter();
        this.closed = new vscode.EventEmitter();
        this.process = null;
        this.line = ''; // Typed, not sent yet
        this.ended = false;
        const args = web ? ['run', '--web', '--no-open'] : ['run'];
        this.terminal = vscode.window.createTerminal({
            name: `${path.basename(folder)} (purr ${args.slice(0, 2).join(' ')})`,
            iconPath: new vscode.ThemeIcon(web ? 'globe' : 'play'),
            pty: {
                onDidWrite: this.output.event,
                onDidClose: this.closed.event,
                open: () => this.start(purr, folder, args, onPage),
                close: () => this.stop(),
                handleInput: data => this.type(data),
            },
        });
        this.terminal.show(true);
    }

    print(text) {
        this.output.fire(text.replace(/\r?\n/g, '\r\n'));
    }

    start(purr, folder, args, onPage) {
        this.print(`${folder}> purr ${args.join(' ')}\n`);
        // Outside Windows, purr and the game it starts get a process group of
        // their own, which stop() ends.
        const child = spawn(purr, args, { cwd: folder, windowsHide: true, detached: process.platform !== 'win32' });
        this.process = child;
        let seen = ''; // purr's output until it says where the page is
        child.stdout.setEncoding('utf8');
        child.stdout.on('data', text => {
            if (onPage && seen !== null) {
                seen += text;
                const page = /purr: the game is at (http:\/\/\S+)/.exec(seen); // See purr_run_web in compiler/cli/build.c
                if (page) {
                    seen = null;
                    onPage(page[1]);
                }
            }
            this.print(text);
        });
        child.stderr.setEncoding('utf8');
        child.stderr.on('data', text => this.print(text));
        child.stdin.on('error', () => {}); // Typed after purr stopped
        child.on('error', error => this.print(`PurrLang: couldn't run ${purr}: ${error.message}\n`));
        child.on('close', () => {
            this.process = null;
            this.ended = true;
            this.print('\npurr stopped. Press any key to close this terminal.\n');
        });
    }

    type(data) {
        if (this.ended) {
            this.closed.fire();
            return;
        }
        if (data === '\x03') { // Ctrl+C
            this.print('^C\n');
            this.stop();
            return;
        }
        if (data.startsWith('\x1b')) return; // Arrows and other keys that aren't text
        for (const c of data) {
            if (c === '\r') {
                this.print('\n');
                this.process?.stdin.write(this.line + '\n');
                this.line = '';
            } else if (c === '\x7f') {
                if (this.line) this.output.fire('\b \b');
                this.line = this.line.slice(0, -1);
            } else if (c >= ' ') {
                this.line += c;
                this.output.fire(c);
            }
        }
    }

    // Stops purr, and the game it started, which would keep running without it.
    stop() {
        const pid = this.process?.pid;
        if (!pid) return;
        if (process.platform === 'win32') {
            spawn('taskkill', ['/pid', String(pid), '/t', '/f'], { windowsHide: true }).on('error', () => this.process?.kill());
        } else {
            try {
                process.kill(-pid, 'SIGTERM');
            } catch {
                this.process?.kill();
            }
        }
    }

    dispose() {
        this.stop();
        this.terminal.dispose();
    }
}

// Shows the page beside the code: in VS Code's integrated browser, in the tab
// of the last run's page if it's still open; else in the Simple Browser, which
// other editors built on VS Code have; else in the system's browser.
async function showPage(address, lastAddress) {
    const url = (await vscode.env.asExternalUri(vscode.Uri.parse(address))).toString(true);
    if ((await vscode.commands.getCommands(true)).includes('workbench.action.browser.open')) {
        await vscode.commands.executeCommand('workbench.action.browser.open',
            { url, openToSide: true, reuseUrlFilter: lastAddress ?? undefined });
        return url;
    }
    try {
        await vscode.commands.executeCommand('simpleBrowser.api.open', vscode.Uri.parse(url),
            { viewColumn: vscode.ViewColumn.Beside });
    } catch {
        await vscode.env.openExternal(vscode.Uri.parse(url));
    }
    return url;
}

const runs = { desktop: null, web: null }; // Running, or that ran last
let lastGame = null;
let lastPage = null; // The web run's address in the editor

// Runs the game, starting it over if it's running: in a window of its own, or
// on the web, in the editor.
async function run(web) {
    const setting = vscode.workspace.getConfiguration('purrlang').get('purr.path');
    const purr = setting ? settingProgram(setting) : installedProgram('purr');
    if (!purr) {
        await missing(setting
            ? `PurrLang: purrlang.purr.path is set to ${setting}, and there's no purr there.`
            : "PurrLang: couldn't find purr. Install it, or set purrlang.purr.path.",
            'purrlang.purr.path');
        return;
    }
    const game = await pickGame(lastGame);
    if (!game) return;
    lastGame = game;
    const kind = web ? 'web' : 'desktop';
    runs[kind]?.dispose();
    runs[kind] = new Run(purr, game, web, web ? async address => {
        lastPage = await showPage(address, lastPage);
    } : null);
}

function activate(context) {
    context.subscriptions.push(
        vscode.commands.registerCommand('purrlang.restartServer', restart),
        vscode.commands.registerCommand('purrlang.run', () => run(false)),
        vscode.commands.registerCommand('purrlang.runWeb', () => run(true)),
        vscode.workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('purrlang.server.path')) restart();
        }),
        { dispose: () => Object.values(runs).forEach(r => r?.dispose()) },
    );
    return start();
}

function deactivate() {
    return stop();
}

module.exports = { activate, deactivate };
