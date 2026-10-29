# Reconstructed Q2PSX fonts

Three local TrueType faces are available under `.branding/fonts/` for artwork
and supporting documents. Their illustrated specimen, rebuild sources and
per-character provenance are retained there. The directory is Git-ignored
because it includes reconstructions of commercial artwork.

Menu version 1.002 refines the whole alphabet against the original title atlas:
curved S terminals, raised C/J tips, rounded corners and joins, the original
K/R construction and Q tail, and more faithful diagonal strokes. C's upper arm
is subtly shorter and thinner, with a gently downturned terminal informed by
the original bevel shading. The Advert
and HUD font binaries remain version 1.000. See the local revision comparison
and per-glyph geometry report for the before/after evidence.

| Family | Local TTF | Use |
| --- | --- | --- |
| Q2PSX Menu | `.branding/fonts/Q2PSX-Menu-Regular.ttf` | Clean redraw of the in-game menu titles; 198 Unicode codepoints; lowercase uses capitals |
| Q2PSX Advert | `.branding/fonts/Q2PSX-Advert-Regular.ttf` | Angular promotional lettering; 198 codepoints; lowercase uses small caps |
| Q2PSX HUD Pixel | `.branding/fonts/Q2PSX-HUD-Pixel-Regular.ttf` | Original HUD pixel silhouettes with normal document punctuation; 95 printable ASCII characters |

Use Menu and Advert for display typography and retain conventional body/code
faces for long text. Both display fonts include kerning and tabular figures.
These are monochrome, unhinted project reconstructions rather than recovered
publisher font masters. Extended symbols and unsupported historical glyphs
are identified as newly drawn in `font-manifest.json`.

The [local usage and source record](../../.branding/fonts/README.md) explains
coverage, optical changes, provenance and rebuilding. The
[eight-page specimen](../../.branding/fonts/Q2PSX-Type-Specimen.pdf) includes
reference comparisons and complete character sheets. The font files were
loaded by Windows, shaped with HarfBuzz, rendered through FreeType and
embedded in that PDF. See `.branding/fonts/validation.json` for checks.

The menu/HUD references came from the user's SLES-01534 disc and this project's
inspector. The advert reference is the UK scan in the
[LaunchBox PlayStation gallery](https://gamesdb.launchbox-app.com/games/images/11167-quake-ii),
cross-checked against the US campaign archived by
[PSX Data Center](https://psxdatacenter.com/games/U/Q/SLUS-00757.html) (scan credit:
JASON). The original commercial artwork is not covered by the repository's
source-code licence. The `Create Q2 PSX design atlas` chat was supplied with
the families, paths, coverage and reconstruction notes for integration.
