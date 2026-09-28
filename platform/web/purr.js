// PurrEngine's web runtime. It runs a WebAssembly program built with clang and
// wasi-libc (cmake/wasi-toolchain.cmake) in a page, and gives it:
//
// - what platform/web/purr_web.h declares: the canvas, input and the frame loop;
// - the OpenGL ES 3 functions raylib's rlgl calls, on WebGL 2;
// - the few WASI functions wasi-libc needs: output, the clock, arguments, exit.
//
// The page defines `var Purr = { canvas, print, printErr, arguments, onExit,
// onAbort }` (all optional) before this script, and PURR_PROGRAM holds the
// program as base64 (cmake/web_page.mjs, or the purr command).

(async () => {
    'use strict';
    const config = typeof Purr !== 'undefined' ? Purr : {};
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

    // A NUL-terminated string in the program's memory.
    function string(ptr) {
        const bytes = u8();
        let end = ptr;
        while (bytes[end]) end++;
        return decoder.decode(bytes.subarray(ptr, end));
    }

    // A string copied into the program's memory, kept for good (glGetString's).
    const kept = new Map();
    function keep(text) {
        if (!kept.has(text)) {
            const bytes = encoder.encode(text);
            const ptr = exports.malloc(bytes.length + 1);
            u8().set(bytes, ptr);
            u8()[ptr + bytes.length] = 0;
            kept.set(text, ptr);
        }
        return kept.get(text);
    }

    // Thrown to leave WebAssembly: purr_web_run unwinds main's stack back to the
    // browser, and exit() ends the program.
    const unwind = { purr: 'unwind' };
    class Exit {
        constructor(code) { this.code = code; }
    }

    let stopped = false;
    function finish(code) {
        stopped = true;
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
            crypto.getRandomValues(u8().subarray(ptr, ptr + len));
            return 0;
        },
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
    // The platform (purr_web.h)

    // Keys by the DOM's `code`, which names physical positions.
    const keyIndex = new Map();
    let keysHeld = new Uint8Array(0);
    // Keys whose browser default (scrolling, moving focus) would fight the game.
    const keepFromBrowser = new Set(['Space', 'Tab', 'Backspace', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
        'PageUp', 'PageDown', 'Home', 'End']);
    function onKey(event, held) {
        const index = keyIndex.get(event.code);
        if (index === undefined) return;
        keysHeld[index] = held;
        if (keepFromBrowser.has(event.code)) event.preventDefault();
    }
    addEventListener('keydown', event => onKey(event, 1));
    addEventListener('keyup', event => onKey(event, 0));
    addEventListener('blur', () => keysHeld.fill(0)); // Keys released elsewhere never send keyup

    // The mouse over the canvas, in canvas pixels. Buttons in raylib's order:
    // left, right, middle, back, forward (the DOM's is left, middle, right, ...).
    const mouse = { x: 0, y: 0, buttons: 0, wheelX: 0, wheelY: 0 };
    const buttonBit = [1, 4, 2, 8, 16];
    function onMouseMove(event) {
        const rect = canvas.getBoundingClientRect();
        mouse.x = (event.clientX - rect.left) * canvas.width / (rect.width || 1);
        mouse.y = (event.clientY - rect.top) * canvas.height / (rect.height || 1);
    }
    canvas.addEventListener('mousemove', onMouseMove);
    canvas.addEventListener('mousedown', event => {
        onMouseMove(event);
        mouse.buttons |= buttonBit[event.button] || 0;
        canvas.focus();
    });
    addEventListener('mouseup', event => { mouse.buttons &= ~(buttonBit[event.button] || 0); });
    canvas.addEventListener('wheel', event => {
        mouse.wheelX -= Math.sign(event.deltaX);
        mouse.wheelY -= Math.sign(event.deltaY); // Positive away from the user
        event.preventDefault();
    }, { passive: false });

    let timerFrames = false;
    function scheduleFrame() {
        if (timerFrames) setTimeout(frame, 0);
        else requestAnimationFrame(frame);
    }
    function frame() {
        if (stopped) return;
        try {
            exports.purr_web_frame();
        } catch (error) {
            if (error instanceof Exit) return;
            fail(error);
            return;
        }
        scheduleFrame();
    }

    function fitToPage() {
        canvas.width = innerWidth;
        canvas.height = innerHeight;
    }

    const platform = {
        init_canvas(width, height, resizable) {
            if (resizable) {
                fitToPage();
                addEventListener('resize', fitToPage);
            } else {
                canvas.width = width;
                canvas.height = height;
            }
            gl = canvas.getContext('webgl2', { alpha: false, antialias: false, depth: true, stencil: false });
            if (!gl) return 0;
            // WebGL only turns on extensions it's asked for; rlgl asks through
            // glGetStringi, so turn them all on.
            for (const name of gl.getSupportedExtensions() || []) gl.getExtension(name);
            return 1;
        },
        canvas_width: () => canvas.width,
        canvas_height: () => canvas.height,
        mouse_x: () => mouse.x,
        mouse_y: () => mouse.y,
        mouse_buttons: () => mouse.buttons,
        take_wheel_x() { const v = mouse.wheelX; mouse.wheelX = 0; return v; },
        take_wheel_y() { const v = mouse.wheelY; mouse.wheelY = 0; return v; },
        watch_key(index, codePtr) {
            keyIndex.set(string(codePtr), index);
            if (index >= keysHeld.length) {
                const grown = new Uint8Array(index + 1);
                grown.set(keysHeld);
                keysHeld = grown;
            }
        },
        key_held: index => keysHeld[index] || 0,
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
            scheduleFrame();
            throw unwind;
        },
        stop() { stopped = true; },
        eval(scriptPtr) { (0, eval)(string(scriptPtr)); },
    };

    // -----------------------------------------------------------------------
    // OpenGL ES 3 on WebGL 2: what rlgl calls. GL names objects with numbers,
    // WebGL with objects, so each kind has a table from number to object.

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
    const idOf = object => {
        for (const kind in tables) {
            const id = tables[kind].indexOf(object);
            if (id > 0) return id;
        }
        return 0;
    };

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
        case 0x1403: case 0x8363: case 0x8033: case 0x8034: case 0x140B: // UNSIGNED_SHORT, 5_6_5, 4_4_4_4, 5_5_5_1, HALF_FLOAT
            return [new Uint16Array(memory.buffer), ptr >> 1];
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

    function writeParameter(value, ptr, float) {
        const values = value === null || value === undefined ? [0]
            : typeof value === 'object' && 'length' in value ? Array.from(value)
            : typeof value === 'object' ? [idOf(value)]
            : [Number(value)];
        values.forEach((v, i) => {
            if (float) f32()[(ptr >> 2) + i] = v;
            else i32()[(ptr >> 2) + i] = v;
        });
    }

    const GL_EXTENSIONS = 0x1F03, GL_NUM_EXTENSIONS = 0x821D, INFO_LOG_LENGTH = 0x8B84;
    const extensions = () => (gl.getSupportedExtensions() || []).map(name => 'GL_' + name);

    const glFunctions = {
        glActiveTexture: t => gl.activeTexture(t),
        glAttachShader: (p, s) => gl.attachShader(get('program', p), get('shader', s)),
        glBindAttribLocation: (p, index, name) => gl.bindAttribLocation(get('program', p), index, string(name)),
        glBindBuffer: (target, b) => gl.bindBuffer(target, get('buffer', b)),
        glBindFramebuffer: (target, f) => gl.bindFramebuffer(target, get('framebuffer', f)),
        glBindRenderbuffer: (target, r) => gl.bindRenderbuffer(target, get('renderbuffer', r)),
        glBindTexture: (target, t) => gl.bindTexture(target, get('texture', t)),
        glBindVertexArray: v => gl.bindVertexArray(get('vertexArray', v)),
        glBlendColor: (r, g, b, a) => gl.blendColor(r, g, b, a),
        glBlendEquation: mode => gl.blendEquation(mode),
        glBlendEquationSeparate: (rgb, alpha) => gl.blendEquationSeparate(rgb, alpha),
        glBlendFunc: (s, d) => gl.blendFunc(s, d),
        glBlendFuncSeparate: (sr, dr, sa, da) => gl.blendFuncSeparate(sr, dr, sa, da),
        glBlitFramebuffer: (sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter) =>
            gl.blitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter),
        glBufferData(target, size, ptr, usage) {
            if (ptr) gl.bufferData(target, u8(), usage, ptr, size);
            else gl.bufferData(target, size, usage);
        },
        glBufferSubData: (target, offset, size, ptr) => gl.bufferSubData(target, offset, u8(), ptr, size),
        glCheckFramebufferStatus: target => gl.checkFramebufferStatus(target),
        glClear: mask => gl.clear(mask),
        glClearColor: (r, g, b, a) => gl.clearColor(r, g, b, a),
        glClearDepthf: d => gl.clearDepth(d),
        glColorMask: (r, g, b, a) => gl.colorMask(!!r, !!g, !!b, !!a),
        glCompileShader: s => gl.compileShader(get('shader', s)),
        glCompressedTexImage2D: (target, level, format, w, h, border, size, ptr) =>
            gl.compressedTexImage2D(target, level, format, w, h, border, u8(), ptr, size),
        glCreateProgram: () => add('program', gl.createProgram()),
        glCreateShader: type => add('shader', gl.createShader(type)),
        glCullFace: mode => gl.cullFace(mode),
        glDeleteBuffers: (n, ptr) => remove('buffer', o => gl.deleteBuffer(o), n, ptr),
        glDeleteFramebuffers: (n, ptr) => remove('framebuffer', o => gl.deleteFramebuffer(o), n, ptr),
        glDeleteProgram(p) {
            gl.deleteProgram(get('program', p));
            tables.program[p] = null;
        },
        glDeleteRenderbuffers: (n, ptr) => remove('renderbuffer', o => gl.deleteRenderbuffer(o), n, ptr),
        glDeleteShader(s) {
            gl.deleteShader(get('shader', s));
            tables.shader[s] = null;
        },
        glDeleteTextures: (n, ptr) => remove('texture', o => gl.deleteTexture(o), n, ptr),
        glDeleteVertexArrays: (n, ptr) => remove('vertexArray', o => gl.deleteVertexArray(o), n, ptr),
        glDepthFunc: f => gl.depthFunc(f),
        glDepthMask: flag => gl.depthMask(!!flag),
        glDetachShader: (p, s) => gl.detachShader(get('program', p), get('shader', s)),
        glDisable: cap => gl.disable(cap),
        glDisableVertexAttribArray: i => gl.disableVertexAttribArray(i),
        glDrawArrays: (mode, first, count) => gl.drawArrays(mode, first, count),
        glDrawArraysInstanced: (mode, first, count, instances) => gl.drawArraysInstanced(mode, first, count, instances),
        glDrawBuffers(n, ptr) {
            gl.drawBuffers(Array.from(i32().subarray(ptr >> 2, (ptr >> 2) + n)));
        },
        glDrawElements: (mode, count, type, offset) => gl.drawElements(mode, count, type, offset),
        glDrawElementsInstanced: (mode, count, type, offset, instances) =>
            gl.drawElementsInstanced(mode, count, type, offset, instances),
        glEnable: cap => gl.enable(cap),
        glEnableVertexAttribArray: i => gl.enableVertexAttribArray(i),
        glFinish: () => gl.finish(),
        glFlush: () => gl.flush(),
        glFramebufferRenderbuffer: (target, attachment, rbTarget, r) =>
            gl.framebufferRenderbuffer(target, attachment, rbTarget, get('renderbuffer', r)),
        glFramebufferTexture2D: (target, attachment, texTarget, t, level) =>
            gl.framebufferTexture2D(target, attachment, texTarget, get('texture', t), level),
        glFrontFace: mode => gl.frontFace(mode),
        glGenBuffers: (n, ptr) => generate('buffer', () => gl.createBuffer(), n, ptr),
        glGenFramebuffers: (n, ptr) => generate('framebuffer', () => gl.createFramebuffer(), n, ptr),
        glGenRenderbuffers: (n, ptr) => generate('renderbuffer', () => gl.createRenderbuffer(), n, ptr),
        glGenTextures: (n, ptr) => generate('texture', () => gl.createTexture(), n, ptr),
        glGenVertexArrays: (n, ptr) => generate('vertexArray', () => gl.createVertexArray(), n, ptr),
        glGenerateMipmap: target => gl.generateMipmap(target),
        glGetAttribLocation: (p, name) => gl.getAttribLocation(get('program', p), string(name)),
        glGetError: () => gl.getError(),
        glGetFloatv: (pname, ptr) => writeParameter(gl.getParameter(pname), ptr, true),
        glGetFramebufferAttachmentParameteriv: (target, attachment, pname, ptr) =>
            writeParameter(gl.getFramebufferAttachmentParameter(target, attachment, pname), ptr, false),
        glGetIntegerv(pname, ptr) {
            writeParameter(pname === GL_NUM_EXTENSIONS ? extensions().length : gl.getParameter(pname), ptr, false);
        },
        glGetProgramInfoLog: (p, max, lengthPtr, ptr) => writeLog(gl.getProgramInfoLog(get('program', p)), max, lengthPtr, ptr),
        glGetProgramiv(p, pname, ptr) {
            const program = get('program', p);
            const value = pname === INFO_LOG_LENGTH ? (gl.getProgramInfoLog(program) || '').length + 1
                : gl.getProgramParameter(program, pname);
            writeParameter(value === true ? 1 : value === false ? 0 : value, ptr, false);
        },
        glGetShaderInfoLog: (s, max, lengthPtr, ptr) => writeLog(gl.getShaderInfoLog(get('shader', s)), max, lengthPtr, ptr),
        glGetShaderiv(s, pname, ptr) {
            const shader = get('shader', s);
            const value = pname === INFO_LOG_LENGTH ? (gl.getShaderInfoLog(shader) || '').length + 1
                : gl.getShaderParameter(shader, pname);
            writeParameter(value === true ? 1 : value === false ? 0 : value, ptr, false);
        },
        glGetString: name => keep(name === GL_EXTENSIONS ? extensions().join(' ') : String(gl.getParameter(name) || '')),
        glGetStringi: (name, index) => keep(extensions()[index] || ''),
        glGetUniformLocation: (p, name) => uniformId(p, string(name)),
        glHint: (target, mode) => gl.hint(target, mode),
        glLineWidth: w => gl.lineWidth(w),
        glLinkProgram: p => gl.linkProgram(get('program', p)),
        glPixelStorei: (pname, param) => gl.pixelStorei(pname, param),
        glPolygonOffset: (factor, units) => gl.polygonOffset(factor, units),
        glReadBuffer: mode => gl.readBuffer(mode),
        glReadPixels(x, y, w, h, format, type, ptr) {
            const [array, offset] = pixels(type, ptr);
            gl.readPixels(x, y, w, h, format, type, array, offset);
        },
        glRenderbufferStorage: (target, format, w, h) => gl.renderbufferStorage(target, format, w, h),
        glRenderbufferStorageMultisample: (target, samples, format, w, h) =>
            gl.renderbufferStorageMultisample(target, samples, format, w, h),
        glScissor: (x, y, w, h) => gl.scissor(x, y, w, h),
        glShaderSource(s, count, stringsPtr, lengthsPtr) {
            let source = '';
            for (let i = 0; i < count; i++) {
                const ptr = i32()[(stringsPtr >> 2) + i];
                const len = lengthsPtr ? i32()[(lengthsPtr >> 2) + i] : -1;
                source += len < 0 ? string(ptr) : decoder.decode(u8().subarray(ptr, ptr + len));
            }
            gl.shaderSource(get('shader', s), source);
        },
        glStencilFunc: (func, ref, mask) => gl.stencilFunc(func, ref, mask),
        glStencilMask: mask => gl.stencilMask(mask),
        glStencilOp: (fail, zfail, zpass) => gl.stencilOp(fail, zfail, zpass),
        glTexImage2D(target, level, internal, w, h, border, format, type, ptr) {
            if (!ptr) {
                gl.texImage2D(target, level, internal, w, h, border, format, type, null);
                return;
            }
            const [array, offset] = pixels(type, ptr);
            gl.texImage2D(target, level, internal, w, h, border, format, type, array, offset);
        },
        glTexParameterf: (target, pname, param) => gl.texParameterf(target, pname, param),
        glTexParameteri: (target, pname, param) => gl.texParameteri(target, pname, param),
        glTexSubImage2D(target, level, x, y, w, h, format, type, ptr) {
            const [array, offset] = pixels(type, ptr);
            gl.texSubImage2D(target, level, x, y, w, h, format, type, array, offset);
        },
        glUniform1f: (l, x) => gl.uniform1f(get('uniform', l), x),
        glUniform1fv: (l, n, ptr) => gl.uniform1fv(get('uniform', l), f32(), ptr >> 2, n),
        glUniform1i: (l, x) => gl.uniform1i(get('uniform', l), x),
        glUniform1iv: (l, n, ptr) => gl.uniform1iv(get('uniform', l), i32(), ptr >> 2, n),
        glUniform2f: (l, x, y) => gl.uniform2f(get('uniform', l), x, y),
        glUniform2fv: (l, n, ptr) => gl.uniform2fv(get('uniform', l), f32(), ptr >> 2, n * 2),
        glUniform2i: (l, x, y) => gl.uniform2i(get('uniform', l), x, y),
        glUniform2iv: (l, n, ptr) => gl.uniform2iv(get('uniform', l), i32(), ptr >> 2, n * 2),
        glUniform3f: (l, x, y, z) => gl.uniform3f(get('uniform', l), x, y, z),
        glUniform3fv: (l, n, ptr) => gl.uniform3fv(get('uniform', l), f32(), ptr >> 2, n * 3),
        glUniform3i: (l, x, y, z) => gl.uniform3i(get('uniform', l), x, y, z),
        glUniform3iv: (l, n, ptr) => gl.uniform3iv(get('uniform', l), i32(), ptr >> 2, n * 3),
        glUniform4f: (l, x, y, z, w) => gl.uniform4f(get('uniform', l), x, y, z, w),
        glUniform4fv: (l, n, ptr) => gl.uniform4fv(get('uniform', l), f32(), ptr >> 2, n * 4),
        glUniform4i: (l, x, y, z, w) => gl.uniform4i(get('uniform', l), x, y, z, w),
        glUniform4iv: (l, n, ptr) => gl.uniform4iv(get('uniform', l), i32(), ptr >> 2, n * 4),
        glUniformMatrix4fv: (l, n, transpose, ptr) => gl.uniformMatrix4fv(get('uniform', l), !!transpose, f32(), ptr >> 2, n * 16),
        glUseProgram: p => gl.useProgram(get('program', p)),
        glVertexAttrib1fv: (i, ptr) => gl.vertexAttrib1fv(i, f32(), ptr >> 2),
        glVertexAttrib2fv: (i, ptr) => gl.vertexAttrib2fv(i, f32(), ptr >> 2),
        glVertexAttrib3fv: (i, ptr) => gl.vertexAttrib3fv(i, f32(), ptr >> 2),
        glVertexAttrib4fv: (i, ptr) => gl.vertexAttrib4fv(i, f32(), ptr >> 2),
        glVertexAttribDivisor: (i, divisor) => gl.vertexAttribDivisor(i, divisor),
        glVertexAttribPointer: (i, size, type, normalized, stride, offset) =>
            gl.vertexAttribPointer(i, size, type, !!normalized, stride, offset),
        glViewport: (x, y, w, h) => gl.viewport(x, y, w, h),
    };

    // A function the program imports but this file lacks fails when called,
    // with its name, rather than stopping the page from loading.
    const lenient = (module, functions, missing) => new Proxy(functions, {
        get: (target, name) => name in target ? target[name] : (...unused) => missing(module, String(name)),
    });

    // -----------------------------------------------------------------------

    function fail(error) {
        stopped = true;
        printErr('purr: ' + (error && error.stack || error));
        if (config.onAbort) config.onAbort(error);
    }

    try {
        const bytes = Uint8Array.from(atob(PURR_PROGRAM), c => c.charCodeAt(0));
        const { instance } = await WebAssembly.instantiate(bytes, {
            wasi_snapshot_preview1: lenient('WASI', wasi, () => ENOSYS),
            purr: platform,
            env: lenient('env', glFunctions, (module, name) => { throw new Error(`purr.js has no ${name}`); }),
        });
        exports = instance.exports;
        memory = exports.memory;
        exports._start(); // main; returns only through exit(), or by starting the frame loop
    } catch (error) {
        if (error !== unwind && !(error instanceof Exit)) fail(error);
    }
})();
