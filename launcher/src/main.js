import './theme.css';
import './layout.css';
import './components.css';
import { listen } from '@tauri-apps/api/event';
import { api, esc, icon, refreshIcons, errText, toast } from './util.js';
import { t, initLang, getLang } from './i18n.js';
import * as play from './pages/play.js';
import * as settings from './pages/settings.js';
import * as setup from './pages/setup.js';
import * as about from './pages/about.js';
import { upd, notify, checkNow, available } from './update.js';
import { initMenu, setKeys, setHelp, confirmBox, focusStart, resetViewFocus } from './menu.js';

/*
 * The screens. "play" is the title screen: its list is the navigation, as on the game's title, and its page shows the state of the
 * install beside it. The others are panel screens in the manner of the game's Settings; back (Esc, B) returns to the title.
 */
const TITLE = 'play';
const PAGES = {
  play: { mod: play, key: 'nav.play', help: 'nav.playHelp', icon: 'play' },
  setup: { mod: setup, key: 'nav.setup', help: 'nav.setupHelp', icon: 'hammer' },
  settings: { mod: settings, key: 'nav.settings', help: 'nav.settingsHelp', icon: 'settings' },
  about: { mod: about, key: 'nav.about', help: 'nav.aboutHelp', icon: 'info' },
};

const nav = document.getElementById('nav');
const view = document.getElementById('view');
let current = null;
let statusHandler = null;
let setupHandler = null;
let logHandler = null;
let titleState = { ready: false, running: false };     // from the title page: whether Play can start the game
let pageKeys = [];                                     // key help of the open page
let leaveGuard = null;                                 // asked before a panel screen is left

const ctx = {
  go,
  onStatus(fn) { statusHandler = fn; },
  onSetup(fn) { setupHandler = fn; },
  onLog(fn) { logHandler = fn; },
  /** Repaints the surrounding chrome only: window title, nav labels, language attribute. The open page keeps its state. */
  shell() { drawShell(); },
  /** Repaints the chrome and renders the open page again from disk. */
  redraw() { redrawForLang(); },
  /** The title page reports whether the game can be started, and whether it is running. */
  title(state) {
    titleState = state;
    drawNav();
  },
  /** Key help of the open page (see setKeys in menu.js); "Back" is added on a panel screen. */
  keys(list) {
    pageKeys = list || [];
    drawKeys();
  },
  /** `fn` resolves to false to keep the screen open when back is pressed (unsaved changes). */
  guard(fn) { leaveGuard = fn; },
  help(text) { setHelp(text); },
  confirm: confirmBox,
};

function drawNav() {
  const item = (id, p) => {
    const isPlay = id === TITLE;
    const label = isPlay && titleState.running ? t('play.running') : t(p.key);
    const off = isPlay && current === TITLE && (!titleState.ready || titleState.running);
    return `<button class="nav-item ${id === current ? 'active' : ''}" data-page="${id}" data-help="${esc(t(p.help))}" ${off ? 'disabled' : ''}>${icon(p.icon)}<span class="label">${esc(label)}</span></button>`;
  };
  nav.innerHTML =
    Object.entries(PAGES).map(([id, p]) => item(id, p)).join('') +
    (available() || upd.status.state === 'done'
      ? `<button class="nav-item update-pill" data-page="about" data-help="${esc(t('nav.aboutHelp'))}">${icon('circle-arrow-up')}<span class="label">${esc(
          upd.status.state === 'done' ? t('nav.restartUpdate') : t('nav.updateTo', { version: upd.check.latest.version }),
        )}</span></button>`
      : '');
  nav.querySelectorAll('[data-page]').forEach((b) =>
    b.addEventListener('click', () => {
      // Play on the title starts the game: the page holds the button that does it
      if (b.dataset.page === TITLE && current === TITLE) view.querySelector('#play')?.click();
      else go(b.dataset.page);
    }),
  );
  refreshIcons();
}

function drawKeys() {
  const onTitle = current === TITLE;
  setKeys([
    ...pageKeys,
    onTitle ? { kbd: 'Enter', pad: 'A', label: t('keys.confirm') } : { kbd: 'Esc', pad: 'B', label: t('keys.back'), run: back },
  ]);
}

/** Esc, Backspace or B: a panel screen goes back to the title. */
async function back() {
  if (current === TITLE) return;
  if (leaveGuard && !(await leaveGuard())) return;
  go(TITLE);
}

/** Repaints the chrome. Translations come from a language pack, so they are escaped like any other text. */
function drawShell() {
  document.title = t('window.title');
  nav.setAttribute('aria-label', t('nav.main'));
  document.getElementById('brand-name').textContent = t('brand.name');
  document.getElementById('screen-title').textContent = current === TITLE || !current ? t('brand.title') : t(PAGES[current].key);
  drawNav();
  drawKeys();
}

/** Redraws the shell and the open page after the interface language changed. */
function redrawForLang() {
  drawShell();
  if (current) go(current);
}

async function go(page, opts = {}) {
  current = page;
  statusHandler = null;
  setupHandler = null;
  logHandler = null;
  pageKeys = [];
  leaveGuard = null;
  resetViewFocus();
  document.body.dataset.screen = page === TITLE ? 'title' : 'panel';
  setHelp('');
  drawShell();
  view.scrollTop = 0;
  try {
    await PAGES[page].mod.render(view, { ...ctx, ...opts });
  } catch (e) {
    view.innerHTML = `<div class="page"><div class="banner danger">${icon('circle-x')}<div>${errText(e)}</div></div></div>`;
    toast(errText(e), 'error');
  }
  refreshIcons();
  focusStart(page === TITLE ? 'nav' : 'view');
}

async function boot() {
  initMenu({ back });
  await listen('game-status', (ev) => statusHandler && statusHandler(ev.payload));
  await listen('setup-status', (ev) => setupHandler && setupHandler(ev.payload));
  await listen('setup-log', (ev) => logHandler && logHandler(ev.payload));
  await listen('update-status', (ev) => {
    upd.status = ev.payload;
    notify();
  });
  upd.listeners.add(drawNav);
  let cfg = { dump_dir: '' };
  try {
    cfg = await api('get_config');
  } catch (e) {
    toast(errText(e), 'error');
  }
  await initLang(() => redrawForLang(), cfg.lang);
  drawShell();
  // first run: no game files chosen yet
  go(cfg.dump_dir ? 'play' : 'setup', { firstRun: !cfg.dump_dir });
  if (!cfg.skip_update_check) checkNow(); // quietly: a failure only shows on the About page
}

boot();

export { redrawForLang, getLang };
