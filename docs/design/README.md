# Design reference sources

Read [the design reference](../DESIGN.md) for the complete text standard.
The illustrated guide and atlas are generated locally in
`../../.branding/design/`; those files contain original commercial artwork and
are intentionally excluded from Git.

- `guide.json` contains the page sequence and editable guidance.
- `atlas.json` contains the 32 selected references, their sources, treatments and uses.
- `tokens.json` contains exact color values, typography and layout decisions.
- `templates/` contains four editable supporting-document starters.
- `../../scripts/build_design_reference.py` builds both PDFs and the text edition.

The text edition is generated. Edit the JSON inputs, then regenerate it rather
than making changes to `docs/DESIGN.md` that the next build would overwrite.

## Rebuilding

Use Python with ReportLab, Pillow and pypdf. The retained prepared images in
`.branding/design/assets/` and `asset-register.json` allow a text or layout
revision without re-extracting the disc or launching the game.

```powershell
python scripts/build_design_reference.py
```

Run from the repository root. `--fonts` selects the directory containing the
Windows reading faces. `--prepare` regenerates reference images from the source paths
in `atlas.json`, and requires those inputs to exist. Originals are read only.

After regeneration, render both PDFs with Poppler and inspect every page.
`layout-check.json` catches text flowing below the page's content limit;
it does not replace visual review. `contrast-check.json` records the flat
color pair calculations used in the guide.

## Evidence and rights

The original archive's `sources.json` supplies source URLs, scanner credits
and checksums. The generated `asset-register.json` records selected assets,
crop bounds, dimensions and checksums. `capture-manifest.json` records the
client and inspector commands for this illustrated edition.

The original art and disc-derived media remain separate from the source-code
licence. Keep illustrated derivatives, reconstructed artwork and local font
files out of source-control and release archives unless their inclusion has
been separately resolved for that use.

## Font reconstructions

The local `.branding/fonts/` package supplies Q2PSX Menu for identifiers,
Q2PSX Advert for short straplines, and Q2PSX HUD Pixel for labelled pixel
specimens. The guide embeds these reconstructed faces alongside Arial,
Arial Narrow Bold and Consolas for reading. See `font-manifest.json`,
`validation.json` and `Q2PSX-Type-Specimen.pdf` in that package for provenance,
coverage and limitations. These are new redraws, not confirmed historical
typeface identifications. `--brand-fonts` selects another location for the
same named files; the generator fails clearly if a required face is missing.
The [font reference](fonts.md) provides the repository's concise usage record.

## Templates

Each starter uses clearly marked `{{fields}}` to replace with verified project
information. They are templates, not release announcements or completed reports.

- [Release note](templates/release-note.md)
- [Technical report](templates/technical-report.md)
- [User guide](templates/user-guide.md)
- [Artwork brief](templates/artwork-brief.md)
