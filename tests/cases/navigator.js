// navigator
console.log(navigator.userAgent.startsWith("rtn/"), navigator.userAgent === `rtn/${rtn.version}`);
console.log(Number.isInteger(navigator.hardwareConcurrency) && navigator.hardwareConcurrency > 0, navigator.platform);
console.log(typeof navigator.language, navigator.languages.length, String(navigator), "navigator" in globalThis);
try { new Navigator(); } catch (e) { console.log(e.name, e.message); }
