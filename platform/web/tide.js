// Tide's web runtime. It runs a WebAssembly program built with clang and
// wasi-libc (cmake/wasi-toolchain.cmake) in a page, and gives it:
//
// - what platform/web/tide_web.h declares: the canvas, input and the frame loop;
// - the OpenGL ES 3 functions its renderer calls (platform/src/gl.h), on WebGL 2;
// - the few WASI functions wasi-libc needs: output, the clock, arguments, exit.
//
// The page defines `var Tide = { canvas, print, printErr, arguments, onExit,
// onAbort, checkGL }` (all optional) before this script, and TIDE_PROGRAM holds
// the program as base64 (cmake/web_page.mjs, or the tide command).
//
// The lines between the `tide run` markers below are for hot reloading, in the
// page `tide run --web` serves; the pages made to ship leave them out.

(async () => {
    'use strict';
    const config = typeof Tide !== 'undefined' ? Tide : {};
    const canvas = config.canvas || document.getElementById('canvas');
    const print = config.print || (text => console.log(text));
    const printErr = config.printErr || (text => console.error(text));

    let memory = null;
    let exports = null;
    let gl = null;

    const u8 = () => new Uint8Array(memory.buffer);
    const i32 = () => new Int32Array(memory.buffer);
    const f32 = () => new Float32Array(memory.buffer);
    const view = () => new DataView(memory.buffer);
    const decoder = new TextDecoder();
    const encoder = new TextEncoder();

    // A NUL-terminated string in the program's memory. Copied out, as for
    // every browser function that won't take shared memory: the program's is
    // shared when it runs on threads.
    function string(ptr) {
        const bytes = u8();
        let end = ptr;
        while (bytes[end]) end++;
        return decoder.decode(bytes.slice(ptr, end));
    }

    // Thrown to leave WebAssembly: tide_web_run unwinds main's stack back to the
    // browser, and exit() ends the program.
    const unwind = { tide: 'unwind' };
    class Exit {
        constructor(code) { this.code = code; }
    }

    // Random bytes, as many as asked: the browser gives 64 KiB at a time.
    function randomBytes(count) {
        const bytes = new Uint8Array(count);
        for (let at = 0; at < count; at += 65536) crypto.getRandomValues(bytes.subarray(at, Math.min(count, at + 65536)));
        return bytes;
    }

    let stopped = false;
    function finish(code) {
        stopped = true;
        endThreads();
        flush(1);
        flush(2);
        if (config.onExit) config.onExit(code);
    }

    // -----------------------------------------------------------------------
    // WASI: output, the clock, arguments and exit. There's no file system.

    const EBADF = 8, ENOSYS = 52, ESPIPE = 70, ENOTCAPABLE = 76;
    const pending = { 1: '', 2: '' };
    function flush(fd) {
        if (pending[fd]) (fd === 1 ? print : printErr)(pending[fd]);
        pending[fd] = '';
    }
    function output(fd, text) {
        const lines = (pending[fd] + text).split('\n');
        pending[fd] = lines.pop();
        for (const line of lines) (fd === 1 ? print : printErr)(line);
    }

    const args = ['program', ...(config.arguments || [])].map(a => encoder.encode(a + '\0'));

    const wasi = {
        args_sizes_get(countPtr, sizePtr) {
            view().setUint32(countPtr, args.length, true);
            view().setUint32(sizePtr, args.reduce((n, a) => n + a.length, 0), true);
            return 0;
        },
        args_get(argvPtr, bufPtr) {
            for (const arg of args) {
                view().setUint32(argvPtr, bufPtr, true);
                u8().set(arg, bufPtr);
                argvPtr += 4;
                bufPtr += arg.length;
            }
            return 0;
        },
        environ_sizes_get(countPtr, sizePtr) {
            view().setUint32(countPtr, 0, true);
            view().setUint32(sizePtr, 0, true);
            return 0;
        },
        environ_get() { return 0; },
        clock_time_get(id, precision, timePtr) {
            const ns = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(performance.now() * 1e6));
            view().setBigUint64(timePtr, ns, true);
            return 0;
        },
        clock_res_get(id, resPtr) {
            view().setBigUint64(resPtr, 1000n, true);
            return 0;
        },
        random_get(ptr, len) {
            u8().set(randomBytes(len), ptr);
            return 0;
        },
        sched_yield: () => 0, // malloc's lock, which threads spin on
        fd_write(fd, iovs, count, writtenPtr) {
            let written = 0;
            for (let i = 0; i < count; i++) {
                const ptr = view().getUint32(iovs + i * 8, true);
                const len = view().getUint32(iovs + i * 8 + 4, true);
                if (fd === 1 || fd === 2) output(fd, decoder.decode(u8().slice(ptr, ptr + len)));
                written += len;
            }
            view().setUint32(writtenPtr, written, true);
            return fd === 1 || fd === 2 ? 0 : EBADF;
        },
        fd_read(fd, iovs, count, readPtr) {
            view().setUint32(readPtr, 0, true);
            return 0;
        },
        // stdout and stderr are terminals, so wasi-libc flushes them line by line.
        fd_fdstat_get(fd, statPtr) {
            if (fd > 2) return EBADF;
            u8().fill(0, statPtr, statPtr + 24);
            u8()[statPtr] = 2; // Character device
            return 0;
        },
        fd_fdstat_set_flags() { return 0; },
        fd_close() { return 0; },
        fd_seek() { return ESPIPE; },
        fd_prestat_get() { return EBADF; }, // No directories
        fd_prestat_dir_name() { return EBADF; },
        path_open() { return ENOTCAPABLE; },
        path_filestat_get() { return ENOTCAPABLE; },
        proc_exit(code) {
            finish(code);
            throw new Exit(code);
        },
    };

    // -----------------------------------------------------------------------
    // The platform (tide_web.h)

    // Keys by the DOM's `code`, which names physical positions. A key pressed
    // and released between two frames still reads as held for one, so no tap
    // is lost however slow the frames are.
    const keyIndex = new Map();
    let keysHeld = new Uint8Array(0);
    let keysTapped = new Uint8Array(0);
    // Keys whose browser default (scrolling, moving focus) would fight the game.
    const keepFromBrowser = new Set(['Space', 'Tab', 'Backspace', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
        'PageUp', 'PageDown', 'Home', 'End']);
    function onKey(event, held) {
        // A phone's keyboard says no position for its keys: Enter is the one
        // the GUI needs from it, as what it types comes through the field.
        const index = keyIndex.get(event.code || (event.key === 'Enter' ? 'Enter' : ''));
        if (index === undefined) return;
        keysHeld[index] = held;
        if (held) keysTapped[index] = 1;
        // Typing into the page's field, a space has to reach it: what's typed there comes from it.
        const intoField = field && event.target === field && event.code === 'Space';
        if (keepFromBrowser.has(event.code) && !intoField) event.preventDefault();
    }
    function tapKey(code) {
        const index = keyIndex.get(code);
        if (index !== undefined) keysTapped[index] = 1;
    }
    // Characters typed, which follow the keyboard layout: `key` is one
    // character for keys that type one, and a name like "Shift" for the others.
    const typed = [];
    function onType(event) {
        if (event.ctrlKey || event.metaKey || event.isComposing) return;
        if (field && event.target === field) return; // Its input says what's typed
        const c = event.key.codePointAt(0);
        if ([...event.key].length !== 1 || c < 32 || c === 127) return;
        if (typed.length < 64) typed.push(c);
    }
    // A field of the page's own, hidden, which has the focus while the player
    // types into the GUI, so phones show their keyboard (tide_web_typing).
    // What's typed there comes as characters typed, and deleting it as
    // Backspace; it always holds a space, so there's something to delete.
    let field = null;
    function typingField() {
        if (field) return field;
        field = document.createElement('input');
        field.type = 'text';
        field.autocomplete = 'off';
        field.setAttribute('autocapitalize', 'off');
        field.setAttribute('autocorrect', 'off');
        field.spellcheck = false;
        // 16px, or iOS zooms into it; on the screen, or browsers won't focus it
        Object.assign(field.style, { position: 'fixed', left: '0', bottom: '0', width: '1px', height: '1px',
            opacity: '0', border: '0', padding: '0', fontSize: '16px' });
        field.value = ' ';
        field.addEventListener('input', event => {
            if (event.inputType === 'deleteContentBackward') tapKey('Backspace');
            else if (event.inputType === 'insertLineBreak') tapKey('Enter');
            else if (event.data) {
                for (const ch of event.data) {
                    const c = ch.codePointAt(0);
                    if (c >= 32 && c !== 127 && typed.length < 1024) typed.push(c);
                }
            }
            field.value = ' ';
        });
        document.body.appendChild(field);
        return field;
    }
    addEventListener('keydown', event => { onKey(event, 1); onType(event); });
    // Ctrl+V (Cmd+V) pastes as typing, but for newlines and tabs.
    addEventListener('paste', event => {
        const text = event.clipboardData ? event.clipboardData.getData('text') : '';
        for (const ch of text) {
            const c = ch.codePointAt(0);
            if (c >= 32 && c !== 127 && typed.length < 1024) typed.push(c);
        }
        event.preventDefault();
    });
    // The clipboard API where the browser has it, else the old way: copying a
    // hidden field's selection.
    function copyText(text) {
        const field = document.createElement('textarea');
        field.value = text;
        field.style.position = 'fixed';
        field.style.opacity = '0';
        document.body.appendChild(field);
        field.select();
        try { document.execCommand('copy'); } catch { /* Nothing more to try */ }
        field.remove();
        canvas.focus();
    }
    addEventListener('keyup', event => onKey(event, 0));
    addEventListener('blur', () => { keysHeld.fill(0); keysTapped.fill(0); }); // Keys released elsewhere never send keyup

    // The canvas's size in CSS pixels, which the program counts in, as desktop
    // builds count in the display's logical pixels. It renders devicePixelRatio
    // times as many, so it's as sharp as the page's text. The whole page when
    // `fill`. Checked every frame, which also catches zooming and moving to a
    // screen with another ratio.
    const size = { width: 0, height: 0, fill: false };
    function fit() {
        if (size.fill) {
            size.width = innerWidth;
            size.height = innerHeight;
        }
        const ratio = devicePixelRatio || 1;
        const width = Math.max(1, Math.round(size.width * ratio));
        const height = Math.max(1, Math.round(size.height * ratio));
        // Setting the size clears the canvas, even to the same size.
        if (canvas.width !== width) canvas.width = width;
        if (canvas.height !== height) canvas.height = height;
        const cssWidth = size.width + 'px', cssHeight = size.height + 'px';
        if (canvas.style.width !== cssWidth) canvas.style.width = cssWidth;
        if (canvas.style.height !== cssHeight) canvas.style.height = cssHeight;
    }

    // The mouse over the canvas, in CSS pixels. Buttons in tide_mouse's order:
    // left, right, middle, back, forward (the DOM's is left, middle, right, ...).
    const mouse = { x: 0, y: 0, buttons: 0, tapped: 0, wheelX: 0, wheelY: 0 }; // Taps as for keys
    const buttonBit = [1, 4, 2, 8, 16];
    function onMouseMove(event) {
        const rect = canvas.getBoundingClientRect();
        mouse.x = (event.clientX - rect.left) * size.width / (rect.width || 1);
        mouse.y = (event.clientY - rect.top) * size.height / (rect.height || 1);
    }
    canvas.addEventListener('mousemove', onMouseMove);
    canvas.addEventListener('mousedown', event => {
        onMouseMove(event);
        mouse.buttons |= buttonBit[event.button] || 0;
        mouse.tapped |= buttonBit[event.button] || 0;
        canvas.focus();
    });
    addEventListener('mouseup', event => { mouse.buttons &= ~(buttonBit[event.button] || 0); });
    canvas.addEventListener('wheel', event => {
        mouse.wheelX -= Math.sign(event.deltaX);
        mouse.wheelY -= Math.sign(event.deltaY); // Positive away from the user
        event.preventDefault();
    }, { passive: false });

    // Fingers on the canvas, from pointer events, in the order they happened;
    // the program takes them each frame. A finger is only the touchscreen's:
    // the browser makes no mouse events of it (its touch events' defaults are
    // canceled), and takes no gestures of its own on the canvas (touch-action).
    // Phases are tide_touch_phase's.
    const touches = [];
    let touchTaken = null;
    function onTouch(event, phase) {
        if (event.pointerType !== 'touch') return;
        event.preventDefault();
        if (phase === 1 && touches.length >= 1024) return; // The program isn't taking them: moves can go
        const rect = canvas.getBoundingClientRect();
        touches.push({
            phase,
            source: event.pointerId >>> 0,
            x: (event.clientX - rect.left) * size.width / (rect.width || 1),
            y: (event.clientY - rect.top) * size.height / (rect.height || 1),
        });
        if (phase === 0) canvas.focus();
    }
    canvas.style.touchAction = 'none';
    canvas.addEventListener('pointerdown', event => onTouch(event, 0));
    canvas.addEventListener('pointermove', event => onTouch(event, 1));
    canvas.addEventListener('pointerup', event => onTouch(event, 2));
    canvas.addEventListener('pointercancel', event => onTouch(event, 3));
    for (const name of ['touchstart', 'touchmove', 'touchend', 'touchcancel']) {
        canvas.addEventListener(name, event => event.preventDefault(), { passive: false });
    }

    // Frames come on animation frames or, with `timerFrames`, as fast as
    // timers allow (headless pages have no animation frames). A hidden page
    // gets neither at its pace: browsers stop its animation frames and slow its
    // timers to about one a second, which would stop this machine's part in a
    // match, a server its players time out of or a player its server drops. A
    // worker's timers keep their pace, so while the page is hidden, frames come
    // from a worker's, when the program says it next needs one (its match's
    // next tick), and draw nothing (tide_web_hidden).
    let timerFrames = false;
    let looping = false;   // The program runs frames
    let scheduled = false; // A frame is scheduled, on the page's own clock
    let heartbeat = null;  // The worker, while the page is hidden
    let beating = false;   // ...which has a frame coming
    let nextFrame = 1000 / 60; // Milliseconds until the program needs a frame, as its last one said
    function scheduleFrame() {
        if (scheduled || heartbeat) return;
        scheduled = true;
        if (timerFrames || document.hidden) setTimeout(scheduledFrame, 0);
        else requestAnimationFrame(scheduledFrame);
    }
    function scheduledFrame() {
        scheduled = false;
        if (looping) frame();
    }
    function beat() {
        if (beating) return; // One frame coming at a time, whatever else runs one
        beating = true;
        heartbeat.postMessage(nextFrame);
    }
    function stopHeartbeat() {
        if (heartbeat) heartbeat.terminate();
        heartbeat = null;
        beating = false;
    }
    function followVisibility() {
        if (document.hidden && !heartbeat && looping) {
            try {
                const wait = 'onmessage = event => setTimeout(() => postMessage(0), event.data);';
                heartbeat = new Worker(URL.createObjectURL(new Blob([wait], { type: 'text/javascript' })));
                heartbeat.onmessage = () => {
                    beating = false;
                    if (!looping) stopHeartbeat(); // The program is over
                    else if (document.hidden) frame();
                };
                beat();
            } catch (error) { // Frames come as slowly as the browser lets them
                heartbeat = null;
                printErr('tide: no worker to keep frames going while the page is hidden: ' + error);
            }
        } else if (!document.hidden && heartbeat) {
            stopHeartbeat();
            if (looping) scheduleFrame();
        }
    }
    document.addEventListener('visibilitychange', followVisibility);
    function frame() {
        if (stopped) {
            looping = false;
            return;
        }
        fit();
        try {
            // Timers take whole milliseconds, dropping the rest: one rounded
            // down would come before the tick, for nothing
            const seconds = exports.tide_web_frame();
            nextFrame = seconds > 0 ? Math.ceil(seconds * 1000) : 0;
        } catch (error) {
            looping = false;
            if (!(error instanceof Exit)) fail(error);
            return;
        }
        if (heartbeat) beat();
        else scheduleFrame();
    }

    const platform = {
        init_canvas(width, height, resizable) {
            size.width = width;
            size.height = height;
            size.fill = !!resizable;
            fit();
            gl = canvas.getContext('webgl2', { alpha: false, antialias: false, depth: true, stencil: false });
            return gl ? 1 : 0;
        },
        canvas_width: () => size.width,
        canvas_height: () => size.height,
        canvas_pixel_width: () => canvas.width,
        canvas_pixel_height: () => canvas.height,
        mouse_x: () => mouse.x,
        mouse_y: () => mouse.y,
        mouse_buttons() { const held = mouse.buttons | mouse.tapped; mouse.tapped = 0; return held; },
        take_wheel_x() { const v = mouse.wheelX; mouse.wheelX = 0; return v; },
        take_wheel_y() { const v = mouse.wheelY; mouse.wheelY = 0; return v; },
        touchscreen: () => (navigator.maxTouchPoints || 0) > 0 ? 1 : 0,
        take_touch() { touchTaken = touches.shift() || null; return touchTaken ? 1 : 0; },
        touch_phase: () => touchTaken ? touchTaken.phase : 0,
        touch_source: () => touchTaken ? touchTaken.source : 0,
        touch_x: () => touchTaken ? touchTaken.x : 0,
        touch_y: () => touchTaken ? touchTaken.y : 0,
        watch_key(index, codePtr) {
            keyIndex.set(string(codePtr), index);
            if (index >= keysHeld.length) {
                const grown = new Uint8Array(index + 1);
                grown.set(keysHeld);
                keysHeld = grown;
                const tapped = new Uint8Array(index + 1);
                tapped.set(keysTapped);
                keysTapped = tapped;
            }
        },
        key_held(index) { const held = keysHeld[index] || keysTapped[index] || 0; keysTapped[index] = 0; return held; },
        take_char: () => typed.length ? typed.shift() : 0,
        typing(on) {
            if (on) typingField().focus({ preventScroll: true });
            else if (field && document.activeElement === field) canvas.focus();
        },
        copy(textPtr) {
            const text = string(textPtr);
            if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).catch(() => copyText(text));
            else copyText(text);
        },
        gamepad_connected(pad) {
            const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
            return gamepads[pad] && gamepads[pad].connected ? 1 : 0;
        },
        gamepad_axis(pad, axis) {
            const gamepad = navigator.getGamepads()[pad];
            return gamepad && axis < gamepad.axes.length ? gamepad.axes[axis] : 0;
        },
        gamepad_button(pad, button) {
            const gamepad = navigator.getGamepads()[pad];
            return gamepad && button < gamepad.buttons.length ? gamepad.buttons[button].value : 0;
        },
        run(timers) {
            timerFrames = !!timers;
            if (!looping) {
                looping = true;
                scheduleFrame();
                followVisibility(); // It may start in a tab in the background
            }
            throw unwind;
        },
        hidden: () => document.hidden ? 1 : 0,
        stop() { stopped = true; },
        eval: scriptPtr => (0, eval)(string(scriptPtr)) ? 1 : 0,
    };

    // -----------------------------------------------------------------------
    // Rooms: matches players find by a code, through the relay (relay/). The
    // relay only introduces players, passing along what WebRTC needs to
    // connect them; their packets then go straight between them, on data
    // channels that neither order nor resend, like UDP. A program is in one
    // room at a time: the one it hosts, or the one it joined. Players in it are
    // numbered: the host is 0 to those who join, and they're 1 and up to it.
    // A room it hosts has a key, which lets the players of its match meet there
    // again when it goes (host migration): the relay makes the first one there
    // its host, and introduces the others to it.

    const relayUrl = config.relay || 'wss://relay.tide-engine.dev';
    const CODE_LETTERS = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ'; // No look-alikes: 0 and O, 1 and I
    let room = null;
    let rooms = 0; // Rooms opened so far, which numbers them

    const newCode = () => Array.from(crypto.getRandomValues(new Uint8Array(6)), b => CODE_LETTERS[b & 31]).join('');
    const newKey = () => Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, '0')).join('');

    // `key`: going to the room again with it, to host it or join its host,
    // whichever the relay says (`moved`: 1 hosting, 2 joining, -1 failed).
    function openRoom(hosting, code, key) {
        closeRoom();
        room = {
            number: ++rooms, hosting, code, failed: false, reachable: true, connected: false, opened: performance.now(),
            ws: null, announced: false, ice: [], peers: new Map(), byRelay: new Map(), next: 1, inbox: [],
            key: key || (hosting ? newKey() : ''), moving: !!key, moved: 0,
        };
        if (/^[2-9A-HJ-NP-Z]{6}$/.test(code)) {
            connectRelay(room);
        } else {
            room.failed = true;
            if (room.moving) room.moved = -1;
            printErr(`tide: '${code}' isn't a room code: they're 6 letters and digits, like K7QF2M`);
        }
        return room.number;
    }

    function closeRoom() {
        if (!room) return;
        const r = room;
        room = null;
        if (r.ws) r.ws.close();
        for (const peer of r.peers.values()) peer.pc.close();
    }

    // The match in room `r`, which this program hosts, ended: the relay keeps
    // the room as ended a while, so its players don't take it over. It may not
    // know the room yet (its hello hasn't come, or it's being reached again),
    // so the room asks for it first: the relay reads a connection's messages
    // in order. A connection still opening, or a new one, says it once open,
    // and the room closing leaves it be.
    function sayEnded(r) {
        const say = (ws, announced) => {
            if (!announced) ws.send(JSON.stringify({ host: r.code, key: r.key }));
            ws.send(JSON.stringify({ end: r.code }));
        };
        if (r.ws && r.ws.readyState === WebSocket.OPEN) {
            say(r.ws, r.announced);
            return;
        }
        let ws = r.ws && r.ws.readyState === WebSocket.CONNECTING ? r.ws : null;
        r.ws = null;
        if (!ws) {
            try {
                ws = new WebSocket(relayUrl);
            } catch {
                return;
            }
        }
        ws.onmessage = ws.onclose = null;
        ws.onopen = () => {
            say(ws, false);
            ws.close();
        };
    }

    // A host that loses the relay opens its room again once it's back, under
    // the same code if it's still free. Players already in keep playing: only
    // joining needs the relay.
    function connectRelay(r) {
        let ws;
        try {
            ws = new WebSocket(relayUrl);
        } catch (error) {
            printErr(`tide: can't reach the relay at ${relayUrl}: ${error.message}`);
            r.failed = !r.hosting;
            r.reachable = false;
            return;
        }
        r.ws = ws;
        r.announced = false; // Whether this connection asked the relay for the room yet
        const send = message => { if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(message)); };
        ws.onmessage = event => {
            if (room !== r) return;
            let m;
            try {
                m = JSON.parse(event.data);
            } catch {
                return;
            }
            if (m.relay) {
                r.ice = m.ice || [];
                send(r.moving ? { migrate: r.code, key: r.key } : r.hosting ? { host: r.code, key: r.key } : { join: r.code });
                r.announced = true;
            } else if (m.hosting) {
                r.reachable = true;
                if (r.moving) { // The room's host was gone: this program is now
                    r.moving = false;
                    r.hosting = true;
                    r.moved = 1;
                }
            } else if (m.ended && r.moving) { // Its host ended the match there
                r.moving = false;
                r.failed = true;
                r.moved = -2;
            } else if (m.taken && r.moving) { // Another key: not the match it was in
                r.moving = false;
                r.failed = true;
                r.moved = -1;
                printErr(`tide: room ${r.code} has another match now`);
            } else if (m.taken) {
                r.code = newCode();
                send({ host: r.code, key: r.key });
            } else if (m.lost && r.hosting) {
                // Its players took the room over, thinking it gone: it hosts it no more
                r.failed = true;
                r.reachable = false;
                printErr(`tide: room ${m.lost} has another host now`);
            } else if (m.missing && !r.hosting && performance.now() - r.opened < 5000) {
                // A room its host just opened, which the relay may not know
                // yet (it can take a few seconds to wake up): ask again soon
                setTimeout(() => { if (room === r) send({ join: r.code }); }, 500);
            } else if (m.missing || m.full || m.closed) {
                if (!r.connected) {
                    r.failed = true;
                    printErr(m.missing ? `tide: no room has the code ${m.missing}`
                        : m.full ? `tide: too many players are joining room ${m.full} at once`
                        : `tide: room ${m.closed} closed`);
                }
            } else if (m.joined) {
                if (r.moving) { // The room's host is there: this program joins it
                    r.moving = false;
                    r.moved = 2;
                }
                offer(r, send);
            } else if (m.peer) {
                const peer = newPeer(r, r.next++, signal => send({ to: m.peer, signal }));
                r.byRelay.set(m.peer, peer);
            } else if (m.from) {
                const peer = r.byRelay.get(m.from);
                if (peer) onSignal(r, peer, m.signal, false);
            } else if (m.signal) {
                const peer = r.peers.get(0);
                if (peer) onSignal(r, peer, m.signal, true);
            } else if (m.left) {
                const peer = r.byRelay.get(m.left);
                r.byRelay.delete(m.left);
                // It gave up before connecting, rather than hanging up once it had
                if (peer && !peer.open && peer.pc.connectionState !== 'connected') dropPeer(r, peer);
            }
        };
        ws.onclose = () => {
            if (room !== r || r.ws !== ws) return;
            r.ws = null;
            if (!r.hosting) {
                if (!r.connected && !r.failed && !r.peers.size && performance.now() - r.opened < 10000) {
                    // Not introduced yet: the relay may be waking up (it
                    // refuses connections for a moment then), so try again
                    setTimeout(() => { if (room === r && !r.ws) connectRelay(r); }, 1000);
                } else if (!r.connected && !r.failed) {
                    r.failed = true;
                    if (r.moving) r.moved = -1;
                    printErr(`tide: lost the relay at ${relayUrl} while joining room ${r.code}`);
                }
                return;
            }
            if (r.reachable) printErr(`tide: lost the relay at ${relayUrl}; room ${r.code} opens again once it's back`);
            r.reachable = false;
            // Players still connecting can't finish without it, and the relay
            // numbers those who join from scratch when it's back.
            for (const peer of r.byRelay.values()) if (!peer.open) dropPeer(r, peer);
            r.byRelay.clear();
            setTimeout(() => { if (room === r) connectRelay(r); }, 3000);
        };
    }

    // A player on the other end of a data channel: the host, to one who joins,
    // and each one who joins, to the host.
    function newPeer(r, number, sendSignal) {
        // Tide.iceTransportPolicy 'relay' makes every connection go through
        // TURN, to test it: normally they're direct whenever they can be.
        const pc = new RTCPeerConnection({ iceServers: r.ice, iceTransportPolicy: config.iceTransportPolicy || 'all' });
        const peer = { number, pc, channel: null, open: false, sendSignal, chain: Promise.resolve() };
        pc.onicecandidate = event => { if (event.candidate) sendSignal({ candidate: event.candidate.toJSON() }); };
        pc.onconnectionstatechange = () => {
            if (pc.connectionState === 'failed' || pc.connectionState === 'closed') dropPeer(r, peer);
        };
        pc.ondatachannel = event => useChannel(r, peer, event.channel);
        r.peers.set(number, peer);
        return peer;
    }

    function useChannel(r, peer, channel) {
        channel.binaryType = 'arraybuffer';
        const opened = () => {
            if (peer.open) return;
            peer.open = true;
            if (!r.hosting) r.connected = true;
        };
        channel.onopen = opened;
        channel.onmessage = event => {
            // Joined: the relay's part is done once the host sends over the
            // channel, which it only does with its end open. Not before: the
            // relay tells the host this one left, and a host that hasn't
            // caught up with its own end yet takes it for a player who gave up
            if (!r.hosting && r.ws) r.ws.close();
            if (room === r && r.inbox.length < 4096 && event.data instanceof ArrayBuffer) {
                r.inbox.push([peer.number, new Uint8Array(event.data)]);
            }
        };
        channel.onclose = () => dropPeer(r, peer);
        peer.channel = channel;
        // A channel the other end made can be open already by the time the
        // page hears of it (ondatachannel), with no open event to come: it
        // would never be sent to, and its player never let in
        if (channel.readyState === 'open') opened();
    }

    function dropPeer(r, peer) {
        if (r.peers.get(peer.number) !== peer) return;
        r.peers.delete(peer.number);
        peer.pc.close();
        if (!r.hosting && !r.connected && !r.failed) {
            r.failed = true;
            printErr(`tide: couldn't connect to the host of room ${r.code}`);
        }
    }

    // Joining: this end offers, and the host answers.
    async function offer(r, send) {
        const peer = newPeer(r, 0, signal => send({ signal }));
        useChannel(r, peer, peer.pc.createDataChannel('tide', { ordered: false, maxRetransmits: 0 }));
        try {
            await peer.pc.setLocalDescription();
            send({ signal: { description: peer.pc.localDescription.toJSON() } });
        } catch (error) {
            printErr('tide: ' + error.message);
            dropPeer(r, peer);
        }
    }

    // Signals are handled one at a time, in order, since each waits on WebRTC:
    // a candidate only goes in after the description before it.
    function onSignal(r, peer, signal, joining) {
        if (!signal || typeof signal !== 'object') return;
        peer.chain = peer.chain.then(async () => {
            if (room !== r || r.peers.get(peer.number) !== peer) return;
            if (signal.description) {
                await peer.pc.setRemoteDescription(signal.description);
                if (!joining && signal.description.type === 'offer') {
                    await peer.pc.setLocalDescription();
                    peer.sendSignal({ description: peer.pc.localDescription.toJSON() });
                }
            } else if (signal.candidate) {
                await peer.pc.addIceCandidate(signal.candidate);
            }
        }).catch(error => printErr('tide: ' + error.message));
    }

    Object.assign(platform, {
        room_host: () => openRoom(true, newCode()),
        room_join: codePtr => openRoom(false, string(codePtr).replace(/\s+/g, '').toUpperCase()),
        room_migrate: (codePtr, keyPtr) => openRoom(false, string(codePtr), string(keyPtr)),
        room_migrated: number => !room || room.number !== number ? -1 : room.failed && !room.moved ? -1 : room.moved,
        // The key of the room it hosts into `out` (33 bytes), "" for none
        room_key(outPtr) {
            const key = room && room.hosting && !room.failed ? room.key : '';
            u8().set(encoder.encode(key), outPtr);
            u8()[outPtr + key.length] = 0;
        },
        room_close(number) { if (room && room.number === number) closeRoom(); },
        room_end(number) { if (room && room.number === number && room.hosting) sayEnded(room); },
        // The code into `out` (7 bytes), "" while there's none: never a room
        // that failed, nor one the relay can't be told about.
        room_code(outPtr) {
            const code = room && !room.failed && room.reachable ? room.code : '';
            u8().set(encoder.encode(code), outPtr);
            u8()[outPtr + code.length] = 0;
        },
        room_failed: () => room && room.failed ? 1 : 0,
        room_send(number, to, ptr, size) {
            const peer = room && room.number === number ? room.peers.get(to) : null;
            const channel = peer && peer.open ? peer.channel : null;
            // Like UDP, what can't go now is lost, and so is what would queue up
            if (channel && channel.readyState === 'open' && channel.bufferedAmount < 262144) {
                channel.send(u8().slice(ptr, ptr + size));
            }
        },
        room_receive(number, fromPtr, ptr, capacity) {
            if (!room || room.number !== number) return 0;
            for (;;) {
                const next = room.inbox.shift();
                if (!next) return 0;
                const [from, bytes] = next;
                if (bytes.length > capacity || bytes.length === 0) continue; // Longer ones are dropped
                view().setUint32(fromPtr, from, true);
                u8().set(bytes, ptr);
                return bytes.length;
            }
        },
    });

    // -----------------------------------------------------------------------
    // OpenGL ES 3 on WebGL 2: the functions the platform layer's renderer
    // calls (platform/src/gl.h), each by its C name. GL names objects with
    // numbers, WebGL with objects, so each kind has a table from number to
    // object.

    const tables = { buffer: [null], texture: [null], framebuffer: [null], renderbuffer: [null], vertexArray: [null],
        program: [null], shader: [null], uniform: [null] };
    function add(kind, object) {
        tables[kind].push(object);
        return tables[kind].length - 1;
    }
    const get = (kind, id) => tables[kind][id] || null;
    function generate(kind, create, count, ptr) {
        for (let i = 0; i < count; i++) i32()[(ptr >> 2) + i] = add(kind, create());
    }
    function remove(kind, destroy, count, ptr) {
        for (let i = 0; i < count; i++) {
            const id = i32()[(ptr >> 2) + i];
            if (tables[kind][id]) destroy(tables[kind][id]);
            tables[kind][id] = null;
        }
    }

    // Uniform locations are numbers per program and name.
    const uniformIds = new Map();
    function uniformId(program, name) {
        const key = program + ':' + name;
        if (!uniformIds.has(key)) {
            const location = gl.getUniformLocation(get('program', program), name);
            uniformIds.set(key, location ? add('uniform', location) : -1);
        }
        return uniformIds.get(key);
    }

    // The typed view and element offset for pixel data of `type` at `ptr`.
    function pixels(type, ptr) {
        switch (type) {
        case 0x1406: return [f32(), ptr >> 2]; // FLOAT
        case 0x1403: return [new Uint16Array(memory.buffer), ptr >> 1]; // UNSIGNED_SHORT
        case 0x1405: return [new Uint32Array(memory.buffer), ptr >> 2]; // UNSIGNED_INT
        default: return [u8(), ptr];
        }
    }

    function writeLog(log, max, lengthPtr, ptr) {
        const bytes = encoder.encode(log || '');
        const n = Math.max(0, Math.min(bytes.length, max - 1));
        if (max > 0) {
            u8().set(bytes.subarray(0, n), ptr);
            u8()[ptr + n] = 0;
        }
        if (lengthPtr) i32()[lengthPtr >> 2] = n;
    }

    // A shader's or program's parameter: a number, or whether, as 1 or 0.
    const writeParameter = (value, ptr) => { i32()[ptr >> 2] = Number(value) || 0; };

    const glFunctions = {
        glActiveTexture: t => gl.activeTexture(t),
        glAttachShader: (p, s) => gl.attachShader(get('program', p), get('shader', s)),
        glBindBuffer: (target, b) => gl.bindBuffer(target, get('buffer', b)),
        glBindFramebuffer: (target, f) => gl.bindFramebuffer(target, get('framebuffer', f)),
        glBindRenderbuffer: (target, r) => gl.bindRenderbuffer(target, get('renderbuffer', r)),
        glBindTexture: (target, t) => gl.bindTexture(target, get('texture', t)),
        glBindVertexArray: v => gl.bindVertexArray(get('vertexArray', v)),
        glBlendFunc: (s, d) => gl.blendFunc(s, d),
        glBufferData(target, size, ptr, usage) {
            if (ptr) gl.bufferData(target, u8(), usage, ptr, size);
            else gl.bufferData(target, size, usage);
        },
        glBufferSubData: (target, offset, size, ptr) => gl.bufferSubData(target, offset, u8(), ptr, size),
        glCheckFramebufferStatus: target => gl.checkFramebufferStatus(target),
        glClear: mask => gl.clear(mask),
        glClearColor: (r, g, b, a) => gl.clearColor(r, g, b, a),
        glCompileShader: s => gl.compileShader(get('shader', s)),
        glCreateProgram: () => add('program', gl.createProgram()),
        glCreateShader: type => add('shader', gl.createShader(type)),
        glDeleteBuffers: (n, ptr) => remove('buffer', o => gl.deleteBuffer(o), n, ptr),
        glDeleteFramebuffers: (n, ptr) => remove('framebuffer', o => gl.deleteFramebuffer(o), n, ptr),
        glDeleteRenderbuffers: (n, ptr) => remove('renderbuffer', o => gl.deleteRenderbuffer(o), n, ptr),
        glDeleteShader(s) {
            gl.deleteShader(get('shader', s));
            tables.shader[s] = null;
        },
        glDeleteTextures: (n, ptr) => remove('texture', o => gl.deleteTexture(o), n, ptr),
        glDepthFunc: f => gl.depthFunc(f),
        glDisable: cap => gl.disable(cap),
        glDrawArrays: (mode, first, count) => gl.drawArrays(mode, first, count),
        glDrawArraysInstanced: (mode, first, count, instances) => gl.drawArraysInstanced(mode, first, count, instances),
        glDrawElementsInstanced: (mode, count, type, offset, instances) =>
            gl.drawElementsInstanced(mode, count, type, offset, instances),
        glEnable: cap => gl.enable(cap),
        glEnableVertexAttribArray: i => gl.enableVertexAttribArray(i),
        glFramebufferRenderbuffer: (target, attachment, rbTarget, r) =>
            gl.framebufferRenderbuffer(target, attachment, rbTarget, get('renderbuffer', r)),
        glGenBuffers: (n, ptr) => generate('buffer', () => gl.createBuffer(), n, ptr),
        glGenFramebuffers: (n, ptr) => generate('framebuffer', () => gl.createFramebuffer(), n, ptr),
        glGenRenderbuffers: (n, ptr) => generate('renderbuffer', () => gl.createRenderbuffer(), n, ptr),
        glGenTextures: (n, ptr) => generate('texture', () => gl.createTexture(), n, ptr),
        glGenVertexArrays: (n, ptr) => generate('vertexArray', () => gl.createVertexArray(), n, ptr),
        glGetProgramInfoLog: (p, max, lengthPtr, ptr) => writeLog(gl.getProgramInfoLog(get('program', p)), max, lengthPtr, ptr),
        glGetProgramiv: (p, pname, ptr) => writeParameter(gl.getProgramParameter(get('program', p), pname), ptr),
        glGetShaderInfoLog: (s, max, lengthPtr, ptr) => writeLog(gl.getShaderInfoLog(get('shader', s)), max, lengthPtr, ptr),
        glGetShaderiv: (s, pname, ptr) => writeParameter(gl.getShaderParameter(get('shader', s), pname), ptr),
        glGetUniformLocation: (p, name) => uniformId(p, string(name)),
        glLinkProgram: p => gl.linkProgram(get('program', p)),
        glPixelStorei: (pname, param) => gl.pixelStorei(pname, param),
        glReadPixels(x, y, w, h, format, type, ptr) {
            const [array, offset] = pixels(type, ptr);
            gl.readPixels(x, y, w, h, format, type, array, offset);
        },
        glRenderbufferStorage: (target, format, w, h) => gl.renderbufferStorage(target, format, w, h),
        glScissor: (x, y, w, h) => gl.scissor(x, y, w, h),
        glShaderSource(s, count, stringsPtr, lengthsPtr) {
            let source = '';
            for (let i = 0; i < count; i++) {
                const ptr = i32()[(stringsPtr >> 2) + i];
                const len = lengthsPtr ? i32()[(lengthsPtr >> 2) + i] : -1;
                source += len < 0 ? string(ptr) : decoder.decode(u8().slice(ptr, ptr + len));
            }
            gl.shaderSource(get('shader', s), source);
        },
        glTexImage2D(target, level, internal, w, h, border, format, type, ptr) {
            if (!ptr) {
                gl.texImage2D(target, level, internal, w, h, border, format, type, null);
                return;
            }
            const [array, offset] = pixels(type, ptr);
            gl.texImage2D(target, level, internal, w, h, border, format, type, array, offset);
        },
        glTexParameteri: (target, pname, param) => gl.texParameteri(target, pname, param),
        glTexSubImage2D(target, level, x, y, w, h, format, type, ptr) {
            const [array, offset] = pixels(type, ptr);
            gl.texSubImage2D(target, level, x, y, w, h, format, type, array, offset);
        },
        glUniform1i: (l, x) => gl.uniform1i(get('uniform', l), x),
        glUniform2f: (l, x, y) => gl.uniform2f(get('uniform', l), x, y),
        glUniformMatrix4fv: (l, n, transpose, ptr) => gl.uniformMatrix4fv(get('uniform', l), !!transpose, f32(), ptr >> 2, n * 16),
        glUseProgram: p => gl.useProgram(get('program', p)),
        glVertexAttribDivisor: (i, divisor) => gl.vertexAttribDivisor(i, divisor),
        glVertexAttribPointer: (i, size, type, normalized, stride, offset) =>
            gl.vertexAttribPointer(i, size, type, !!normalized, stride, offset),
        glViewport: (x, y, w, h) => gl.viewport(x, y, w, h),
    };

    // With `checkGL` (test pages), a GL error stops the program, naming the
    // call: WebGL only warns about one in the console, where no test sees it.
    // Each call waits for glGetError, which is too slow for pages that ship.
    if (config.checkGL) {
        const errors = { 0x0500: 'INVALID_ENUM', 0x0501: 'INVALID_VALUE', 0x0502: 'INVALID_OPERATION',
            0x0505: 'OUT_OF_MEMORY', 0x0506: 'INVALID_FRAMEBUFFER_OPERATION', 0x9242: 'CONTEXT_LOST_WEBGL' };
        for (const [name, call] of Object.entries(glFunctions)) {
            glFunctions[name] = (...args) => {
                const result = call(...args);
                const error = gl.getError();
                if (error) throw new Error(`${name}(${args.join(', ')}): GL_${errors[error] || error}`);
                return result;
            };
        }
    }

    // A function the program imports but this file lacks fails when called,
    // with its name, rather than stopping the page from loading.
    const lenient = (module, functions, missing) => new Proxy(functions, {
        get: (target, name) => name in target ? target[name] : (...unused) => missing(module, String(name)),
    });

    // -----------------------------------------------------------------------

    function fail(error) {
        stopped = true;
        endThreads();
        printErr('tide: ' + (error && error.stack || error));
        if (config.onAbort) config.onAbort(error);
    }

    // -----------------------------------------------------------------------
    // Threads (wasi-threads). Programs import their memory, which they're built
    // to share between threads. Where the page is cross-origin isolated, it is
    // shared, and each thread the program starts (pthread_create, through
    // thread-spawn) is a worker running the same program on it. Elsewhere,
    // browsers won't share memory: the program gets memory of its own, and runs
    // on one thread (tide_web_threads says 1), with the same results.

    const canShare = typeof SharedArrayBuffer !== 'undefined' && globalThis.crossOriginIsolated === true;
    let module = null;   // The running program, compiled: its threads instantiate it too
    let shared = false;  // Its memory is shared
    let threads = [];    // Its workers
    let nextThread = 1;

    function endThreads() {
        for (const worker of threads) worker.terminate();
        threads = [];
    }

    // The memory a program imports (env.memory): its limits, and where their
    // flags are in its bytes. Read from its import section, since browsers
    // don't say.
    function memoryImport(bytes) {
        let at = 8; // After the magic number and version
        const leb = () => {
            let value = 0, scale = 1, byte;
            do {
                byte = bytes[at++];
                value += (byte & 0x7f) * scale;
                scale *= 128;
            } while (byte & 0x80);
            return value;
        };
        const name = () => {
            const length = leb();
            at += length;
            return decoder.decode(bytes.subarray(at - length, at));
        };
        while (at < bytes.length) {
            const id = bytes[at++];
            const end = leb() + at;
            if (id !== 2) { // Not the imports
                at = end;
                continue;
            }
            for (let count = leb(); count > 0; count--) {
                const from = name(), field = name(), kind = bytes[at++];
                if (kind === 0) leb(); // A function: its type
                else if (kind === 3) at += 2; // A global: its type, and whether it changes
                else if (kind === 4) { at++; leb(); } // A tag
                else { // A table's or a memory's limits, after a table's element type
                    if (kind === 1) at++;
                    const flagsAt = at, flags = bytes[at++];
                    const initial = leb(), maximum = flags & 1 ? leb() : undefined;
                    if (kind === 2 && from === 'env' && field === 'memory') {
                        return { flagsAt, shared: (flags & 2) !== 0, initial, maximum };
                    }
                }
            }
            return null;
        }
        return null;
    }

    // What each thread's worker runs: the program on the page's memory, from
    // wasi_thread_start, with WASI's output, clock and random numbers. What
    // only the page has (WebGL, the canvas, input) fails on a thread.
    function threadMain() {
        onmessage = ({ data: { module, memory, tid, startArg, origin } }) => {
            const decoder = new TextDecoder();
            const view = () => new DataView(memory.buffer);
            const u8 = () => new Uint8Array(memory.buffer);
            const wasi = {
                fd_write(fd, iovs, count, writtenPtr) {
                    let text = '', written = 0;
                    for (let i = 0; i < count; i++) {
                        const ptr = view().getUint32(iovs + i * 8, true), len = view().getUint32(iovs + i * 8 + 4, true);
                        text += decoder.decode(u8().slice(ptr, ptr + len));
                        written += len;
                    }
                    if (fd === 1 || fd === 2) postMessage({ fd, text });
                    view().setUint32(writtenPtr, written, true);
                    return fd === 1 || fd === 2 ? 0 : 8;
                },
                clock_time_get(id, precision, timePtr) {
                    const ms = id === 0 ? Date.now() : performance.timeOrigin + performance.now() - origin; // The page's clock
                    view().setBigUint64(timePtr, BigInt(Math.round(ms * 1e6)), true);
                    return 0;
                },
                random_get(ptr, len) {
                    const bytes = new Uint8Array(len);
                    for (let at = 0; at < len; at += 65536) crypto.getRandomValues(bytes.subarray(at, Math.min(len, at + 65536)));
                    u8().set(bytes, ptr);
                    return 0;
                },
                sched_yield: () => 0,
                proc_exit(code) {
                    postMessage({ exit: code });
                    throw new Error('exit');
                },
            };
            const imports = {};
            for (const { module: from, name, kind } of WebAssembly.Module.imports(module)) {
                imports[from] = imports[from] || {};
                if (kind === 'memory') imports[from][name] = memory;
                else if (from === 'wasi_snapshot_preview1') imports[from][name] = wasi[name] || (() => 52); // ENOSYS
                else imports[from][name] = () => { throw new Error(`${name} only works on the page's own thread`); };
            }
            new WebAssembly.Instance(module, imports).exports.wasi_thread_start(tid, startArg);
        };
    }
    let threadURL = null;

    // thread-spawn: a thread of the running program, as a worker. Its ID, or
    // a negative number when there's none to be had.
    function spawnThread(startArg) {
        if (!shared) return -1;
        try {
            threadURL = threadURL || URL.createObjectURL(new Blob([`(${threadMain})()`], { type: 'text/javascript' }));
            const worker = new Worker(threadURL);
            const tid = nextThread++ & 0x1fffffff;
            worker.onmessage = ({ data }) => {
                if ('exit' in data) {
                    finish(data.exit);
                } else {
                    output(data.fd, data.text);
                }
            };
            worker.onerror = event => fail(event.message || 'a thread failed');
            worker.postMessage({ module, memory, tid, startArg, origin: performance.timeOrigin });
            threads.push(worker);
            return tid;
        } catch (error) {
            printErr('tide: no thread: ' + error);
            return -1;
        }
    }
    platform.threads = () => shared ? Math.max(1, Math.min(navigator.hardwareConcurrency || 1, 32)) : 1;

    const imports = {
        wasi_snapshot_preview1: lenient('WASI', wasi, () => ENOSYS),
        wasi: { 'thread-spawn': spawnThread },
        tide: platform,
        env: lenient('env', glFunctions, (module, name) => { throw new Error(`tide.js has no ${name}`); }),
    };

    // A program and its memory: shared where the page can share it, or else
    // its own, with the program's import of it changed to match. Compiles on
    // the spot where the browser allows it (Chrome: up to 8 MB on the main
    // thread). The page has nothing else to do meanwhile, and headless tests
    // run on virtual time, which keeps running while a compile happens in the
    // background, so it can run out before main.
    async function instantiate(bytes) {
        const wanted = memoryImport(bytes);
        let made = null, isShared = false;
        if (wanted) {
            const limits = { initial: wanted.initial, maximum: wanted.maximum };
            if (wanted.shared && canShare) {
                try {
                    made = new WebAssembly.Memory({ ...limits, shared: true });
                    isShared = true;
                } catch (error) { // No room for it, as on some phones: one thread
                    printErr('tide: running on one thread: ' + error);
                }
            }
            if (!isShared && wanted.shared) {
                bytes = bytes.slice();
                bytes[wanted.flagsAt] &= ~2;
            }
            made = made || new WebAssembly.Memory(limits);
        }
        const env = lenient('env', { ...glFunctions, memory: made }, (from, name) => { throw new Error(`tide.js has no ${name}`); });
        const all = { ...imports, env };
        let compiled;
        try {
            compiled = new WebAssembly.Module(bytes);
        } catch (error) {
            if (!(error instanceof RangeError)) throw error;
            compiled = await WebAssembly.compile(bytes);
        }
        let instance;
        try {
            instance = new WebAssembly.Instance(compiled, all);
        } catch (error) {
            if (!(error instanceof RangeError)) throw error;
            instance = await WebAssembly.instantiate(compiled, all);
        }
        return { instance, module: compiled, shared: isShared };
    }

    // Runs a program's main, which returns only through exit(), or by starting
    // the frame loop.
    function begin(program) {
        endThreads();
        module = program.module;
        shared = program.shared;
        const instance = program.instance;
        exports = instance.exports;
        memory = exports.memory;
        try {
            exports._start();
        } catch (error) {
            if (error !== unwind && !(error instanceof Exit)) fail(error);
        }
    }

    // <tide run>
    // Hot reloading: tide run --web serves the page, and says which build is
    // the newest. Each new one starts in the running one's place, carrying
    // over what the running one leaves it (platform/src/reload.c).
    let resumeWith = null;
    platform.resume_size = () => resumeWith ? resumeWith.length : 0;
    platform.resume_copy = ptr => u8().set(resumeWith, ptr);

    // What the running program made in WebGL goes with it.
    function forgetGL() {
        const destroy = {
            buffer: o => gl.deleteBuffer(o), texture: o => gl.deleteTexture(o),
            framebuffer: o => gl.deleteFramebuffer(o), renderbuffer: o => gl.deleteRenderbuffer(o),
            vertexArray: o => gl.deleteVertexArray(o), program: o => gl.deleteProgram(o),
            shader: o => gl.deleteShader(o), uniform: () => {},
        };
        for (const kind in tables) {
            for (const object of tables[kind]) if (object) destroy[kind](object);
            tables[kind] = [null];
        }
        uniformIds.clear();
    }

    // Starts `bytes` in the running program's place: fresh, or going on from
    // where it is.
    async function replace(bytes, fresh) {
        const program = await instantiate(bytes);
        resumeWith = null;
        if (!fresh && exports && exports.tide_reload_save && !stopped) {
            try {
                const at = exports.tide_reload_save();
                if (at) resumeWith = u8().slice(at, at + view().getUint32(at, true));
            } catch (error) {
                printErr('tide: the running build could not hand over its state: ' + error);
            }
        }
        if (gl) forgetGL();
        closeRoom(); // The new build plays on alone, like any web game for now
        stopped = false;
        begin(program);
        resumeWith = null;
    }

    // Asks tide for the newest build, and whether to start over, 4 times a
    // second. When tide has stopped, the page stays as it is.
    if (config.reload) {
        let build = 0, restarts = 0, bytes = null;
        const poll = async () => {
            try {
                const [newest, restart] = (await (await fetch('build', { cache: 'no-store' })).text())
                    .trim().split(' ').map(Number);
                if (newest && newest !== build) {
                    const response = await fetch(`game-${newest}.wasm`, { cache: 'no-store' });
                    if (response.ok) {
                        const next = new Uint8Array(await response.arrayBuffer());
                        const fresh = !bytes;
                        build = newest;
                        restarts = restart;
                        bytes = next;
                        await replace(bytes, fresh);
                    }
                } else if (bytes && restart !== restarts) {
                    restarts = restart;
                    await replace(bytes, true);
                }
            } catch (error) {
                if (!(error instanceof TypeError)) printErr('tide: ' + (error && error.stack || error)); // TypeError: tide isn't there
            }
            setTimeout(poll, 250);
        };
        poll();
        return;
    }
    // </tide run>

    try {
        begin(await instantiate(Uint8Array.from(atob(TIDE_PROGRAM), c => c.charCodeAt(0))));
    } catch (error) {
        fail(error);
    }
})();
