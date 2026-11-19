# Stella

A VCV Rack sample player that lays a folder of one-shots out on a 2D map by how they sound.

- **LOAD** (or drop a folder on the module) to pick a folder. Stella listens to every WAV/AIFF (up to 1000, subfolders included, each trimmed to 2 s) and places it on the map.
- **AXIS**: AUTO arranges by overall similarity; FIXED puts brightness on X and length on Y.
- Four voices, each with **TRIG**, an **X/Y** point (knobs + CV, ±5 V covers the whole map), **SPREAD** (ring size) and **CHANCE** (+CV): the chance that a sample inside the ring plays instead of the nearest one. Closer samples are more likely.
- **DRIFT** (+CV) makes all four points wander slowly; **GRAVITY** pulls a point towards each sample it plays.
- Click or drag on the map to move the nearest voice's point.
- Each voice has its own **OUT**; patching it takes the voice out of **MIX**.

Right-click menu: load / re-scan folder, include subfolders, even out sample levels.

Build: GitHub Actions builds a Mac (Apple Silicon) `.vcvplugin` on every push.
Panel: `python3 res/make_panel.py`. Analysis test without Rack: `make test DIR=/path/to/samples`.
