// rtn run <script> [args...]: runs a "scripts" entry of the nearest package.json,
// like `npm run` / `bun run`. `rtn run` alone lists the scripts.
//
// The script runs in /bin/sh from the package's directory, with every
// node_modules/.bin up the tree and rtn's own directory first on PATH, and the
// npm_* variables tools look for. "pre<name>" and "post<name>" run around it.
(function (native, internal) {
  "use strict";

  const path = internal.path;
  const fs = internal.modules.fs;

  const color = Boolean(process.stderr.isTTY) && !process.env.NO_COLOR && process.env.TERM !== "dumb";
  const paint = (code) => (s) => (color ? `${code}${s}\x1b[0m` : s);
  const blue = paint(/truecolor|24bit/.test(process.env.COLORTERM ?? "") ? "\x1b[38;2;59;130;246m" : "\x1b[38;5;33m");
  const dim = paint("\x1b[2m");
  const bold = paint("\x1b[1m");
  const red = paint("\x1b[31m");

  function findPackage(dir) {
    for (let d = dir; ; d = path.dirname(d)) {
      const file = path.join(d, "package.json");
      if (fs.existsSync(file)) return { dir: d, file };
      if (d === "/") return null;
    }
  }

  // 'it''s' quoting for /bin/sh.
  const shellQuote = (s) => (/^[\w@%+=:,./-]+$/.test(s) ? s : `'${s.replace(/'/g, `'\\''`)}'`);

  function scriptEnv(pkg, dir, name) {
    const bins = [];
    for (let d = dir; ; d = path.dirname(d)) {
      const bin = path.join(d, "node_modules", ".bin");
      if (fs.existsSync(bin)) bins.push(bin);
      if (d === "/") break;
    }
    bins.push(path.dirname(process.execPath));  // `rtn` in a script is this rtn
    const env = { ...process.env };
    env.PATH = [...bins, env.PATH].filter(Boolean).join(":");
    env.npm_lifecycle_event = name;
    env.npm_lifecycle_script = pkg.scripts[name];
    env.npm_package_json = path.join(dir, "package.json");
    env.npm_execpath = process.execPath;
    env.npm_node_execpath = process.execPath;
    env.INIT_CWD = process.cwd();
    if (typeof pkg.name === "string") env.npm_package_name = pkg.name;
    if (typeof pkg.version === "string") env.npm_package_version = pkg.version;
    return env;
  }

  // Runs one script with the terminal attached. Returns its exit code.
  function runOne(pkg, dir, name, args) {
    const command = [pkg.scripts[name], ...args.map(shellQuote)].join(" ");
    process.stderr.write(`${dim("$")} ${bold(command)}\n`);
    const r = native.spawnSync("/bin/sh", ["sh", "-c", command], dir, Object.entries(scriptEnv(pkg, dir, name))
      .map(([k, v]) => `${k}=${v}`), ["inherit", "inherit", "inherit"], null, 0, 15, 0);
    if (r.error) {
      process.stderr.write(`${red("error:")} could not start /bin/sh: ${r.error.message}\n`);
      return 1;
    }
    if (r.signal !== null) return 128 + r.signal;
    return r.status ?? 1;
  }

  function listScripts(pkg, file) {
    const scripts = Object.entries(pkg.scripts ?? {});
    if (!scripts.length) {
      process.stdout.write(`No scripts in ${file}\n`);
      return 0;
    }
    const width = Math.max(...scripts.map(([k]) => k.length));
    process.stdout.write(`\n${blue("●")} ${bold("Scripts")} ${dim(file)}\n\n`);
    for (const [k, v] of scripts) process.stdout.write(`  ${blue(k.padEnd(width))}  ${dim(String(v))}\n`);
    process.stdout.write(`\n${dim("Run one with")} rtn run <name>\n\n`);
    return 0;
  }

  internal.runScript = async (args) => {
    const found = findPackage(process.cwd());
    if (!found) {
      process.stderr.write(`${red("error:")} no package.json in ${process.cwd()} or above\n`);
      return 1;
    }
    let pkg;
    try {
      pkg = JSON.parse(fs.readFileSync(found.file, "utf8"));
    } catch (err) {
      process.stderr.write(`${red("error:")} could not read ${found.file}: ${err.message}\n`);
      return 1;
    }
    if (args.length === 0) return listScripts(pkg, found.file);

    const [name, ...rest] = args;
    const scripts = pkg.scripts ?? {};
    if (typeof scripts[name] !== "string") {
      process.stderr.write(`${red("error:")} Script not found "${name}"\n`);
      const names = Object.keys(scripts);
      if (names.length) process.stderr.write(`${dim("Available:")} ${names.join(", ")}\n`);
      return 1;
    }
    const extra = rest[0] === "--" ? rest.slice(1) : rest;
    for (const [step, stepArgs] of [[`pre${name}`, []], [name, extra], [`post${name}`, []]]) {
      if (typeof scripts[step] !== "string") continue;
      const code = runOne(pkg, found.dir, step, stepArgs);
      if (code !== 0) return code;
    }
    return 0;
  };
});
