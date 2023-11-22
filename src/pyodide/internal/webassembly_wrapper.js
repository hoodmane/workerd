import {default as Eval} from "pyodide-internal:eval";

const origWebAssembly = globalThis.WebAssembly;

export const WebAssembly = new Proxy(origWebAssembly, {get(target, val) {
  const result = Reflect.get(...arguments);
  if (val !== "Module") {
    return result;
  }
  return new Proxy(result, {
    construct() {
      try {
        Eval.enableEval();
        return Reflect.construct(...arguments);
      } finally {
        Eval.disableEval();
      }
    }
  });
}});
