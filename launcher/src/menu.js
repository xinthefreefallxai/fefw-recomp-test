/*
 * Game-menu behaviour for the launcher: a cursor that follows the focus, the arrow keys and a controller to move it,
 * confirm and back, tabs on Q / E (L / R on a pad), the help box and the key help at the bottom right, a yes / no prompt.
 * The mouse and the Tab key keep working as in any page: this only adds to them.
 *
 * Markup it reads:
 *   .field            a settings row: up / down move between rows, left / right change a list or a switch in it
 *   .row              a line of buttons: left / right move along it
 *   .tab              a tab button; .tab.active is the open one (tabs are turned, never focused)
 *   [data-help]       text for the help box while the focus is inside
 */
import { esc } from './util.js';

const view = () => document.getElementById('view');
const nav = () => document.getElementById('nav');
const cursor = () => document.getElementById('cursor');
const helpBox = () => document.getElementById('help');
const keysBox = () => document.getElementById('keys');
const dialog = () => document.getElementById('dialog');

const FOCUSABLE = 'button:not(:disabled):not([tabindex="-1"]), input:not(:disabled), select:not(:disabled), [tabindex="0"]';
const isTyping = (el) => el && el.tagName === 'INPUT' && (el.type === 'text' || el.type === 'number');
const visible = (el) => el.offsetParent !== null || el.getClientRects().length > 0;
const onTitle = () => document.body.dataset.screen === 'title';

let opts = { back() {} };
let keytabs = [];
let defaultHelp = '';
let lastKey = { view: '', nav: '' };
let dialogDone = null;

// ----- focus stops -----
const stops = (root) => (root ? [...root.querySelectorAll(FOCUSABLE)].filter(visible) : []);
const groupOf = (el) => el.closest('.field') || el.closest('.row') || el;
const scopeOf = (el) => (dialog().contains(el) ? dialog() : nav().contains(el) ? nav() : view().contains(el) ? view() : null);

/** A selector that finds the same control again after its page was drawn anew. */
function keyOf(el) {
  if (el.dataset.f) return `[data-f="${el.dataset.f}"]${el.dataset.part ? `[data-part="${el.dataset.part}"]` : ''}`;
  if (el.id) return `#${CSS.escape(el.id)}`;
  for (const k of ['page', 'open', 'browse']) if (el.dataset[k]) return `[data-${k}="${el.dataset[k]}"]`;
  return '';
}

function focus(el) {
  if (!el) return false;
  el.focus({ preventScroll: true });
  el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  return true;
}

/** Puts the focus where it was (or on the first stop) when a redraw dropped it. */
export function restoreFocus(prefer) {
  const active = document.activeElement;
  if (active && active !== document.body && document.contains(active) && scopeOf(active)) return;
  if (!dialog().hidden) return void focus(stops(dialog())[0]);
  const order = prefer === 'nav' || (onTitle() && prefer !== 'view') ? ['nav', 'view'] : ['view', 'nav'];
  for (const name of order) {
    const root = name === 'nav' ? nav() : view();
    if (!visible(root)) continue;
    const again = lastKey[name] && root.querySelector(lastKey[name]);
    if (again && again.matches(FOCUSABLE) && visible(again)) return void focus(again);
    const first = name === 'nav' ? root.querySelector('.nav-item.active:not(:disabled)') || stops(root)[0] : stops(root)[0];
    if (first) return void focus(first);
  }
}

function moveVertical(dir) {
  const el = document.activeElement;
  const scope = scopeOf(el);
  if (!scope) return restoreFocus();
  const list = stops(scope);
  const g = groupOf(el);
  let i = list.indexOf(el);
  for (i += dir; i >= 0 && i < list.length; i += dir) {
    const other = groupOf(list[i]);
    if (other !== g) return void focus(list.find((x) => groupOf(x) === other));     // the first control of that row
  }
}

function moveHorizontal(dir) {
  const el = document.activeElement;
  const scope = scopeOf(el);
  if (!scope) return restoreFocus();
  const inGroup = stops(groupOf(el));
  const next = inGroup[inGroup.indexOf(el) + dir];
  if (next) return void focus(next);
  if (onTitle() && scope === nav() && dir > 0) return void (focus(view().querySelector(lastKey.view || 'none')) || focus(stops(view())[0]));
  if (onTitle() && scope === view() && dir < 0) return void (focus(nav().querySelector(lastKey.nav || 'none')) || focus(stops(nav())[0]));
}

/** Left / right on a value: the next or previous choice of a list (wrapping), the other state of a switch. */
function changeValue(el, dir) {
  if (el.tagName === 'SELECT') {
    const n = el.options.length;
    if (!n) return true;
    el.selectedIndex = (el.selectedIndex + dir + n) % n;
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return true;
  }
  if (el.tagName === 'INPUT' && el.type === 'checkbox') {
    el.click();
    return true;
  }
  if (el.tagName === 'INPUT' && el.type === 'range') {
    const step = 5 * dir;
    el.value = String(Math.min(Number(el.max), Math.max(Number(el.min), Number(el.value) + step)));
    el.dispatchEvent(new Event('input', { bubbles: true }));
    return true;
  }
  return false;
}

function turnTab(dir) {
  const tabs = [...view().querySelectorAll('.tab')];
  if (!tabs.length) return;
  const i = tabs.findIndex((x) => x.classList.contains('active'));
  tabs[(i + dir + tabs.length) % tabs.length].click();
}

function confirm() {
  const el = document.activeElement;
  if (!el || el === document.body) return restoreFocus();
  if (!changeValue(el, 1)) el.click();
}

function back() {
  if (!dialog().hidden) return closeDialog(false);
  opts.back();
}

// ----- the cursor, the help box -----
function placeCursor() {
  const c = cursor();
  const el = document.activeElement;
  if (!el || el === document.body || !scopeOf(el) || !document.hasFocus()) {
    if (!c.hidden) c.hidden = true;
    return;
  }
  // a settings row is pointed at as a whole; anything else (a button in a line of buttons) by itself
  const row = el.closest('.field');
  const r = (row || el).getBoundingClientRect();
  // a row scrolled out of its list is not pointed at (the mouse wheel moves the list without moving the focus)
  for (let box = (row || el).parentElement; box && box !== document.body; box = box.parentElement) {
    const flow = getComputedStyle(box).overflowY;
    if (flow !== 'auto' && flow !== 'scroll' && flow !== 'hidden') continue;
    const b = box.getBoundingClientRect();
    const middle = r.top + Math.min(r.height, 44) / 2;
    if (middle < b.top || middle > b.bottom) {
      if (!c.hidden) c.hidden = true;
      return;
    }
  }
  const inNav = nav().contains(el);
  const x = Math.max(2, inNav ? r.left + 8 : r.left - 30);
  const y = Math.max(2, r.top - (inNav ? 16 : 20));
  const at = `translate(${Math.round(x)}px, ${Math.round(y)}px)`;
  if (c.style.transform !== at) c.style.transform = at;
  if (c.hidden) c.hidden = false;
}

/** The row under the cursor can move without any event (a page finishing its layout, a log growing): follow it every frame. */
function followCursor() {
  placeCursor();
  requestAnimationFrame(followCursor);
}

function showHelp() {
  const el = document.activeElement;
  const from = el && el.closest ? el.closest('[data-help]') : null;
  helpBox().textContent = (from && from.dataset.help) || defaultHelp;
}

/** The text of the help box while nothing with its own help has the cursor. */
export function setHelp(text) {
  defaultHelp = text || '';
  showHelp();
}

// ----- key help -----
/**
 * The tabs at the bottom right. Each is { kbd, pad, label, run, disabled, note }: `kbd` is the key's name as `KeyboardEvent.key`
 * gives it for a letter, or "Esc" / "Enter"; `pad` the controller button (A, B, X, Y, +). A tab without `run` only explains.
 */
export function setKeys(list) {
  keytabs = list.filter(Boolean);
  keysBox().innerHTML = keytabs
    .map(
      (k, i) =>
        `<button class="keytab ${k.run ? '' : 'plain'}" tabindex="-1" data-keytab="${i}" ${k.disabled ? 'disabled' : ''}>` +
        (k.kbd ? `<span class="key kbd">${esc(k.kbd === 'Enter' ? '↵' : k.kbd.toUpperCase())}</span>` : '') +
        (k.pad ? `<span class="key pad">${esc(k.pad)}</span>` : '') +
        `<span>${esc(k.label)}</span>${k.note ? `<span class="note">${esc(k.note)}</span>` : ''}</button>`,
    )
    .join('');
}

function runKey(match) {
  const k = keytabs.find((x) => !x.disabled && x.run && match(x));
  if (k) k.run();
  return !!k;
}

// ----- the yes / no prompt -----
function closeDialog(answer) {
  const done = dialogDone;
  dialogDone = null;
  dialog().hidden = true;
  dialog().innerHTML = '';
  restoreFocus();
  if (done) done(answer);
}

/** Asks a yes / no question in the game's manner. Resolves true for yes; back answers no. */
export function confirmBox(message, yes, no) {
  if (dialogDone) closeDialog(false);
  const d = dialog();
  d.innerHTML = `<div class="box"><p>${esc(message)}</p><div class="row"><button class="btn" id="dialog-yes">${esc(yes)}</button><button class="btn" id="dialog-no">${esc(no)}</button></div></div>`;
  d.hidden = false;
  d.querySelector('#dialog-yes').addEventListener('click', () => closeDialog(true));
  d.querySelector('#dialog-no').addEventListener('click', () => closeDialog(false));
  focus(d.querySelector('#dialog-no'));
  return new Promise((resolve) => {
    dialogDone = resolve;
  });
}

// ----- keyboard -----
function onKeyDown(e) {
  if (e.altKey || e.metaKey) return;
  document.body.dataset.input = 'kbd';
  const el = document.activeElement;
  const typing = isTyping(el);
  const stop = () => {
    e.preventDefault();
    e.stopPropagation();
  };
  switch (e.key) {
    case 'ArrowUp':
      stop();
      return moveVertical(-1);
    case 'ArrowDown':
      stop();
      return moveVertical(1);
    case 'ArrowLeft':
    case 'ArrowRight': {
      if (typing) return;
      stop();
      const dir = e.key === 'ArrowLeft' ? -1 : 1;
      if (!changeValue(el, dir)) moveHorizontal(dir);
      return;
    }
    case 'Enter':
      if (typing || !el || el.tagName === 'BUTTON') return;       // a text box and a button handle Enter themselves
      stop();
      return confirm();
    case 'Escape':
      stop();
      return back();
    case 'Backspace':
      if (typing) return;
      stop();
      return back();
    default:
  }
  if (typing || e.ctrlKey || e.key.length !== 1 || !dialog().hidden) return;
  const k = e.key.toLowerCase();
  if (k === 'q' || k === 'e') {
    stop();
    return turnTab(k === 'q' ? -1 : 1);
  }
  if (runKey((x) => x.kbd && x.kbd.toLowerCase() === k)) stop();
  else if (el && el.tagName === 'SELECT' && k !== ' ') e.preventDefault();      // no jumping to a choice by its first letter
}

// ----- controller -----
const PAD = { 0: 'A', 1: 'B', 2: 'X', 3: 'Y', 4: 'L', 5: 'R', 9: '+', 12: 'up', 13: 'down', 14: 'left', 15: 'right' };
const REPEAT_FIRST = 380;
const REPEAT_NEXT = 120;
const held = {};
let polling = false;

function padAction(name) {
  document.body.dataset.input = 'pad';
  const el = document.activeElement;
  if (name === 'up') return moveVertical(-1);
  if (name === 'down') return moveVertical(1);
  if (name === 'left' || name === 'right') {
    const dir = name === 'left' ? -1 : 1;
    if (isTyping(el) || !changeValue(el, dir)) moveHorizontal(dir);
    return;
  }
  if (name === 'A') return confirm();
  if (name === 'B') return back();
  if (!dialog().hidden) return;
  if (name === 'L') return turnTab(-1);
  if (name === 'R') return turnTab(1);
  runKey((x) => x.pad === name);
}

function poll(now) {
  const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter(Boolean) : [];
  if (!pads.length) {
    polling = false;
    return;
  }
  const down = {};
  for (const p of pads) {
    for (const [i, name] of Object.entries(PAD)) if (p.buttons[i] && p.buttons[i].pressed) down[name] = true;
    const [x, y] = p.axes;
    if (y < -0.6) down.up = true;
    if (y > 0.6) down.down = true;
    if (x < -0.6) down.left = true;
    if (x > 0.6) down.right = true;
  }
  if (document.hasFocus()) {
    for (const name of Object.values(PAD)) {
      const h = held[name];
      if (!down[name]) {
        delete held[name];
      } else if (!h) {
        held[name] = now + REPEAT_FIRST;
        padAction(name);
      } else if (now >= h && ['up', 'down', 'left', 'right'].includes(name)) {
        held[name] = now + REPEAT_NEXT;
        padAction(name);
      }
    }
  }
  requestAnimationFrame(poll);
}

function startPolling() {
  if (polling) return;
  polling = true;
  requestAnimationFrame(poll);
}

/** Wires the behaviour up once. `back` is called for Esc, Backspace and B. */
export function initMenu(options) {
  opts = { ...opts, ...options };
  window.addEventListener('keydown', onKeyDown, true);
  window.addEventListener('gamepadconnected', startPolling);
  startPolling();
  document.addEventListener('focusin', (e) => {
    const scope = scopeOf(e.target);
    if (scope === view()) lastKey.view = keyOf(e.target);
    if (scope === nav()) lastKey.nav = keyOf(e.target);
    showHelp();        // first: a longer help text moves the rows
    placeCursor();
  });
  requestAnimationFrame(followCursor);
  document.addEventListener('scroll', placeCursor, true);
  window.addEventListener('resize', placeCursor);
  const moved = new ResizeObserver(placeCursor);
  moved.observe(view());
  moved.observe(helpBox());
  document.addEventListener('mousemove', () => {
    if (document.body.dataset.input === 'pad') document.body.dataset.input = 'kbd';
  });
  // the title list follows the mouse, as the game's does
  nav().addEventListener('mouseover', (e) => {
    const item = e.target.closest('.nav-item:not(:disabled)');
    if (item && document.activeElement !== item) item.focus({ preventScroll: true });
  });
  keysBox().addEventListener('click', (e) => {
    const b = e.target.closest('[data-keytab]');
    const k = b && keytabs[Number(b.dataset.keytab)];
    if (k && k.run && !k.disabled) k.run();
  });
  // a page that draws itself again drops the focus: put it back, and keep the cursor on its row
  const again = new MutationObserver(() => queueMicrotask(() => restoreFocus()));
  again.observe(view(), { childList: true, subtree: true });
  again.observe(nav(), { childList: true });
}

/** Forgets where the cursor was on the page (a new page starts at its first row). */
export function resetViewFocus() {
  lastKey.view = '';
}

/** Puts the cursor at the start of a screen: the open entry of the title list, or the first row of a panel. */
export function focusStart(name) {
  const root = name === 'nav' ? nav() : view();
  const first = name === 'nav' ? root.querySelector('.nav-item.active:not(:disabled)') || stops(root)[0] : stops(root)[0];
  if (!focus(first)) restoreFocus(name);
  placeCursor();
}
