# Screenshots — HTML configuration tool

Place screenshots of the HTML config tool here, named to match the references in
the root `README.md`. PNG format recommended (lossless, good for UI).

## Expected files

| File | What to capture |
|------|----------------|
| `01-header.png` | Top header with title, Connect/Save buttons, language selector (PT/EN), status |
| `02-tab-odrive.png` | ODrive tab open showing brake/power/comm/GPIO sections |
| `03-tab-controller.png` | Controller tab with PID gains + Power monitoring (spinout) sections |
| `04-tab-ffb-wheel.png` | FFB Wheel tab with all 3 groups (Force scaling, Axis effects, Slew/curve) |
| `05-tab-ffb-effects.png` | FFB Effects tab — master + per-effect gains |
| `06-tab-ffb-filters.png` | FFB Filters tab — biquad freq/Q for CF/Friction/Damper/Inertia |
| `07-tab-ffb-live.png` | FFB Live with auto-poll active, showing FFB state, counters, effects, chart |
| `08-tab-debug.png` | Debug / Status tab with live chart (vbus/ibus/Iq/Ibrake) running |
| `09-tooltip-example.png` | Mouse hover on a parameter showing the description tooltip |

## Tips

- Window width ≥ 1400px for charts and grids to look good
- Connect to the device first so values are populated (not all "—" placeholders)
- For FFB Live, run a quick ForceTest cycle so counters show non-zero values
- For Debug chart, enable Auto-refresh and let it run 10–20s to populate the trace
- Use the browser's native screenshot tool or any external tool

## Adding to the repo

Once you have the PNGs:

```bash
git add docs/screenshots/*.png
git commit -m "Add HTML config tool screenshots"
git push
```

The root README references these by relative path so they render automatically
on GitHub.
