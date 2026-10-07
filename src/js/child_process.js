// node:child_process — spawn, exec, execFile and their Sync versions.
// The processes, pipes and exit tracking live in src/bindings/child_process.cpp.
(function (native, internal) {
  "use strict";

  const EventEmitter = internal.modules.events;
  const { promisify } = internal.modules.util;

  const SIGNALS = {
    SIGHUP: 1, SIGINT: 2, SIGQUIT: 3, SIGILL: 4, SIGTRAP: 5, SIGABRT: 6, SIGBUS: 7, SIGFPE: 8, SIGKILL: 9,
    SIGUSR1: 10, SIGSEGV: 11, SIGUSR2: 12, SIGPIPE: 13, SIGALRM: 14, SIGTERM: 15, SIGSTKFLT: 16, SIGCHLD: 17,
    SIGCONT: 18, SIGSTOP: 19, SIGTSTP: 20, SIGTTIN: 21, SIGTTOU: 22, SIGURG: 23, SIGXCPU: 24, SIGXFSZ: 25,
    SIGVTALRM: 26, SIGPROF: 27, SIGWINCH: 28, SIGIO: 29, SIGPWR: 30, SIGSYS: 31,
  };
  const SIGNAL_NAMES = Object.fromEntries(Object.entries(SIGNALS).map(([k, v]) => [v, k]));

  function codedError(Ctor, message, code) {
    const e = new Ctor(message);
    e.code = code;
    return e;
  }

  function signalNumber(signal) {
    if (signal === undefined || signal === null) return SIGNALS.SIGTERM;
    if (typeof signal === "number") return signal;
    const n = SIGNALS[String(signal).toUpperCase()];
    if (n === undefined) throw codedError(TypeError, `Unknown signal: ${signal}`, "ERR_UNKNOWN_SIGNAL");
    return n;
  }

  function validateString(value, name) {
    if (typeof value !== "string") {
      throw codedError(TypeError, `The "${name}" argument must be of type string. Received ${value === null ? "null" : typeof value}`,
        "ERR_INVALID_ARG_TYPE");
    }
    if (value.length === 0) {
      throw codedError(TypeError, `The argument '${name}' cannot be empty. Received ''`, "ERR_INVALID_ARG_VALUE");
    }
  }

  // Node's "spawn ls ENOENT" error.
  function spawnError(info, file, args, syscall = "spawn") {
    const err = new Error(`${syscall} ${file} ${info.code}`);
    err.errno = info.errno;
    err.code = info.code;
    err.syscall = `${syscall} ${file}`;
    err.path = file;
    err.spawnargs = args;
    return err;
  }

  function envList(env) {
    const out = [];
    for (const [k, v] of Object.entries(env ?? process.env)) {
      if (v !== undefined) out.push(`${k}=${v}`);
    }
    return out;
  }

  // "pipe" / "inherit" / "ignore" for fds 0..2 (numbers 0-2 mean inherit, like Node's [0, 1, 2]).
  function normalizeStdio(stdio) {
    if (stdio === undefined || stdio === null) stdio = "pipe";
    if (typeof stdio === "string") stdio = [stdio, stdio, stdio];
    const out = [];
    for (let i = 0; i < 3; i++) {
      let s = stdio[i] ?? "pipe";
      if (s === "overlapped") s = "pipe";
      if (s === i || s === process.stdin || s === process.stdout || s === process.stderr) s = "inherit";
      if (s !== "pipe" && s !== "inherit" && s !== "ignore") {
        throw codedError(TypeError, `The value "${String(s)}" is invalid for option "stdio"`, "ERR_INVALID_ARG_VALUE");
      }
      out.push(s);
    }
    return out;
  }

  // file, args, options -> what the native side needs (with shell: true, run through /bin/sh -c).
  function normalizeSpawnArgs(file, args, options) {
    validateString(file, "file");
    if (args !== undefined && args !== null && !Array.isArray(args)) {
      if (typeof args === "object") {
        options = args;
        args = [];
      } else {
        throw codedError(TypeError, 'The "args" argument must be an instance of Array', "ERR_INVALID_ARG_TYPE");
      }
    }
    args = (args ?? []).map(String);
    options = { ...(options ?? {}) };
    if (options.shell) {
      const command = [file, ...args].join(" ");
      file = typeof options.shell === "string" ? options.shell : "/bin/sh";
      args = ["-c", command];
    }
    const cwd = options.cwd instanceof URL ? internal.modules.url.fileURLToPath(options.cwd) : options.cwd;
    return {
      file,
      args: [options.argv0 ?? file, ...args],
      cwd: cwd === undefined || cwd === null ? null : String(cwd),
      env: envList(options.env),
      options,
    };
  }

  // ------------------------------------------------------------------
  // Streams: just enough of Readable / Writable for the usual code
  // ------------------------------------------------------------------

  class ChildReadable extends EventEmitter {
    #encoding = null;
    #paused = false;
    #queue = [];
    #ended = false;
    readable = true;
    readableEnded = false;

    setEncoding(encoding) {
      this.#encoding = encoding;
      return this;
    }

    pause() {
      this.#paused = true;
      return this;
    }

    resume() {
      this.#paused = false;
      while (this.#queue.length && !this.#paused) this.#emitData(this.#queue.shift());
      if (!this.#queue.length && this.#ended) this.#finish();
      return this;
    }

    isPaused() { return this.#paused; }

    pipe(dest, { end = true } = {}) {
      this.on("data", (chunk) => dest.write(chunk));
      if (end && dest !== process.stdout && dest !== process.stderr) this.on("end", () => dest.end?.());
      return dest;
    }

    destroy() {
      this.readable = false;
      return this;
    }

    async *[Symbol.asyncIterator]() {
      const chunks = [];
      let done = false;
      let wake = null;
      this.on("data", (c) => { chunks.push(c); wake?.(); });
      this.on("end", () => { done = true; wake?.(); });
      while (true) {
        if (chunks.length) {
          yield chunks.shift();
        } else if (done) {
          return;
        } else {
          await new Promise((r) => (wake = r));
          wake = null;
        }
      }
    }

    _push(bytes) {
      const chunk = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      if (this.#paused) this.#queue.push(chunk);
      else this.#emitData(chunk);
    }

    _end() {
      this.#ended = true;
      if (!this.#paused || !this.#queue.length) this.#finish();
    }

    #emitData(chunk) {
      this.emit("data", this.#encoding ? chunk.toString(this.#encoding) : chunk);
    }

    #finish() {
      if (this.readableEnded) return;
      this.readable = false;
      this.readableEnded = true;
      this.emit("end");
      this.emit("close");
    }
  }

  class ChildWritable extends EventEmitter {
    #id;
    writable = true;
    writableEnded = false;

    constructor(id) {
      super();
      this.#id = id;
    }

    write(chunk, encoding, callback) {
      if (typeof encoding === "function") [callback, encoding] = [encoding, undefined];
      if (this.writableEnded) {
        const err = codedError(Error, "write after end", "ERR_STREAM_WRITE_AFTER_END");
        process.nextTick(() => (callback ? callback(err) : this.emit("error", err)));
        return false;
      }
      const bytes = typeof chunk === "string" ? Buffer.from(chunk, encoding) : chunk;
      native.childWrite(this.#id, bytes);
      if (callback) process.nextTick(callback);
      return true;
    }

    end(chunk, encoding, callback) {
      if (typeof chunk === "function") [callback, chunk] = [chunk, undefined];
      if (typeof encoding === "function") [callback, encoding] = [encoding, undefined];
      if (this.writableEnded) return this;
      if (chunk !== undefined && chunk !== null) this.write(chunk, encoding);
      this.writableEnded = true;
      this.writable = false;
      native.childEnd(this.#id);
      process.nextTick(() => {
        this.emit("finish");
        this.emit("close");
        callback?.();
      });
      return this;
    }

    destroy() {
      return this.end();
    }
  }

  // ------------------------------------------------------------------
  // ChildProcess
  // ------------------------------------------------------------------

  class ChildProcess extends EventEmitter {
    #id = 0;
    #openStreams = 0;
    #exited = false;
    pid = undefined;
    exitCode = null;
    signalCode = null;
    killed = false;
    connected = false;
    stdin = null;
    stdout = null;
    stderr = null;
    stdio = [null, null, null];
    spawnfile = "";
    spawnargs = [];

    _spawn({ file, args, cwd, env, options }) {
      this.spawnfile = file;
      this.spawnargs = args;
      const stdio = normalizeStdio(options.stdio);
      const streams = [null, null, null];
      for (let i = 1; i < 3; i++) {
        if (stdio[i] !== "pipe") continue;
        streams[i] = new ChildReadable();
        this.#openStreams++;
      }
      const result = native.spawnChild(file, args, cwd, env, stdio, (kind, a, b) => {
        // A throwing 'data' / 'exit' listener is an uncaught error, as in Node.
        try {
          this.#onEvent(kind, a, b, streams);
        } catch (err) {
          queueMicrotask(() => { throw err; });
        }
      });
      if (result.errno !== undefined) {
        const err = spawnError(result, file, args.slice(1));
        for (const s of streams) s?._end();
        this.#exited = true;
        this.exitCode = result.errno;
        process.nextTick(() => {
          this.emit("error", err);
          this.emit("close", result.errno, null);
        });
        return;
      }
      this.#id = result.id;
      this.pid = result.pid;
      if (stdio[0] === "pipe") streams[0] = new ChildWritable(result.id);
      [this.stdin, this.stdout, this.stderr] = streams;
      this.stdio = streams;
      process.nextTick(() => this.emit("spawn"));
      if (options.signal) {
        const signal = options.signal;
        const onAbort = () => {
          if (this.kill(options.killSignal)) {
            const err = codedError(Error, "The operation was aborted", "ABORT_ERR");
            err.name = "AbortError";
            err.cause = signal.reason;
            this.emit("error", err);
          }
        };
        if (signal.aborted) process.nextTick(onAbort);
        else signal.addEventListener("abort", onAbort, { once: true });
      }
      if (options.timeout > 0) {
        const timer = setTimeout(() => this.kill(options.killSignal), options.timeout);
        this.once("exit", () => clearTimeout(timer));
      }
    }

    #onEvent(kind, a, b, streams) {
      if (kind === 0) {
        streams[a]?._push(b);
      } else if (kind === 1) {
        streams[a]?._end();
        this.#openStreams--;
        this.#maybeClose();
      } else {
        this.#exited = true;
        this.exitCode = a;
        this.signalCode = b === null ? null : SIGNAL_NAMES[b] ?? b;
        this.emit("exit", this.exitCode, this.signalCode);
        this.#maybeClose();
      }
    }

    #maybeClose() {
      if (!this.#exited || this.#openStreams > 0 || this._closed) return;
      this._closed = true;
      // After the streams' own 'end' / 'close', like Node.
      process.nextTick(() => this.emit("close", this.exitCode, this.signalCode));
    }

    kill(signal = "SIGTERM") {
      if (!this.#id || this.#exited) return false;
      const sent = native.childKill(this.#id, signalNumber(signal));
      if (sent) this.killed = true;
      return sent;
    }

    ref() {}
    unref() {}
    disconnect() {}
    send() { throw codedError(Error, "IPC channels are not supported by rtn", "ERR_IPC_CHANNEL_CLOSED"); }

    get [Symbol.toStringTag]() { return "ChildProcess"; }
  }

  function spawn(file, args, options) {
    const plan = normalizeSpawnArgs(file, args, options);
    const child = new ChildProcess();
    child._spawn(plan);
    return child;
  }

  // ------------------------------------------------------------------
  // exec / execFile: buffered output and a callback
  // ------------------------------------------------------------------

  const DEFAULT_MAX_BUFFER = 1024 * 1024;

  function execFile(file, args, options, callback) {
    if (typeof args === "function") [callback, args, options] = [args, [], {}];
    else if (typeof options === "function") [callback, options] = [options, undefined];
    if (args && !Array.isArray(args) && typeof args === "object") [options, args] = [args, []];
    options = { encoding: "utf8", maxBuffer: DEFAULT_MAX_BUFFER, killSignal: "SIGTERM", ...(options ?? {}) };
    const child = spawn(file, args ?? [], { ...options, stdio: "pipe" });
    const out = { stdout: [], stderr: [] };
    const sizes = { stdout: 0, stderr: 0 };
    let failure = null;
    const encoding = options.encoding === "buffer" || !Buffer.isEncoding(options.encoding) ? null : options.encoding;
    const decode = (name) => {
      const buf = Buffer.concat(out[name]);
      return encoding ? buf.toString(encoding) : buf;
    };
    for (const name of ["stdout", "stderr"]) {
      child[name]?.on("data", (chunk) => {
        sizes[name] += chunk.length;
        if (sizes[name] > options.maxBuffer) {
          if (!failure) {
            failure = codedError(RangeError, `${name} maxBuffer length exceeded`, "ERR_CHILD_PROCESS_STDIO_MAXBUFFER");
            out[name].push(chunk.subarray(0, chunk.length - (sizes[name] - options.maxBuffer)));
            child.kill(options.killSignal);
          }
          return;
        }
        out[name].push(chunk);
      });
    }
    const cmd = [file, ...(args ?? [])].join(" ");
    let done = false;
    const finish = (err) => {
      if (done) return;
      done = true;
      const stdout = decode("stdout");
      const stderr = decode("stderr");
      if (err) {
        err.cmd ??= cmd;
        err.stdout = stdout;
        err.stderr = stderr;
      }
      callback?.(err, stdout, stderr);
    };
    child.on("error", (err) => finish(err));
    child.on("close", (code, signal) => {
      if (failure) return finish(failure);
      if (code === 0 && signal === null) return finish(null);
      const text = decode("stderr");
      const err = new Error(`Command failed: ${cmd}${text ? `\n${text}` : ""}`);
      err.code = code;
      err.killed = child.killed;
      err.signal = signal;
      err.cmd = cmd;
      finish(err);
    });
    return child;
  }

  function exec(command, options, callback) {
    if (typeof options === "function") [callback, options] = [options, undefined];
    options = { ...(options ?? {}) };
    const shell = typeof options.shell === "string" ? options.shell : "/bin/sh";
    delete options.shell;
    const child = execFile(shell, ["-c", command], options, (err, stdout, stderr) => {
      if (err) err.cmd = command;
      callback?.(err, stdout, stderr);
    });
    return child;
  }

  // util.promisify(exec)(cmd) -> { stdout, stderr }; the error carries them too.
  function promisifyExec(run) {
    return (...args) => {
      let child;
      const promise = new Promise((resolve, reject) => {
        child = run(...args, (err, stdout, stderr) => (err ? reject(err) : resolve({ stdout, stderr })));
      });
      promise.child = child;
      return promise;
    };
  }
  Object.defineProperty(exec, promisify.custom, { value: promisifyExec(exec), enumerable: false });
  Object.defineProperty(execFile, promisify.custom, { value: promisifyExec(execFile), enumerable: false });

  // ------------------------------------------------------------------
  // Synchronous versions
  // ------------------------------------------------------------------

  function spawnSync(file, args, options) {
    const { file: f, args: argv, cwd, env, options: opts } = normalizeSpawnArgs(file, args, options);
    const stdio = normalizeStdio(opts.stdio);
    let input = opts.input ?? null;
    if (typeof input === "string") input = Buffer.from(input, opts.encoding && opts.encoding !== "buffer" ? opts.encoding : "utf8");
    else if (input !== null && !(input instanceof Uint8Array)) {
      input = input instanceof DataView ? new Uint8Array(input.buffer, input.byteOffset, input.byteLength) : Buffer.from(input);
    }
    const maxBuffer = opts.maxBuffer === undefined ? DEFAULT_MAX_BUFFER : opts.maxBuffer === Infinity ? 0 : opts.maxBuffer;
    const r = native.spawnSync(f, argv, cwd, env, stdio, stdio[0] === "pipe" ? input : null,
      opts.timeout > 0 ? opts.timeout : 0, signalNumber(opts.killSignal), maxBuffer);
    const encoding = opts.encoding && opts.encoding !== "buffer" ? opts.encoding : null;
    const wrap = (bytes) => {
      if (bytes === undefined) return null;
      const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      return encoding ? buf.toString(encoding) : buf;
    };
    const result = {
      pid: r.pid ?? 0,
      output: [null, wrap(r.stdout), wrap(r.stderr)],
      stdout: wrap(r.stdout),
      stderr: wrap(r.stderr),
      status: r.status ?? null,
      signal: r.signal === null || r.signal === undefined ? null : SIGNAL_NAMES[r.signal] ?? r.signal,
    };
    if (r.error) {
      result.error = r.error.code === "ENOENT" || r.pid === undefined
        ? spawnError(r.error, f, argv.slice(1), "spawnSync")
        : codedError(Error, `spawnSync ${f} ${r.error.code}`, r.error.code);
      result.error.errno ??= r.error.errno;
      result.error.syscall ??= `spawnSync ${f}`;
      result.error.path ??= f;
      result.error.spawnargs ??= argv.slice(1);
      if (r.pid === undefined) result.status = null;
    }
    return result;
  }

  function checkSync(result, cmd, stdio) {
    if (result.error) {
      result.error.stdout = result.stdout;
      result.error.stderr = result.stderr;
      throw result.error;
    }
    if (result.status !== 0 || result.signal !== null) {
      const text = result.stderr ? result.stderr.toString() : "";
      const err = new Error(`Command failed: ${cmd}${text ? `\n${text}` : ""}`);
      Object.assign(err, result);
      throw err;
    }
    return result.stdout;
  }

  // stderr goes to our own stderr unless `stdio` says otherwise (as in Node).
  function execFileSync(file, args, options) {
    if (args && !Array.isArray(args) && typeof args === "object") [options, args] = [args, []];
    options = { ...(options ?? {}) };
    options.stdio ??= ["pipe", "pipe", "inherit"];
    const result = spawnSync(file, args ?? [], options);
    return checkSync(result, [file, ...(args ?? [])].join(" "), options.stdio);
  }

  function execSync(command, options) {
    options = { ...(options ?? {}) };
    const shell = typeof options.shell === "string" ? options.shell : "/bin/sh";
    delete options.shell;
    options.stdio ??= ["pipe", "pipe", "inherit"];
    const result = spawnSync(shell, ["-c", command], options);
    return checkSync(result, command, options.stdio);
  }

  function fork() {
    throw codedError(Error, "child_process.fork() is not supported by rtn (no IPC channel); use spawn(process.execPath, [file])",
      "ERR_NOT_SUPPORTED");
  }

  internal.modules.child_process = {
    ChildProcess,
    spawn,
    exec,
    execFile,
    spawnSync,
    execSync,
    execFileSync,
    fork,
  };
  internal.signals = SIGNALS;
});
