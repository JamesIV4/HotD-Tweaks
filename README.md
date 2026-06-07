# House of the Dead: Overkill Tweaks

## Features

- Upscale the game's rendering resolution up to 8K, which is 6x the original fixed 720p internal resolution.
- Fix broken cutscene post-processing effects that occur when the rendering resolution is upscaled.
- Optionally disable the cutscene depth-of-field effect for a sharper image.
- Enable or disable crosshairs for PC light guns such as Sinden and Retro Shooter.
- Fix missing zombie torsos (optional in case it causes any issues on AMD graphics cards).
- Also works with _Typing of the Dead: Overkill_.

The crosshair toggle uses a compact, reversible patch against the decompressed
level data. It validates each known asset with SHA-256, changes only the
reticule bytes, recompresses atomically, and restores the original files
byte-for-byte when crosshairs are enabled again. The full replacement levels
are not bundled.

## Screenshots

[![Comparison 1](https://raw.githubusercontent.com/JamesIV4/HotD-Tweaks/refs/heads/master/Screenshots/HotD%20Overkill%20Comparison%201.png)](<![docs/screenshots/combat-gnomish-mines.png](https://raw.githubusercontent.com/JamesIV4/HotD-Tweaks/refs/heads/master/Screenshots/HotD%20Overkill%20Comparison%201.png)>)

[![Comparison 2](https://raw.githubusercontent.com/JamesIV4/HotD-Tweaks/refs/heads/master/Screenshots/HotD%20Overkill%20Comparison%202.png)](![https://raw.githubusercontent.com/JamesIV4/HotD-Tweaks/refs/heads/master/Screenshots/HotD%20Overkill%20Comparison%202.png)
