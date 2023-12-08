import { loadPyodide } from "pyodide:python";
import worker from "./worker.py";
import lockFile from "./pyodide-lock.json";

async function setupPyodide() {
  const pyodide = await loadPyodide();
  pyodide.FS.writeFile(`/session/worker.py`, new Uint8Array(worker), {
    canOwn: true,
  });
  initializePackageIndex(pyodide, JSON.parse(lockFile));
  const t2 = performance.now();
  // Must disable check integrity:
  // Subrequest integrity checking is not implemented. The integrity option
  // must be either undefined or an empty string.
  const origLoadPackage = pyodide.loadPackage;
  pyodide.loadPackage = async function(packages, options) {
    return await origLoadPackage(packages, {checkIntegrity: false, ...options});
  }
  return pyodide;
}

function initializePackageIndex(pyodide, lockfile) {
  if (!lockfile.packages) {
    throw new Error(
      "Loaded pyodide lock file does not contain the expected key 'packages'.",
    );
  }
  const API = pyodide._api;
  API.config.indexURL = "https://cdn.jsdelivr.net/pyodide/v0.25.0a1/full/";
  globalThis.location = "https://cdn.jsdelivr.net/pyodide/v0.25.0a1/full/";
  API.lockfile_info = lockfile.info;
  API.lockfile_packages = lockfile.packages;
  API.lockfile_unvendored_stdlibs_and_test = [];

  // compute the inverted index for imports to package names
  API._import_name_to_package_name = new Map();
  for (let name of Object.keys(API.lockfile_packages)) {
    const pkg = API.lockfile_packages[name];

    for (let import_name of pkg.imports) {
      API._import_name_to_package_name.set(import_name, name);
    }

    if (pkg.package_type === "cpython_module") {
      API.lockfile_unvendored_stdlibs_and_test.push(name);
    }
  }

  API.lockfile_unvendored_stdlibs =
    API.lockfile_unvendored_stdlibs_and_test.filter(
      (lib) => lib !== "test",
    );
}


export default {
  async fetch(request) {
    const t1 = performance.now();
    const pyodide = await setupPyodide();
    const fetchHandler = pyodide.pyimport("worker").fetch;
    const t2 = performance.now();
    const result = fetchHandler(request);
    const t3 = performance.now();
    console.log("bootstrap", t2 - t1);
    console.log("handle", t3 - t2);
    return result;
  },
  async test() {
    const pyodide = await setupPyodide();
    const fetchHandler = pyodide.pyimport("worker").fetch;
    // Must disable check integrity:
    // Subrequest integrity checking is not implemented. The integrity option
    // must be either undefined or an empty string.
    await pyodide.loadPackage("micropip");
    const result = fetchHandler(new Request(""));
    console.log(result);
  }
};
