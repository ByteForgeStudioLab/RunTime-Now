// rtn init [dir] [--ts|--js]: a new TypeScript or JavaScript project, ready for
// `rtn index.ts` (or index.js) and `rtn test`. The interactive language prompt lives
// in main.cpp; it passes the answer here as --ts / --js. TypeScript is the default.
(function (native, internal) {
  "use strict";

  const path = internal.path;
  const fs = internal.modules.fs;

  const color = Boolean(process.stdout.isTTY) && !process.env.NO_COLOR && process.env.TERM !== "dumb";
  const paint = (code) => (s) => (color ? `${code}${s}\x1b[0m` : s);
  const blue = paint(/truecolor|24bit/.test(process.env.COLORTERM ?? "") ? "\x1b[38;2;59;130;246m" : "\x1b[38;5;33m");
  const dim = paint("\x1b[2m");
  const bold = paint("\x1b[1m");

  const compilerOptions = {
    target: "ES2022",
    module: "ESNext",
    moduleResolution: "Bundler",
    allowImportingTsExtensions: true,
    verbatimModuleSyntax: true,
    strict: true,
    noEmit: true,
    skipLibCheck: true,
  };
  const json = (value) => JSON.stringify(value, null, 2) + "\n";
  const testFile = (ext) => `import { test, expect } from "rtn:test";\nimport { greet } from "./greet.${ext}";\n\n` +
    `test("greets by name", () => {\n  expect(greet("Ali")).toBe("Hello, Ali!");\n});\n`;

  function files(name, ts) {
    const ext = ts ? "ts" : "js";
    const out = {
      "package.json": json({
        name,
        version: "0.1.0",
        type: "module",
        private: true,
        scripts: { start: `rtn index.${ext}`, test: "rtn test" },
      }),
      [`index.${ext}`]: `import { greet } from "./greet.${ext}";\n\nconsole.log(greet("world"));\n`,
    };
    if (ts) {
      out["greet.ts"] = "export function greet(name: string): string {\n  return `Hello, ${name}!`;\n}\n";
      out["greet.test.ts"] = testFile("ts");
      out["tsconfig.json"] = json({ compilerOptions });
    } else {
      out["greet.js"] = "/**\n * @param {string} name\n * @returns {string}\n */\n" +
        "export function greet(name) {\n  return `Hello, ${name}!`;\n}\n";
      out["greet.test.js"] = testFile("js");
      // Editors use jsconfig.json for JavaScript projects; checkJs turns the JSDoc types above into checks.
      const { allowImportingTsExtensions, ...jsOptions } = compilerOptions;
      out["jsconfig.json"] = json({ compilerOptions: { ...jsOptions, checkJs: true } });
    }
    out[".gitignore"] = "node_modules/\n";
    return out;
  }

  internal.initProject = async (args) => {
    const ts = !args.some((a) => a === "--js" || a === "--javascript");
    const target = path.resolve(process.cwd(), args.find((a) => !a.startsWith("-")) ?? ".");
    const name = path.basename(target).toLowerCase().replace(/[^a-z0-9._-]+/g, "-").replace(/^[._-]+/, "") || "my-app";
    fs.mkdirSync(target, { recursive: true });
    process.stdout.write(`\n${blue("●")} ${bold("rtn init")} ${dim(`${ts ? "TypeScript" : "JavaScript"} · ${target}`)}\n\n`);
    for (const [file, content] of Object.entries(files(name, ts))) {
      const full = path.join(target, file);
      if (fs.existsSync(full)) {
        process.stdout.write(`  ${dim("·")} ${file.padEnd(16)}${dim("exists, kept as is")}\n`);
        continue;
      }
      fs.writeFileSync(full, content);
      process.stdout.write(`  ${blue("✓")} ${file.padEnd(16)}${dim("created")}\n`);
    }
    const rel = path.relative(process.cwd(), target);
    process.stdout.write(`\n${bold("Next:")}\n`);
    if (rel) process.stdout.write(`  ${blue(`cd ${rel}`)}\n`);
    process.stdout.write(`  ${blue(`rtn index.${ts ? "ts" : "js"}`)}   ${dim("run it")}\n  ${blue("rtn test")}       ${dim("run the tests")}\n\n`);
    return 0;
  };
});
