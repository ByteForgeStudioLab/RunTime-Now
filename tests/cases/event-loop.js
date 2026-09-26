// Order: sync code -> microtasks -> timers (by delay, then insertion order)
const log = [];
log.push("sync 1");
setTimeout(() => log.push("timeout 80"), 80);
setTimeout(() => log.push("timeout 0 (a)"), 0);
setTimeout(() => log.push("timeout 0 (b)"), 0);
Promise.resolve().then(() => log.push("promise"));
queueMicrotask(() => log.push("microtask"));
process.nextTick(() => log.push("nextTick"));
log.push("sync 2");

let n = 0;
const id = setInterval((tag) => {
  log.push(`${tag} ${++n}`);
  if (n === 3) clearInterval(id);
}, 5, "interval");
const cancelled = setTimeout(() => log.push("never"), 1);
clearTimeout(cancelled);

await new Promise((r) => setTimeout(r, 150));
log.push("after await");
setTimeout(() => {
  Promise.resolve().then(() => log.push("promise inside timer"));
  log.push("timer body");
}, 0);
await new Promise((r) => setTimeout(r, 10));
console.log(log.join("\n"));
