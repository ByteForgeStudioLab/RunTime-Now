const handled = Promise.reject(new Error("handled later"));
setTimeout(() => {}, 0);
handled.catch((e) => console.log("caught:", e.message));
await null;
Promise.reject(new Error("nobody catches this"));
