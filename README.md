# fz2kicad

A small C++17 command-line converter that imports OpenBoardView-style ASC bundles and FZ board files, then writes KiCad PCB and connectivity schematic files.

## Usage

For an ASC bundle, pass any one of `FORMAT.ASC`, `PINS.ASC`, `NAILS.ASC`, or the directory containing all three. The importer finds the bundle files case-insensitively.

```powershell
fz2kicad.exe C:\boards\FORMAT.ASC C:\boards\board.kicad_pcb
```

Encrypted `.FZ` files are first tried with a built-in OpenBoardView-compatible key. If a board uses a different key, provide the same 44 comma- or whitespace-separated hexadecimal words configured as `FZKey` in OpenBoardView:

```powershell
fz2kicad.exe C:\boards\board.fz C:\boards\board.kicad_pcb --fz-key "0x12345678, 0xabcd1234, ..."
```

An explicit `--fz-key` overrides the built-in key and must contain exactly 44 words that pass OpenBoardView's parity check. Plaintext `.FZ` files with a zlib header are detected and do not need a key.

If the output path is omitted, the converter creates a `.kicad_pcb` file beside the input. It also writes sibling `.kicad_sch` connectivity schematic and `.kicad_pro` project files, which can be opened together in KiCad. Board coordinates are converted to millimeters, the bottom-view Y axis is flipped, component pins are grouped into footprints, test nails become through-hole pads, and source outline points become `Edge.Cuts` segments.

## Current scope and limitations

- Imported input formats: OpenBoardView-style `FORMAT.ASC`/`PINS.ASC`/`NAILS.ASC` bundles and `.FZ` files.
- Encrypted `.FZ` input uses a built-in OpenBoardView-compatible key when applicable; supply `--fz-key` for boards encrypted with another key. Plaintext `.FZ` files are imported without a key. Component references, pins, nets, test points, and available part descriptions are mapped; graphical data is not exported.
- Other OpenBoardView formats (such as BRD, BDV, GenCAD, and XZZPCB) are not yet supported.
- Schematic output is a connectivity map: each part is represented by a generated box symbol with its available pin numbers/names, and matching global labels connect pins assigned to the same source net. When available, component locations from FZ records or ASC `Parts.asc` are used at 10× the board-coordinate scale, matching ASC-Kicad's placement convention; otherwise symbols are arranged automatically. Component centers retain those placements; if generated pin anchors would overlap, the corresponding synthetic symbol pins are nudged by the minimum 0.0001 mm increments to prevent unintended electrical shorts. Since boardviews do not provide logical symbol types or circuit intent, generated pins are passive and this is not a reconstructed functional schematic. Large boards may extend beyond the A0 page; use KiCad's Zoom to Objects to view the full drawing.
- The converter does not synthesize missing copper tracks or exact component-body geometry. Many boardview files do not contain that information.
- ASC files without explicit units default to inches, matching OpenBoardView's ASC importer. Header labels for inches, mils/thou, and millimeters are honored.

## Format references

- [ASC-Kicad](https://github.com/corma16/ASC-Kicad) documents the five-file tester dataset and KiCad output goal.
- [OpenBoardView](https://github.com/OpenBoardView/OpenBoardView) is the source of the ASC/FZ format conventions adapted here. It is MIT-licensed.
