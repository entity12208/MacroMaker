<img src="logo.png" width="150" alt="the mod's logo" />
# Automatic Macro Maker
**Developer:** entity12208  
**Platform:** Windows (Geode SDK)

## Overview
Automatic Macro Maker is a Geometry Dash mod for Geode that:
- Analyzes any level's geometry and triggers in real time.
- Runs a deterministic, frame-by-frame solver to find a perfect jump sequence.
- Records the solution as a `.gdr` macro file for use in external bots.
- Includes an in-game UI for starting, previewing, and exporting the macro.

This mod aims for **full accuracy** on all levels by simulating the game’s
physics inside a frozen state, ensuring the macro matches real gameplay exactly.

---

## Features
- **"M" button**: Start macro generation at any point.
- **Deterministic solver**: Predicts jumps using exact hitboxes, portals, slopes, triggers, and player physics.
- **Export Menu**: Save macro to `.gdr` format with a single click.
- **Real bot compatibility**: Output uses the same binary layout expected by most Geometry Dash bots.
- **Minimal codebase**: 2–3 `.cpp` files for easy reading and modification.

---

## Usage
1. Place the compiled `.geode` file in your Geometry Dash mods folder.
2. Launch Geometry Dash.
3. Open any level.
4. Click the **"M"** button to start macro generation.
5. Wait for the solver to complete.
6. In the menu, press **Export** to save the macro.

---

## Limitations
- Macro generation time depends on level complexity and your CPU speed.
- Only tested and supported on **Windows**.
- The `.gdr` format used matches common bot standards, but exact compatibility may vary.
