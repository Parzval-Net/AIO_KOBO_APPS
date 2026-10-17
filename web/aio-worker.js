// Kobo AIO, browser mode. Runs the same Python engine (tools/kobo_aio) inside Pyodide, in this worker.
//
// Device access:
//   "rw"  File System Access API (Chrome, Edge): the user grants the KOBOeReader folder; only changed paths
//         are written back, after the backup was saved to a folder of the user's computer.
//   "zip" read-only (Safari, Firefox): files come from <input webkitdirectory>; every change is returned in one
//         KoboRoot.tgz that the user drops into .kobo/ (Nickel installs it on the next boot).
// The engine sees an in-memory mirror (kobo_aio/web.py). Files are read from the device only when needed,
// synchronously, with FileReaderSync (allowed in workers).
"use strict";

const SKIP_WALK = new Set([".Spotlight-V100", ".Trashes", ".fseventsd", ".TemporaryItems", ".kobo/images",
  ".kobo-images", "System Volume Information", "$RECYCLE.BIN"]);
const AIO_HOME = "/home/pyodide/Kobo-AIO";

let py = null, web = null, base = "", mode = null, device = null, backupDir = null;
const files = new Map();   // rel -> File (device snapshot, read lazily)
const reader = new FileReaderSync();

const send = (msg, transfer) => self.postMessage(msg, transfer || []);
const progress = m => send({ progress: String(m) });

function readDevice(rel) {
  const f = files.get(rel);
  if (!f) throw new Error(`No existe en el Kobo: ${rel}`);
  return new Uint8Array(reader.readAsArrayBuffer(f));
}
function fetchSync(url) {   // synchronous binary XHR: allowed in workers, lets Python stay synchronous
  const xhr = new XMLHttpRequest();
  xhr.open("GET", new URL(url, base).href, false);
  xhr.responseType = "arraybuffer";
  xhr.send(null);
  if (xhr.status !== 200) throw new Error(`HTTP ${xhr.status} en ${url}`);
  return new Uint8Array(xhr.response);
}

async function boot(baseUrl) {
  if (web) return true;
  base = baseUrl;
  progress("Cargando el motor (solo la primera vez, unos 15 MB)…");
  importScripts(new URL("pyodide/pyodide.js", base).href);
  py = await loadPyodide({ indexURL: new URL("pyodide/", base).href });
  await py.loadPackage(["pillow", "sqlite3"]);
  const zip = await (await fetch(new URL("py/kobo_aio.zip", base).href, { cache: "no-cache" })).arrayBuffer();
  py.unpackArchive(zip, "zip", { extractDir: "/aio" });
  py.runPython("import sys; sys.path.insert(0, '/aio')");
  web = py.pyimport("kobo_aio.web");
  return true;
}

// -- device snapshot --------------------------------------------------------
async function walk(dir, prefix, entries) {
  for await (const [name, h] of dir.entries()) {
    const rel = prefix + name;
    if (h.kind === "directory") {
      entries.push([rel, -1]);
      if (!SKIP_WALK.has(rel)) await walk(h, rel + "/", entries);
    } else {
      const f = await h.getFile();
      files.set(rel, f);
      entries.push([rel, f.size]);
    }
  }
}
async function mountEntries(entries) {
  progress(`Leyendo la estructura del Kobo (${entries.length} elementos)…`);
  device = JSON.parse(web.mount(JSON.stringify(entries), readDevice, fetchSync, "artifacts", progress));
  return device;
}
async function connectHandle(handle) {
  mode = "rw"; device = null; files.clear();
  self.deviceHandle = handle;
  const entries = [];
  progress("Revisando las carpetas del Kobo…");
  await walk(handle, "", entries);
  return mountEntries(entries);
}
async function connectFiles(list) {   // [{rel, file}] from <input webkitdirectory>
  mode = "zip"; device = null; files.clear(); self.deviceHandle = null;
  const entries = [], dirs = new Set();
  for (const { rel, file } of list) {
    if ([...SKIP_WALK].some(s => rel === s || rel.startsWith(s + "/"))) continue;
    const parts = rel.split("/");
    for (let i = 1; i < parts.length; i++) {
      const d = parts.slice(0, i).join("/");
      if (!dirs.has(d)) { dirs.add(d); entries.push([d, -1]); }
    }
    files.set(rel, file);
    entries.push([rel, file.size]);
  }
  return mountEntries(entries);
}

// -- writing ----------------------------------------------------------------
async function dirFor(root, rel, create) {
  const parts = rel.split("/");
  let d = root;
  for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create });
  return [d, parts[parts.length - 1]];
}
async function writeFile(root, rel, bytes) {
  const [d, name] = await dirFor(root, rel, true);
  const fh = await d.getFileHandle(name, { create: true });
  const w = await fh.createWritable();
  await w.write(bytes);
  await w.close();
  return fh;
}
async function removeFile(root, rel) {
  try {
    const [d, name] = await dirFor(root, rel, false);
    await d.removeEntry(name);
  } catch (e) { if (e.name !== "NotFoundError") throw e; }
}
function memRead(path) { return py.FS.readFile(path); }
function memExists(path) { try { return py.FS.isFile(py.FS.stat(path).mode); } catch (_) { return false; } }

async function copyTreeOut(memDir, handle, relPrefix) {   // MEMFS directory -> folder of the computer
  for (const name of py.FS.readdir(memDir)) {
    if (name === "." || name === "..") continue;
    const p = `${memDir}/${name}`, rel = `${relPrefix}/${name}`;
    if (py.FS.isDir(py.FS.stat(p).mode)) await copyTreeOut(p, handle, rel);
    else await writeFile(handle, rel, memRead(p));
  }
}
async function loadTreeIn(dirHandle, memDir, skip = () => false) {   // folder of the computer -> MEMFS
  py.FS.mkdirTree(memDir);
  for await (const [name, h] of dirHandle.entries()) {
    if (skip(name, h)) continue;
    if (h.kind === "directory") await loadTreeIn(h, `${memDir}/${name}`, skip);
    else py.FS.writeFile(`${memDir}/${name}`, new Uint8Array(await (await h.getFile()).arrayBuffer()));
  }
}
async function subdir(handle, rel, create = false) {
  let d = handle;
  for (const p of rel.split("/").filter(Boolean)) d = await d.getDirectoryHandle(p, { create });
  return d;
}

// Write the in-memory result onto the device. If anything fails, put back what was already written.
async function syncToDevice(ops, changes, backupMem) {
  const done = [], existed = new Map((changes || []).map(c => [c.rel, c.existed]));
  try {
    for (const o of ops) {
      progress(`${o.op === "write" ? "Escribiendo" : "Eliminando"} ${o.rel}`);
      if (o.op === "write") {
        const fh = await writeFile(self.deviceHandle, o.rel, memRead(`/kobo/${o.rel}`));
        files.set(o.rel, await fh.getFile());
      } else {
        await removeFile(self.deviceHandle, o.rel);
        files.delete(o.rel);
      }
      done.push(o);
    }
  } catch (e) {
    progress("Error al escribir en el Kobo: devolviendo los archivos a como estaban…");
    for (const o of done.reverse()) {
      try {
        const orig = backupMem ? `${backupMem}/files/${o.rel}` : null;
        if (existed.get(o.rel) && orig && memExists(orig)) await writeFile(self.deviceHandle, o.rel, memRead(orig));
        else if (!existed.get(o.rel)) await removeFile(self.deviceHandle, o.rel);
      } catch (_) { /* reported below */ }
    }
    throw new Error(`No se pudo escribir en el Kobo (${e.message}). Se revirtió lo escrito; revisa que siga conectado.`);
  }
}

// -- commands ---------------------------------------------------------------
const CMDS = {
  boot: ({ base }) => boot(base),
  connectHandle: ({ handle }) => connectHandle(handle),
  connectFiles: ({ list }) => connectFiles(list),
  catalog: () => JSON.parse(web.catalog_json()),
  doctor: () => JSON.parse(web.doctor_json()),
  upload: async ({ name, file }) => JSON.parse(web.save_upload(name, new Uint8Array(await file.arrayBuffer()))),
  plan: ({ selection, uninstall }) => JSON.parse(web.run_json("plan", JSON.stringify(selection), JSON.stringify(uninstall || []))),
  async apply({ selection, uninstall, backupHandle }) {
    const res = JSON.parse(web.run_json("apply", JSON.stringify(selection), JSON.stringify(uninstall || [])));
    if (!res.sync) return res;   // nothing changed
    if (mode === "zip") {
      const tgz = web.export_koboroot(JSON.stringify(res.sync)), zip = tgz.toJs(); tgz.destroy();
      let backupZip = null;
      if (res.changes.some(c => c.existed)) { const b = web.export_backup_zip(res.backup_dir); backupZip = b.toJs(); b.destroy(); }
      return { ...res, zip, backupZip };
    }
    if (!backupHandle) throw new Error("Elige una carpeta de tu computador para guardar los respaldos.");
    progress("Guardando el respaldo en tu computador…");
    await copyTreeOut(res.backup_dir, backupHandle, `Kobo-AIO/${res.backup_rel}`);
    await syncToDevice(res.sync, res.changes, res.backup_dir);
    progress("Listo.");
    return res;
  },
  async backups({ backupHandle }) {
    if (!backupHandle || !device) return [];
    let dir;
    try { dir = await subdir(backupHandle, `Kobo-AIO/backups/${device.serial || "unknown"}`); } catch (_) { return []; }
    // Only manifests and journals: the backed-up files are loaded when a rollback needs them.
    await loadTreeIn(dir, `${AIO_HOME}/backups/${device.serial || "unknown"}`, (name, h) => h.kind === "directory" && name === "files");
    return JSON.parse(web.backups_json());
  },
  async rollback({ manifest, backupHandle, dryRun }) {
    if (mode !== "rw") throw new Error("Revertir necesita Chrome o Edge (o Kobo AIO en tu computador).");
    const memDir = manifest.replace(/\/manifest\.json$/, "");
    const rel = memDir.slice(AIO_HOME.length + 1);
    const files_ = await subdir(backupHandle, `Kobo-AIO/${rel}/files`).catch(() => null);
    if (files_) await loadTreeIn(files_, `${memDir}/files`);
    const res = JSON.parse(web.rollback_json(manifest, !!dryRun));
    if (dryRun) return { actions: res.actions };
    for (const p of res.persist) await copyTreeOut(`${AIO_HOME}/${p}`, backupHandle, `Kobo-AIO/${p}`);
    await syncToDevice(res.sync, null, null);
    return { actions: res.actions };
  },
};

self.onmessage = async ({ data }) => {
  const { id, cmd, args } = data;
  try {
    if (cmd !== "boot" && !web) throw new Error("El motor del navegador no está listo");
    const result = await CMDS[cmd](args || {});
    const transfer = result && result.zip ? [result.zip.buffer, ...(result.backupZip ? [result.backupZip.buffer] : [])] : [];
    send({ id, ok: true, result }, transfer);
  } catch (e) {
    let msg = e && e.message ? e.message : String(e);
    const m = msg.match(/(?:AioError|SafetyError|RuntimeError): ([^\n]+)\s*$/m);   // Python traceback -> last line
    if (m) msg = m[1];
    send({ id, ok: false, error: msg });
  }
};
