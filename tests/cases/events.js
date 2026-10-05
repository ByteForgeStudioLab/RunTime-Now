// EventTarget, Event, CustomEvent, AbortController, AbortSignal.
const target = new EventTarget();
const log = [];
target.addEventListener("ping", (e) => log.push(["fn", e.type, e.target === target, e.eventPhase]));
target.addEventListener("ping", { handleEvent(e) { log.push(["object", e.currentTarget === target]); } });
target.addEventListener("ping", () => log.push("once"), { once: true });
const dup = () => log.push("dup");
target.addEventListener("ping", dup);
target.addEventListener("ping", dup);  // same listener twice: ignored
target.dispatchEvent(new Event("ping"));
target.removeEventListener("ping", dup);
target.dispatchEvent(new Event("ping"));
console.log(log);

const cancelable = new Event("x", { cancelable: true });
target.addEventListener("x", (e) => e.preventDefault());
console.log("dispatch returned", target.dispatchEvent(cancelable), cancelable.defaultPrevented);
console.log("after dispatch", cancelable.eventPhase, cancelable.currentTarget, cancelable.target === target);
const passive = new Event("p", { cancelable: true });
target.addEventListener("p", (e) => e.preventDefault(), { passive: true });
console.log("passive listener can't cancel:", target.dispatchEvent(passive));

const order = [];
target.addEventListener("s", () => order.push(1));
target.addEventListener("s", (e) => { order.push(2); e.stopImmediatePropagation(); });
target.addEventListener("s", () => order.push(3));
target.dispatchEvent(new Event("s"));
console.log("stopImmediatePropagation:", order);

const plain = new Event("z");
console.log(new CustomEvent("c", { detail: { n: 1 } }).detail, plain.type, plain.bubbles, plain.cancelable, typeof plain.timeStamp);
class Emitter extends EventTarget {}
const em = new Emitter();
em.addEventListener("hi", (e) => console.log("subclass got", e.type, e instanceof Event));
em.dispatchEvent(new CustomEvent("hi"));
try { target.dispatchEvent({ type: "fake" }); } catch (e) { console.log(e.name + ":", e.message); }

// AbortController
const controller = new AbortController();
controller.signal.onabort = (e) => console.log("onabort:", e.type, e.isTrusted);
controller.signal.addEventListener("abort", () => console.log("listener: reason is", controller.signal.reason.name));
console.log(controller);
controller.abort();
controller.abort();  // only the first call counts
console.log(controller.signal.aborted, controller.signal.reason instanceof DOMException);
try { controller.signal.throwIfAborted(); } catch (e) { console.log("throwIfAborted:", e.name, e.message); }

console.log("AbortSignal.abort:", AbortSignal.abort("why").reason);
const any = AbortSignal.any([new AbortController().signal, AbortSignal.abort(1)]);
console.log("AbortSignal.any (already aborted):", any.aborted, any.reason);
const source = new AbortController();
const combined = AbortSignal.any([source.signal, new AbortController().signal]);
combined.onabort = () => console.log("AbortSignal.any follows its sources:", combined.reason);
source.abort(42);
try { new AbortSignal(); } catch (e) { console.log("new AbortSignal():", e.message); }

const removable = new AbortController();
target.addEventListener("y", () => console.log("y fired"), { signal: removable.signal });
target.dispatchEvent(new Event("y"));
removable.abort();
target.dispatchEvent(new Event("y"));  // the listener was removed by the signal

const timeout = AbortSignal.timeout(10);
timeout.onabort = () => console.log("AbortSignal.timeout:", timeout.reason.name, "-", timeout.reason.message);
AbortSignal.timeout(60_000);  // pending, but doesn't keep the process alive
setTimeout(() => console.log("done"), 30);
