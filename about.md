## Automatic Macro Maker

This mod automatically plays any Geometry Dash level in a frozen simulation, deciding exactly when to jump.  
It then exports the jump sequence to a `.gdr` macro file you can use with real bots.

**Developer:** entity12208  
**Platform:** Windows  
**Requires:** Geode SDK

---

### How it works
- Pauses real gameplay.
- Runs a deterministic solver on the in-game state.
- Finds a perfect sequence of jumps to complete the level.
- Saves the run as a `.gdr` macro.

---

**UI Controls**
- **"M" Button**: Start macro generation.
- **Export**: Save `.gdr` to disk.
- **X**: Close menu without saving.
