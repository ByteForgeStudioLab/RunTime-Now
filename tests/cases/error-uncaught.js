function inner() { throw new TypeError("something broke", { cause: new Error("root cause") }); }
function outer() { inner(); }
console.log("before");
outer();
console.log("never printed");
