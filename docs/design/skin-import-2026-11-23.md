# Skin import — design note

Nov 23, 2026. Source: design discussion. Covers bringing other players' skins into
Blackwing by converting them: Winamp modern (`.wal`), Windows Media Player (`.wmz`),
VLC (`.vlt`) and XMPlay (`.xmpskin`). This is not a spec: no names, keys or file layouts
are fixed here, and the examples are illustrations.

"Decided" = settled direction; everything else is a consideration to resolve during
design.

This is a design record: it is kept as it stood on its date. Skin import is not in
[the master PRD](../PRDs/Blackwing-Master-PRD.md) yet; where it goes and in which phase
is under [Open](#open).

**Principles** (Korvid's `docs/principles.md`): 2, declarative stomps immediate (a skin
becomes markup, and script only where the source had script); 9, minimize dependencies
(no MAKI or JScript runtime is taken on); 5, fail loudly, with names (the conversion
report).

## Decided

| question | settled as |
|---|---|
| Interpret or convert | Convert. A skin is imported once into a Krome skin. Blackwing runs no MAKI and no JScript |
| What conversion covers | Layout, images, fonts, and every behaviour the source states declaratively |
| What it does not cover | Script. Each script becomes a Lua skeleton wrapped around what was discovered in it, for a person to finish |
| Where the capabilities come from | Krome. It gains what these formats need ([What Krome needs](#what-krome-needs)); the converter does not work around a missing one |
| Formats | `.wal`, `.wmz`, `.vlt`, `.xmpskin` at least |
| What ships | A handful of CC0 or CC-BY skins, converted. Nobody else's skin is bundled |

## Why convert

- A runtime for someone else's script language is a compatibility promise with no end.
  Winamp 3's lesson, from [the Nov 22 notes](design-notes-2026-11-22.md), applies twice
  over to an emulation of it.
- The fiddly part of a skin is mechanical: sub-rectangles, hit masks, colour keys, anchor
  rules, frame strips. A converter gets those right every time, and they are most of the
  work of porting by hand.
- The script in a typical skin is small next to its layout, and much of it repeats from
  skin to skin. Rewriting it in Lua against a clean API is hours, where redrawing the
  layout is days.
- A converted skin is an ordinary Blackwing skin. It hot-reloads, it can be edited, and
  it does not depend on the converter once made.

The cost, accepted: a scripted skin imports looking right with some parts inert. Import
is offered as a starting point for `.wal` and `.wmz`, and as a finished skin for `.vlt`
and `.xmpskin`, which have no script.

## What comes out

One directory per skin, data only (the PRD: skins and screens are data and load at
runtime in every phase):

| part | what |
|---|---|
| Markup | `.kim` screens: one per window or layout of the source |
| Images | PNG, with colour keys already turned into alpha and palette masks kept as they are |
| Fonts | The skin's own, where it shipped any and their format can be read |
| Script | One Lua file per source script: the skeleton |
| Manifest | Name, author, licence, the source file's hash, the converter's version |
| Report | Every source element as converted, approximated or dropped, with its file and line |

The report is the contract with the person finishing the skin. Nothing is dropped
silently.

## The face contract

A skin binds to names: model keys to read, events to emit. The Nov 22 notes asked for "a
versioned widget contract plus fallback docking"; this is where it becomes necessary,
because four source vocabularies have to land on one.

- A host face writes the model from the player's tables and turns the skin's events into
  the player's commands, as `ui_dev` does for its own panels today. The skin's Lua runs
  in Krome's state and sees only the model and `emit`.
- The contract is versioned as the script-module requirements version a surface
  ([requirements](script-module-requirements-2026-11-23.md), "Versions").
- Every control the converter emits carries a role and a label from the source's
  vocabulary (a mask index, an `action=`, a `vlc.play()`), so a converted skin can be
  given an accessibility tree later without being converted again.

What each format calls the same things:

| | `.xmpskin` | `.vlt` | `.wmz` | `.wal` |
|---|---|---|---|---|
| Play | mask index 6 | `vlc.play()` | `player.controls.play()` | `action="PLAY"` |
| Seek bar | mask index 15 | `Slider value="time"` | `SLIDER` on `player.controls.currentPosition` | `action="SEEK"` |
| Volume | mask index 13 | `Slider value="volume"` | `player.settings.volume` | `action="VOLUME"` |
| Title text | mask index 2 | `$N` | `wmpprop:player.currentMedia.name` | a `text` with `display="songname"` |
| Enabled or shown when | fixed by the player | a boolean expression | `wmpenabled:`, `wmpprop:` | script |

A control the skin has that Blackwing lacks (VLC's DVD menu, XMPlay's MOD sample view) is
dropped and reported. A control Blackwing has that the skin does not place goes to
fallback docking.

## The formats

| | container | definition | script | converts to |
|---|---|---|---|---|
| `.xmpskin` | zip | fixed file names, 8-bit masks, `skinconfig.txt` | none | a finished skin |
| `.vlt` | tar.gz or zip | `theme.xml`, a fixed vocabulary | none | a finished skin |
| `.wmz` | zip | `.wms` XML | JScript, as source | layout, bindings, and a skeleton |
| `.wal` | zip | `skin.xml` and its includes | MAKI, compiled | layout, built-in actions, and a skeleton |

### `.xmpskin`

Read from the skinning kit XMPlay publishes (`xmp-skin.txt`, in `xmp-skin.zip`).

- Files have fixed names: `panel_main`, `panel_mini`, `panel_list`, `panel_output`, each
  with a `mask_*` of the same size, plus the info window's pieces, sliders, knobs, button
  states, ticks and markers. Panels and parts are 24- or 32-bit BMP or PNG.
- **Masks say what is where.** A mask is an 8-bit BMP whose palette index names the
  control under each pixel: on the main panel 5 is previous, 6 play and pause, 7 stop, 13
  the volume slider, 15 the position slider, up to 28. The playlist panel has twenty
  fixed track slots by index. The table is in the kit and is the whole vocabulary.
- **States are computed.** Hover and pressed are the panel's own pixels under the mask,
  multiplied by `scaler_lit` and `scaler_pressed` (RGB factors, usually above 1).
  Switches take an `onoff` image or a per-button "on" bitmap.
- A slider's direction is whichever of its mask's width and height is greater. Single
  mask pixels at the corners set its extent without being lit.
- A knob is a strip of states stacked vertically, any number of them.
- See-through is alpha, or `color_seethru`, or with neither the colour of each panel's
  top-left pixel.
- The info window stretches: mask indexes 253, 254 and 255 mark where it grows.
- `skinconfig.txt` is `name = value` lines: fonts by family name, colours, the panels'
  offsets and how far they slide out, the level meter's scheme.

No script, so nothing is left over. Left to judgment: fonts named by Windows family
(`arial`), and the bitmap `.fon` files some skins carry.

### `.vlt`

Read from VideoLAN's skins2 guide.

- A tar.gz, or a zip since VLC 0.8.5. VLC searches it for the first `theme.xml`.
- `Bitmap` and `SubBitmap` (a region of a file), with `alphacolor` as a colour key and
  `nbframes`, `fps`, `loop` for frames stacked vertically. `Font` and `BitmapFont`.
- `Window`, each with `Layout`s it can switch between (`minwidth` to `maxheight` bound a
  resize), `Panel`s as containers, and `Anchor`s for magnetism: a point or Bezier curve,
  a range, and a priority that says which window follows which.
- Placement is `x`, `y`, and `lefttop` and `rightbottom` tying each corner to one of the
  container's corners; `xkeepratio` and `ykeepratio` keep a proportion instead.
- Controls: `Image` (with `action` of `move` or a resize grip, and `resize` of `mosaic`
  or `scale`), `Button`, `Checkbox` (two sets of states and a `state` expression),
  `Text`, `Slider` (along Bezier `points`, with a `SliderBackground` grid of frames),
  `RadialSlider`, `Playtree`, `Video`.
- **Behaviour is a fixed vocabulary.** Actions are calls from a list (`vlc.play()`,
  `playlist.next()`, `equalizer.enable()`, `WindowID.show()`,
  `WindowID.setLayout(LayoutID)`), several separated by semicolons. `visible` and `state`
  take boolean expressions over `and`, `or`, `not` and a list of flags (`vlc.isPlaying`,
  `playlist.isRandom`, `WindowID.isVisible`). Text takes variables (`$N`, `$T`, `$D`,
  `$V`, `$B`). Slider values are `time`, `volume`, `equalizer.preamp`,
  `equalizer.band(0)` to `(9)`.

No script. The actions become events, the expressions become bindings on model keys the
host face keeps (or a line of generated Lua where an expression combines several), and
the text variables become `{key}`s.

### `.wmz`

Read from Microsoft's skin programming reference. As of Nov 10, 2026 Windows Media
Player Legacy no longer loads skins on current Windows, so there is no longer a
reference player to compare a conversion against on a new machine. The container and
the property names in the examples (`player.settings.volume` and the like) are from
memory, not from the pages read, and want checking.

- A zip of one `.wms` (XML), the art, and JScript files.
- `THEME` holds `VIEW`s; a `VIEW` holds `SUBVIEW`s and controls: `BUTTON`,
  `BUTTONGROUP` with `BUTTONELEMENT`s, `SLIDER`, `CUSTOMSLIDER`, `PROGRESSBAR`, `TEXT`,
  `PLAYLIST`, `LISTBOX`, `EDITBOX`, `POPUP`, `EFFECTS`, `VIDEO`, `EQUALIZERSETTINGS`.
- **Button groups hit-test by colour.** A `mappingImage` the size of the art has each
  button's area filled with one colour, and each `BUTTONELEMENT` names its
  `mappingColor`. The group has one image per state for all its buttons: `image`,
  `hoverImage`, `downImage`, `hoverDownImage`, `disabledImage`.
- A `CUSTOMSLIDER` picks its picture from a `positionImage`, a greyscale map of where
  each value lies.
- Window shape comes from `backgroundImage` with `transparencyColor` and
  `clippingColor`. Images take `hueShift` and `saturation`.
- **Three attribute prefixes are declarative.** `wmpprop:` keeps an attribute equal to a
  property (`value="wmpprop:player.settings.volume"`); `wmpenabled:` and `wmpdisabled:`
  follow whether a transport method is available. These convert to bindings.
- **Script is JScript, shipped as source**: files named by a view's `scriptFile`, and
  `jscript:` in event attributes (`onclick`, `onload`, `ontimer` at `timerInterval`,
  a slider's `onPositionChange`). Script sees `player`, `theme`, `view` and `event`.

The skeleton gets one handler per event attribute and per function in a script file,
with the JScript beside it as a comment. A handler that is a single call from a short
list (`player.controls.play()`, `theme.openView('x')`, `view.close()`) is converted
outright; that needs a recogniser for one statement, and no JScript parser beyond it.

### `.wal`

Read from Webamp's MAKI parser (`webamp-modern`) for the compiled script; the XML side
is from memory of the Winamp 3 and 5 skin documentation and wants checking against it
before design.

- A zip. `skin.xml` includes other XML. `elements` name bitmaps as regions of files,
  bitmap fonts, TrueType fonts and colours, some in gamma groups a colour theme shifts.
- `container`s (windows) hold `layout`s (normal, shade), which hold `group`s defined by
  `groupdef`; inside are `layer`, `button`, `togglebutton`, `text`, `slider`, `vis`,
  `animatedlayer` and components.
- Placement is `x`, `y`, `w`, `h`, each with a `relat*` flag that measures it from the
  far edge instead.
- **Built-in actions need no script.** A button's `action="PLAY"`, a slider's
  `action="SEEK"` or `"VOLUME"`, a text's `display="songname"` convert to events and
  bindings as the other formats' do.
- **Components come from Winamp, not the skin.** `Wasabi:StandardFrame`,
  `Wasabi:TitleBar` and their kin are defined in the player's own install. A converted
  skin needs Blackwing's own definitions of the common ones, as a Krome prelude.
- **MAKI is compiled.** A `.maki` begins `FG`, then holds: classes as GUIDs; methods as
  a class and a name; variables as fixed-size records with no names; constants, which
  are the strings; bindings; and code for a stack machine of about forty opcodes.
- **A binding is three numbers**: a variable, a method, a code offset. So without
  decompiling, the converter can list which object handles which event
  (`onLeftButtonUp`, `onSetPosition`), and the string constants give the ids those
  objects were looked up by.

The skeleton gets one stub per binding, named by its method, with the object's id where
a constant gives it and the variable's index where none does. A skin that ships its `.m`
source gets that beside the stub as a comment. Not attempted: decompiling the code
section.

**Stock scripts.** Many skins carry the same scripts that came with Winamp. Recognising
one by its hash and substituting a Lua equivalent written for Blackwing would make those
skins work with no porting. The equivalents are written from what the script does, never
from Nullsoft's source or a decompilation of it; the same holds for the Wasabi
components.

## The skeleton

An illustration, for a `.wal` script with three bindings and one recognised constant:

```lua
-- from scripts/drawer.maki (no source shipped). 3 handlers, 0 converted.
-- Objects: toggle = "drawer.toggle" (button), panel = variable 14 (group, id not found)
return {
  model = { drawer_open = false },
  on = {
    -- toggle.onLeftButtonUp(x, y)
    ["drawer.toggle"] = function() --[[ TODO ]] end,
    -- panel.onTargetReached()
    panel_target_reached = function() --[[ TODO ]] end,
    -- System.onScriptLoaded()
    loaded = function() --[[ TODO ]] end,
  },
}
```

A skeleton loads and runs as it is: every stub is a handler that does nothing. The
report lists the stubs still empty, so "how finished is this skin" is a number.

## The converter

A host tool, not part of the player: one intermediate form, a front end per format, one
back end.

```
.xmpskin ─┐
.vlt     ─┤  unpack → parse → skin IR → emit → .kim, .png, .lua, manifest, report
.wmz     ─┤                    (windows, layouts, images, controls,
.wal     ─┘                     bindings, actions, script stubs)
```

- **The archive is hostile.** Entry paths are confined to the output directory; sizes,
  counts and nesting are capped; an image is decoded with bounds on its dimensions. The
  front ends are fuzzed, as the script-module requirements ask of a binding.
- **The intermediate form is the design.** It has to hold a mask-indexed XMPlay panel
  and a `.wal` group tree in one shape. `.xmpskin` and `.vlt` between them cover most of
  it with no script in the way, so they are written first.
- **Decoders the tool needs** that Korvid's `image` lacks today: BMP (8-bit palettised
  for masks, 24- and 32-bit), GIF for some `.wmz` art. The tool also reads tar and gzip
  for `.vlt`; zip and XML it has from `inflate` and `xml`.
- **Checked by rendering.** Each shipped skin has a golden conversion, and the converted
  skin drawn headless (`krome_viewport`) is compared with a capture of the original
  player. The report's counts (converted, approximated, dropped, stubs empty) are kept
  per skin, so a converter change that loses ground is seen.

Order: `.xmpskin`, then `.vlt`, then `.wmz`, then `.wal`. The first two prove the
intermediate form and Krome's new capabilities on formats that convert completely.
`.wmz` adds the skeleton with source to copy from. `.wal` is last because it needs all of
that and the prelude of components.

## What Krome needs

Decided: Krome gains these. Read against Krome's module page as it stands. Each is
asked of the engine in [Korvid changes](../PRDs/Korvid-Changes-For-Blackwing.md) as KR28
to KR42, in the order of these two tables, with what is asked and how to tell it is done.

| capability | wanted by | today |
|---|---|---|
| An image node drawing a region of a file | all four | a whole file |
| A frame strip, the frame chosen by a bound value or played at a rate | `.vlt`, `.xmpskin` knobs, `.wmz`, `.wal` | none |
| Placement at an offset in a parent, each edge tied to a parent edge or kept in proportion | `.vlt`, `.wal`, `.wmz` | a stack places by nine anchors |
| An image stretched, tiled, or stretched with fixed borders | `.vlt`, `.xmpskin` info window, `.wal` | none |
| Bitmap fonts: glyphs as regions of an image | `.wal`, `.vlt`, and `.wsz` | TrueType only |
| Hit testing through a mask image: by alpha, by colour, by palette index | `.xmpskin`, `.wmz`, shaped controls everywhere | a node's rect |
| A look computed from the pixels under a mask: scaled, hue-shifted, gamma-shifted | `.xmpskin`, `.wmz`, `.wal` | `tint`, `opacity` |
| Position and size easing, and a timer a screen's script can ask for | `.wal` drawers, `.wmz` `ontimer` | colours and opacity ease; sizes and positions do not move |
| A slider whose thumb follows a path, and a radial one | `.vlt` | a Lua behaviour could do it, given bound offsets |
| Text that scrolls when it does not fit | `.wal`, `.vlt`, `.xmpskin` | `clip` |
| A screen opened from a directory at runtime, uncooked | all four | `krome_provide`, and `kimc` as a tool |

And from the window, which is `blackwing_window`'s and the platform's more than Krome's:

| capability | wanted by |
|---|---|
| A window with no frame, shaped by its image's alpha | all four |
| Several windows up at once, each a screen | `.vlt`, `.wal`, `.xmpskin` panels |
| Regions that drag the window, and grips that resize it | all four |
| Windows that snap to each other and move together | `.vlt` anchors, `.wal`, `.xmpskin` |

Already there: a look's image per state, bound properties, themes and tokens (which a
`.wal` colour theme can become), Lua behaviours, a sparse list for a playlist.

Not Krome's: level meters, spectrum and scope are the visualizer's (B27), placed by a
skin as a node.

## Shipped skins and licences

- Blackwing ships a handful of skins that are CC0 or CC-BY, already converted and
  finished, script included. A CC-BY skin's author and licence are in its manifest and
  shown where skins are chosen.
- The converter carries the source's author and licence fields into the manifest and
  never removes them.
- Converting a skin a user has is the user's business; the converter is a tool and
  bundles nothing.
- A skin's licence does not cover a font it carries. A shipped skin's fonts are checked
  on their own.

## Open

- **Where this lives.** A fifth face that hosts any converted skin, or the form
  `ui_clean` and `ui_compact` already take. And which phase: it is in no requirement of
  the master PRD.
- **`.wsz`.** The PRD has `ui_wsz` as an interpreter of the format. With an intermediate
  form and a bitmap-skin back end, `.wsz` could be a fifth front end and `ui_wsz` the
  host face. The format is fixed-layout and script-free, so it would convert completely.
- **Is a skin's Lua trusted?** A finished skin with Lua in it will be shared. Krome's
  state has no files and no exits, but the script-module requirements' caps (memory,
  instruction count) are not Krome's today.
- **Scale.** These skins are drawn for one pixel density. Whole-number scaling with
  nearest sampling, or anything else, per skin or per display.
- **Fonts by name.** `.xmpskin` and `.wal` name system fonts that are not there on
  Linux or a Mac. A substitution table, with the report saying what was substituted.
- **Text.** Krome draws left to right with no shaping, and a bitmap font has only the
  glyphs its author drew. What a converted skin shows for a title it cannot draw.
- **How many `.wal` skins ship `.m` source, and how much of the corpus is stock
  scripts.** Both are measurable over a sample, and they decide how much the stock-script
  table is worth.
- **`.wal` XML.** The element and attribute names above are from memory; checked against
  the format's documentation and a sample of skins before a front end is designed.
- **Other formats.** K-Jöfol and BSPlayer are bitmaps and a text file, in `.xmpskin`'s
  class, and would be front ends only.
