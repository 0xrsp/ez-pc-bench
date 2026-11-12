# Themes: plain Light / Dark and named color schemes (2026-11-12)

Replaces the "graphite / cyan / orchid" palette (docs/notes/theme-refresh.md; its shapes, radii, borders and
group boxes stay, only colors changed). The default Light and Dark are now deliberately plain, and ten popular
color schemes can be picked in Settings > Appearance. Everything lives in `src/app/theme.*`: one static table
`THEMES[]` (name, ini id, dark flag, 13 role colors, 9 graph hues), the rest of the palette is derived in
`make_palette()`.

## Theme list

| Combo entry       | ini / `--theme` id   | mode  | primary (accent) | secondary (selection) |
|-------------------|----------------------|-------|------------------|-----------------------|
| Follow Windows    | `system`             | Light or Dark from the Windows app mode | | |
| Light             | `light`              | light | `#005FB8` blue   | same                  |
| Dark              | `dark`               | dark  | `#4CA2F5` blue   | same                  |
| Solarized Dark    | `solarized-dark`     | dark  | blue `#268BD2`   | violet `#6C71C4`      |
| Solarized Light   | `solarized-light`    | light | blue `#268BD2`   | violet `#6C71C4`      |
| Monokai           | `monokai`            | dark  | cyan `#66D9EF`   | purple `#AE81FF`      |
| Dracula           | `dracula`            | dark  | purple `#BD93F9` | pink `#FF79C6`        |
| Nord              | `nord`               | dark  | nord8 `#88C0D0`  | nord9 `#81A1C1`       |
| Gruvbox Dark      | `gruvbox-dark`       | dark  | yellow `#FABD2F` | aqua `#8EC07C`        |
| One Dark          | `one-dark`           | dark  | blue `#61AFEF`   | magenta `#C678DD`     |
| Catppuccin Mocha  | `catppuccin-mocha`   | dark  | mauve `#CBA6F7`  | pink `#F5C2E7`        |
| Catppuccin Latte  | `catppuccin-latte`   | light | mauve `#8839EF`  | blue `#1E66F5`        |
| GitHub Light      | `github-light`       | light | `#0969DA` blue   | same                  |

- ini: `theme=` takes any id (case-insensitive); unknown values fall back to `system`. `system` / `light` /
  `dark` are unchanged, so existing ini files keep working.
- `--light` / `--dark` work as before; new dev flag `--theme <id>` (session only, like the others: the ini keeps
  its value unless the theme is changed in Settings).
- The combo shows a swatch per entry (the theme's card color, a text line, primary / secondary-or-ok / error
  dots; Follow Windows = Light | Dark halves) and a separator before the named schemes.
- The dark flag drives `theme_is_dark()`: DWM immersive dark title bar, popup fill (raised in dark, card in
  light), alpha steps. Title bar caption = the theme's page color, title text = its text color.
- Switching themes (also between two dark ones) triggers the restyle of every window (`theme_take_change`).

## Plain Light / Dark

Neutral grays with no tint, one blue accent (also used for selected tabs / chips / rows: `secondary =
primary`), group titles in the text color (Inter Medium) instead of an accent, status colors from Windows 11
(WinUI) for Light. Reference: Windows 11 / VS Code "Modern" defaults.

| role            | Light     | Dark      |
|-----------------|-----------|-----------|
| page            | `#F3F3F3` | `#181818` |
| card            | `#FFFFFF` | `#1F1F1F` |
| raised (frames, popups in dark) | `#F7F7F7` | `#272727` |
| line (outline_variant) | `#E0E0E0` | `#333333` |
| border (outline) | `#8A8A8A` | `#6E6E6E` |
| text            | `#1F1F1F` | `#E6E6E6` |
| dim             | `#5E5E5E` | `#A3A3A3` |
| primary         | `#005FB8` (white text) | `#4CA2F5` (dark text) |
| ok / warn / error | `#0F7B0F` / `#9D5D00` / `#C42B1C` | `#6CCB5F` / `#E5B33E` / `#FF6B6B` |

## Role mapping

Rows of `THEMES[]` (theme.cpp) hold these roles; each named scheme maps its published colors as below.

| row field  | Palette role(s)                  | from the scheme                                          |
|------------|----------------------------------|----------------------------------------------------------|
| `page`     | `page` (window background)       | the scheme's darkest (dark) / dimmest (light) background: Mantle, bg0_h, Solarized base2 (light), GitHub canvas.subtle; derived one step darker where the scheme has none (Solarized Dark `#00212B`, Monokai `#1E1F1C`, Nord `#272C36`, Dracula `#21222C`, One Dark `#21252B` = its UI gutter) |
| `card`     | `card` (panels)                  | the editor background: base03 / base3, `#272822`, `#282A36`, nord0, bg0, `#282C34`, Base, white |
| `raised`   | `container` (inputs, combos, popups in dark) | the "current line" / highlight background: base02, `#3E3D32`, `#343746`, nord1, bg1, `#2C313A`, Surface0, Crust (Latte), Primer canvas.subtle |
| `line`     | `outline_variant` (cards, groups, tables, graph grid) | selection / border: `#44475A`, nord2, Surface0, `#3E4451`, Primer border.muted; Solarized Light `#D9D2BC` (derived between base2 and base1, so group frames show on base2) |
| `border`   | `outline` (button, checkbox borders) | comment / overlay color (base00 / base0, `#75715E`, `#6272A4` comment, bg4, Overlay0 / Overlay2, Primer neutral) |
| `text`     | `on_surface`                     | foreground: base1 (dark) / base02 (light), `#F8F8F2`, nord4, fg1, Text, fg.default |
| `dim`      | `on_surface_variant` (labels, secondary text) | subtext: base0 / base01, fg4, Subtext0 / Subtext1, fg.muted; derived where the comment color is under 4.5:1 (Monokai `#A6A28C`, Dracula `#A4ACCB`, Nord `#A3ACBD`; One Dark uses its fg `#ABB2BF` with a lighter `#D7DAE0` as text) |
| `title`    | `group_title` (new role)         | primary (plain Light / Dark and GitHub Light: text)      |
| `primary`  | `primary` (+ `on_primary`, `primary_container` derived) | the scheme's signature accent (table above) |
| `secondary`| `secondary` (+ `on_secondary`, `secondary_container`) | second accent; selected tabs / chips / list rows |
| `ok` / `warn` / `error` | `ok`, `warn`, `error`  | green / yellow or orange / red of the scheme            |
| `series[9]`| `series` (graph hues)            | see Graph colors                                          |

Derived in `make_palette()` (no extra table columns):

- `container_high` / `container_highest` = raised mixed 6 % / 12 % toward text.
- `on_primary` / `on_secondary` = card or text, whichever contrasts more with the fill.
- `primary_container`, `secondary_container`, `warn_container` = card mixed toward the accent (14 / 13 / 14 % in
  light, 22 / 20 / 16 % in dark); text on them = the body text color.
- `heat_lo` / `heat_hi` (matrix cells) = card mixed 25-30 % toward ok / 25-32 % toward error.

Contrast (WCAG 2.x) against the card color, from the table (`themes.py` in the agent's scratchpad):

| theme | text | dim | primary | secondary | ok | warn | error | border |
|---|---|---|---|---|---|---|---|---|
| Light | 16.5 | 6.5 | 6.3 | 6.3 | 5.4 | 5.3 | 5.7 | 3.5 |
| Dark | 13.2 | 6.5 | 6.1 | 6.1 | 8.1 | 8.5 | 5.9 | 3.2 |
| Solarized Dark | 5.6 | 4.7 | 4.1 | 3.4 | 4.7 | 4.7 | 3.2 | 3.4 |
| Solarized Light | 12.1 | 5.0 | 3.4 | 4.1 | 3.0 | 3.0 | 4.3 | 2.9 |
| Monokai | 13.9 | 5.8 | 9.0 | 5.2 | 9.6 | 6.8 | 3.9 | 3.0 |
| Dracula | 13.4 | 6.3 | 5.9 | 6.0 | 10.4 | 8.4 | 4.5 | 3.0 |
| Nord | 9.2 | 5.5 | 6.2 | 4.6 | 6.1 | 8.0 | 3.1 | 3.5 |
| Gruvbox Dark | 10.7 | 5.3 | 8.7 | 7.0 | 7.1 | 5.8 | 4.3 | 3.0 |
| One Dark | 10.0 | 6.6 | 5.9 | 4.8 | 6.9 | 8.1 | 4.4 | 3.1 |
| Catppuccin Mocha | 11.3 | 7.4 | 8.1 | 10.7 | 11.0 | 9.3 | 7.1 | 3.4 |
| Catppuccin Latte | 7.1 | 5.5 | 4.8 | 4.3 | 3.0 | 2.6 | 4.8 | 3.5 |
| GitHub Light | 15.8 | 6.1 | 5.2 | 5.2 | 5.1 | 4.9 | 5.2 | 3.0 |

Body text and dim text pass AA (4.5:1) everywhere. Solarized Light uses base02 for body text (its base00 body
color is 4.1:1 on base3) and base01 for dim. Accents and status colors are the schemes' own and are left as
published even where they are under 4.5:1 (Solarized's accents, Nord's red, Latte's peach / green), since that
is the scheme's look.

## Graph colors

- Box border = `ImGuiCol_Border` (frames), grid = `outline_variant`, background = text color at 2.5 % / 2 %:
  neutral in every theme, no longer the unit's hue. Only the data line, its fill, legend chips and stat dots use
  the unit color.
- Nine hues per theme in `GraphKind` order (load, clock, power, temperature, memory, latency, disk, compute,
  other), by hue: cyan, violet, yellow, red, magenta, blue, green, orange, gray. Where a scheme lacks a hue the
  nearest one is used or a mix is derived (Monokai memory `#D670D6` and latency `#7AA6FF`, Dracula latency
  `#8DA2F7`, One Dark memory `#D373A9`; Nord: load nord8, clock nord9, latency nord10, other nord7).
- Plain Dark = the previous dark hues (Tailwind 400); plain Light = Tailwind 600 / 700 (700 for cyan, yellow,
  green, which were under 4.5:1 on white).
- The overlay (GDI, its own dark panel, other thread) keeps using the plain Dark hues (`graph_rgb(kind, true)`,
  a static table read, no theme state).
- Sensors overview tiles (temps_ui) still tint the selected tile in its kind's hue; that is selection, not a
  graph frame.

## Sources

- Solarized: Ethan Schoonover, https://ethanschoonover.com/solarized/ (base03..base3 and the eight accents).
- Monokai: Wimer Hazenberg's original TextMate / Sublime Text "Monokai" (`#272822`, `#F8F8F2`, `#75715E`,
  `#F92672`, `#FD971F`, `#E6DB74`, `#A6E22E`, `#66D9EF`, `#AE81FF`, line highlight `#3E3D32`).
- Dracula: https://draculatheme.com/spec (Background, Current Line, Foreground, Comment, Cyan, Green, Orange,
  Pink, Purple, Red, Yellow; darker UI backgrounds `#21222C` / `#343746` from the official VS Code theme).
- Nord: https://www.nordtheme.com/docs/colors-and-palettes (nord0..nord15).
- Gruvbox: https://github.com/morhetz/gruvbox (dark mode bg0_h..fg0, bright and neutral accents).
- One Dark: Atom's One Dark syntax / UI theme (`#282C34`, `#21252B`, `#2C313A`, `#3E4451`, `#ABB2BF`,
  `#5C6370`, accents `#E06C75` `#98C379` `#E5C07B` `#D19A66` `#61AFEF` `#C678DD` `#56B6C2`).
- Catppuccin: https://catppuccin.com/palette (Mocha and Latte).
- GitHub Light: Primer primitives, light default (canvas, border, fg, accent, success, attention, danger, done).
- Plain Light / Dark: Windows 11 (WinUI) neutral surfaces and status colors, VS Code "Modern" themes for the
  accent blue; graph hues: Tailwind CSS palette.

## Screenshots

DPI-aware PrintWindow at 125 %, for every theme (via `--theme <id>`): the main window, Settings (Appearance with
the combo) and Sensors > CPU. In the agent's scratchpad: `themes_shots/` and the contact sheets
`themes_sheet_*.png`. Not committed.
