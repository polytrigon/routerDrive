# RouterDrive

A wireless "USB drive" for the Shaper Origin: a Seeed XIAO ESP32-S3 plugs into
the Origin's USB-A port and presents itself as a flash drive full of SVGs,
while you upload new SVGs to it over Wi-Fi from your phone or laptop.

This is a starting point, not a finished/tested product - you're the first
person to run this particular firmware on this particular hardware
combination. Budget an evening for the bring-up steps below before you trust
it at the tool.

**Already built and flashed?** This README covers parts, setup, and
first bring-up. For day-to-day use once that's done - uploading files,
folders, cut types, the per-line cut editor, restarting, resetting
Wi-Fi - see [`HOW_TO_USE.md`](HOW_TO_USE.md) instead.

## How it works (read this before you flash anything)

- **Storage** is the XIAO's onboard flash (no SD card needed), formatted as
  FAT and exposed to the Origin over USB using the ESP32-S3's native
  USB-OTG peripheral in Mass Storage Class (MSC) mode.
- **Both sides go through the same wear-levelling layer.** `FFat.begin()`
  doesn't put the filesystem straight on the partition - it mounts through
  Espressif's wear-levelling (WL) layer, which remaps logical sectors onto
  different physical ones so that the handful of sectors FAT rewrites
  constantly (the allocation table, the root directory) don't wear out
  first. The USB block callbacks in `storage.cpp` therefore go through WL
  too, via `wl_read`/`wl_write`/`wl_erase_range` on a handle that is
  mounted while USB owns the flash and unmounted before the firmware
  mounts its own - only ever one live instance, since each caches its
  mapping in RAM and writes state back into the partition. WL's state
  lives in the partition rather than the instance, so whichever side
  mounts next picks up exactly the mapping the other left behind.

  This is worth understanding before changing anything in `storage.cpp`,
  because it replaced a bug that looked like working code. The MSC
  callbacks used to read and write the raw partition
  (`esp_partition_read/write` at `lba * 4096`) while the filesystem went
  through WL - two translations over one flash. That behaves perfectly on
  a lightly written partition, where WL's mapping is still close enough to
  identity that the two views agree, and then breaks once enough erase
  cycles have accumulated for the mapping to migrate and the host's view
  to slide out from under the filesystem. A host-side write would also
  have gone in behind WL's back, leaving its bookkeeping inconsistent with
  the data it describes. **The invariant to preserve: both paths address
  the flash through the same WL handle, and only one WL instance is ever
  mounted.** `test_storage_modes.cpp` models that state machine - 200
  access cycles, nested begins, stray ends, and injected mount failures -
  and is the cheapest way to check a change here without hardware.
  *Credit to Beau for spotting the original.*
- **Wi-Fi upload** and **USB exposure** can't literally touch the flash at
  the same instant - that's a hard limitation of how the flash's
  wear-levelled filesystem works, confirmed by Espressif's own USB-MSC
  example ("they can't be allowed to access the partition at the same
  time"). So RouterDrive *switches* between "USB owns it" and "firmware
  owns it" automatically, for well under a second, every time you upload or
  delete a file. The cable to the Origin never needs to be unplugged for
  this.
- After every switch back, the firmware still sends the USB host a standard
  SCSI "media changed" hint automatically - that part isn't user-facing,
  it just happens as part of every upload/delete. Getting the Origin to
  actually notice a new file on its own, though, has not worked out that
  way in practice: the web UI used to also offer a manual "Nudge" button
  that fired that same hint on demand, without restarting anything, but
  real testing showed it never once got the Origin to pick up a new file -
  and clicking it several times in a row once made the Origin appear to
  lose the drive entirely instead of helping. That button has been removed.
  The one thing that has actually worked is the "Restart RouterDrive" button in
  the web UI: the arduino-esp32 core doesn't expose a way to bounce just
  the USB peripheral, so this reboots the whole device, which does force a
  genuine USB disconnect/reconnect, but also drops Wi-Fi for several
  seconds. Worst case, a physical unplug/replug of the USB cable will
  always work, the same as it would with any ordinary flash drive.
- **Where exactly the Origin expects SVGs in the filesystem is not
  publicly documented.** See "Step 0" below - figure this out with a
  normal flash drive before you rely on this firmware.

## Parts

- Seeed XIAO ESP32S3 (the plain one - no SD card needed)
- A USB-A-to-USB-C cable to connect it to the Origin
- A USB-C cable to your computer for flashing

## Step 0: learn the Origin's file layout requirement (do this first)

Before writing any code-dependent behavior around it, use a **plain FAT32
USB flash drive** and the Origin itself to answer:

- Does it want SVGs in the root of the drive, or a specific folder name?
- Any file naming rules (case sensitivity, forbidden characters, a maximum
  name length)?
- Does it tolerate *other* files/folders being present on the drive, or
  does it need the drive to contain only SVGs?
- Does it cache the file list once per session, or re-read the drive each
  time you open the "load a design" screen?

Once you know the folder, open `config.h` in this sketch and change:

```cpp
static const char *SVG_FOLDER = "/";   // change e.g. to "/SVGs" if needed
```

## Setting up Arduino IDE

1. Install [Arduino IDE](https://www.arduino.cc/en/software) (2.x).
2. **File > Preferences**, add this to "Additional boards manager URLs":
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. **Tools > Board > Boards Manager**, search "esp32" (by Espressif
   Systems), install it. This can take a while the first time.
4. Plug the XIAO into your computer via USB-C, then **Tools > Board**,
   select **XIAO_ESP32S3** (under the esp32 boards list).
5. **Tools > Port**, select the port that appeared when you plugged it in.

### Required Tools-menu settings

These matter - the sketch will fail to compile or MSC won't work without
them:

| Setting | Value |
|---|---|
| USB Mode | **USB-OTG (TinyUSB)** |
| USB CDC On Boot | **Enabled** (keeps Serial working over the same port) |
| USB Firmware MSC On Boot | **Disabled** (see below - not something RouterDrive uses) |
| Partition Scheme | **TinyUF2 8MB No OTA (4MB APP/3.7MB FFAT)** |
| Upload Mode | default is fine |

"USB Firmware MSC On Boot" is a separate, built-in Arduino core feature -
when Enabled, it exposes an extra **read-only** drive (labeled something
like `ESP32-FWMSC`, containing a raw dump of the currently-flashed app as
`FIRMWARE.BIN`) purely as a debugging/backup convenience, alongside
whatever drive the sketch itself creates. RouterDrive doesn't use or need
it - leaving it Disabled avoids a second, confusing drive showing up next
to RouterDrive's own (unlabeled/"NO NAME") one every time you plug in.

The Seeed XIAO ESP32S3 board package doesn't offer a plain "Custom"
partition scheme, so the `partitions.csv` in this sketch folder is unused
and can be ignored/deleted - Arduino's own "TinyUF2 8MB No OTA" option
already ships a data partition named `ffat`, which is exactly what
`storage.cpp` looks for, and gives ~3.7MB of file storage. ("TinyUF2" in
the name just refers to how the partition layout was originally designed
to coexist with an optional drag-and-drop bootloader - picking it does
**not** require using that bootloader or change how you upload; the
normal Upload button keeps working exactly as before.)

### Sketch layout

Arduino IDE treats every `.h`/`.cpp` file that sits next to the `.ino` in
the same folder as an extra tab - you don't need to do anything special,
just make sure all these files stay together in one `RouterDrive/` folder:

```
RouterDrive/
  RouterDrive.ino
  config.h
  storage.h / storage.cpp
  wifi_portal.h / wifi_portal.cpp
  web_server.h / web_server.cpp
  led.h / led.cpp
  dxf2svg_js.h
  dxf2svg.js
  style_css.h
  style.css
  partitions.csv
```

`dxf2svg.js` and `style.css` are the actual DXF-to-SVG converter source
(plain, dependency-free JavaScript) and the page's stylesheet - both kept
in the folder for readability/editing, but neither is read from the
filesystem at runtime. `dxf2svg_js.h`/`style_css.h` embed those same
sources as C++ string constants so they compile into the firmware and get
served at `/dxf2svg.js`/`/style.css`. **If you ever edit `dxf2svg.js` or
`style.css`, you need to regenerate the matching `.h` file** - they're not
kept in sync automatically.

No external libraries need installing - everything used (`WiFi`, `WebServer`,
`DNSServer`, `Preferences`, `ESPmDNS`, `FFat`, `USB`, `USBMSC`) ships with the
ESP32 board package you just installed.

## First flash & bring-up checklist

1. Open `RouterDrive.ino`, verify/compile first (checkmark icon) before
   uploading, to catch any Tools-menu misconfiguration early.
2. Upload. Open **Tools > Serial Monitor** at 115200 baud - the sketch logs
   what it's doing (mounting storage, Wi-Fi state, USB events).
3. On first boot it has no Wi-Fi credentials, so it starts an access point
   called `RouterDrive-Setup` (password `routerdrive` - change this in
   `config.h`). Join it from your phone/laptop; a "sign in to network" page
   should pop up, or open `http://192.168.4.1/` manually. Either way,
   while the device is in this setup mode you'll always get the same
   streamlined page - just the Wi-Fi form, plus a note on how to skip
   Wi-Fi setup entirely and use the device straight off its own hotspot
   instead (see "Using it without joining Wi-Fi" below), and a reminder of
   where to go once it's connected (`http://routerdrive.local/`). The full
   page (file list, uploads, DXF converter) isn't reachable at all while
   in setup mode unless you deliberately ask for it via the "skip this"
   instructions - see below.
4. Enter your home Wi-Fi's SSID/password, submit. The device reboots and
   tries to join it (watch the Serial Monitor). Once connected, it's
   reachable at `http://routerdrive.local/` (or the IP shown in Serial
   Monitor, if `.local` addressing doesn't resolve on your network).
5. **Before** plugging it into the Origin: connect it to your computer's
   USB port instead and confirm your computer sees it as a plain USB
   flash drive, that you can see/copy files onto it from the web UI, and
   that deleting works too.
6. Only once that works, plug it into the Origin's USB-A port (power it
   either from the Origin's port or your own USB power - both should be
   fine) and confirm the Origin sees it and can open an SVG you uploaded
   via Wi-Fi.
7. Test the "stays plugged into the Origin while you upload a new file"
   flow last: upload a second SVG while the cable is still connected to
   the Origin, and see whether the Origin's file browser picks it up on
   its own or needs "Restart RouterDrive". Expect to need restart - across
   several real tests, restart has worked every time but one, while the
   old manual "nudge" (since removed - see "How it works" above) never
   once picked up a new file and, clicked repeatedly, once made the Origin
   appear to lose the drive entirely. So plan on restart being your
   steady-state workflow, not a rare fallback; and if it ever fails too, a
   physical unplug/replug of the USB cable always works. The restart
   button's confirmation page waits for the device to come back and takes
   you back to "/" on its own - no need to refresh manually (it'll tell
   you if it's taking unusually long).

## Using it without joining Wi-Fi

You don't have to give RouterDrive your home Wi-Fi credentials at all.
While the device is in setup mode (broadcasting `RouterDrive-Setup`), it
always shows the streamlined Wi-Fi-only page - whether you're looking at
it through the "sign in to network" captive popup or a regular browser
tab makes no difference, it's the same page either way. To reach the full
page (file list, uploads, DXF converter) without giving it your home
Wi-Fi:

1. Close the popup if one's showing (or just switch away from it), then
   open your own browser - Safari, Chrome, whatever you'd normally use,
   **not** the popup itself.
2. Visit `http://routerdrive.local/nowifi` (or `http://192.168.4.1/nowifi`
   if that address doesn't resolve) while still connected to the
   `RouterDrive-Setup` network.

That unlocks the full page for the rest of this boot - you won't need to
repeat step 2 until the device restarts or joins a real Wi-Fi network. The
tradeoff: you'll need to stay connected to `RouterDrive-Setup` on your
phone/laptop every time you want to use it this way (which also means no
internet access on that device while you're doing so), instead of it
being reachable from anywhere on your normal Wi-Fi.

**Why "not the popup itself":** on a Mac or iPhone, the "sign in to
network" window is Apple's own Captive Network Assist - a stripped-down
mini-browser (no address bar, can't open new tabs or bookmarks) that
macOS/iOS launch on their own. It'll follow a plain link/typed URL to
`/nowifi` just fine, but whether it actually supports the file-upload
form after that is untested and undocumented by Apple - safer to just
open your own browser instead, where everything is guaranteed to work
normally.

(Earlier versions of RouterDrive tried to detect the captive popup
specifically via its `User-Agent` and show the streamlined page only
there, falling back to the full page for anything else in setup mode.
Real-hardware testing showed that guess was unreliable even after a
couple of rounds of tuning, so it's been replaced with the simpler,
always-accurate rule above: setup mode means streamlined, no guessing.)

## Checking which build is running

Every page has a small footer, anchored to the bottom of the page (it'll
sit at the very bottom of the browser window on a short page, or just
after the content on a longer one) - "RouterDrive - By MTseng & Claude -
Built <date> <time>", stamped automatically from the Arduino compiler's
own build timestamp (no manual version number to remember to bump).
Useful for confirming a flash actually took, especially
mid-troubleshooting: if the footer's timestamp doesn't match when you last
compiled and uploaded, the device is still running older firmware. The
same date/time is also printed to Serial on boot.

Below that, a smaller disclaimer line - "RouterDrive is an independent,
unofficial project and is not affiliated with, endorsed by, or sponsored
by Shaper Tools, Festool, or their parent companies." - appears on every
page the footer shows up on, including the AP-mode setup page.

## Resetting Wi-Fi credentials

Two ways:
- In the web UI, "Forget Wi-Fi" button (under the Wi-Fi section once
  connected).
- Physically: the XIAO has two small buttons, silkscreened **B** (BOOT) and
  **R** (RESET) - hold down **B**, then while still holding it tap **R** to
  reset the board, and keep holding **B** for about 3 seconds after that.
  **R** alone just restarts it and doesn't touch saved credentials - **B**
  is the one that matters here. Once you let go, saved credentials are
  wiped and it comes up in setup-AP mode again.

If you never even get that far - say you submitted the wrong password on
the setup page - `RouterDrive-Setup` now stays up the whole time RouterDrive
is retrying a saved network in the background, so you should always be able
to rejoin it and try again (or hit "Forget Wi-Fi" if that's showing) without
needing the BOOT-button reset above. (Earlier builds would drop the setup AP
for good after the first failed retry, since testing a saved network briefly
switched the radio out of AP mode and nothing brought the AP back up when
that attempt failed - fixed by keeping the AP and STA radios up
simultaneously during retries instead of switching between them.)

## Connection status

The top of the web UI shows two live indicators:

- **USB** - green when the native USB port has a host enumerated on it
  (plugged into a powered-on computer or the Origin), red otherwise. This
  only confirms the physical/electrical link is up - it can't tell you
  whether the Origin specifically has indexed the drive, since that's
  internal to Origin's own closed software.
- **Wi-Fi** - shows the joined network and signal strength (dBm + a
  rough Excellent/Good/Fair/Weak label) when connected, or "setup mode"
  when it's broadcasting the `RouterDrive-Setup` access point instead.

## Status LED

The XIAO's onboard user LED (separate from its tiny charging-status LED)
gives you a glance-able status without needing Serial Monitor open:

| Pattern | Meaning |
|---|---|
| Solid on | Connected to your Wi-Fi and idle - ready to use |
| Slow blink | Setup/AP mode - broadcasting `RouterDrive-Setup`, waiting for Wi-Fi credentials |
| Fast blink | Busy - a file upload or delete is in progress |
| A few quick flashes, then off | Restart was just triggered (confirms the button press registered) |

This is a first pass covering the states that seemed most worth signaling;
it's all driven from `led.cpp` if you want to add more (e.g. a distinct
pattern for "USB not connected").

It's a single fixed-color (yellow) LED, not RGB - GPIO21 only supports
on/off/blink, no color options.

If you'd rather it just stayed dark, there's a small "Turn status LED
off" link at the very bottom of the page (deliberately understated, not a
normal button, so it doesn't compete with anything else on the page).
Click it again to turn it back on. The setting is saved and survives
restarts/power cycles. It's only on the full page, not the streamlined
Wi-Fi setup popup.

## Converting DXF designs (Onshape etc.)

The web UI has one "Upload files" section that accepts DXF and SVG files
together, in any mix, from a single file picker. DXFs are converted to an
SVG entirely in your browser (no server round-trip, nothing leaves the
page until you hit "Convert & upload") before uploading; SVGs upload as-is
alongside them in the same batch.

- **Multiple files at once, mixed types:** select any combination of DXF
  and SVG files and "Convert & upload" handles each appropriately (DXFs
  get converted first, one output SVG per input DXF, named after it; SVGs
  pass through untouched) then uploads the whole batch in turn. All
  conversions happen first, then any overwrite conflicts across the whole
  batch are confirmed in one prompt, then the files upload one at a time.
  The status line below the button tracks progress and reports which
  files succeeded and which (if any) failed, with the reason.
- **Same name twice in one batch:** if a DXF converts to the same output
  name as another file in the same selection (e.g. you picked both
  `part.dxf` and `part.svg`), the later one is automatically renamed
  `part_1.svg`, `part_2.svg`, and so on rather than silently overwriting
  the other on the device. This only applies within a single batch - a
  name that collides with a file already on the drive still goes through
  the normal overwrite confirmation below instead.
- **Supported entities:** LINE, ARC, CIRCLE, LWPOLYLINE (including bulged/
  curved segments), and SPLINE. Curves are tessellated into short line
  segments before being written out, so there's no dependency on how any
  particular viewer interprets SVG arc commands.
- **Text:** text placed with Onshape's Text tool and exported directly -
  no need to explode or convert it to sketch geometry first - already
  comes out as plain line/arc geometry, confirmed against a real Onshape
  export, so it converts normally like any other shape. What's still not
  supported is a native DXF TEXT/MTEXT entity (the kind that references a
  font rather than carrying its own outline geometry, which some other
  CAD tools may still export) - there's no font-rendering here, so those
  are silently skipped (the status message after conversion tells you how
  many were dropped). If your tool exports text that way, convert it to
  outline curves first, or export/upload a finished SVG instead.
- **Units:** source coordinates are assumed to be millimeters (matches
  Onshape's DXF export). Pick "Units: mm" or "Units: inches" before
  converting - this only changes the SVG's physical `width`/`height`, not
  the geometry itself. SVGs in the same batch ignore this dropdown; it
  only affects DXF conversion.
- Annotation/dimension/centerline-style layers are filtered out
  automatically based on common layer-name patterns, so exported drawings
  with dimension callouts shouldn't need manual cleanup first.

## Uploading

The "Upload files" section accepts more than one file at once (DXF, SVG,
or a mix - see "Converting DXF designs" above for the conversion details).
Every file, converted or not, uploads one at a time in sequence right
after you click "Convert & upload".

If a file you're about to send has the same name as one already on the
drive, the browser asks you to confirm before it overwrites it - a plain
JS `confirm()` dialog listing which name(s) would be replaced. Cancel it
and nothing is sent.

After an upload or delete finishes, the page shows a short confirmation
message above the file list - green with a checkmark on success (e.g.
"Uploaded 3 files"), red if something failed (e.g. a file that couldn't
be written, listed by name). It's a one-time message: it clears itself as
soon as the page is shown once, so refreshing won't leave it stuck there.
(The upload section's own status line, shown while it's converting/
uploading, tracks the whole batch; the flash message you see after the
page reloads reflects only the last file in that batch, since each
converted file uploads as its own request.)

Every file that just landed - whether it's brand new or overwrote an
existing one - also gets a small green checkmark next to its "Uploaded"
timestamp in the file list, covering the whole batch (unlike the flash
message above). It's shown once, the same way: it won't still be there
after a plain page refresh.

**Note:** the plain form's multi-file upload relies on the ESP32
`WebServer` library calling the upload handler once per selected file when
several share the same form field - this is how standard
multipart/form-data works, but it hasn't been verified against this
library's implementation on real hardware yet. If it doesn't behave as
expected, uploading one file at a time is the fallback. The DXF
converter's multi-file support doesn't depend on this, since it sends one
upload request per file itself.

## Cut type & cut depth

The "Upload files" section has optional controls above the "Convert &
upload" button: a **Cut type** dropdown (Outside / Inside / Pocket / On
Line / Guide) and a **Cut depth** field with a small unit dropdown
(mm/in, defaults to mm). Set either of these and every shape in every
file you're about to upload gets them stamped on, in-browser, before the
file ever reaches the device - the ESP32 itself doesn't touch this, same
"do it client-side in JS" approach as the DXF converter.

There is deliberately no bit-size control here; see "Bit size isn't in
the file" below.

### How cut types are actually encoded

This is the single most important thing to understand about this
feature, and it cost a long real-hardware bug hunt to pin down:

> **Origin reads a shape's cut type from its fill and stroke COLOR, not
> from the `shaper:cutType` attribute.**

Shaper's own documentation calls this "cut type encoding" and describes
Origin as accepting "color-coded vector shapes indicating cut types";
their Inkscape guide states outright that a gray *stroke* is an On Line
cut and a gray *fill* is a Pocket cut. The full table, confirmed against
a real Shaper Studio export of a real part (see `testFiles/`):

| Cut type | fill | stroke |
|---|---|---|
| Outside (exterior) | `#000000` | none |
| Inside (interior) | `#FFFFFF` | `#000000` |
| Pocket | `#7F7F7F` | none |
| On Line | none | `#7F7F7F` |
| Guide | none | `#0068FF` |

Origin matches these tolerantly rather than by exact hex - Shaper's own
gray guide just says "make your R, G and B values equal" - but the
values above are what Shaper's own tools emit, so that's what this app
writes.

**The `shaper:` attributes are metadata, not the mechanism.** Both
writers still add them (`shaper:cutType`, and `shaper:cutDepth` when a
depth is set, plus a
single `xmlns:shaper="http://www.shapertools.com/namespaces/shaper"`
declaration on the root `<svg>`) because Shaper Studio's own exports
carry them, `shaper:cutDepth` *is* documented as a real depth override,
and this app's own file list and per-line editor read `shaper:cutType`
back to show you what's set. But on their own they do nothing on the
machine - a file with perfect `shaper:` attributes and an unrecognized
stroke color imports with no cut type at all.

**A side effect worth knowing:** because Outside/Inside/Pocket are
encoded as *fills*, a file with those cut types set renders as solid
black/white/gray shapes in Preview, Illustrator, or any other SVG
viewer, rather than as thin outlines. That looks alarming the first time
but is exactly right - it's what a Shaper Studio export looks like too.
RouterDrive's own per-line editor deliberately ignores those colors for
its on-screen preview and draws thin outlines in its own palette (see
the legend under the viewer), so the drawing stays readable while you
work on it.

**Bit size isn't in the file.** Outside/Inside/Pocket are *offset* cuts,
so Origin does need a bit diameter to work out how far to offset the
toolpath - but it takes that from the bit you tell the machine you've
loaded, not from the SVG. Earlier versions of this app wrote a
`shaper:toolDia` (and a `shaper:cutOffset`) attribute and offered a bit
size dropdown to go with it. Testing on a real Origin showed the setting
on the tool never moved no matter what the file said, so both the
attribute and the dropdown were removed: they were clutter that implied
the file was in charge when the machine always was. The per-line editor
also strips those two attributes from files that still carry them, so an
old file gets tidied up the first time you edit it. **Set your bit on the
Origin, as you would for any other file.**

Note the value format for the attributes that do matter: Shaper writes
them with **no space** between number and unit (`9mm`, `0.25in`) - the
documented `shaper:cutDepth` examples do the same - so this app does
too. An earlier version wrote `9 mm` with a space, which is one of
several things that had to be corrected before this worked at all.
Reading is still tolerant of the old spaced form, so files saved by
those builds still reopen correctly.

Leave the cut type dropdown on "unset" and the depth field
blank to upload a file exactly as-is (the pre-existing behavior) -
useful if a file already has its own per-shape cut types you don't want
overwritten, e.g. one you exported from Origin itself. Whichever values
you do pick here apply uniformly to every shape in that upload - this is
still a whole-file, one-type choice. To give individual lines within a
single file their own cut type (a box within a box, say, where the outer
line is an Inside cut and the inner one is a Pocket), upload the file
first and then use the file list's per-line editor - see "Editing
individual lines' cut type" below.

## Editing individual lines' cut type

Sometimes one file needs more than one cut type - the classic case is a
box-within-a-box design, where the outer line should be cut all the way
through (Inside) and the inner one should be a shallower Pocket. The
Upload section's cut type dropdown (above) can only apply one type to an
entire file, so for this, upload the file first (leave cut type
"unset," or set whatever the majority of lines should be - either
way, you can change any of it after), then click that file's **Cut
type** cell in the file list.

Clicking it opens an editor showing the file's actual SVG, rendered right
in the browser. Click a line to select it (its outline turns cyan);
shift-click to add more lines to the selection so you can set several at
once. With something selected, pick a **Cut type** in the side panel
(same Outside/Inside/Pocket/On Line/Guide choices as the Upload section,
plus **Cut depth**), then click **Apply to selected** - the selected lines are recolored to
match their new cut type (a small legend in the panel shows which color
means what) so you can see at a glance what's been set and what hasn't
(unset lines stay black). Repeat for as many different lines/cut types
as the design needs, then click **Save changes** to write the whole
file back to the device - this re-uploads it over the original (same
mechanism as a normal upload, so it goes through the same brief USB-mode
switch as any other write).

Nothing is written to the device until you click **Save changes** -
closing the editor (the **Close**/**Cancel** buttons) without saving
asks you to confirm first if you've applied anything. Once a file has
more than one cut type on it (either from this editor, or because it was
uploaded with cut type "unset" and already had mixed types baked in
from wherever it came from), its **Cut type** column shows **Mixed**
instead of a single type - see "The file list" below.

## Folders

Confirmed on real hardware: the Origin renders subfolders on the drive as
their own groups in its import list, so RouterDrive lets you organize
uploads into them - one project per folder, say. Folders are a single
level deep (no folders within folders) and are just plain directories on
the same FAT filesystem everything else lives on. Root is labeled
**"HOME"** throughout the UI (both Folder dropdowns, and the move panel's
destination list).

To upload into a folder, pick it from the **Folder** dropdown in the
Upload section (defaults to "Folder: HOME"). To create a new one, choose
"+ New folder..." - you'll be prompted for a name (letters, numbers,
spaces, `-` and `_`, up to 24 characters) and it's added to the dropdown
immediately; the folder itself is only actually created on the device
once you upload the first file into it, same as it always worked for the
root folder. Invalid names (anything with `/`, `..`, or characters
outside that set) are rejected client-side, and re-checked server-side
too - never trust the client alone with something that touches the
filesystem.

The Files section above the upload controls has its own **Folder**
dropdown for browsing - pick HOME or any existing folder to switch the
list (search, paging, and multi-select delete/move all stay scoped to
whichever folder you're viewing), or "+ New folder..." to create an
empty one on the spot immediately (unlike the Upload section's version of
this option, which waits for the first upload into it) - picking it
submits a real form POST to the device and the page lands on the new
folder once the device redirects back, the same reliable submit-and-
redirect flow every other button on this page already uses, rather than
a background request that could leave the dropdown looking stale until
you refresh by hand. Browsing and uploading are independent - you can
browse "projectA" while uploading into "projectB", or vice versa; each
upload batch targets exactly the folder picked in the Upload section's
own dropdown, regardless of what the Files section happens to be
showing.

Any macOS system folders that show up if a Mac has ever mounted the
drive directly (`.Trashes`, `.fseventsd`, and the like) are filtered out
of every folder list and dropdown - they're not something you created
and not worth cluttering the UI with.

To move files between folders: check any number of rows in the Files
table (the same checkboxes used for delete), then click **"Move"**. That
reveals a panel beneath the table with a destination dropdown (HOME plus
every other folder) and a **"Confirm"** button - pick where the
files should go and confirm there, or click "Cancel" to back out without
moving anything. This is a real rename on the filesystem, not a copy, so
it's instant regardless of file size. A file that already exists under
that name in the destination is left alone and reported as failed,
rather than silently overwritten.

While you're inside a folder (not HOME), **"Delete this folder"**
appears as a plain text link on the same line as "Delete"/"Move", at the
right edge of that row - it removes every file in that folder and the
folder itself, after the same confirmation described above. This can't
be undone.

## The file list

Each file's row shows a checkbox, its name, size, upload date, and cut
type. Check any number of files - this also turns on the
**"Rename"**, **"Move"** and **"Delete"** buttons beneath the table (all
start greyed out/inactive, since none of them does anything with nothing
selected). The checkbox in the header selects/deselects every row
currently shown.

Those three are split across the two ends of the `.file-actions` row on
purpose: the reversible actions (Rename, Move) group together on the
left, and Delete sits alone on the right, well away from them, so the
one that destroys work isn't the button next to the one you meant to
press. "Delete this folder" joins Delete on the right for the same
reason.

**Rename** and **Move** both open a dialog (`#renameOverlay` /
`#moveOverlay`), sharing the same `.modal-overlay`/`.modal-box` shell as
the cut editor via `.dialog-narrow` - one interaction shape for both
batch actions rather than one opening a modal and the other unfolding a
strip below the table.

- **Rename** builds one row per checked file and posts them all at once
  to `/rename` as matched `from`/`to` pairs, so several files are
  renamed in a single round trip. The editable field holds only the
  *name*; the extension is rendered beside it as fixed text and is
  reassembled on submit, so a `.svg` can't quietly become a `.svh` - a
  file the Origin then stops seeing. (The visible field carries no
  `name` attribute; a hidden `to` per row is filled in by
  `confirmRename()` from stem + extension. Splitting at the *last* dot
  keeps `part.v2.svg` working.) Fields left alone are skipped rather
  than reported as failures, and a rename onto an existing name is
  refused rather than overwriting it - including the case
  `FFat.exists()` alone can't see, two files in one batch renamed onto
  the same name, where the first rename creates the very file the second
  would collide with, handled by tracking names claimed earlier in the
  loop.
- **Move** lists the files it's about to move and asks for a
  destination folder (see "Folders" above). Its `<select>` and confirm
  button live inside the overlay but carry `form='deleteForm'`, so the
  checked rows and the chosen destination still travel together in one
  POST - form association is by attribute, not DOM position. Because the
  dialog already shows exactly what is moving and where, there's no
  extra `confirm()` step; moving is reversible. **Delete** keeps its
  confirmation.

Selection only applies to what's currently visible (this page/this
search) - it doesn't reach across a search filter or onto other pages.

The **"Uploaded" date** (shown as month/day/year, no time) needs the
device to have synced time over the internet (it does this automatically
once it joins your Wi-Fi - see `config.h`'s
`NTP_SERVER`/`GMT_OFFSET_SEC`/`DAYLIGHT_OFFSET_SEC` if you want local
time instead of UTC). If your network has no internet access, or a file
was uploaded before the very first sync completed, its date just shows
as "-" instead of guessing.

The **Cut type** column reads straight off each file's own content (see
"Cut type & cut depth" above) - RouterDrive doesn't keep a separate
database of what you uploaded, it peeks at the file itself instead. It
prefers `shaper:cutType` when the file has it and falls back to the
fill/stroke colors otherwise, so a file that was color-coded in Affinity,
Illustrator or Inkscape still reports its cut types here. If every shape in
the file agrees on one cut type, that's what shows; if they don't (either
because you gave individual lines different types with the per-line
editor - see "Editing individual lines' cut type" above - or because the
file already had more than one cut type baked in when it arrived, e.g.
uploaded with cut type left "unset"), the column shows **Mixed**
instead of guessing which one to display.

Files that carry no `shaper:` attributes at all get a second pass: the
same fill/stroke colors Origin itself reads (see "How cut types are
actually encoded" above) are scanned for directly, so a file coloured
correctly in Affinity, Illustrator or Inkscape reports its real cut types
rather than looking empty. That scan is deliberately tolerant about
spelling - `#000`, `#000000`, `rgb(0,0,0)` and CSS `style="fill:..."`
all count, and any equal-RGB gray is a gray, matching Shaper's own
guidance. It is also deliberately not per-element (associating a fill
with its own shape would mean parsing whole `<path>` tags, `d` attribute
and all, which won't fit the small streaming buffer `readShaperInfo()`
uses) - it reports which cut types appear anywhere in the file, which is
exactly what the column is for.

One judgement call worth knowing about: a **plain gray stroke on its own
counts as nothing set, not as On Line**. Gray stroke is what the DXF
converter emits for every path so that a freshly converted file is valid
to Origin, so treating it as a deliberate choice would label every
single unedited upload "On Line". An explicit `shaper:cutType="online"`
does show as On Line - that's a statement of intent, the bare color is a
baseline. The practical cost is that a file where someone deliberately
set *everything* to On Line in another app reads as Unset; it's
byte-identical to the baseline, so there's nothing to tell them apart,
and the resulting cut is the same either way.

A file with nothing detectable by either route shows **Unset** in both
columns. The **Cut type** cell is itself a button: click it (whatever it
currently shows) to open the per-line editor for that file.

Once there are more than 10 files (`FILES_PER_PAGE` in `config.h`), a
search box and Prev/Next pager appear above the table so the list stays
usable instead of growing into one long scroll.

## Known rough edges / things to improve next

- Storage capacity is whatever's left of the XIAO's 8MB flash after the
  firmware itself (roughly ~4-5MB with the included `partitions.csv`) -
  plenty for SVGs, but this isn't a place to also store project photos etc.
- The upload UI has no drag-and-drop (just the file picker) and its
  "progress bar" is really just a status line, not a per-byte indicator.
  Fine for a v1; easy to improve once the core flow is proven out.
- No authentication on the web UI - anyone on your Wi-Fi (or joined to the
  setup AP) can upload/delete files. Fine for a home shop; worth locking
  down if this ever leaves that environment.
- The captive-portal redirect is a basic "send everything to /" approach;
  some phones/OSes are pickier about what makes them auto-pop the sign-in
  page. Manually browsing to the AP's IP always works as a fallback.
- Outside and Pocket are hardware-confirmed on a real Origin (a
  box-within-a-box test part, outer line Outside, inner line Pocket, cut
  types read correctly off the file). Inside, On Line and Guide are
  written with the same color encoding from the same table, so they
  should behave identically, but nobody has put those three on the
  machine yet - worth a test cut before trusting them for a real job.
- Folders (see "Folders" above), including creating/deleting them and
  moving files between them, are new and not yet hardware-tested -
  confirm the Origin actually shows subfolder contents the way you expect
  before relying on them for a real job. Folders are one level deep only
  (no nesting), and deleting one deletes its contents too - there's no
  "empty this folder first" requirement, so double-check the confirmation
  dialog before clicking through.
- The Files section's "+ New folder..." now does a real form POST/redirect
  (instead of the earlier background-request version, which could leave
  the new folder invisible until a manual refresh) and the Move UI now
  reveals its destination picker as its own panel below the file list
  rather than an always-visible dropdown - both are Playwright-verified
  against the real extracted page script but not yet exercised against
  the device itself.
- The file list's **Cut type** column used to read only the first 4KB of
  each file, which was a real bug: a per-line edit further into the file
  was invisible, so a genuinely mixed file reported a single type (or
  nothing at all). `readShaperInfo()` now streams the whole file in
  overlapping chunks instead, with a generous hard cap, so an attribute
  anywhere in the file is found. It prefers `shaper:cutType` and falls
  back to classifying fill/stroke colors, which is what lets a file
  color-coded in another app report its cut types here. Covered by
  standalone C++ tests (`test_stream_scan.cpp`, `test_color_detect.cpp`)
  including attributes straddling a chunk boundary and a real Shaper
  Studio export, and confirmed on the device against real uploaded files.
- The per-line cut editor (see "Editing individual lines' cut type"
  above) is Playwright-tested against the real extracted page script
  (opening a fetched SVG, clicking to select single and multiple lines,
  applying a cut type and depth to the selection, the "Mixed" round trip
  end to end), screenshot-checked for layout, and confirmed end to end on
  a real Origin. It also adds a new `GET /svg` route
  (`handleGetSvg()` in `web_server.cpp`) that reads a stored file's
  entire content into memory and serves it back to the browser - unlike
  the cut-type scan, which streams the file a chunk at a time and never
  holds more than a couple of KB, this one reads the whole file into
  SRAM - acceptable because it only ever runs for one file at a time, on
  demand. It works on real files but hasn't been pushed with an unusually
  large SVG, so that's where to expect trouble first.
- Very rarely, restarting RouterDrive while it's plugged into the Origin
  has shown the drive briefly enumerate (Origin shows "No files found",
  meaning it did mount it) and then drop out again (back to "No USB drive
  attached"). This looks like a known ESP32-S3 rough edge where starting
  Wi-Fi right on top of a freshly-started USB session can disrupt it - a
  short settle delay was added between the two (`USB_WIFI_STARTUP_SETTLE_MS`
  in `config.h`) to give USB enumeration room to finish first, but since
  this is an intermittent timing race, that's a mitigation, not a
  guaranteed fix. If it happens, hitting "Restart RouterDrive" again has
  reliably cleared it.
