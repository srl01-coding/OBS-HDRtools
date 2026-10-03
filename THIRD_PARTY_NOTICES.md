# Third-party notices

| Component | Use | Licence | Notes |
|---|---|---|---|
| obs-plugintemplate @ 3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc | Build system, CI workflows, `plugin-support` | GPL-2.0 (template `LICENSE`) | Copied and modified (buildspec, CMake sources). |
| OBS Studio (libobs) | Linked API, headers | GPL-2.0-or-later | Not redistributed. Numerical constants (Rec.2020->Rec.709 matrix) match libobs `color.effect`; they are derived colorimetric facts. |
| StreamFX | Behaviour reference only | GPL-2.0 text / README says GPLv3+ (unresolved) | **No code copied.** See `docs/STREAMFX_REFERENCE.md`. |
| ITU-R BT.2100 HLG constants | Pattern source maths | Standard | a, b, c constants. |

If code from any other project is copied or adapted, record repository, full
commit SHA, file, licence and modifications here before distributing.
