// Event loop tartibi: sinxron kod -> microtask -> taymerlar
console.log("1. sinxron");
setTimeout(() => console.log("5. setTimeout 0"), 0);
setTimeout((a, b) => console.log("7. setTimeout 50, argumentlar:", a, b), 50, "x", "y");
Promise.resolve().then(() => console.log("3. promise.then"));
queueMicrotask(() => console.log("4. queueMicrotask"));
console.log("2. sinxron");

let n = 0;
const id = setInterval(() => {
  n++;
  console.log(`6. setInterval #${n}`);
  if (n === 3) clearInterval(id);
}, 10);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
console.time("sleep");
await sleep(100);                     // top-level await
console.timeEnd("sleep");
console.log("8. top-level await tugadi");
