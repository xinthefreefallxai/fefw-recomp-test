# Launcher language packs

The launcher ships with built-in English and Simplified Chinese. Additional
languages are plain JSON files in a `locales` folder next to `ObeliskChamber-launcher.exe`:

```
ObeliskChamber-launcher.exe
locales/
  template.json   <- starting point for a new translation
  de.json         <- example: German
```

## Making a translation

1. Copy `template.json` to `<code>.json` (`de.json`, `fr.json`, ...).
2. Set `code` to the same `<code>` as the file name. A pack whose `code` does
   not match its file name is skipped, and the reason is printed on stderr:
   the launcher lists languages by `code` but opens `<code>.json`, so a
   mismatch would show a language that cannot be loaded.
3. Fill in the strings you want to translate. The key on the left is a stable
   identifier, not text - do not change it. Entries left empty or missing fall
   back to English, so a partial translation is always safe to ship.
4. Keep the file UTF-8 (with or without BOM).
5. Start the launcher and pick the language in Settings. It appears in the
   list automatically.

```json
{
  "code": "de",
  "name": "Deutsch",
  "strings": {
    "nav.play": "Spielen",
    "nav.setup": "Einrichten"
  }
}
```

## Rules

- Only `code`, `name` and `strings` are read; everything else is ignored.
- Only string values are used; numbers, booleans, nested objects and `null`
  entries are skipped.
- Nothing in the file is ever executed: a pack cannot add code, change a
  command or reach the filesystem.
- An empty or missing key falls back to English, so a partial translation is
  always safe to ship.
- Every translated string is escaped before it reaches the page, so a pack
  cannot inject markup either.
- A file that does not parse, declares no strings, or declares a `code` that
  differs from its file name is skipped once with a note on stderr; the rest
  of the folder is unaffected. `template.json` is never listed as a language.

## Keys

`template.json` lists every key the launcher uses, grouped by prefix:
`nav.*`, `common.*`, `play.*`, `setup.*`, `settings.*`, `about.*`, plus
`window.title`. Placeholders in braces (`{version}`, `{reason}`, ...) are
filled in by the launcher - keep them as they are.
