import { t } from './i18n.js';

/*
 * Text that comes from the Rust side instead of the front end.
 *
 * A few strings are produced by the backend because it is the side that knows the facts: the outcome of a dump check,
 * the name of a setup step, the line a run ends with, the phase of an update. They are written in English there and are
 * translated here.
 *
 * Matching is done on a stable id (a setup step id) or on the fixed English sentence, and the English text that came
 * from Rust is always the fallback. A message a newer runtime adds is therefore shown as it is instead of disappearing
 * behind a missing key.
 */

/** A setup step: `x` carries both `id` and the English `title` from setup_status. */
export function stepTitle(x) {
  const key = `setup.step.${x.id}`;
  const text = t(key);
  return text === key ? x.title : text;
}

/** One entry of a dump check list ("exefs/main is decrypted", "supported version (...)"). */
export function checkText(label) {
  if (label === 'exefs/main is decrypted') return t('setup.check.decrypted');
  const m = /^supported version \((.+)\)$/.exec(label);
  return m ? t('setup.check.version', { version: m[1] }) : label;
}

/** A sentence the dump check or a finished setup run reports. */
const DUMP_ERRORS = {
  'No folder chosen yet.': 'setup.err.noFolder',
  'That folder does not exist or cannot be read.': 'setup.err.unreadable',
  'This folder has no exefs/ folder, and no base/ or updates/ folder holding one. Pick the folder of your dump that contains exefs/ and romfs/.': 'setup.err.noExefs',
  'These game files are a different version from the one this recomp supports. Dump the supported update.': 'setup.err.differentVersion',
  'No usable game files found. The folder needs exefs/ (main, sdk, subsdk0, subsdk1, subsdk2, rtld, decrypted) and romfs/.': 'setup.err.noUsableFiles',
  'Setup finished. Press Play.': 'setup.doneMsg',
  'Paused. Set up continues where it stopped.': 'setup.pausedMsg',
  'Update cancelled.': 'about.cancelled',
  Installing: 'about.step.installing',
  'Checking whether the game has to be built again': 'about.step.checkingRebuild',
  'Checking the download': 'about.step.verifying',
  Unpacking: 'about.step.unpacking',
};
export function dumpText(s) {
  const key = DUMP_ERRORS[s];
  return key ? t(key) : s;
}

/** The line a run of the game ends with (play.rs). */
export function statusText(s) {
  const m = (s && s.message) || '';
  if (m === 'Starting ObeliskChamber.exe...') return t('play.starting');
  if (m === 'ObeliskChamber.exe exited normally.') return t('play.exitedOk');
  if (m === 'ObeliskChamber.exe was ended by the system.') return t('play.killedBySystem');
  let x = /^ObeliskChamber\.exe started \(process (\d+)\)$/.exec(m);
  if (x) return t('play.started', { pid: x[1] });
  x = /^Could not start ObeliskChamber\.exe: (.+)$/.exec(m);
  if (x) return t('play.startFailed', { error: x[1] });
  x = /^Lost track of ObeliskChamber\.exe: (.+)$/.exec(m);
  if (x) return t('play.lostTrack', { error: x[1] });
  x = /^ObeliskChamber\.exe exited with code (-?\d+) \(0x([0-9A-Fa-f]+)\)\.$/.exec(m);
  if (x) return t('play.exitedCode', { code: x[1], hex: x[2].toUpperCase() });
  return m;
}

/** A phase line of the updater (update.rs); the download line is rebuilt so its numbers follow the language. */
export function updText(s) {
  const m = (s && s.message) || '';
  const fixed = dumpText(m);
  if (fixed !== m) return fixed;
  const dl = /^Downloading (.+) \((\d+) of (\d+) MiB\)$/.exec(m);
  if (dl) return t('about.step.downloading', { asset: dl[1], done: dl[2], total: dl[3] });
  const inst = /^Version (.+) is installed\. Restart the launcher to use it\.$/.exec(m);
  if (inst) return t('about.installed', { version: inst[1] });
  return m;
}

/** A build prerequisite setup.rs lists (`check_tools`): the name and the hint are written in English there. */
const TOOLS = {
  'Visual Studio C++ tools': 'visualStudio',
  'LLVM (clang-cl)': 'llvm',
  CMake: 'cmake',
  Ninja: 'ninja',
  'uv (Python)': 'uv',
  '.NET SDK': 'dotnet',
  Git: 'git',
};
export function toolText(name) {
  const id = TOOLS[name];
  return id ? t(`setup.tool.${id}`) : name;
}
const TOOL_HINTS = {
  'Visual Studio 2022 Build Tools or newer with "Desktop development with C++".': 'visualStudio',
  'LLVM 23 or newer from github.com/llvm/llvm-project/releases (or set NX_LLVM to its bin folder, or NX_TOOLCHAIN to a folder with llvm\\bin).': 'llvm',
  'CMake 3.25 or newer (winget install Kitware.CMake).': 'cmake',
  'winget install Ninja-build.Ninja': 'ninja',
  'winget install astral-sh.uv': 'uv',
  '.NET 10 SDK (winget install Microsoft.DotNet.SDK.10).': 'dotnet',
  "winget install Git.Git (fetches the shader translator's sources).": 'git',
};
export function toolHint(hint) {
  const id = TOOL_HINTS[hint];
  return id ? t(`setup.tool.${id}.hint`) : hint;
}

/** The sentence `upscaler_files` (main.rs) builds around the folders it searched. */
const WHAT = {
  'amd_fidelityfx_upscaler_vk.dll': 'settings.files.whatUpscaler',
  'amd_fidelityfx_vk.dll (FidelityFX SDK v1.1.4)': 'settings.files.whatFfx',
  'the Streamline files': 'settings.files.whatStreamline',
  'the Streamline frame generation files': 'settings.files.whatStreamlineFg',
};
export function fileDetail(d) {
  const s = String(d || '');
  let m = /^(.+) lacks (.+)$/.exec(s);
  if (m) return t('settings.files.lacks', { path: m[1], files: m[2] });
  m = /^(.+?) not found in (.+)$/.exec(s);
  if (m) {
    const what = WHAT[m[1]] ? t(WHAT[m[1]]) : m[1];
    return t('settings.files.missingIn', { what, dirs: m[2] });
  }
  return s;
}