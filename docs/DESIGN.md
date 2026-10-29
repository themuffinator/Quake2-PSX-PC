# Q2PSX Design Reference

Version 1.0 | 28 October 2026

Editable source: [guide.json](design/guide.json), [atlas.json](design/atlas.json), [tokens.json](design/tokens.json).
Illustrated local editions: [Design guide](../.branding/design/Q2PSX-Design-Guide.pdf) and [Design atlas](../.branding/design/Q2PSX-Design-Atlas.pdf).
Illustrated files include commercial reference artwork and remain outside Git. This text edition contains no embedded game assets.

## 01 Q2PSX Design Reference

Artwork and supporting documents for Q2PSX-PC

### Purpose

A practical visual standard for project artwork, release material, manuals, technical reports and reference sheets. It connects the original PlayStation edition's printed identity to the current native PC recreation, with an indexed design atlas and repeatable production rules.

### Edition

Version 1.0. 28 October 2026. Based on the local original-art archive and the current working tree at a590ce9. The document describes a project design baseline; it is not an official id Software, Hammerhead, Activision or Sony brand manual.

Source basis: Local original-art archive; README.md; current launcher and renderer.

## 02 How to use this reference

Choose the kind of work first, then use the corresponding visual mode and atlas IDs. The principles and specifications are editable; the illustrated PDFs provide the visual reference.

### Artwork

Begin with identity, color, material and composition rules. Choose one dominant subject from the packaging or world atlas. Put the project identifier, subject and release information in separate visual layers. Check the work at its final size before adding detail.

### Supporting documents

Start with the pale manual mode and the nearest worked specimen. Use actual captures with short captions, conventional headings and a clear reading order. Long prose stays in a clean text area. The atlas is for selection and comparison, not a source of decorative backgrounds for every page.

### Three evidence labels

Original means an archival scan of commercial printed material. Observed means a current source constant, decoded asset or renderer output. Design means a new project rule recommended by this document. A design rule must not be cited as a recovered retail specification.

### Companion files

The local package includes the full guide, an eight-plate atlas, a preview and an asset register. docs/DESIGN.md is the searchable text edition; docs/design/guide.json, atlas.json and tokens.json are its editable inputs. The atlas PDF retains the guide's original page numbers for cross-reference.

Source basis: Document structure and usage rules are design decisions.

## 03 The visual premise

Lead with the PlayStation edition's cool blue and violet atmosphere, heavy industrial forms and prominent light effects. Use orange as the secondary campaign accent, connecting the game's scenes with the orange advertising material.

### Mass and construction

Favor broad armor planes, cylinders, rivets, recessed panels and visible joints. Let silhouette carry the image before scratches or small circuitry. A tank shoulder, a doorway or a weapon barrel can anchor a whole composition. Leave enough dark or pale space for the title to remain immediate.

### Wear with a cause

Oxidation belongs at seams, bolts and damaged edges. Keep warm rust and flesh visible within cool environments. Let blue and violet describe the light, sky or designed surround; do not wash every source image with one tint. A bright, source-linked flare can become the focal point.

### Three working modes

Scene mode uses blue-black space, cobalt machinery, violet atmosphere and pale luminous details. Campaign mode adds orange text or a limited orange panel. Document mode translates that hierarchy into cool pale paper, blue headings and small orange markers, retaining the original manual's ordered structure.

### The project promise

Use the description 'A native PC recreation of Quake II for the PlayStation'. Refer to Hammerhead's 1999 edition when context helps. Make feature claims from the current build and evidence. Reserve words such as exact or bit-identical for results established by the relevant fidelity checks.

Source basis: User art direction; original adverts; scene references A13, A29-A32; original manual; README.md and docs/FIDELITY.md.

## 04 Sources and historical boundaries

The local archive supplies contemporary reference material. Its printed presentation, regional differences and scan limitations are part of the evidence.

### Use the strongest reference

For a title or creature image, inspect the original cover. For menu lettering or HUD behavior, inspect the disc-derived atlas and the renderer. For documentation structure, inspect the PAL manual. Use code for exact current color constants and layout behavior; use scans for visual relationships.

### Keep regions identifiable

The PAL front in this archive carries Spanish code 1000373.202.SP; the English PAL back carries 1000373.201.UK. They are independent references, not a matched UK case. The US spread advert also promotes Nintendo 64, and its second page includes Nintendo 64 screenshots.

### Record limitations

These are scans, not verified publisher master files. Print screening, yellowed paper, compression and scanner color can affect appearance. Do not call a sampled scan color an official color specification or identify a font from visual resemblance alone.

### Credit and storage

Keep the original files unchanged in .branding/originals. Store crops, captures and illustrated documents in ignored local output. Retain archive links and scanner credits in the register. The repository's code licence does not turn commercial game art into GPL artwork.

Source basis: .branding/originals/README.md and sources.json; README.md Legal.

## 05 Names and identity placement

Use Q2PSX-PC as the project identifier. Use the original Quake II wordmark as referenced artwork when the composition calls for it, while keeping the native recreation's identity explicit.

### Naming

Write Q2PSX-PC in release headings, filenames and project credits. In body copy, introduce Quake II for the PlayStation once, then use the PlayStation edition or Q2PSX-PC as appropriate. Preserve the Roman numeral in Quake II. Treat PSX as the project's shorthand, not a replacement platform badge.

### Hierarchy and clear space

Place the project identifier above or beside the subject, then the document type, then version or date. Keep at least half the identifier's cap height clear on all sides; use one cap height for covers. Start at 24 px on digital artwork or 12 pt in print, then check the actual output size.

### Historical marks

Preserve the proportions, open counter and downward tail of the Q emblem. Do not rebuild the wordmark by typing an approximate font. Crop into a scan only for a labelled study or local composition, keeping the original intact. An extracted mark needs a suitable source and a clean, reviewed edge.

### Avoid ambiguity

Do not assemble Sony, publisher or developer marks into a new endorsement strip for the PC project. In archival plates they remain visible in their historical context. Keep the project descriptor next to a standalone cover-derived wordmark so the material identifies the recreation clearly.

Source basis: Original cover scans; README.md project name and description. Spacing and minimum sizes are design decisions.

## 06 The color system

Blue and violet are the primary brand colors. Orange supports selected text, labels and occasional backgrounds. The values below are authored project tokens inspired by the references, not sampled retail specifications or a change to the launcher.

| Token | sRGB | Use |
| --- | --- | --- |
| void | `#080B18` | Deep blue-black cover field |
| navy | `#101932` | Primary dark design surface |
| panel | `#1B284A` | Raised blue panel or diagram field |
| violet | `#3E315E` | Secondary cool atmosphere and chapter fields |
| blue | `#4363C7` | Cobalt graphic emphasis |
| cyan | `#8BD6F2` | Luminous detail and selected text on dark fields |
| steel | `#A7B9D7` | Cool secondary text on dark fields |
| orange | `#EF8A3B` | Secondary text on dark fields and limited background panels |
| rust | `#71371F` | Dark warm material or campaign field; pale text only |
| paper | `#EDF1FA` | Cool reading surface |
| ink | `#141B2E` | Primary document text and text on orange |
| printOrange | `#8C3F1D` | Readable orange text on pale paper |
| printBlue | `#263D85` | Document headings and links |
| white | `#F4F6FF` | Primary text on dark fields |

### Primary atmosphere

For a designed dark field, start near 60 percent blue-black or navy, 25 percent blue and violet, 10 percent pale type and cool metal, and 5 percent orange. Treat the ratio as a starting point, not a quota. Actual captures keep their original scene colors.

### Orange with a job

Use orange for a short strapline, section number, important label or one bounded background panel. An orange-led campaign variant may use a larger block, while a blue or violet image remains its partner. Keep rust material distinct from graphic orange and avoid placing orange behind every heading.

### Documents and meaning

Use cool paper and ink for long reading, deep blue for headings and printOrange for small warm text. A navy opening page can carry pale type and an orange label. Label statuses in words; blue and orange do not imply selection or error on their own.

### Transfer and provenance

Author screen artwork in sRGB and keep the token hex values in source. Proof physical output through the printer's specified profile. tokens.json separately records the current launcher colors so the new artwork palette cannot be confused with implemented UI constants.

Source basis: Design decisions following user direction; A09-A10 promotional references and A13, A29-A32 scene/light references. Launcher values retained separately in tokens.json.

## 07 Color application and readability

Check text against the surface on which it is actually drawn. Texture, transparency and imagery can invalidate a contrast calculation made on a flat swatch.

### Project thresholds

Use a minimum contrast target of 4.5:1 for body text and 3:1 for large text and essential graphic boundaries. Aim higher for captions and commands. These are production targets for this design system, not a claim that the whole game or generated PDF has received an accessibility certification.

### Orange is contextual

Orange is readable on navy but too light for body text on cool paper. Use printOrange for small warm text on a pale page, and dark ink on an orange panel. Violet and cobalt are background or graphic roles unless a tested text pair says otherwise. Give focus a clear boundary and written context.

### State is more than color

Pair ERROR with an actionable sentence, SELECTED with a marker, and complete or pending states with words. Keep labels readable in grayscale. In graphs, combine color with line style, direct labels or symbols. Do not force readers to distinguish two similarly dark swatches.

### Image treatment

Place copy outside a busy screenshot whenever possible. If a cover needs type over art, give the copy a consistently quiet image region or a solid field. Avoid transparent panels whose opacity only works against one test frame. Inspect both a small thumbnail and a full-size export.

Source basis: Measured sRGB pair ratios generated from docs/design/tokens.json. Thresholds are project design targets.

## 08 Typography and lettering

Use the reconstructed Q2PSX Menu face for identifiers and Q2PSX Advert for short technical straplines. Retain clean reading faces for headings, prose and code; show original bitmap lettering as separately labelled evidence.

| Role | Face | Size / leading |
| --- | --- | --- |
| identifier | Q2PSX Menu  | 44 / 48 pt |
| strapline | Q2PSX Advert  | 18 / 22 pt |
| pixelSpecimen | Q2PSX HUD Pixel  | Short pixel specimens |
| heading | Arial Narrow Bold | 28 / 31 pt |
| subheading | Arial Bold | 11.5 / 14 pt |
| body | Arial  | 10.5 / 15 pt |
| caption | Arial  | 8.5 / 12 pt |
| code | Consolas  | 9 / 13 pt |

### Display reconstructions

Q2PSX Menu redraws the title forms with curved terminals and rounded corners; Q2PSX Advert reconstructs angular campaign copy. Neither is an identified historical font. Menu lowercase aliases capitals; Advert lowercase uses 78 percent small caps. Check spacing at final size.

### Coverage and limits

Menu and Advert each cover 198 codepoints; HUD Pixel covers 95 printable ASCII characters. Use the manifest for exact coverage and inferred glyphs. These unhinted display faces are unsuitable for long prose. Precomposed supported accents work; combining marks and Greek, Cyrillic or CJK need another face.

### Reading faces and scale

Use Menu at 44/48 pt for a cover identifier and Advert at 18/22 for a short strapline. Use Arial Narrow Bold at 28/31 for page titles, Arial Bold at 11.5/14 for subheads, Arial at 10.5/15 for body and 8.5/12 for captions, and Consolas at 9/13 for code.

### Original pixels

Menu source cells measure 16 by 11 with advance 16, or 32 by 20 with advance 32; HUD glyphs are 8 by 8. Display aspect affects their apparent shape. Keep these assets and the new TTFs distinct. The official wordmark remains artwork; typing Menu does not reproduce it.

Source basis: docs/FORMATS.md sections 10.7 and 11.2; .branding/fonts/font-manifest.json and validation.json; type specimen PDF. Font builds are local artwork references.

## 09 Composition and page geometry

Use strong rectangular fields and a small number of alignment lines. Technical grids belong behind a specimen or diagram, where they explain placement or scale.

### Paper layouts

Use A4 portrait with 15 mm margins for guides and reports. A two-column grid with a 7 mm gutter gives room for images and short sections. Set long procedures in one column. Keep titles, captions, tables and page numbers on consistent baselines; move content to the next page before shrinking it.

### Digital artwork

Use a 12-column grid on a 1920 by 1080 master with 64 px outer margins and 24 px gutters. Put the focal subject in four to eight columns and reserve a clean title field. For a 1200 by 630 crop, recompose the layout; do not simply slice through the focal subject or identifier.

### Spacing and framing

Use a spacing ladder of 4, 8, 12, 16, 24, 32, 48 and 64 px. Favor square corners and occasional clipped corners. A frame should establish grouping or recall the UI panel. Blue rules provide structure; a short orange marker can indicate the current section without bordering every paragraph.

### Silhouette before detail

Check the composition at roughly 10 percent of its working size. The project name, subject and document type should still separate. Move detail away from letters and key facial features. If two heavy borders compete with the subject, remove one.

Source basis: Manual layout and cover composition are historical references. Dimensions and spacing are project design decisions.

## 10 Materials and surface treatment

The material atlas is a set of observed surface studies. It explains how to choose and place wear; it is not a tileable texture library.

### Armor and structural metal

Start with quiet gunmetal or olive-gray material and let blue or violet lighting reveal broad planes. Add seams, a few rivets and restrained edge highlights. Keep the strongest contrast at a meaningful edge, face or luminous source. Do not distribute bright scratches uniformly.

### Rust and heat

Use rust orange and brown in recesses, wall panels and worn corners. Place hot orange near a lamp, vent or weapon interior. Separate the surface color from the light that falls on it. On artwork with a strong blue source, keep warm material visible instead of tinting the whole scene blue.

### Paper and technical grids

Use a nearly uniform cool pale page for current supporting documents. The original manual's warmer paper remains visible in source plates. A fine blue-gray grid can support a weapon study, control diagram or dimensions. Keep it lighter than annotations and out of dense prose.

### Texture limits

Use a texture at a scale that leaves its features believable. Avoid stretched rivets, repeating stamped damage, oversized paper fibers and a global grunge overlay. A cropped source remains a reference crop: it should not be described as seamless or ready for in-game use without additional authoring.

Source basis: Atlas A21-A26; original covers and current render targets. Application rules are design decisions.

## 11 Subjects and art direction

Use the PlayStation edition's assets and packaging as the visual anchor. A strong shape and a clear construction language matter more than adding modern surface detail.

### Creatures

Build an art brief around the selected creature's silhouette, armor mass, exposed tissue, mechanical integration and signature limb or weapon. For a new illustration, name the exact reference views. Keep flesh and machine connected by an understandable joint or harness. Do not silently borrow a remaster model as a PSX reference.

### Weapons and props

Show the barrel direction, grip, receiver and silhouette at small size. Use a neutral background for inventory or technical studies. Include a side or three-quarter view plus a detail only when it answers a question. A world pickup and the weapon in the player's hands may be different assets; record which was used.

### Environment

Choose a clear architectural rhythm: machinery hall, rock passage or monumental doorway. Use blue illumination and violet atmosphere as the main artwork direction, with local warm lamps and rust preserving contrast. Pick a focal light and keep lens flares attached to plausible bright sources. A specific location retains its actual palette.

### New artwork brief

State the subject, camera, silhouette, materials, cool light direction, flare source, reserved title field and destination. Assign orange to specific text or a background block. Attach atlas IDs with a reason for each. Deliver layered source and flattened proofs, labelling new artwork separately from scans and renderer output.

Source basis: Original tank cover; model and environment specimens A11-A14, A19-A20 and A27.

## 12 The rendering character

The recognizable PSX image emerges from the rendering pipeline and authored assets. The current project treats hardware fidelity as a conformance target, with known limits recorded in its renderer audit.

### What the pixels communicate

Integer projected vertices produce visible snapping; affine UV interpolation produces texture distortion; ordered dithering and RGB555 output produce the color structure. Authored ordering and packet chains determine what appears in front. These are meaningful constraints for in-game work, not interchangeable decorative filters.

### Two kinds of image

The raw PAL capture in this set is 512 by 248. A 1024 by 768 display capture shows the fitted 4:3 presentation. Do not resize the raw image as if its pixels were square and then call the result the intended display. Label raw data and aspect-correct presentation separately.

### Show limits honestly

A still image cannot demonstrate vertex motion or texture swimming over time. Use a short deterministic sequence or measured frames for those claims. Distinguish a current port render from original hardware evidence. The source audit explicitly does not claim established hardware bit-exactness.

### Optional presentation

Capture baseline examples with CRT and bloom off. If either enhancement is the subject, provide a labelled display capture and record its setting. Keep the unfiltered raw image available for fidelity work. Do not add CRT effects, bloom or artificial damage to an evidence image in an external editor.

Source basis: docs/FIDELITY.md; docs/RENDERER-AUDIT.md; README.md Display aspect ratios, CRT and Bloom; capture manifest.

## 13 Light and lens flares

Treat light as part of the identity: cool atmosphere, luminous sources and visible flare geometry. In new artwork, a flare should reinforce a lamp or energy source and leave the typography clear.

### Keep a source and a direction

Place the bright core at the lamp, weapon energy or other credible source. Use cool blue or violet ambient light and a smaller contrasting warm source where useful. In a hero composition, one dominant flare is usually enough; preserve the authored density in actual captures.

### Use the native visual language

The documented system uses an untextured glow, an eight-spoke burst and, for some styles, polygonal ghosts along the light-to-screen-center axis. The four styles add different numbers of elements. Treat this as the reference vocabulary, not a requirement to paste the same effect everywhere.

### Flare and bloom are separate

The BASE1 example is a native engine flare with optional display bloom disabled. Bloom spreads bright areas; it does not establish the source-to-center ghost arrangement. Keep a clean baseline when comparing either effect, and label optional CRT or bloom settings.

### Application in supporting art

Reserve flare detail for a cover, chapter opener or focal image. Avoid ghost shapes behind small text, a long streak across a title, or bright halos on every border. Historical scans and evidence captures remain unretouched; newly authored effects must be labelled as artwork.

Source basis: Observed: A31-A32 and src/game/flare.c; docs/FORMATS.md section 17.3. Composition rules and axis diagram are design guidance.

## 14 Interface and information hierarchy

The original interface pairs a dark surround with luminous bitmap forms. New project tools and supporting graphics can borrow the hierarchy while retaining clear modern reading behavior.

### Selection

In the retail reconstruction, a selected row uses two gradient halves with a bright center and dark ends; the blue, red, purple and green variants are documented. Keep that narrow horizontal emphasis in game-derived examples. A new supporting UI can use a solid blue focus specimen, clearly identified as a design adaptation.

### Panels

The original panel uses two half-strength black passes, leaving roughly a quarter of the underlying scene, plus an opaque border assembled from frontend.lbm. The observed frame is a useful reference for one mission or information panel. Do not turn every document paragraph into a beveled game window.

### HUD and prompts

Keep status values together with their icon or label. A HUD crop should include enough context to explain which number means what. Label input diagrams by the device they describe. In documentation, preserve the actual key or controller symbol shown by the current build rather than inventing a universal prompt.

### New components

For a project tool, use concise labels, a visible focus boundary, explicit disabled and error states, and a readable text font. Use blue as the main surface and orange for selected labels. If a supporting graphic imitates the retail menu, call it a specimen and avoid presenting it as a shipped feature.

Source basis: docs/FORMATS.md sections 10.8 and 11.10; src/client/launcher.c; observed front and mission captures.

## 15 Capture and image handling

Every new game capture in this reference was produced by the engine or the offline inspector. Capture with deterministic headless settings and retain enough metadata to reproduce the view.

### Baseline recipe

Run from .install with the user's disc, --headless, a fixed frame count, --aspect 4:3, --no-crt and --no-bloom. Write --shot for the raw render target and --shot-display for presentation. Put --saves in a disposable task profile. The example commands in the text edition include the exact paths used by this guide.

### Choose the moment

Most examples use frame 60. BASE0 and BASE3 use frame 510 so the briefing has cleared; MATRIX5 and THEVAT use frame 180. The mission specimen keeps its panel. Record map, zone, frame, disc, build and camera. For the BASE1 angle, pass --at together with --yaw as recorded in the manifest.

### Cropping and scaling

Preserve a full source image. Record crops as pixel coordinates or normalized bounds. Use nearest-neighbor enlargement for pixel-level studies, and label the factor. Use the engine's aspect-correct display output for general illustration. Do not use AI enlargement or sharpen an evidence image.

### Comparison and caption

Match build configuration, scene, frame, camera, aspect and enhancements before calling two pictures a before-and-after pair. State what changed and where to look. A useful caption gives location, purpose and capture mode. Keep promotional crops separate from unmodified evidence captures.

Source basis: AGENTS.md capture rules; src/client/main.c options; capture manifest and logs.

## 16 Editorial voice and labels

Supporting documents should make the project easier to use, assess and reproduce. Give the reader the result first, then the procedure or evidence needed to understand it.

### Tone

Write directly and concretely. Use short headings such as Installation, Rendering or Known limits. Describe visible behavior and user impact before implementation detail. Let artwork carry atmosphere; operating instructions do not need invented military orders, in-universe jargon or theatrical warnings.

### Evidence language

Use observed for a measured render or source value, implemented for a behavior present in the current code, and proposed for a new design decision. Separate target from achieved result. A screenshot can establish visible appearance at one moment; it cannot establish all-level parity, performance or hardware correctness.

### Names and code

Preserve exact map IDs such as BASE3 in monospace. Pair them with a display name only when verified for the relevant disc; localization can change spelling. Keep flags, filenames and error messages exact. Use sentence case for prose and conventional numerals for measurements.

### Useful captions and credits

A caption can read 'BASE0, frame 510, PAL disc, 4:3 display capture; rusted rock and the narrow entry frame the route.' For an archival image, give the edition, source and scanner if known. Link sources with descriptive text, and keep the archive's file and checksum in the register.

Source basis: README.md regional naming; source manifest. Editorial rules are project design decisions.

## 17 A family of supporting documents

Use one recognizable system across documents, with density and image scale matched to the task. The four worked specimens later in this guide show the same rules in use.

| Format | Required information | Visual emphasis |
| --- | --- | --- |
| Release note | Version, date, changed behavior, setup impact, known limits | One meaningful game capture |
| Technical report | Question, reproduction, evidence, result, limits, sources | Matched figures or a labelled pipeline |
| User guide | Task, prerequisites, numbered steps, expected outcome, recovery | One action per figure |
| Artwork brief | Subject, use, dimensions, references, constraints, deliverables | Reference IDs and reserved text area |
| Atlas entry | ID, image, source, treatment, intended use, caution | A comparable specimen |

### Shared structure

Keep the project identifier, document type, edition date and page number easy to find. Repeat title hierarchy, caption treatment and table style. Use a pale page for reading and a dark opening image only where it adds a useful sense of place. Keep the source text searchable.

### Adapt density

A release note needs a compact narrative; a test report needs reproducible settings. A specimen sheet needs larger images and shorter text. Do not force every format into the same number of boxes or the same page count. Add a contents page once the document needs navigation.

Source basis: Design decisions, informed by the PAL manual. Worked specimens on pages 28-31.

## 18 Sizes and delivery specifications

Agree the destination before finishing artwork. Produce a master that can be edited, a reviewed export, and enough metadata to recreate the result.

| Deliverable | Working size | Export and check |
| --- | --- | --- |
| Guide or report | A4; 15 mm safe margins | Searchable PDF; check fonts, links and page breaks |
| Presentation cover | 1920 x 1080; 16:9 | PNG or PDF; check at 960 x 540 |
| Release preview | 1200 x 630 | PNG; check 400 px wide and destination crop |
| Square feature | 1080 x 1080 | PNG; keep title and subject inside 6% inset |
| Pixel study | Native source plus integer enlargement | Lossless PNG; label raw dimensions and scale |
| Print artwork | Final physical size; printer's bleed | Profiled PDF; proof at actual size |

### Exports

Use PNG for screenshots, pixel studies and graphics with text. Preserve original JPEG scans without recompression in the archive. Keep body text and simple diagram strokes as vectors in PDF. Use RGB for screen work. For print, confirm trim, bleed, profile and minimum line weight with the production destination.

### File naming

Use q2psx_[type]_[subject]_[variant]_vNN.ext. Evidence adds map, disc, frame and presentation, for example q2psx_capture_BASE0_PAL_f0510_display.png. Keep originals, working sources and outputs separate. A new revision replaces a superseded draft; preserve a release only when it has continuing reference value.

Source basis: Production sizes and naming are design decisions; capture requirements follow AGENTS.md.

## 19 Review before delivery

Review identity, evidence and usability independently. A visually convincing document still fails if it uses the wrong edition, hides a limitation or cannot be read at its intended size.

### Identity review

Confirm the project name, descriptor, edition, source and crop. Check that blue and violet lead the design, orange has a clear secondary role, and flares connect to visible light sources. Preserve mark proportions and keep metadata readable. Historical plates retain their original colors.

### Evidence review

Check every observed claim against code, capture metadata or the cited original. Label current port output and optional presentation effects. Match settings for comparisons. Confirm that atlas crops name their source and that scan-derived observations are not being presented as exact retail color or type specifications.

### Production review

Inspect every final PDF page at normal reading size, then examine dense pages and small captions closely. Look for clipped text, low contrast, distorted images, split tables, missing characters and orphaned headings. Check output dimensions and source links. Open the final artifact after export.

### Repository review

Keep commercial scans, decoded assets and their illustrated derivatives out of Git and release packages. Retain the text standard, tokens, atlas index and generator as project sources. Refresh the staged executable as required by AGENTS.md even when the change is documentation only.

Source basis: AGENTS.md; scripts/check_paths.py; project design review rules.

## 20 Packaging and identity

The shared cover subject provides the strongest identity cue. Regional packaging supplies different framing systems, not interchangeable source parts.

### A01 PAL front cover

**Original.** Primary reference for tank mass, warm industrial metal, white wordmark and the Q emblem.

Spanish PAL print code 1000373.202.SP. Use as an archival cover, not a newly assembled UK case.

Source: `.branding/originals/packaging/quake-ii-psx-pal-front.jpg`.
Archive: [PSX Data Center; scanner not identified](https://psxdatacenter.com/games/SLES-01534.html).

### A02 US front cover

**Original.** Compare the same central art with a different side rail and publisher band. Preserve the focal face and weapon.

NTSC U/C packaging. Scan color and wear differ from the PAL reference.

Source: `.branding/originals/packaging/quake-ii-psx-us-front.jpg`.
Archive: [QWERTYUIOP / PSX Data Center](https://psxdatacenter.com/games/U/Q/SLUS-00757.html).

### A03 English PAL back cover

**Original.** Reference for compact information hierarchy, screenshot grouping, descriptive copy and the physical spine.

UK code 1000373.201.UK. The included spines and legal marks belong to historical packaging.

Source: `.branding/originals/packaging/quake-ii-psx-pal-back-with-spines.jpg`.
Archive: [VIDEO GAME COLLECTOR / PSX Data Center](https://psxdatacenter.com/games/SLES-01534.html).

### A04 PAL disc label

**Original.** A restrained circular composition with a large quiet field and minimal identity elements.

Do not use a printed disc ring as a generic UI decoration or infer modern distribution from it.

Source: `.branding/originals/packaging/quake-ii-psx-pal-disc-label.jpg`.
Archive: [PSX Data Center; scanner not identified](https://psxdatacenter.com/games/SLES-01534.html).

Source basis: Original scans; detailed source and treatment records in the asset register.

## 21 The manual as a document system

The manual combines technical alignment, pale paper, condensed headings and small reference images. Use those relationships to organize new documents.

### A05 Manual contents

**Original.** Diagrammatic lines, title hierarchy and pale paper establish the manual's technical voice.

PDF page 4; printed page 1. Scanned paper tint is variable, not a measured production color.

Source: `.branding/design/evidence/manual-4.png`.
Archive: [HFS DB / Games Database](https://db.hfsplay.fr/games/87888-quake-ii?lang=en).

### A06 Manual controls

**Original.** Use leader lines and direct control labels to make a compact diagram readable.

PDF page 6; printed page 3. Historical controller instructions are not a current PC binding map.

Source: `.branding/design/evidence/manual-6.png`.
Archive: [HFS DB / Games Database](https://db.hfsplay.fr/games/87888-quake-ii?lang=en).

### A07 Manual arsenal

**Original.** An excellent reference for a specimen column, light technical grid and adjacent practical description.

PDF page 21; printed page 18. The reproduced scan remains historical material, not new game documentation.

Source: `.branding/design/evidence/manual-21.png`.
Archive: [HFS DB / Games Database](https://db.hfsplay.fr/games/87888-quake-ii?lang=en).

### A08 Manual enemy register

**Original.** Compare creature silhouettes at a common visual size with short role and behavior annotations.

PDF page 28; printed page 25. Scale is editorial, not a common world-unit measurement.

Source: `.branding/design/evidence/manual-28.png`.
Archive: [HFS DB / Games Database](https://db.hfsplay.fr/games/87888-quake-ii?lang=en).

Source basis: UK PAL instruction manual scan. PDF page numbers include three packaging pages before the printed manual.

## 22 Promotional composition

The campaigns are deliberately different. Borrow their composition principles selectively while keeping their historical origin and cross-platform context explicit.

### What transfers

The US campaign's warm orange field and angular technical copy support bold, bounded orange panels and short warm text in new layouts. The UK advert uses a single photographic metaphor and a compact evidence strip. Pair these campaign cues with the cooler scene palette; keep one immediate focal idea.

### What stays historical

Keep the Nintendo 64 material and historical publisher marks inside source plates. Do not use that material as evidence of the PSX renderer. The archive's promotional scans are references for art direction; they do not establish feature support in the current native recreation.

### A09 US campaign first page

**Original.** The orange campaign field, machinery frame and angular technical copy establish the warm secondary direction for new project artwork.

Cross-promotes PlayStation and Nintendo 64. Its companion page is retained in the archive, not used as PSX render evidence.

Source: `.branding/originals/advertisements/quake-ii-psx-us-advert-spread-page-1.jpg`.
Archive: [JASON / PSX Data Center](https://psxdatacenter.com/games/U/Q/SLUS-00757.html).

### A10 UK campaign advert

**Original.** One photographic metaphor, a large stamp and an inset screenshot strip create a strong reading order.

The violet and meat-red photographic campaign remains a historical reference. Its palette does not prescribe colors for evidence captures.

Source: `.branding/originals/advertisements/quake-ii-psx-uk-advert.jpg`.
Archive: [LaunchBox; scanner not identified](https://gamesdb.launchbox-app.com/games/images/11167-quake-ii).

Source basis: JASON / PSX Data Center US spread; LaunchBox UK advert; original archive manifest.

## 23 World and light

Four current renders show how location changes color and rhythm. Use the architecture and focal light to structure the composition before adding decorative texture.

### A11 BASE0 entry passage

**Observed.** Rock planes, muted warm material and a narrow route establish a compact directional composition.

PAL client display capture, frame 510, 4:3. Stationary view; no movement or hardware-equivalence claim.

Source: `.branding/design/evidence/BASE0-clean.png`.

### A12 BASE3 industrial corridor

**Observed.** Repetition, rust color and a receding corridor produce the industrial rhythm.

PAL client display capture, frame 510. This is the current port, with CRT and bloom disabled.

Source: `.branding/design/evidence/BASE3-clean.png`.

### A13 SECURITY blue machinery

**Observed.** Cold blue equipment, symmetrical stairs and a dark middle field separate structure from luminous detail.

PAL client display capture, frame 60. Lighting is scene-specific; do not apply the blue tint indiscriminately.

Source: `.branding/design/evidence/SECURITY.png`.

### A14 WASTE1 rock and green light

**Observed.** A warm rock envelope meets a localized green source. The opening provides the main focal shape.

PAL client display capture, frame 60. The view weapon and HUD belong to the actual capture.

Source: `.branding/design/evidence/WASTE1.png`.

Source basis: SLES-01534 disc; current client; deterministic headless display captures; CRT and bloom off.

## 24 Cool atmosphere and native flares

Blue architecture and violet sky establish the primary direction. A warm lamp and its native flare show how a concentrated orange light can sit within the wider system. Source images keep their original colors.

### A29 THEVAT blue chamber

**Observed.** Cobalt-lit walls and deep blue recesses establish a cool environment across most of the image. Useful for the primary brand atmosphere.

PAL client display capture, frame 180, default spawn. No tint or optional bloom applied.

Source: `.branding/design/evidence/THEVAT-blue.png`.

### A30 MATRIX5 violet sky

**Observed.** The violet sky and purple-blue upper light coexist with warm rock and orange illumination. Keep both temperature families legible.

PAL client display capture, frame 180, default spawn. A location study, not a claim that every level shares this palette.

Source: `.branding/design/evidence/MATRIX5-sky.png`.

### A31 BASE1 source-linked flare

**Observed.** A bright lamp anchors the glow and narrow burst. Low-contrast ghost elements extend toward and beyond the frame center.

PAL client, frame 60; recorded position and yaw 256. Native flare with CRT and bloom off.

Source: `.branding/design/evidence/BASE1-flare.png`.

### A32 Native flare detail

**Observed.** The bright core, tapered rays and faint polygonal ghosting remain attached to a visible source. Compare the dark space around the effect.

Crop of A31, without recoloring or added glow. The exact element behavior is documented in FORMATS section 17.3.

Source: `.branding/design/evidence/BASE1-flare.png`.
Crop bounds in source pixels: `[180, 225, 850, 475]`.

Source basis: SLES-01534; current client; headless display captures. THEVAT/MATRIX5 frame 180, BASE1 frame 60; CRT and bloom off.

## 25 Interface and bitmap forms

The complete interface and its source sheets answer different questions. A displayed screen demonstrates hierarchy; a decoded sheet demonstrates pixel construction.

### A15 Front-end selection

**Observed.** The large emblem, quiet black field and narrow blue band establish an economical hierarchy.

PAL client display capture, frame 60. The host input prompt is current project behavior.

Source: `.branding/design/evidence/front.png`.

### A16 Mission panel

**Observed.** A luminous metal bracket separates a compact mission message from the scene behind it.

PAL client display capture, frame 60, --objectives 1. The panel is intentionally visible.

Source: `.branding/design/evidence/mission.png`.

### A17 Frontend bitmap sheet

**Observed.** Inspect the menu letterforms, small symbols, prompts and frame components as source pixels.

Inspector VRAM view using its text palette. Individual glyph draw palettes can differ; this is not a scalable font.

Source: `.branding/design/evidence/menu.ppm.frontend.png`.

### A18 HUD text and icon specimen

**Observed.** Inspect runtime font colors, message vocabulary and fixed-pixel spacing in the inspector's overlay specimen.

Offline HUD test composition. It is not a captured gameplay event or a new menu layout.

Source: `.branding/design/evidence/hud.png`.

Source basis: Current client and q2psx-inspect; frontend.lbm and chars.lbm decoded from the user's disc.

## 26 Models and status information

Keep silhouette studies separate from scene lighting and HUD evidence. The neutral model views below are inspector renders, not marketing renders or original hardware captures.

### A19 Soldier model study

**Observed.** The front view makes the helmet, armor mass, exposed arms and held weapon legible without a scene behind them.

Offline BASE0 model render; clip 0, frame 0, yaw 2048. Inspector view and lighting. Recorded crop; not original hardware evidence.

Source: `.branding/design/evidence/soldier-front.png`.
Crop bounds in source pixels: `[141, 121, 373, 350]`.

### A20 Blaster model study

**Observed.** Inspect the muzzle, compact receiver and first-person mesh construction. Do not infer a separate world pickup from this view-model asset.

Offline BASE0 Blaster G render; clip 0, frame 0, yaw 256. This is a view-model asset. Framed with a recorded crop.

Source: `.branding/design/evidence/blaster.png`.
Crop bounds in source pixels: `[161, 187, 344, 290]`.

### A27 BOSS1 focal doorway

**Observed.** Dark foreground structure isolates a warm doorway and gives the composition a single deep focal point.

PAL client display capture, frame 60. This is an environment and status reference, not a boss model portrait.

Source: `.branding/design/evidence/BOSS1.png`.

### A28 HUD status detail

**Observed.** Health, armor and ammunition are grouped with icons. Pale outlined numerals stay readable against the darker scene.

Crop of A13. The values are state-dependent and are not prescribed layout constants.

Source: `.branding/design/evidence/SECURITY.png`.
Normalized crop bounds: `[0, 0.78, 0.8, 0.98]`.

Source basis: q2psx-inspect model BASE0; current BOSS1 and SECURITY captures; exact commands in the manifest.

## 27 Material and detail studies

These six crops isolate useful visual behavior. Each retains its source colors and geometry; none is advertised as a seamless game texture.

### A21 Armor plane

**Original.** Broad olive-gray planes, a restrained bevel and a few construction details carry the heavy material.

Cover detail crop. Preserve scale and mass; this is not a repeatable texture.

Source: `.branding/originals/packaging/quake-ii-psx-pal-front.jpg`.
Archive: [PSX Data Center](https://psxdatacenter.com/games/SLES-01534.html).
Normalized crop bounds: `[0.26, 0.14, 0.59, 0.34]`.

### A22 Oxidized panel

**Original.** Directional warm corrosion breaks up a large surface while leaving the underlying plane legible.

Cover detail crop. Printed halftone and scan variation remain in the image.

Source: `.branding/originals/packaging/quake-ii-psx-pal-front.jpg`.
Archive: [PSX Data Center](https://psxdatacenter.com/games/SLES-01534.html).
Normalized crop bounds: `[0.18, 0.59, 0.37, 0.8]`.

### A23 Weapon cylinder

**Original.** Orange cylindrical machinery, a dark housing and small metallic edges create depth with few shapes.

Cover detail crop. Treat the orange as material and light context, not a universal warning color.

Source: `.branding/originals/packaging/quake-ii-psx-pal-front.jpg`.
Archive: [PSX Data Center](https://psxdatacenter.com/games/SLES-01534.html).
Normalized crop bounds: `[0.77, 0.08, 0.94, 0.3]`.

### A24 Blue equipment

**Observed.** A concentrated blue detail field contrasts with the darker structural surround.

Crop of A13. Light and surface are combined in this render; it is not an albedo texture.

Source: `.branding/design/evidence/SECURITY.png`.
Normalized crop bounds: `[0.12, 0.04, 0.4, 0.27]`.

### A25 Rock surface

**Observed.** Irregular broad shapes and warm-to-green variation make the organic enclosure distinct from metal.

Crop of A14. Perspective and scene lighting are preserved; the crop is not seamless.

Source: `.branding/design/evidence/WASTE1.png`.
Normalized crop bounds: `[0.04, 0.1, 0.34, 0.44]`.

### A26 Panel corner

**Observed.** A compact bevel and clipped bracket turn a small source tile into a recognizable frame component.

46 x 29 source-pixel crop at (32,224). Magnified without smoothing for study.

Source: `.branding/design/evidence/menu.ppm.frontend.png`.
Crop bounds in source pixels: `[32, 224, 78, 253]`.

Source basis: Cover and engine-render crops. Normalized crop bounds and source checksums are in the asset register.

## 28 Release note layout

A release note should tell players what changed, show one relevant image and explain what they need to do next. This is a designed example, not a claim that a new release has shipped.

### Content sequence

Start with the release number and date. Follow with one sentence about user-visible behavior. Group changes into a short narrative, add setup or save implications only when relevant, then state known limits and the verification performed. Use a direct link to the release or installation instructions.

### Figure and metadata

Use a wide crop only if it preserves the change being discussed. Put the map, disc, frame and presentation mode below it. A before-and-after requires matched settings. Keep the project identifier above the figure and leave the bottom region for useful version and source information.

### Typography

Use a 28-32 pt condensed title, 11-12 pt lead, 10.5 pt body and 8.5 pt caption. Use one orange label for the release category, choosing printOrange on a pale page. A blue link is enough interactive emphasis; a dense row of badges weakens the main message.

### Reusable writing frame

Write: what the user encounters; what now happens; where it applies; any remaining limit; how it was checked. Replace each field with verified facts. Avoid broad claims such as complete parity when the evidence describes one map or one rendered state.

Source basis: Design specimen using A13 as a real capture. It announces no actual new release.

## 29 Technical report layout

Technical documents use the same visual family but give priority to reproducibility and comparison. The source, process and visible result should be traceable.

### Opening

State the concrete question and result in the first paragraph. Identify the tested build and disc. If there is no conclusion yet, name the remaining uncertainty. Follow with the smallest reproduction that demonstrates the behavior and the exact settings that can affect it.

### Evidence

Place comparison figures at equal size with labels that name the actual states. Include a close-up when the relevant difference is small. Put factual observations beside the image; reserve interpretation for the explanation that follows. A chart needs units, method and sample size.

### Diagram vocabulary

Use rectangles for processing stages, a file shape or named block for inputs, and arrows for direction. Blue carries the main path; an orange panel marks the stage under discussion. Keep labels descriptive and explain any omitted stages in the caption.

### Close

Record checks completed, limitations and the next decision. Link source files and tests with stable paths. Avoid screenshots of terminal text when searchable commands or a compact results table communicate the same evidence more clearly.

Source basis: Design specimen; pipeline vocabulary from docs/ARCHITECTURE.md and docs/FIDELITY.md.

## 30 User guide layout

A user guide should support a specific task from setup to a recognizable success state. Brand the page lightly so instructions remain fast to scan.

### Task framing

Give the task a plain title such as Choose a disc image. List the required input, then use numbered steps with one action each. Name labels exactly as they appear in the current UI. State what the reader should see after a successful step.

### Image choices

Use a current engine or tool render where it explains the task. Crop to the relevant control only when the surrounding context is preserved elsewhere. For controller diagrams, identify the device and distinguish a physical control label from an in-game action.

### Recovery

Place a brief recovery instruction immediately after the failure-prone step. For disc issues, explain how to check that a cue and its referenced tracks are together. Avoid presenting future support as current behavior. Link to the full troubleshooting material rather than placing every possible failure in a single diagram.

### Reading order

Keep steps in one column, captions under figures and warnings close to the relevant action. Use stable step numbering and explicit nouns instead of references such as the box on the right. Check the page at ordinary print size as well as on screen.

Source basis: Design specimen. Disc and launcher behavior from README.md and .install/Play.cmd; original controller plate A06.

## 31 Artwork brief layout

A usable brief identifies what to make, where it will appear and which references control each decision. Reference images should reduce ambiguity.

| Field | Example brief value |
| --- | --- |
| Subject and destination | Q2PSX-PC project cover; 1920 x 1080 master |
| Reference roles | A01 mass; A13/A29 blue light; A30 violet sky; A31 flare; A09 orange |
| Composition | Three-quarter subject on right; quiet title field on left |
| Color and light | Blue/violet primary; source-linked flare; orange strapline or panel |
| Text | Q2PSX-PC; Artwork reference; version and date |
| Deliverables | Layered source, sRGB PNG, 1200 x 630 adaptation |
| Acceptance | Readable at 400 px; preserved silhouette; source credits |

### Make decisions visible

Specify whether the work is a new illustration, a layout using archived art, or a renderer capture. State camera, lighting, material, title-safe area and crop constraints. Name which parts may be reinterpreted. A design reference is not an instruction to reproduce every scratch.

### Review the right things

Review silhouette and composition first, then color and light, then material detail and type. Keep separate exports for alternate crops and record the revision. Add credit and provenance information before final handoff; do not leave it to reconstruction from filenames later.

Source basis: New project brief specimen. References identify art-direction roles, not rights clearance or commissioned work.

## 32 Source register

The atlas uses a selected subset of the original archive plus reproducible current renders. The full local manifest retains 15 original media files and their checksums.

### S01 Original retail packaging

PSX Data Center's US and PAL Quake II archives. US packaging is credited there to QWERTYUIOP; the PAL back is credited to VIDEO GAME COLLECTOR. The archive also contains independent Spanish and French back covers. Local file-level details are preserved in .branding/originals/sources.json.

### S02 Contemporary advertising

JASON / PSX Data Center supplied the US two-page campaign scan. LaunchBox hosts the UK advert and the Meat Market advert. The local manifest records the publication identification of the latter as Electronic Gaming Monthly, September 1999, page 73. Only selected campaign pages appear in this guide.

### S03 Instruction manual

The 30-page UK PAL scan, SLES-01534, printed code 1000373.221.UK, is hosted by HFS DB; its metadata credits Games Database. The atlas cites both PDF page and printed page where applicable, so packaging pages are not mistaken for manual page numbers.

### S04 Current project evidence

README.md, docs/FIDELITY.md, docs/FORMATS.md, src/client/launcher.c and current tool output establish project naming, rendering constraints, menu geometry and launcher colors. Current captures identify the dirty working tree at a590ce9; they do not represent a pristine tagged release or an original-hardware test.

Source basis: Source links are clickable in the illustrated PDF and text edition; exact source files are retained in the local archive.

## 33 Asset records and reproducibility

Every atlas ID resolves to a source file, treatment and intended use. The generated asset-register.json adds source and output checksums so a future revision can identify exactly what changed.

| Record field | Required value |
| --- | --- |
| Identity | Atlas ID, title, evidence class, edition or map |
| Source | Repository-relative path; archive URL or capture command |
| Treatment | Full image, aspect-correct capture, or normalized crop bounds |
| Dimensions | Source pixel size and delivered pixel size |
| Provenance | SHA-256 of source and output; reference date |
| Use and limit | What to learn from it; what it does not establish |

### Captures in this edition

All world captures use SLES-01534, --headless, 1024x768, 4:3, CRT off and bloom off. BASE0/BASE3 use frame 510; SECURITY, WASTE1, BOSS1 and BASE1 use frame 60; MATRIX5/THEVAT use frame 180. BASE1 specifies the recorded position and yaw; the others use default spawns.

### Local reconstruction

The builder reads the original archive and retained captures, prepares the atlas images, and writes the guide, atlas and text edition. Rebuilding from the retained prepared assets preserves the illustrated edition. Refreshing captures against another build creates new evidence and requires reviewing its metadata and captions.

Source basis: scripts/build_design_reference.py; docs/design/atlas.json; generated asset-register.json and capture-manifest.json.

## 34 Maintain the design reference

Keep this guide useful by changing the smallest responsible part of the system, then regenerating and reviewing the affected outputs.

### Change a rule

Edit the relevant entry in guide.json or tokens.json and describe the reason in the document revision record. Check the worked specimens as well as the prose. A color change needs new contrast calculations; a type change needs a complete layout review; a new mark needs placement examples.

### Add an atlas entry

Choose a clear ID, identify the source, record the crop and explain what the specimen teaches. Prefer a new entry only when it adds a distinct material, format, subject or behavior. Keep old evidence when it explains a historical decision; do not accumulate interchangeable views without a purpose.

### Refresh observed material

Rebuild and stage the client, capture headlessly into a disposable save profile, inspect the render target and record the build. Keep originals unchanged. When a reference becomes obsolete, mark its scope or replace it with a documented revision. Do not silently overwrite a historical reference with new output.

### Ready for handoff

The final package should contain the reviewed guide, atlas, editable text and token sources, provenance register and relevant capture settings. Confirm links and filenames, remove disposable rendering files, and keep useful logs in the project task area. A future contributor should be able to choose a reference and understand why it belongs.

Source basis: Version 1.0 establishes this design baseline on 28 October 2026. No game behavior is changed.

## Exact capture recipe

Run from `.install/`. This example reproduces A13; use a new disposable task directory for a new edition.

```powershell
.\q2psx.exe --disc disc/game.cue --map SECURITY --headless --frames 60 `
  --size 1024x768 --aspect 4:3 --no-crt --no-bloom `
  --saves ..\.agents\tmp\design-document\saves\SECURITY `
  --shot ..\.agents\tmp\design-document\captures\SECURITY-raw.ppm `
  --shot-display ..\.agents\tmp\design-document\captures\SECURITY.ppm
```

For noninteractive scripting on Windows, launch the GUI-subsystem client through a process API that waits, such as Python `subprocess.run`, and check the returned exit code. All captures are engine render-target output. No OS screen capture or mouse/keyboard automation is used.

## Rebuild the illustrated reference

Use Python with `reportlab`, `Pillow` and `pypdf` installed. On this task the Codex bundled Python was used. The generator reads the retained prepared images in `.branding/design/assets/` and does not need to launch the game for a text or layout revision.

```powershell
python scripts/build_design_reference.py
```

Pass `--prepare` only when the inputs listed in `atlas.json` exist; it regenerates the prepared images and register. Original scans are read only. `--fonts` selects the Windows reading faces; `--brand-fonts` selects the local reconstructed Q2PSX faces. Exact embedded font hashes are saved in font-register.json. Do not commit font binaries or illustrated outputs.

## Historical source links

- [PSX Data Center US archive](https://psxdatacenter.com/games/U/Q/SLUS-00757.html)
- [PSX Data Center PAL archive](https://psxdatacenter.com/games/SLES-01534.html)
- [HFS DB manual source](https://db.hfsplay.fr/games/87888-quake-ii?lang=en)
- [LaunchBox PlayStation image archive](https://gamesdb.launchbox-app.com/games/images/11167-quake-ii)
- [Vintage Computing and Gaming publication identification](https://www.vintagecomputing.com/index.php/archives/683)
