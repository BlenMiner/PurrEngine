// Runs an Android program on a device or emulator through adb, the way CTest
// runs Android builds' tests: node run_android.mjs <program> [arguments...]
// The program goes to a folder of its own under /data/local/tmp, runs there,
// and goes again; the exit code is the program's.
//
// adb is the SDK's (ANDROID_HOME, ANDROID_SDK_ROOT, or where Android Studio
// puts it), or else PATH's. With several devices, ANDROID_SERIAL picks one.

import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { homedir, platform } from 'node:os';
import { basename, join } from 'node:path';

const [program, ...args] = process.argv.slice(2);

function findAdb() {
    const exe = platform() === 'win32' ? 'adb.exe' : 'adb';
    const sdks = [process.env.ANDROID_HOME, process.env.ANDROID_SDK_ROOT];
    if (platform() === 'win32') sdks.push(process.env.LOCALAPPDATA && join(process.env.LOCALAPPDATA, 'Android', 'Sdk'));
    else if (platform() === 'darwin') sdks.push(join(homedir(), 'Library', 'Android', 'sdk'));
    else sdks.push(join(homedir(), 'Android', 'Sdk'));
    for (const sdk of sdks) {
        if (sdk && existsSync(join(sdk, 'platform-tools', exe))) return join(sdk, 'platform-tools', exe);
    }
    return 'adb';
}

const adb = findAdb();
function run(adbArgs, inherit) {
    const result = spawnSync(adb, adbArgs, { stdio: inherit ? 'inherit' : 'pipe', encoding: 'utf8' });
    if (result.error) {
        console.error(`run_android: can't run adb (${adb}): ${result.error.message}`);
        process.exit(1);
    }
    return result;
}

// Each run has a folder of its own, so tests running at once never write over
// each other's programs.
const folder = `/data/local/tmp/tide/${process.pid}-${Date.now()}`;
const name = basename(program);
const mkdir = run(['shell', `mkdir -p ${folder}`]);
if (mkdir.status !== 0) {
    console.error(`run_android: no device to run on: ${(mkdir.stderr || '').trim()}`);
    process.exit(1);
}
const push = run(['push', program, `${folder}/${name}`]);
if (push.status !== 0) {
    console.error(`run_android: can't copy ${program} to the device: ${(push.stderr || '').trim()}`);
    process.exit(1);
}
const quote = s => `'${String(s).replaceAll("'", `'\\''`)}'`;
// Tide's settings (TIDE_RTC_LOCAL and the like) go along, as tests set them.
const settings = Object.entries(process.env).filter(([key]) => /^TIDE_[A-Z0-9_]+$/.test(key))
    .map(([key, value]) => `${key}=${quote(value)} `).join('');
const command = `cd ${folder} && chmod 755 ./${quote(name)} && ${settings}./${[quote(name), ...args.map(quote)].join(' ')}`;
const result = run(['shell', command], true);
run(['shell', `rm -rf ${folder}`]);
process.exit(result.status ?? 1);
