// PurrLang for VS Code (and Cursor, VSCodium, Windsurf): the grammar colors
// the text, and purrls, the language server that comes with purr, does the rest.

const fs = require('fs');
const os = require('os');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

const exe = process.platform === 'win32' ? 'purrls.exe' : 'purrls';
let client = null;

function isFile(file) {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

// The setting; else the open folder's own build/tools/purrls, when it's
// PurrEngine itself; else purrls on PATH; else where the installers put purr:
// an editor started before purr was installed doesn't have it on its PATH yet.
function findServer() {
    const setting = vscode.workspace.getConfiguration('purrlang').get('server.path');
    const folders = (vscode.workspace.workspaceFolders ?? []).map(f => f.uri.fsPath);
    if (setting) {
        const file = setting.replaceAll('${workspaceFolder}', folders[0] ?? '');
        if (process.platform === 'win32' && !isFile(file) && isFile(file + '.exe')) return file + '.exe';
        return isFile(file) ? file : null;
    }
    for (const folder of folders) {
        if (isFile(path.join(folder, 'build', 'tools', exe))) return path.join(folder, 'build', 'tools', exe);
    }
    for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
        if (dir && isFile(path.join(dir, exe))) return path.join(dir, exe);
    }
    const installed = process.platform === 'win32'
        ? path.join(process.env.LOCALAPPDATA ?? '', 'Purr', 'bin', exe)
        : path.join(os.homedir(), '.purr', 'bin', exe);
    return isFile(installed) ? installed : null;
}

async function start() {
    const server = findServer();
    if (!server) {
        const setting = vscode.workspace.getConfiguration('purrlang').get('server.path');
        const message = setting
            ? `PurrLang: purrlang.server.path is set to ${setting}, and there's no language server there.`
            : "PurrLang: couldn't find purrls, the language server that comes with purr. Install purr, or set purrlang.server.path.";
        const choice = await vscode.window.showErrorMessage(message, 'Install purr', 'Settings');
        if (choice === 'Install purr') {
            vscode.env.openExternal(vscode.Uri.parse('https://github.com/BlenMiner/PurrEngine#install'));
        } else if (choice === 'Settings') {
            vscode.commands.executeCommand('workbench.action.openSettings', 'purrlang.server.path');
        }
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

function activate(context) {
    context.subscriptions.push(
        vscode.commands.registerCommand('purrlang.restartServer', restart),
        vscode.workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('purrlang.server.path')) restart();
        }),
    );
    return start();
}

function deactivate() {
    return stop();
}

module.exports = { activate, deactivate };
