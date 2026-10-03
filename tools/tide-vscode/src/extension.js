// Tide for VS Code (and Cursor, VSCodium, Windsurf): the grammar colors
// the text, and tidels, the language server that comes with tide, does the rest.
// Tide: Run plays the game in a window, Run on the Web beside the code, and
// Run on Android on the phone or emulator that's connected.
//
// In Restricted Mode (an untrusted workspace), nothing from the workspace
// runs: the server is the one that comes with tide, and games don't run.

const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

const suffix = process.platform === 'win32' ? '.exe' : '';
let client = null;
let output = null; // The Tide output channel, which every client shares
let trace = null; // Where tide.trace.server shows what VS Code and tidels say

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

// Whether `file` is `folder` or in it.
function inFolder(file, folder) {
    const relative = path.relative(folder, file);
    return relative !== '..' && !relative.startsWith('..' + path.sep) && !path.isAbsolute(relative);
}

// The setting tide.<key> for `resource` (a game's folder, or none for the
// language server, which serves them all), and the workspace folder it comes
// from: the one whose settings set it (the resource's, or for the server, the
// first that does); else, for user and workspace settings, the resource's, or
// the first. Only the user's own settings count until the workspace is
// trusted.
function setting(key, resource) {
    const folders = vscode.workspace.workspaceFolders ?? [];
    const own = resource ? vscode.workspace.getWorkspaceFolder(resource) : undefined;
    const trusted = vscode.workspace.isTrusted;
    if (trusted) {
        for (const folder of resource ? (own ? [own] : []) : folders) {
            const value = vscode.workspace.getConfiguration('tide', folder.uri).inspect(key)?.workspaceFolderValue;
            if (value) return { value, folder: folder.uri.fsPath };
        }
    }
    const inspected = vscode.workspace.getConfiguration('tide', resource).inspect(key);
    const value = (trusted ? inspected?.workspaceValue : undefined) || inspected?.globalValue || '';
    return { value, folder: (own ?? folders[0])?.uri.fsPath };
}

// The path a setting holds: `~` is the home folder, `${workspaceFolder}` and
// relative paths are in `folder`, and `${workspaceFolder:name}` is the open
// folder of that name. null when it names a folder that isn't open.
function settingPath(value, folder) {
    const home = os.homedir();
    let missing = false;
    let file = value.trim()
        .replace(/^~(?=$|[\\/])/, home)
        .replace(/\$\{userHome\}/g, home)
        .replace(/\$\{workspaceFolder(?::([^}]*))?\}/g, (_, name) => {
            const found = name ? vscode.workspace.workspaceFolders?.find(f => f.name === name)?.uri.fsPath : folder;
            if (!found) missing = true;
            return found ?? '';
        });
    if (missing) return null;
    if (!path.isAbsolute(file)) {
        if (!folder) return null;
        file = path.resolve(folder, file);
    }
    return path.normalize(file);
}

// The program the setting tide.<key> names, `what` it is: null when it's
// empty; else { file }, or { error } saying why there's none to run, and
// whether that's only for want of trust (`untrusted`).
function settingProgram(key, what, resource) {
    const { value, folder } = setting(key, resource);
    if (!value) return null;
    const named = `tide.${key} is set to ${value}`;
    const file = settingPath(value, folder);
    if (!file) return { error: `${named}, which is in a folder that isn't open.` };
    if (!vscode.workspace.isTrusted && openFolders().some(f => inFolder(file, f))) {
        return { error: `${named}, which is in this workspace: it runs once you trust the workspace.`, untrusted: true };
    }
    if (process.platform === 'win32' && !isFile(file) && isFile(file + '.exe')) return { file: file + '.exe' };
    return isFile(file) ? { file } : { error: `${named}, and there's no ${what} there.` };
}

// A program that comes with tide: on PATH, else where the installers put
// tide, as an editor started before tide was installed doesn't have it on its
// PATH yet. Only PATH's absolute folders: a relative one would be wherever
// the editor runs.
function installedProgram(name) {
    const exe = name + suffix;
    for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
        if (dir && path.isAbsolute(dir) && isFile(path.join(dir, exe))) return path.join(dir, exe);
    }
    const installed = process.platform === 'win32'
        ? path.join(process.env.LOCALAPPDATA ?? '', 'Tide', 'bin', exe)
        : path.join(os.homedir(), '.tide', 'bin', exe);
    return isFile(installed) ? installed : null;
}

// Says a program is missing, offering to install tide or to change `setting`.
async function missing(message, setting) {
    const choice = await vscode.window.showErrorMessage(message, 'Install tide', 'Settings');
    if (choice === 'Install tide') {
        vscode.env.openExternal(vscode.Uri.parse('https://github.com/BlenMiner/tide-engine#install'));
    } else if (choice === 'Settings') {
        vscode.commands.executeCommand('workbench.action.openSettings', setting);
    }
}

// The language server: the setting's; else the open folder's own
// build/tools/tidels, when it's Tide itself; else the one that comes with
// tide. In an untrusted workspace, never one in the workspace. { file }, or
// { error }.
function findServer() {
    const configured = settingProgram('server.path', 'language server');
    if (configured && !configured.untrusted) return configured;
    if (vscode.workspace.isTrusted) {
        for (const folder of openFolders()) {
            const built = path.join(folder, 'build', 'tools', 'tidels' + suffix);
            if (isFile(built)) return { file: built };
        }
    }
    const installed = installedProgram('tidels');
    if (installed) return { file: installed };
    return configured ?? { error: "couldn't find tidels, the language server that comes with tide. Install tide, or set tide.server.path." };
}

// The trace of what VS Code and tidels say to each other, as tide.trace.server
// asks. The language client only traces into a log channel whose level is
// Trace, so this one is at Trace while the setting isn't off.
function traceChannel() {
    const channel = vscode.window.createOutputChannel('Tide Trace');
    const changed = new vscode.EventEmitter();
    const level = () => vscode.workspace.getConfiguration('tide').get('trace.server') === 'off' ? vscode.LogLevel.Info : vscode.LogLevel.Trace;
    const listener = vscode.workspace.onDidChangeConfiguration(e => {
        if (e.affectsConfiguration('tide.trace.server')) changed.fire(level());
    });
    const write = message => channel.appendLine(`[${new Date().toLocaleTimeString()}] ${message instanceof Error ? message.message : message}`);
    return {
        name: channel.name,
        get logLevel() {
            return level();
        },
        onDidChangeLogLevel: changed.event,
        trace: write,
        debug: write,
        info: write,
        warn: write,
        error: write,
        append: value => channel.append(value),
        appendLine: value => channel.appendLine(value),
        replace: value => channel.replace(value),
        clear: () => channel.clear(),
        show: (...args) => channel.show(...args),
        hide: () => channel.hide(),
        dispose: () => {
            listener.dispose();
            changed.dispose();
            channel.dispose();
        },
    };
}

// Starts the language server. It never throws: what went wrong shows in a
// notification, so that Restart Language Server always has a server to stop.
async function start() {
    const server = findServer();
    if (server.error) {
        missing(`Tide: ${server.error}`, 'tide.server.path'); // Not awaited: the editor goes on meanwhile
        return;
    }
    const next = new LanguageClient('tide', 'Tide', { command: server.file }, {
        documentSelector: [
            { scheme: 'file', language: 'tide' },
            { scheme: 'untitled', language: 'tide' },
        ],
        outputChannel: output,
        traceOutputChannel: trace,
    });
    client = next;
    try {
        await next.start();
    } catch {
        // The client said why, in a notification and in the Tide output.
    }
}

// Stops the language server, whatever state it's in: a server that failed to
// start can't be stopped, only let go.
async function stop() {
    const running = client;
    client = null;
    if (!running) return;
    try {
        await running.dispose();
    } catch {
        // It never started, or it stopped already.
    }
}

// Starts and stops one after the other, as settings changes and the restart
// command come in.
let queue = Promise.resolve();
function serially(step) {
    queue = queue.then(step).catch(() => {});
    return queue;
}

function restart() {
    return serially(async () => {
        await stop();
        await start();
    });
}

// ---------------------------------------------------------------------------
// Run, Run on the Web and Run on Android

// The games of the open folders that build them with CMake, like Tide
// itself: tide_add_game lists them in build/tools/games.txt, a game and one of
// its files per line, or a folder ending in '/' (see cmake/Tide.cmake).
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

// A folder's tide.packages (see docs/guide/packages.md), as lines without
// their comments, or null when it has none.
function packagesLines(folder) {
    try {
        return fs.readFileSync(path.join(folder, 'tide.packages'), 'utf8').split(/\r?\n/)
            .map(line => line.replace(/(^|\s)#.*$/, '').trim()).filter(line => line);
    } catch {
        return null;
    }
}

// The package a folder is, by its `package` line, or null for a game.
function packageName(folder) {
    const line = (packagesLines(folder) ?? []).find(l => /^package\s/.test(l));
    return line ? line.split(/\s+/)[1] : null;
}

// Where tide keeps packages from git, as compiler/lsp/packages.c says.
function packagesCache() {
    if (process.env.TIDE_PACKAGES) return process.env.TIDE_PACKAGES;
    if (process.platform === 'win32') return path.join(process.env.LOCALAPPDATA ?? '.', 'Tide', 'packages');
    return path.join(os.homedir(), '.tide', 'packages');
}

// Whether the game in `game` lists the package in `folder`: a folder, or a
// repository's commit (with //sub for a folder in it) in tide's cache.
function listsPackage(game, folder) {
    for (const line of packagesLines(game) ?? []) {
        const [source, commit] = line.split(/\s+/);
        if (source === 'package' || source === 'tide') continue;
        let dir;
        if (/^(\.|\/|\\|[A-Za-z]:)/.test(source)) {
            dir = path.resolve(game, source);
        } else if (commit) {
            const slash = source.indexOf('/');
            const at = source.lastIndexOf('@');
            const plain = at > slash ? source.slice(0, at) : source;
            const [repo, sub = ''] = plain.split('//');
            dir = path.join(packagesCache(), ...repo.split('/'), commit, ...sub.split('/').filter(p => p));
        } else {
            continue;
        }
        if (path.relative(dir, folder) === '') return true;
    }
    return false;
}

// The folder `tide run` plays for the .tide file `file`, found as tidels finds
// the game a file is in (game_folder in compiler/lsp/server.c): the folder a
// manifest lists it in; else the innermost folder with a tide.packages it's
// in, up to the open folder it's in, and for a package, the open folder whose
// game lists it; else the innermost open folder it's in, unless a manifest
// lists games there, whose other files stand alone; else its own folder.
// { error } when tide run can't play it.
function gameFolder(file) {
    const games = manifestGames();
    const game = games.find(g => g.path.endsWith('/') ? inFolder(file, g.path) : path.relative(g.path, file) === '');
    if (game) {
        if (game.path.endsWith('/')) return { folder: path.resolve(game.path) };
        return { error: `${path.basename(file)} is in ${game.name}, which CMake builds from a list of files, and tide run plays a whole folder.` };
    }
    const root = openFolders().filter(f => inFolder(file, f)).sort((a, b) => b.length - a.length)[0];
    for (let dir = path.dirname(file); ; dir = path.dirname(dir)) {
        if (packagesLines(dir)) {
            const name = packageName(dir);
            if (!name) return { folder: dir };
            const user = openFolders().find(f => !packageName(f) && listsPackage(f, dir));
            if (user) return { folder: user };
            return { error: `${path.basename(file)} is in package ${name}, and tide run plays a game: run one whose tide.packages lists it.` };
        }
        if ((root && path.relative(root, dir) === '') || path.dirname(dir) === dir) break;
    }
    if (!root) return { folder: path.dirname(file) };
    if (games.some(g => inFolder(g.path, root))) {
        return { error: `${path.basename(file)} is in none of the games build/tools/games.txt lists.` };
    }
    return { folder: root };
}

// The game to run: the active .tide file's; else the last one run; else the
// one game there is, or the one picked from the open folders' games.
async function pickGame(last) {
    const document = vscode.window.activeTextEditor?.document;
    if (document?.languageId === 'tide' && document.uri.scheme === 'file') {
        const found = gameFolder(document.uri.fsPath);
        if (found.error) vscode.window.showErrorMessage(`Tide: ${found.error}`);
        return found.folder;
    }
    if (last) return last;
    const games = manifestGames();
    const folders = [
        ...games.filter(g => g.path.endsWith('/')).map(g => path.resolve(g.path)),
        ...openFolders().filter(f => !games.some(g => inFolder(g.path, f))),
    ];
    if (folders.length <= 1) {
        if (!folders.length) vscode.window.showErrorMessage('Tide: open a .tide file of the game to run.');
        return folders[0];
    }
    const picked = await vscode.window.showQuickPick(
        folders.map(f => ({ label: path.basename(f), description: f, folder: f })),
        { placeHolder: 'The game to run' });
    return picked?.folder;
}

// How each kind of run runs tide, and its terminal's icon
const KINDS = {
    desktop: { args: ['run'], icon: 'play' },
    web: { args: ['run', '--web', '--no-open'], icon: 'globe' },
    android: { args: ['run', '--android'], icon: 'device-mobile' },
};

// `tide run` in a terminal of its own, which shows tide's output and the
// game's, and sends tide what's typed there (r and Enter starts the game
// over, and y answers whether to download Android's tools). On the web, tide
// serves the page without opening a browser, and `onPage` gets its address
// once tide says where it is.
class Run {
    constructor(tide, folder, kind, onPage) {
        this.output = new vscode.EventEmitter();
        this.closed = new vscode.EventEmitter();
        this.process = null;
        this.line = ''; // Typed, not sent yet
        this.ended = false;
        const args = KINDS[kind].args;
        this.terminal = vscode.window.createTerminal({
            name: `${path.basename(folder)} (tide ${args.slice(0, 2).join(' ')})`,
            iconPath: new vscode.ThemeIcon(KINDS[kind].icon),
            pty: {
                onDidWrite: this.output.event,
                onDidClose: this.closed.event,
                open: () => this.start(tide, folder, args, onPage),
                close: () => this.stop(),
                handleInput: data => this.type(data),
            },
        });
        this.terminal.show(true);
    }

    print(text) {
        this.output.fire(text.replace(/\r?\n/g, '\r\n'));
    }

    start(tide, folder, args, onPage) {
        this.print(`${folder}> tide ${args.join(' ')}\n`);
        // Outside Windows, tide and the game it starts get a process group of
        // their own, which stop() ends. TIDE_INTERACTIVE: someone can answer
        // tide's questions here, though its input isn't a terminal.
        const child = spawn(tide, args, {
            cwd: folder,
            windowsHide: true,
            detached: process.platform !== 'win32',
            env: { ...process.env, TIDE_INTERACTIVE: '1' },
        });
        this.process = child;
        let seen = ''; // tide's output until it says where the page is
        child.stdout.setEncoding('utf8');
        child.stdout.on('data', text => {
            if (onPage && seen !== null) {
                seen += text;
                const page = /tide: the game is at (http:\/\/\S+)/.exec(seen); // See tide_run_web in compiler/cli/build.c
                if (page) {
                    seen = null;
                    onPage(page[1]);
                }
            }
            this.print(text);
        });
        child.stderr.setEncoding('utf8');
        child.stderr.on('data', text => this.print(text));
        child.stdin.on('error', () => {}); // Typed after tide stopped
        child.on('error', error => this.print(`Tide: couldn't run ${tide}: ${error.message}\n`));
        child.on('close', () => {
            this.process = null;
            this.ended = true;
            this.print('\ntide stopped. Press any key to close this terminal.\n');
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

    // Stops tide, and the game it started, which would keep running without it.
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

const runs = { desktop: null, web: null, android: null }; // Running, or that ran last
let lastGame = null;
let lastPage = null; // The web run's address in the editor

// Runs the game, starting it over if it's running: in a window of its own, on
// the web, in the editor, or on Android. Running a game runs its code, so not
// in an untrusted workspace.
async function run(kind) {
    if (!vscode.workspace.isTrusted) {
        const choice = await vscode.window.showErrorMessage(
            "Tide: running a game runs its code, so it only runs once you trust this workspace.", 'Manage Workspace Trust');
        if (choice) vscode.commands.executeCommand('workbench.trust.manage');
        return;
    }
    const game = await pickGame(lastGame);
    if (!game) return;
    const configured = settingProgram('path', 'tide', vscode.Uri.file(game));
    const tide = configured ? configured.file : installedProgram('tide');
    if (!tide) {
        await missing(configured ? `Tide: ${configured.error}` : "Tide: couldn't find tide. Install it, or set tide.path.", 'tide.path');
        return;
    }
    lastGame = game;
    runs[kind]?.dispose();
    runs[kind] = new Run(tide, game, kind, kind === 'web' ? async address => {
        lastPage = await showPage(address, lastPage);
        vscode.commands.executeCommand('setContext', 'tide.gamePage', true);
    } : null);
}

// The developer tools of the game's page, whose console shows what each
// reload did: the integrated browser's own, on the game's tab; or, for the
// Simple Browser, the window's, which reach into the pages it shows.
async function openDevTools() {
    if (!lastPage) {
        vscode.window.showInformationMessage('Tide: run the game on the web first (Tide: Run on the Web).');
        return;
    }
    const commands = await vscode.commands.getCommands(true);
    if (commands.includes('workbench.action.browser.open') && commands.includes('workbench.action.browser.toggleDevTools')) {
        // Without a url, the page's tab comes forward without loading again.
        await vscode.commands.executeCommand('workbench.action.browser.open', { openToSide: true, reuseUrlFilter: lastPage });
        await vscode.commands.executeCommand('workbench.action.browser.toggleDevTools');
        return;
    }
    await vscode.commands.executeCommand('workbench.action.webview.openDeveloperTools');
}

function activate(context) {
    output = vscode.window.createOutputChannel('Tide', { log: true });
    trace = traceChannel();
    context.subscriptions.push(
        output,
        trace,
        vscode.commands.registerCommand('tide.restartServer', restart),
        vscode.commands.registerCommand('tide.run', () => run('desktop')),
        vscode.commands.registerCommand('tide.runWeb', () => run('web')),
        vscode.commands.registerCommand('tide.runAndroid', () => run('android')),
        vscode.commands.registerCommand('tide.openDevTools', openDevTools),
        vscode.workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('tide.server.path')) restart();
        }),
        // Trusted now: the workspace's own server and settings can run.
        vscode.workspace.onDidGrantWorkspaceTrust(restart),
        { dispose: () => Object.values(runs).forEach(r => r?.dispose()) },
    );
    serially(start);
}

function deactivate() {
    return serially(stop);
}

module.exports = { activate, deactivate };
