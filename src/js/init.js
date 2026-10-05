// rtn init [dir]: a new TypeScript project, ready for `rtn index.ts` and `rtn test`.
(function (native, internal) {
  "use strict";

  const path = internal.path;
  const fs = internal.modules.fs;

  const color = Boolean(process.stdout.isTTY) && !process.env.NO_COLOR && process.env.TERM !== "dumb";
  const paint = (code) => (s) => (color ? `${code}${s}\x1b[0m` : s);
  const blue = paint(/truecolor|24bit/.test(process.env.COLORTERM ?? "") ? "\x1b[38;2;59;130;246m" : "\x1b[38;5;33m");
  const dim = paint("\x1b[2m");
  const bold = paint("\x1b[1m");

  function files(name) {
    return {
      "package.json": JSON.stringify({
        name,
        version: "0.1.0",
        type: "module",
        private: true,
        scripts: { start: "rtn index.ts", test: "rtn test" },
      }, null, 2) + "\n",
      "index.ts": `import { greet } from "./greet.ts";\n\nconsole.log(greet("world"));\n`,
      "greet.ts": "export function greet(name: string): string {\n  return `Hello, ${name}!`;\n}\n",
      "greet.test.ts": `import { test, expect } from "rtn:test";\nimport { greet } from "./greet.ts";\n\n` +
        `test("greets by name", () => {\n  expect(greet("Ali")).toBe("Hello, Ali!");\n});\n`,
      "tsconfig.json": JSON.stringify({
        compilerOptions: {
          target: "ES2022",
          module: "ESNext",
          moduleResolution: "Bundler",
          allowImportingTsExtensions: true,
          verbatimModuleSyntax: true,
          strict: true,
          noEmit: true,
          skipLibCheck: true,
        },
      }, null, 2) + "\n",
      ".gitignore": "node_modules/\n",
    };
  }

  internal.initProject = async (args) => {
    const target = path.resolve(process.cwd(), args.find((a) => !a.startsWith("-")) ?? ".");
    const name = path.basename(target).toLowerCase().replace(/[^a-z0-9._-]+/g, "-").replace(/^[._-]+/, "") || "my-app";
    fs.mkdirSync(target, { recursive: true });
    process.stdout.write(`\n${blue("●")} ${bold("rtn init")} ${dim(target)}\n\n`);
    for (const [file, content] of Object.entries(files(name))) {
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
    process.stdout.write(`  ${blue("rtn index.ts")}   ${dim("run it")}\n  ${blue("rtn test")}       ${dim("run the tests")}\n\n`);
    return 0;
  };
});
