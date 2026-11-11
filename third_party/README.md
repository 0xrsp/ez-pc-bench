# Third-party code

| Library | Version | Source | Files kept |
|---|---|---|---|
| Dear ImGui | v1.93.0 (`c1aaac65`) | https://github.com/ocornut/imgui | core sources + headers, `backends/imgui_impl_win32.*`, `backends/imgui_impl_dx11.*`, LICENSE |
| Inter (font, SIL OFL 1.1) | 4.1 | https://github.com/rsms/inter/releases/tag/v4.1 | `extras/ttf/Inter-Regular.ttf`, `Inter-Medium.ttf`, LICENSE.txt — embedded in the GUI exe via `src/app/gui.rc` |

Update: copy the same files from a newer release tag, bump the table, rebuild.
