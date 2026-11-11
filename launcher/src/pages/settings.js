import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';
import { t, getLang, listLangs, setLang } from '../i18n.js';
import { fileDetail } from '../rusttext.js';

/*
 * Settings schema. Every key is described in runtime/docs/CONFIG.md.
 *   file: "game" (fefw.ini), "controls" (controls.ini, written by the game on its first start) or "config" (the launcher's own options)
 * Types: select, toggle, size, autonum, range, path, text, number, language. A toggle with `on` / `off` writes those words
 * instead of 1 / 0 (`off`, 0, false or no read as off, anything else as on).
 * Text is a key into src/locales: `label` and `hint` are always translated, and an option name that contains a dot is a key
 * too (a plain name such as "720p" or "DLAA" is shown as it is).
 */
// The WebKitGTK launcher on Linux (the Windows one is WebView2)
const IS_LINUX = /Linux/.test(navigator.userAgent) && !/Android/.test(navigator.userAgent);

const SECTIONS = [
  {
    title: 'settings.sec.picture', icon: 'monitor', file: 'game',
    fields: [
      { id: 'output', type: 'select', label: 'settings.output.label', sec: 'fefw', key: 'output', def: 'console',
        options: [['console', 'settings.output.console'], ['720p', '720p'], ['1080p', '1080p'], ['1440p', '1440p'], ['2160p', '2160p (4K)'],
          ['2560x1080', 'settings.output.uw2560'], ['3440x1440', 'settings.output.uw3440'], ['5120x1440', 'settings.output.uw5120'],
          ['window', 'settings.output.window']],
        hint: 'settings.output.hint' },
      { id: 'ultrawide', type: 'toggle', label: 'settings.ultrawide.label', sec: 'fefw', key: 'ultrawide', def: 'auto', on: 'auto', off: 'off',
        onLabel: 'settings.ultrawide.auto', offLabel: 'settings.ultrawide.off', hint: 'settings.ultrawide.hint' },
      { id: 'render_ratio', type: 'select', label: 'settings.renderRatio.label', sec: 'fefw', key: 'render_ratio', def: '0',
        options: [['0', 'settings.renderRatio.game'], ['0.5', '50%'], ['0.67', '67%'], ['0.75', '75%'], ['1.0', 'settings.renderRatio.full']],
        hint: 'settings.renderRatio.hint' },
      { id: 'sr_tier', type: 'select', label: 'settings.srTier.label', sec: 'fefw', key: 'render_ratio', def: '0',
        options: [['0', 'settings.srTier.game'], ['0.33', 'settings.srTier.ultraPerformance'], ['0.5', 'settings.srTier.performance'],
          ['0.59', 'settings.srTier.balanced'], ['0.67', 'settings.srTier.quality'], ['0.75', 'settings.srTier.ultraQuality'],
          ['0.85', 'settings.srTier.highQuality'], ['1.0', 'settings.srTier.dlaa']],
        hint: 'settings.srTier.hint' },
      { id: 'upscaler', type: 'select', label: 'settings.upscaler.label', sec: 'dlss', key: 'upscaler', def: 'taau',
        options: [['taau', 'settings.upscaler.taau'], ['dlss', 'settings.upscaler.dlss'], ['fsr3', 'settings.upscaler.fsr3'],
          ['bilinear', 'settings.upscaler.bilinear'], ['auto', 'settings.upscaler.auto']],
        hint: 'settings.upscaler.hint' },
      { id: 'fsr_dll', type: 'path', label: 'settings.fsrDll.label', sec: 'upscale', key: 'fsr_dll', def: '', placeholder: 'settings.fsrDll.placeholder', status: 'fsr',
        hint: 'settings.fsrDll.hint' },
      { id: 'dlss_dll', type: 'path', label: 'settings.dlssDll.label', sec: 'upscale', key: 'dlss_dll', def: '', placeholder: 'settings.dlssDll.placeholder', status: 'dlss',
        hint: 'settings.dlssDll.hint' },
      { id: 'quality', type: 'select', label: 'settings.quality.label', sec: 'fefw', key: 'quality', def: 'console',
        options: [['console', 'settings.quality.console'], ['docked', 'settings.quality.docked']] },
      { id: 'shadow_size', type: 'select', label: 'settings.shadowSize.label', sec: 'fefw', key: 'shadow_size', def: '0',
        options: [['0', 'settings.shadowSize.game'], ['2048', '2048'], ['4096', '4096'], ['8192', '8192']] },
      { id: 'texture_detail', type: 'select', label: 'settings.textureDetail.label', sec: 'gfx', key: 'texture_detail', def: 'auto',
        options: [['auto', 'settings.textureDetail.auto'], ['full', 'settings.textureDetail.full'], ['half', 'settings.textureDetail.half']],
        hint: 'settings.textureDetail.hint' },
    ],
  },
  {
    // [tweaks] language of controls.ini (runtime/docs/CONFIG.md). The names are the ones the runtime reads (services/common.cpp); the list is
    // the languages the game ships (games/fefw/fefw_data.cpp). A [system] language in fefw.ini wins over this.
    title: 'settings.sec.language', icon: 'languages', file: 'controls',
    fields: [
      { id: 'language', type: 'select', label: 'settings.gameLang.label', sec: 'tweaks', key: 'language', def: 'auto',
        options: [
          ['auto', 'settings.gameLang.auto'],
          ['AmericanEnglish', 'settings.gameLang.americanEnglish'], ['BritishEnglish', 'settings.gameLang.britishEnglish'],
          ['Japanese', 'settings.gameLang.japanese'], ['French', 'settings.gameLang.french'],
          ['CanadianFrench', 'settings.gameLang.canadianFrench'], ['German', 'settings.gameLang.german'],
          ['Italian', 'settings.gameLang.italian'], ['Spanish', 'settings.gameLang.spanish'],
          ['LatinAmericanSpanish', 'settings.gameLang.latinSpanish'], ['Korean', 'settings.gameLang.korean'],
          ['SimplifiedChinese', 'settings.gameLang.simplifiedChinese'], ['TraditionalChinese', 'settings.gameLang.traditionalChinese'],
        ],
        hint: 'settings.gameLang.hint' },
    ],
  },
  {
    title: 'settings.sec.frameRate', icon: 'gauge', file: 'game',
    fields: [
      { id: 'fps60', type: 'toggle', label: 'settings.fps60.label', sec: 'fefw', key: 'fps60', def: '0', hint: 'settings.fps60.hint' },
      { id: 'fps120', type: 'toggle', label: 'settings.fps120.label', sec: 'fefw', key: 'fps120', def: '0', hint: 'settings.fps120.hint' },
      { id: 'smooth_step', type: 'toggle', label: 'settings.smoothStep.label', sec: 'fefw', key: 'smooth_step', def: '0', hint: 'settings.smoothStep.hint' },
      { id: 'frame_gen', type: 'select', label: 'settings.frameGen.label', sec: 'dlss', key: 'frame_gen', def: '0',
        options: [['0', 'settings.frameGen.off'], ['dlss', 'settings.frameGen.dlss'], ['fsr3', 'settings.frameGen.fsr3']],
        hint: 'settings.frameGen.hint' },
    ],
  },
  {
    title: 'settings.sec.window', icon: 'monitor', file: 'controls',
    fields: [
      { id: 'window_mode', type: 'select', label: 'settings.windowMode.label', sec: 'display', key: 'window_mode', def: 'windowed',
        options: [['windowed', 'settings.windowMode.windowed'], ['borderless', 'settings.windowMode.borderless'], ['exclusive', 'settings.windowMode.exclusive']],
        hint: 'settings.windowMode.hint' },
      { id: 'size', type: 'size', label: 'settings.windowSize.label', sec: 'display', keys: ['width', 'height'], def: ['1280', '720'] },
      { id: 'fps_limit', type: 'autonum', label: 'settings.fpsLimit.label', sec: 'display', key: 'fps_limit', def: 'auto',
        min: 0, max: 1000, numDefault: 60, unit: 'settings.fpsLimit.unit', autoLabel: 'settings.fpsLimit.auto', hint: 'settings.fpsLimit.hint' },
      { id: 'vsync', type: 'toggle', label: 'settings.vsync.label', sec: 'display', key: 'vsync', def: '1' },
    ],
  },
  {
    title: 'settings.sec.sound', icon: 'volume-2', file: 'controls',
    fields: [
      { id: 'volume', type: 'range', label: 'settings.volume.label', sec: 'audio', key: 'volume', def: '1.0', unit: 'settings.volume.unit' },
      { id: 'mute_unfocused', type: 'toggle', label: 'settings.muteUnfocused.label', sec: 'audio', key: 'mute_unfocused', def: '0' },
      // [audio] channels of controls.ini: the game's mix, stereo or 5.1 (runtime/gfx/play/play.cpp maps it to the audio services at start).
      { id: 'audio_channels', type: 'select', label: 'settings.audioChannels.label', sec: 'audio', key: 'channels', def: '2',
        options: [['2', 'settings.audioChannels.stereo'], ['6', 'settings.audioChannels.surround']],
        hint: 'settings.audioChannels.hint' },
      { id: 'prompt_style', type: 'select', label: 'settings.promptStyle.label', sec: 'prompts', key: 'style', def: 'auto',
        options: [['auto', 'settings.promptStyle.auto'], ['switch', 'settings.promptStyle.switch'], ['xbox', 'settings.promptStyle.xbox'],
          ['playstation', 'settings.promptStyle.playstation'], ['keyboard', 'settings.promptStyle.keyboard']],
        hint: 'settings.promptStyle.hint' },
      // [pad] label_abxy: which physical button the Switch's A is mapped from. The button prompts follow it (runtime/gfx/play/prompt_resolve.cpp).
      { id: 'face_buttons', type: 'select', label: 'settings.faceButtons.label', sec: 'pad', key: 'label_abxy', def: '1',
        options: [['1', 'settings.faceButtons.label_'], ['0', 'settings.faceButtons.position']],
        hint: 'settings.faceButtons.hint' },
      // [pad] haptic: how the game's HD Rumble value reaches the pad (runtime/gfx/haptic.cpp). The launcher only writes the
      // setting; the game reads it when it starts, so a change lands on the next run.
      { id: 'haptic', type: 'select', label: 'settings.haptic.label', sec: 'pad', key: 'haptic', def: 'auto',
        options: [['auto', 'settings.haptic.auto'], ['off', 'settings.haptic.off'], ['rumble', 'settings.haptic.rumble'],
          ['dualsense', 'settings.haptic.dualsense']],
        hint: 'settings.haptic.hint' },
      // [pad] haptic_strength: 0..100 percent for the haptic backends, or -1 to keep using the rumble strength above.
      { id: 'haptic_strength', type: 'number', label: 'settings.hapticStrength.label', sec: 'pad', key: 'haptic_strength',
        def: '-1', min: -1, max: 100, hint: 'settings.hapticStrength.hint' },
    ],
  },
  {
    title: 'settings.sec.input', icon: 'folder', file: 'game',
    fields: [
      { id: 'jc2_mouse', type: 'select', label: 'settings.jc2Mouse.label', sec: 'input', key: 'jc2_mouse', def: 'auto',
        options: [['auto', 'settings.jc2Mouse.auto'], ['on', 'settings.jc2Mouse.on'], ['off', 'settings.jc2Mouse.off']], hint: 'settings.jc2Mouse.hint' },
      { id: 'save_dir', type: 'path', label: 'settings.saveDir.label', sec: 'fs', key: 'save_dir', def: '', placeholder: 'settings.saveDir.placeholder' },
      { id: 'log_level', type: 'select', label: 'settings.logLevel.label', sec: 'log', key: 'level', def: 'info',
        options: [['error', 'settings.logLevel.error'], ['warn', 'settings.logLevel.warn'], ['info', 'settings.logLevel.info'], ['debug', 'settings.logLevel.debug']] },
    ],
  },
  {
    title: 'settings.sec.launcher', icon: 'wrench', file: 'config',
    fields: [
      // The launcher's own interface language: kept in launcher.json, applied at once, no file patching involved.
      { id: 'lang', type: 'language', label: 'settings.lang.label', hint: 'settings.lang.hint' },
      { id: 'repo_dir', type: 'path', label: 'settings.repoDir.label', sec: '', key: 'repo_dir', def: '', placeholder: 'settings.repoDir.placeholder', hint: 'settings.repoDir.hint' },
      { id: 'game_dir', type: 'path', label: 'settings.gameDir.label', sec: '', key: 'game_dir', def: '', placeholder: 'settings.gameDir.placeholder', hint: 'settings.gameDir.hint' },
      { id: 'jobs', type: 'number', label: 'settings.jobs.label', sec: '', key: 'jobs', def: '0', min: 0, max: 256, hint: 'settings.jobs.hint' },
      { id: 'minimize_on_play', type: 'toggle', label: 'settings.minimizeOnPlay.label', sec: '', key: 'minimize_on_play', def: '0' },
      // Linux only: fefw.exe runs through Wine/Proton (launcher/src-tauri/src/play.rs runner())
      ...(IS_LINUX ? [{ id: 'runner', type: 'text', label: 'settings.runner.label', sec: '', key: 'runner', def: '', placeholder: 'settings.runner.placeholder', hint: 'settings.runner.hint' }] : []),
    ],
  },
];

const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v).trim().toLowerCase());
const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));
const fwd = (s) => String(s).trim().replace(/\\/g, '/');

/** An option label: a dotted name is a key, anything else ("720p", "DLAA") is shown as written. */
const optName = (label) => (String(label).includes('.') ? t(label) : label);

export async function render(el, ctx) {
  const [cfg, status] = await Promise.all([api('get_config'), api('game_status')]);
  const data = await api('read_settings').catch(() => ({ game_dir: '', game: {}, controls: {}, controls_exists: false }));
  const files = { game: data.game, controls: data.controls, config: { repo_dir: cfg.repo_dir, game_dir: cfg.game_dir, jobs: String(cfg.jobs || 0), minimize_on_play: cfg.minimize_on_play ? '1' : '0', runner: cfg.runner || '' } };
  const exists = { game: true, controls: data.controls_exists, config: true };
  const keyOf = (f, k) => (f.file === 'config' ? k : `${f.sec}.${k}`);
  const get = (f, k, def) => files[f.file][keyOf(f, k)] ?? def;

  // ----- model: one UI value per field -----
  const READ = {
    select: (f) => get(f, f.key, f.def),
    toggle: (f) => (f.off !== undefined ? ![f.off, '0', 'false', 'no'].includes(String(get(f, f.key, f.def)).trim().toLowerCase()) : truthy(get(f, f.key, f.def))),
    size: (f) => ({ w: get(f, f.keys[0], f.def[0]), h: get(f, f.keys[1], f.def[1]) }),
    autonum: (f) => {
      const v = String(get(f, f.key, f.def));
      return v.toLowerCase() === 'auto' ? { auto: true, num: String(f.numDefault) } : { auto: false, num: v };
    },
    range: (f) => Math.round(clamp(parseFloat(get(f, f.key, f.def)) || 0, 0, 1) * 100),
    path: (f) => get(f, f.key, f.def),
    text: (f) => get(f, f.key, f.def),
    number: (f) => String(get(f, f.key, f.def)),
    language: () => getLang(),
  };
  const WRITE = {
    select: (f, x) => [[f.sec, f.key, x]],
    toggle: (f, x) => [[f.sec, f.key, x ? (f.on ?? '1') : (f.off ?? '0')]],
    size: (f, x) => [
      [f.sec, f.keys[0], String(clamp(parseInt(x.w, 10) || 1280, 320, 16384))],
      [f.sec, f.keys[1], String(clamp(parseInt(x.h, 10) || 720, 320, 16384))],
    ],
    autonum: (f, x) => [[f.sec, f.key, x.auto ? 'auto' : String(clamp(Math.round(parseFloat(x.num)) || f.numDefault, f.min, f.max))]],
    range: (f, x) => [[f.sec, f.key, (x / 100).toFixed(2)]],
    path: (f, x) => [[f.sec, f.key, fwd(x)]],
    text: (f, x) => [[f.sec, f.key, String(x).trim()]],
    number: (f, x) => [[f.sec, f.key, String(clamp(parseInt(x, 10) || 0, f.min, f.max))]],
    language: () => [],
  };

  const all = SECTIONS.flatMap((s) => s.fields.map((f) => ({ ...f, file: s.file })));
  const model = {};
  const orig = {};
  for (const f of all) {
    model[f.id] = READ[f.type](f);
    orig[f.id] = JSON.stringify(model[f.id]);
  }
  const dirtyFields = () => all.filter((f) => JSON.stringify(model[f.id]) !== orig[f.id]);

  // ----- markup -----
  const dis = (f) => (exists[f.file] ? '' : 'disabled');
  const CONTROL = {
    select: (f, x) => {
      const opts = f.options.some(([v]) => String(v) === String(x)) ? f.options : [...f.options, [x, x]];
      return `<select data-f="${f.id}" ${dis(f)}>${opts.map(([v, l]) => `<option value="${esc(v)}" ${String(v) === String(x) ? 'selected' : ''}>${esc(optName(l))}</option>`).join('')}</select>`;
    },
    toggle: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" ${x ? 'checked' : ''} ${dis(f)}>${esc(t(x ? f.onLabel || 'common.on' : f.offLabel || 'common.off'))}</label>`,
    size: (f, x) => `<input type="number" data-f="${f.id}" data-part="w" min="320" max="16384" value="${esc(x.w)}" ${dis(f)}><span class="unit">x</span><input type="number" data-f="${f.id}" data-part="h" min="320" max="16384" value="${esc(x.h)}" ${dis(f)}>`,
    autonum: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" data-part="auto" ${x.auto ? 'checked' : ''} ${dis(f)}>${esc(optName(f.autoLabel))}</label><input type="number" data-f="${f.id}" data-part="num" min="${f.min}" max="${f.max}" value="${esc(x.num)}" ${x.auto || !exists[f.file] ? 'disabled' : ''}><span class="unit">${esc(optName(f.unit))}</span>`,
    range: (f, x) => `<input type="range" data-f="${f.id}" min="0" max="100" step="1" value="${x}" ${dis(f)}><span class="unit" data-readout="${f.id}" style="min-width: 3.5ch; text-align: right">${x}${esc(optName(f.unit || '%'))}</span>`,
    text: (f, x) => `<input type="text" data-f="${f.id}" spellcheck="false" placeholder="${esc(f.placeholder ? optName(f.placeholder) : '')}" value="${esc(x)}" ${dis(f)}>`,
    number: (f, x) => `<input type="number" data-f="${f.id}" min="${f.min}" max="${f.max}" value="${esc(x)}" ${dis(f)}>`,
    path: (f, x) => `<input type="text" data-f="${f.id}" spellcheck="false" placeholder="${esc(f.placeholder ? optName(f.placeholder) : '')}" value="${esc(x)}" ${dis(f)}><button class="btn" data-browse="${f.id}" ${dis(f)}>${icon('folder-open')}${esc(t('common.browse'))}</button>`,
    language: (f, x) => `<select data-f="${f.id}">${listLangs().map((l) => `<option value="${esc(l.code)}" ${l.code === x ? 'selected' : ''}>${esc(l.name)}</option>`).join('')}</select>`,
  };

  const fieldHtml = (f) => `
    <div class="field">
      <label class="name">${esc(t(f.label))}${f.hint ? `<span class="hint">${esc(t(f.hint))}</span>` : ''}${f.status ? `<span class="hint" data-status="${f.status}">${esc(t('settings.files.checking'))}</span>` : ''}</label>
      <div class="control">${CONTROL[f.type](f, model[f.id])}</div>
    </div>`;

  const barText = (n) => (n ? (n > 1 ? t('common.unsavedMany', { count: n }) : t('common.unsavedOne')) : t('common.noChanges'));

  function draw() {
    const n = dirtyFields().length;
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>${esc(t('settings.title'))}</h1>
          <p class="dim">${esc(t('settings.introBefore'))}<code>${esc(data.game_dir)}</code>${esc(t('settings.introAfter'))}</p>
        </div>
        ${status.state === 'running' ? `<div class="banner warn">${icon('triangle-alert')}<div>${esc(t('settings.gameRunning'))}</div></div>` : ''}
        ${!data.controls_exists ? `<div class="banner info">${icon('info')}<div>${esc(t('settings.noControlsBefore'))}<code>${esc(data.game_dir)}</code>${esc(t('settings.noControlsAfter'))}</div></div>` : ''}
        ${SECTIONS.map((s) => `<div class="card"><h2>${icon(s.icon)}${esc(t(s.title))}</h2>${s.fields.map((f) => fieldHtml({ ...f, file: s.file })).join('')}</div>`).join('')}
        <div class="savebar">
          <span class="dim">${esc(barText(n))}</span>
          <div class="row">
            <button class="btn quiet" id="revert" ${n ? '' : 'disabled'}>${icon('undo-2')}${esc(t('common.revert'))}</button>
            <button class="btn primary" id="save" ${n ? '' : 'disabled'}>${icon('save')}${esc(t('common.save'))}</button>
          </div>
        </div>
      </div>`;
    refreshIcons();
    bind();
    checkFiles();
  }

  /** Puts the (unsaved) model back into freshly drawn markup. */
  function applyModel() {
    for (const f of all) {
      const x = model[f.id];
      if (f.type === 'size') {
        const w = el.querySelector(`[data-f="${f.id}"][data-part="w"]`);
        const h = el.querySelector(`[data-f="${f.id}"][data-part="h"]`);
        if (w) w.value = x.w;
        if (h) h.value = x.h;
      } else if (f.type === 'autonum') {
        const a = el.querySelector(`[data-f="${f.id}"][data-part="auto"]`);
        const num = el.querySelector(`[data-f="${f.id}"][data-part="num"]`);
        if (a) a.checked = x.auto;
        if (num) {
          num.value = x.num;
          num.disabled = x.auto || !exists[f.file];
        }
      } else if (f.type === 'range') {
        const r = el.querySelector(`[data-f="${f.id}"]`);
        if (r) r.value = x;
        const out = el.querySelector(`[data-readout="${f.id}"]`);
        if (out) out.textContent = `${x}${optName(f.unit || '%')}`;
      } else if (f.type === 'toggle') {
        const c = el.querySelector(`[data-f="${f.id}"]`);
        if (c) c.checked = !!x;
      } else {
        const c = el.querySelector(`[data-f="${f.id}"]`);
        if (c) c.value = x;
      }
    }
    updateBar();
  }

  /**
   * Switches the interface language: saved at once, then the page is redrawn in the new language with the
   * unsaved edits kept. `draw()` rebuilds the markup from `model`, and `applyModel()` puts the edited values
   * back into it, so nothing has to be read from disk again. Only the shell (navigation, window title) is
   * repainted; the page itself is not re-rendered, or the edits kept here would be dropped with this closure.
   */
  async function liveLang(code) {
    if (code === getLang()) return;
    const keep = { ...model };
    try {
      await api('save_config', { config: { ...cfg, lang: code } });
    } catch (e) {
      toast(errText(e), 'error');
      return;
    }
    cfg.lang = code;
    // Keep the in-game F1 menu in step: it reads [gfx] ui_language from fefw.ini.
    // zh maps to the zh pack; any other launcher language falls back to English there.
    try {
      await api('write_settings', { edits: [{ file: 'game', section: 'gfx', key: 'ui_language', value: code === 'zh' ? 'zh' : 'auto' }] });
    } catch (e) { /* fefw.ini may not exist yet; the menu keeps its current language */ }
    await setLang(code, () => {
      for (const id of Object.keys(keep)) model[id] = keep[id];
      // The switch was saved above, so it is not a pending edit: the picker shows the new language and the
      // save bar does not count it. Everything else keeps the value it had before the switch.
      model.lang = code;
      orig.lang = JSON.stringify(code);
      draw();
      applyModel();
      ctx.shell(); // navigation labels and the window title; the page keeps the edits above
    });
  }

  // The upscaler files the game will find with the folders as typed (found only when every file a feature needs is in one folder).
  let checkSeq = 0;
  async function checkFiles() {
    const seq = ++checkSeq;
    const r = await api('upscaler_files', { fsrDir: fwd(model.fsr_dll || ''), dlssDir: fwd(model.dlss_dll || '') }).catch(() => null);
    if (seq !== checkSeq || !r) return;
    const line = (name, x) => `${t(name)}: ${x.found ? t('settings.files.found', { detail: x.detail }) : t('settings.files.notFound', { detail: fileDetail(x.detail) })}`;
    const put = (which, text) => el.querySelectorAll(`[data-status="${which}"]`).forEach((n) => { n.textContent = text; });
    put('fsr', `${line('settings.files.upscaling', r.fsr_upscaling)}. ${line('settings.files.frameGen', r.fsr_framegen)}.`);
    put('dlss', `${line('settings.files.dlss', r.dlss)}. ${line('settings.files.dlssFrameGen', r.dlss_framegen)}.`);
  }
  let checkTimer = 0;

  function bind() {
    const byId = Object.fromEntries(all.map((f) => [f.id, f]));
    el.querySelectorAll('[data-f]').forEach((input) => {
      const f = byId[input.dataset.f];
      const part = input.dataset.part;
      const onChange = () => {
        const v = input.type === 'checkbox' ? input.checked : input.value;
        if (part) model[f.id] = { ...model[f.id], [part]: v };
        else model[f.id] = f.type === 'range' ? parseInt(v, 10) : v;
        if (f.type === 'language') {
          liveLang(v);
          return;
        }
        if (f.type === 'range') {
          el.querySelector(`[data-readout="${f.id}"]`).textContent = `${model[f.id]}${optName(f.unit || '%')}`;
          updateBar();
        } else if (input.type === 'text' || input.type === 'number') {
          updateBar();
          if (f.status) {
            clearTimeout(checkTimer);
            checkTimer = setTimeout(checkFiles, 300);
          }
        } else {
          draw(); // toggles and selects change dependent markup (labels, disabled inputs)
        }
      };
      input.addEventListener(input.type === 'range' || input.type === 'text' || input.type === 'number' ? 'input' : 'change', onChange);
    });
    el.querySelectorAll('[data-browse]').forEach((b) =>
      b.addEventListener('click', async () => {
        const f = byId[b.dataset.browse];
        const picked = await api('pick_folder', { title: t('common.choose', { label: t(f.label) }), start: model[f.id] || null });
        if (picked) {
          model[f.id] = picked;
          draw();
        }
      }),
    );
    el.querySelector('#revert').addEventListener('click', () => {
      for (const f of all) model[f.id] = JSON.parse(orig[f.id]);
      draw();
    });
    el.querySelector('#save').addEventListener('click', save);
  }

  function updateBar() {
    const n = dirtyFields().length;
    const bar = el.querySelector('.savebar');
    if (!bar) return;
    bar.querySelector('span').textContent = barText(n);
    bar.querySelector('#save').disabled = !n;
    bar.querySelector('#revert').disabled = !n;
  }

  async function save() {
    const edits = [];
    const cfgNew = { ...cfg };
    for (const f of dirtyFields()) {
      for (const [sec, key, value] of WRITE[f.type](f, model[f.id])) {
        if (f.file === 'config') {
          cfgNew[key] = key === 'minimize_on_play' ? value === '1' : key === 'jobs' ? parseInt(value, 10) || 0 : key === 'runner' ? String(value).trim() : fwd(value);
        } else {
          edits.push({ file: f.file, section: sec, key, value });
        }
      }
    }
    try {
      if (edits.length) await api('write_settings', { edits });
      if (JSON.stringify(cfgNew) !== JSON.stringify(cfg)) await api('save_config', { config: cfgNew });
      toast(t('common.settingsSaved'), 'ok');
      ctx.go('settings');
    } catch (e) {
      toast(errText(e), 'error');
    }
  }

  draw();
}
