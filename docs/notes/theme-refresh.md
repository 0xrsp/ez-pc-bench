# Theme refresh: graphite / cyan / orchid (2026-11-12)

Replaces the Material 3 look (seed #0B57D0, lavender-blue accents, pill buttons, tonal fills). Dark stays the
main look; light and "Follow Windows" work as before. Everything lives in `src/app/theme.*`; the palette table
with measured contrast is a comment above `light_palette()` in `theme.cpp`.

## Palette

Three surface steps (page < card < container = raised), one strong accent for actions and data (cyan), a
secondary accent for navigation and selection (orchid). Separation comes from 1 px outlines, not tonal fills.

| role                   | dark      | light     | use                                         | contrast vs card (d / l) |
|------------------------|-----------|-----------|---------------------------------------------|--------------------------|
| page                   | `#0B0E14` | `#F2F4F8` | window background                           |                          |
| card                   | `#11161F` | `#FFFFFF` | panels, cards                               |                          |
| container              | `#161C27` | `#EFF2F6` | raised: frames, popups (dark)               |                          |
| container_high         | `#1B2230` | `#E7EBF1` | frame hover, framed headers                 |                          |
| container_highest      | `#232C3D` | `#DCE2EA` | frame active, progress track                |                          |
| on_surface             | `#E4E9F2` | `#121826` | body text                                   | 14.9 / 17.7              |
| on_surface_variant     | `#9BA6B9` | `#4A5466` | secondary text, labels                      | 7.4 / 7.6                |
| outline                | `#5D6A80` | `#7C8799` | button / checkbox borders (non-text, 3:1)   | 3.3 / 3.6                |
| outline_variant        | `#2D3749` | `#CDD4DE` | card, group box, table lines (decorative)   |                          |
| primary                | `#3CCFEA` | `#00779C` | filled buttons, checks, sliders, data, group titles | 9.8 / 5.1        |
| on_primary             | `#002A33` | `#FFFFFF` | text on primary                             | 8.2 / 5.1 (on primary)   |
| primary_container      | `#0C3440` | `#D3F1FA` | tonal primary fill                          |                          |
| on_primary_container   | `#B4EDFA` | `#00303E` | text on it                                  | 10.4 / 11.9 (on fill)    |
| secondary (new)        | `#D07BFF` | `#9C2BC4` | selected tab / chip outline                 | 6.9 / 5.9                |
| on_secondary (new)     | `#2B0740` | `#FFFFFF` | text on secondary                           |                          |
| secondary_container    | `#2C1A3B` | `#F5E3FC` | selected tab / chip / list row fill         |                          |
| on_secondary_container | `#F0D6FF` | `#3B0B4F` | text on it                                  | 12.0 / 12.8 (on fill)    |
| ok                     | `#5BD68A` | `#17784A` | pass, done, good                            | 9.9 / 5.5                |
| error                  | `#FF6E6E` | `#C42B2B` | fail, errors                                | 6.7 / 5.6                |
| warn                   | `#FFB648` | `#9A5800` | caution, notes, banner stripe               | 10.4 / 5.6               |
| warn_container         | `#33240C` | `#FFEBCC` | warning banner                              |                          |
| on_warn_container      | `#FFE3B3` | `#3D2400` | banner text                                 | 12.1 / 12.4 (on banner)  |
| heat_lo .. heat_hi     | `#123D2B` .. `#5C1C24` | `#CBF2DA` .. `#FFD8D8` | matrix cells, green .. red |          |

All text roles pass WCAG AA (4.5:1) against card, page and container; light primary on container is 4.5.
ok / warn / error differ in hue (green / amber / red) and stay distinguishable from both accents.
`Palette` gained `secondary` and `on_secondary`; no field was removed or renamed.

## Radii (at 100 %, scaled with DPI)

| element                                  | before            | after |
|------------------------------------------|-------------------|-------|
| buttons (primary, outlined), tabs        | pill (half height)| 3 px  |
| frames: inputs, combos, sliders, headers | 6                 | 3     |
| cards / panels (`ChildRounding`)         | 12                | 4     |
| popups                                   | 8                 | 4     |
| checkboxes                               | 3                 | 2     |
| scrollbar, slider grab                   | 8 / 6             | 2     |
| icon buttons (gear: was a circle)        | pill / circle     | 3 (square for icon-only) |
| warning banner                           | 12 (child)        | 3     |
| progress bars                            | half height       | 1     |
| group boxes                              | 12 (child)        | 3, drawn |
| health chips / status boxes, LP grid cells, score bars | ~fs/2, 4, half height | 3, 2, 2 |

Window rounding stays 0 (full-viewport windows).

## Border rules

- `FrameBorderSize = 1` everywhere: inputs, combos, sliders, framed tree headers, raw `ImGui::Button`s.
  Global `ImGuiCol_Border` = outline_variant mixed 40 % toward outline (frames, popups).
- Cards (`theme_begin_panel`): card fill + 1 px outline_variant.
- `theme_button`: transparent, 1 px `outline`, on_surface text (outlined secondary button).
  `theme_button_primary`: primary fill, border in the same color.
- `theme_tab`: outlined segments (outline_variant, dim text); selected = secondary_container fill +
  secondary outline. The disk-letter toggles and Compare's suite filter use it too.
- `theme_icon_button`: surface fill + 1 px `outline`; hover tint.
- `theme_checkbox`: 1.5 px border (on_surface_variant off, primary on).
- `theme_progress`: no border (thin bar).
- Warning banner: warn_container fill, 1 px border (warn_container 55 % toward warn), 3 px warn stripe on the left.
- Popups: 1 px border, raised fill (container in dark, card in light).
- Tables keep their own flags; `TableRowBgAlt` is now a 3 % on_surface tint, so tables with `RowBg` (System
  details tables, SMART) get a faint zebra.
- `ImGuiCol_Button` is transparent: combo arrow boxes sit on the frame instead of a filled block.

## Group box (`theme_group_begin(title, width, help)` / `theme_group_end()`)

Same API plus an optional third parameter `help` (tooltip on the title; existing calls compile unchanged).
Look: 1 px outline_variant frame with 3 px corners, the title (Inter Medium, primary color) sitting on the top
line with a gap cut around it. `"##id"`-only titles give an untitled box that starts at the cursor.
Implementation: an auto-height child without its own border; the frame is stroked on the parent's draw list in
`theme_group_end()` (the child's height is known then) and the title is drawn in `theme_group_begin()`. Depth
up to 8 nested groups.

## Screens that got groups

- **Stability**: left column Tests / Run / Options / Safety (replaced the `section()` headings); right column
  Test status (table + WHEA line), Errors by logical processor (help on the title), Sensors (help on the
  title; graph calls unchanged inside), First errors. LP grid cells are outlined.
- **System details**: each section's key/value block is a "General" group, each table is a group titled with
  the table's name; the selected section in the list uses the secondary container.
- **Disk health**: Drive information (info grid, features, partitions); Self-test log; NVMe details as
  Controller / Firmware slots / Power states (help on title) / Error information log. Drive chips are
  outlined with the selected one in the secondary color; status boxes are outlined in their status color.
- **History**: Trend (picker + chart; chart call unchanged).
- **About the score** (modal from History / Compare / Score tab): Formula, How indicative is it?, Overall
  weights, Inputs, Validity flags. Bullets now wrap (ImGui's `BulletText` doesn't).
- **Main window**: the expanded System info is an untitled group under the System header.
- Settings and Benchmark settings already used groups; they pick up the new look unchanged.
- Not touched: Sensors window (being rebuilt), adv_ui (PawnIO), Compare layout (already a card + toolbar).

## Screenshots

Taken with the DPI-aware PrintWindow capture at 125 %, every window in dark and light:
`main`, `settings`, `bench`, `stability`, `system`, `health`, `history`, `compare`, plus `main-run` (after a
short `cpu.int` + smoke run) and `stability-run` (12 s CPU stability run). After only: `sensors`,
`main-system` (System expanded), `about-score`, `main-score`, `gpu-combo`. Before / after sets are in the
agent's scratchpad (`theme_shots/before`, `theme_shots/after`); none are committed.

## Notes for others

- Graphs: `ImGui::PlotLines` frames now get the 1 px frame border (global `FrameBorderSize`). The new graph
  widget should draw its own frame (outline_variant, radius 3) and use `primary` for the main series.
- The history chart and `mem_charts` / `temps_ui` backgrounds still use their own radii (left for the graph
  rewrite).
