console.log(process.argv.length, process.argv[0].endsWith("rtn"), process.argv[1].endsWith("process.js"), process.argv.slice(2));
console.log(process.platform, typeof process.pid, process.execPath === process.argv[0], typeof process.env.PATH);
console.log(process.cwd() === process.env.PWD || typeof process.cwd() === "string", process.versions.rtn === process.version.slice(1));
const t = process.hrtime();
console.log(process.hrtime(t)[0] === 0, typeof process.hrtime.bigint(), process.uptime() >= 0, process.memoryUsage().rss > 0);
process.stdout.write("stdout.write works\n");
process.stdout.write(new TextEncoder().encode("bytes too\n"));
process.exitCode = 3;
