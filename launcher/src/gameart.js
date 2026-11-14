/*
 * The launcher drawn with the game's own menu art, read from the player's own game files when the launcher starts
 * (Settings, Launcher, "Use the game's assets"). Nothing is copied or cached: the Rust side hands over a layout archive
 * of the dump (read_game_layout), the pictures the launcher needs are decoded here and live in memory until it closes.
 * Without the files, or when anything in here fails, the launcher keeps its own drawn look (theme.css).
 *
 * Formats (as this game stores them):
 *   archive  SARC: SFAT (name hash, name offset / 4, data start, data end per file) and SFNT (the names)
 *   textures timg/__Combined.bntx: one BRTI per texture, block-linear ("swizzled") BC1 / BC3 / BC4 / BC5 data
 *
 * The first part of this file has no browser in it, so it can be run under Node against real files.
 */

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const u64 = (b, o) => u32(b, o) + u32(b, o + 4) * 4294967296;
const tag = (b, o) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);

/** The files of a SARC archive: name -> bytes (views into `b`). */
export function parseSarc(b) {
  if (tag(b, 0) !== 'SARC') throw new Error('not a SARC archive');
  const dataStart = u32(b, 12);
  const sfat = u16(b, 4);
  if (tag(b, sfat) !== 'SFAT') throw new Error('SARC without SFAT');
  const count = u16(b, sfat + 6);
  const names = sfat + 12 + 16 * count + 8;
  const files = new Map();
  for (let i = 0; i < count; i++) {
    const e = sfat + 12 + 16 * i;
    let at = names + (u32(b, e + 4) & 0xffffff) * 4;
    let name = '';
    while (b[at]) name += String.fromCharCode(b[at++]);
    files.set(name, b.subarray(dataStart + u32(b, e + 8), dataStart + u32(b, e + 12)));
  }
  return files;
}

// BRTI format word -> [kind, bytes per 4x4 block]
const FORMATS = { 0x1a01: ['bc1', 8], 0x1a06: ['bc1', 8], 0x1c01: ['bc3', 16], 0x1c06: ['bc3', 16], 0x1d01: ['bc4', 8], 0x1e01: ['bc5', 16] };

/** The textures of a BNTX: name (without the game's "^q" kind of suffix) -> { name, kind, w, h, blockHeight, offset }. */
export function listTextures(b) {
  if (tag(b, 0) !== 'BNTX') throw new Error('not a BNTX file');
  const count = u32(b, 0x24);
  const table = u64(b, 0x28);
  const out = new Map();
  for (let i = 0; i < count; i++) {
    const p = u64(b, table + 8 * i);
    if (tag(b, p) !== 'BRTI') continue;
    const fmt = FORMATS[u32(b, p + 0x1c)];
    const nameAt = u64(b, p + 0x60);
    let name = '';
    for (let k = 0, n = u16(b, nameAt); k < n; k++) name += String.fromCharCode(b[nameAt + 2 + k]);
    out.set(name.replace(/[\^+][a-z]$/, ''), {
      name,
      kind: fmt ? fmt[0] : '',
      blockBytes: fmt ? fmt[1] : 0,
      w: u32(b, p + 0x24),
      h: u32(b, p + 0x28),
      blockHeight: 1 << u32(b, p + 0x34),
      offset: u64(b, u64(b, p + 0x70)),
    });
  }
  return out;
}

/** Where block (x, y) of a block-linear texture is: 512-byte groups of 64 bytes by 8 rows, stacked `blockHeight` high. */
function blockAddress(t, x, y, groupsWide) {
  const xb = x * t.blockBytes;
  const group =
    t.offset +
    Math.floor(y / (8 * t.blockHeight)) * 512 * t.blockHeight * groupsWide +
    Math.floor(xb / 64) * 512 * t.blockHeight +
    Math.floor((y % (8 * t.blockHeight)) / 8) * 512;
  return group + (xb % 16) + (y & 1) * 16 + ((y >> 1) & 1) * 32 + ((xb >> 4) & 1) * 64 + ((y >> 2) & 1) * 128 + ((xb >> 5) & 1) * 256;
}

function colours565(b, o, alwaysFour) {
  const c0 = u16(b, o);
  const c1 = u16(b, o + 2);
  const rgb = (c) => [((c >> 11) * 527 + 23) >> 6, (((c >> 5) & 63) * 259 + 33) >> 6, ((c & 31) * 527 + 23) >> 6];
  const a = rgb(c0);
  const d = rgb(c1);
  if (alwaysFour || c0 > c1) {
    return [a, d, a.map((v, i) => Math.floor((2 * v + d[i] + 1) / 3)), a.map((v, i) => Math.floor((v + 2 * d[i] + 1) / 3)), 255];
  }
  return [a, d, a.map((v, i) => (v + d[i]) >> 1), [0, 0, 0], 0];      // the fourth colour is transparent
}

/** The eight values of a BC4-style block (also BC3's alpha). */
function ramp(a0, a1) {
  const v = [a0, a1];
  if (a0 > a1) for (let i = 1; i < 7; i++) v.push(Math.floor(((7 - i) * a0 + i * a1 + 3) / 7));
  else {
    for (let i = 1; i < 5; i++) v.push(Math.floor(((5 - i) * a0 + i * a1 + 2) / 5));
    v.push(0, 255);
  }
  return v;
}

/** Decodes one texture to RGBA (straight alpha), 8 bits a channel. */
export function decodeTexture(b, t) {
  if (!t.kind) throw new Error(`texture ${t.name}: a format the launcher does not read`);
  const bw = (t.w + 3) >> 2;
  const bh = (t.h + 3) >> 2;
  const groupsWide = Math.ceil((bw * t.blockBytes) / 64);
  const rgba = new Uint8ClampedArray(t.w * t.h * 4);
  const put = (x, y, r, g, bl, a) => {
    if (x >= t.w || y >= t.h) return;
    const o = (y * t.w + x) * 4;
    rgba[o] = r;
    rgba[o + 1] = g;
    rgba[o + 2] = bl;
    rgba[o + 3] = a;
  };
  for (let by = 0; by < bh; by++) {
    for (let bx = 0; bx < bw; bx++) {
      const o = blockAddress(t, bx, by, groupsWide);
      if (t.kind === 'bc1' || t.kind === 'bc3') {
        const c = t.kind === 'bc3' ? o + 8 : o;
        const pal = colours565(b, c, t.kind === 'bc3');
        const bits = u32(b, c + 4);
        const alpha = t.kind === 'bc3' ? ramp(b[o], b[o + 1]) : null;
        // BC3's 48 bits of alpha indices, read as two 24-bit halves
        const lo = alpha ? b[o + 2] | (b[o + 3] << 8) | (b[o + 4] << 16) : 0;
        const hi = alpha ? b[o + 5] | (b[o + 6] << 8) | (b[o + 7] << 16) : 0;
        for (let i = 0; i < 16; i++) {
          const k = (bits >>> (2 * i)) & 3;
          const a = alpha ? alpha[((i < 8 ? lo >> (3 * i) : hi >> (3 * (i - 8))) & 7)] : k === 3 ? pal[4] : 255;
          put(bx * 4 + (i & 3), by * 4 + (i >> 2), pal[k][0], pal[k][1], pal[k][2], a);
        }
      } else {
        // BC4 (one channel, shown as grey) and BC5 (two channels, red and green)
        const one = ramp(b[o], b[o + 1]);
        const two = t.kind === 'bc5' ? ramp(b[o + 8], b[o + 9]) : null;
        const idx = (base, i) => {
          const half = i < 8 ? b[base + 2] | (b[base + 3] << 8) | (b[base + 4] << 16) : b[base + 5] | (b[base + 6] << 8) | (b[base + 7] << 16);
          return (half >> (3 * (i & 7))) & 7;
        };
        for (let i = 0; i < 16; i++) {
          const r = one[idx(o, i)];
          const g = two ? two[idx(o + 8, i)] : r;
          put(bx * 4 + (i & 3), by * 4 + (i >> 2), r, g, two ? 0 : r, 255);
        }
      }
    }
  }
  return { w: t.w, h: t.h, rgba };
}

/** The box around everything that is not (nearly) transparent: { x, y, w, h }. */
export function opaqueBox(img, floor = 8) {
  let x0 = img.w, y0 = img.h, x1 = -1, y1 = -1;
  for (let y = 0; y < img.h; y++) {
    for (let x = 0; x < img.w; x++) {
      if (img.rgba[(y * img.w + x) * 4 + 3] > floor) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
    }
  }
  return x1 < 0 ? { x: 0, y: 0, w: img.w, h: img.h } : { x: x0, y: y0, w: x1 - x0 + 1, h: y1 - y0 + 1 };
}

// ---------------------------------------------------------------------------------------------------------------------
// The browser part: pictures become object URLs, CSS variables carry them, and `html.game-art` turns the rules on
// (the "game art" part of components.css).

/** A texture pack of one archive: get(name) decodes on demand. */
function pack(archive) {
  const files = parseSarc(new Uint8Array(archive));
  const bntx = files.get('timg/__Combined.bntx');
  if (!bntx) throw new Error('archive without textures');
  const list = listTextures(bntx);
  return {
    has: (name) => list.has(name) && !!list.get(name).kind,
    get(name) {
      const t = list.get(name);
      if (!t) throw new Error(`no texture ${name}`);
      return decodeTexture(bntx, t);
    },
  };
}

function canvasOf(img, box = { x: 0, y: 0, w: img.w, h: img.h }) {
  const full = document.createElement('canvas');
  full.width = img.w;
  full.height = img.h;
  full.getContext('2d').putImageData(new ImageData(img.rgba, img.w, img.h), 0, 0);
  if (box.w === img.w && box.h === img.h) return full;
  const c = document.createElement('canvas');
  c.width = box.w;
  c.height = box.h;
  c.getContext('2d').drawImage(full, box.x, box.y, box.w, box.h, 0, 0, box.w, box.h);
  return c;
}

// A data: URL, not a blob: one: the launcher's content security policy lets pictures come from itself and from data: only, and a
// blob: picture was refused in the built launcher (no cursor, no window frames), though a browser preview showed it.
const urlOf = async (canvas) => canvas.toDataURL('image/png');

/**
 * Nine pieces (corners, edges, centre) put together as one picture for `border-image`; the edges and the centre are a few
 * pixels wide. `names` replaces a piece's name where a window has several versions of it (its top left corner, say).
 */
function nineSlice(p, prefix, names = {}) {
  const part = Object.fromEntries(['TL', 'TC', 'TR', 'CL', 'C', 'CR', 'BL', 'BC', 'BR'].map((k) => [k, canvasOf(p.get(names[k] || prefix + k))]));
  const mid = 16;
  const left = part.TL.width, right = part.TR.width, top = part.TL.height, bottom = part.BL.height;
  const c = document.createElement('canvas');
  c.width = left + mid + right;
  c.height = top + mid + bottom;
  const g = c.getContext('2d');
  g.drawImage(part.TL, 0, 0);
  g.drawImage(part.TC, left, 0, mid, top);
  g.drawImage(part.TR, left + mid, 0);
  g.drawImage(part.CL, 0, top, left, mid);
  g.drawImage(part.C, left, top, mid, mid);
  g.drawImage(part.CR, left + mid, top, right, mid);
  g.drawImage(part.BL, 0, top + mid);
  g.drawImage(part.BC, left, top + mid, mid, bottom);
  g.drawImage(part.BR, left + mid, top + mid);
  return { canvas: c, slice: [top, right, bottom, left] };
}

/** A left end, a stretchable middle (its last column) and the same end mirrored on the right, as one picture for `border-image`. */
function threeSlice(leftImg) {
  const left = canvasOf(leftImg);
  const mid = 8;
  const c = document.createElement('canvas');
  c.width = left.width * 2 + mid;
  c.height = left.height;
  const g = c.getContext('2d');
  g.drawImage(left, 0, 0);
  g.drawImage(left, left.width - 1, 0, 1, left.height, left.width, 0, mid, left.height);
  g.translate(c.width, 0);
  g.scale(-1, 1);
  g.drawImage(left, 0, 0);
  return { canvas: c, slice: [0, left.width, 0, left.width] };
}

/** A white picture given a colour (the game tints these when it draws them); its shading and its soft edge stay. */
function tinted(img, top, bottom) {
  const src = canvasOf(img);
  const c = document.createElement('canvas');
  c.width = src.width;
  c.height = src.height;
  const g = c.getContext('2d');
  g.drawImage(src, 0, 0);
  g.globalCompositeOperation = 'multiply';
  const fill = g.createLinearGradient(0, 0, src.width, src.height);
  fill.addColorStop(0, top);
  fill.addColorStop(1, bottom);
  g.fillStyle = fill;
  g.fillRect(0, 0, c.width, c.height);
  g.globalCompositeOperation = 'destination-in';
  g.drawImage(src, 0, 0);
  return c;
}

const cssColour = (img, x = 0, y = 0) => {
  const o = (y * img.w + x) * 4;
  return `rgba(${img.rgba[o]}, ${img.rgba[o + 1]}, ${img.rgba[o + 2]}, ${(img.rgba[o + 3] / 255).toFixed(3)})`;
};

/**
 * Reads the game's menu art and switches the launcher over to it. `read(name)` resolves to the bytes of a layout archive
 * (an ArrayBuffer), or rejects when the dump does not have it. Returns the names of the parts that were switched.
 */
export async function applyGameArt(read) {
  const root = document.documentElement;
  const done = [];
  const set = (name, value) => root.style.setProperty(name, value);
  const part = async (name, fn) => {
    try {
      await fn();
      done.push(name);
    } catch (e) {
      console.warn(`game art: ${name} keeps the launcher's own look (${e && e.message ? e.message : e})`);
    }
  };

  // the title list: the light behind the entry under the cursor
  await part('title', async () => {
    const p = pack(await read('Title.arc'));
    set('--art-streak', `url("${await urlOf(canvasOf(p.get('Title_MenuItem_Select')))}")`);
    set('--art-streak-line', `url("${await urlOf(canvasOf(p.get('Title_MenuItem_Select02')))}")`);
  });

  // windows and buttons, from the menus the game opens with +
  await part('menus', async () => {
    const p = pack(await read('CommonMenu.arc'));
    const wdw = nineSlice(p, 'SubMenuWdw_');
    set('--art-window', `url("${await urlOf(wdw.canvas)}")`);
    set('--art-window-slice', wdw.slice.join(' '));
    // a button is the plaque of a menu entry: its ends keep their shape, the middle stretches
    for (const [key, name] of [['btn', 'SystemMenu_FrmMenuItem_Default'], ['btn-select', 'SystemMenu_FrmMenuItem_Select']]) {
      const img = p.get(name);
      const box = opaqueBox(img, 40);
      set(`--art-${key}`, `url("${await urlOf(canvasOf(img, box))}")`);
      if (key === 'btn') {
        const leftEnd = Math.round(box.w * 0.17);
        const rightEnd = Math.round(box.w * 0.42);
        set('--art-btn-slice', `0 ${rightEnd} 0 ${leftEnd}`);
        set('--art-btn-ratio-left', String(leftEnd / box.h));
        set('--art-btn-ratio-right', String(rightEnd / box.h));
      }
    }
  });

  // the Settings screen's own pieces: its window, the help window, the prompt, rows, value boxes, arrows
  await part('settings', async () => {
    const p = pack(await read('Textures/Common.arc'));
    const windows = [
      ['panel', nineSlice(p, 'Common_Wdw_', { TL: 'Common_Wdw_TL00' })],
      ['info', nineSlice(p, 'Common_WdwInfo_')],
      ['dialog', nineSlice(p, 'Dialog_Wdw_')],
      ['value', threeSlice(p.get('Common_BgKey_Default_L'))],
      ['value-select', threeSlice(p.get('Common_BgKey_Select_L'))],
    ];
    for (const [key, w] of windows) {
      set(`--art-${key}`, `url("${await urlOf(w.canvas)}")`);
      set(`--art-${key}-slice`, w.slice.join(' '));
    }
    // a row is a flat fill inside a one-colour frame: the two colours are all there is to read
    set('--art-row-frame', cssColour(p.get('Common_FrmMenuItem_Default_TL')));
    set('--art-row-fill', cssColour(p.get('Common_FrmMenuItem_Default_C')));
    set('--art-arrow', `url("${await urlOf(tinted(p.get('Arrow'), '#fff6dc', '#e8dcb8'))}")`);
  });

  // the cursor (white in the file, gold on the screen)
  await part('cursor', async () => {
    const p = pack(await read('Common.arc'));
    const holder = document.getElementById('cursor');
    const url = await urlOf(tinted(p.get('Cursor'), '#ffe9a0', '#e0a92e'));
    holder.innerHTML = '';
    const img = document.createElement('img');
    img.src = url;
    img.alt = '';
    holder.appendChild(img);
  });

  const parts = ['title', 'menus', 'settings', 'cursor'];
  if (done.length) root.classList.add('game-art');
  for (const name of parts) root.classList.toggle(`game-art-${name}`, done.includes(name));
  return done;
}
