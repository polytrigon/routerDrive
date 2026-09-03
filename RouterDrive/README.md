# RouterDrive

A wireless "USB drive" for the Shaper Origin: a Seeed XIAO ESP32-S3 plugs into
the Origin's USB-A port and presents itself as a flash drive full of SVGs,
while you upload new SVGs to it over Wi-Fi from your phone or laptop.

This is a starting point, not a finished/tested product - you're the first
person to run this particular firmware on this particular hardware
combination. Budget an evening for the bring-up steps below before you trust
it at the tool.

## How it works (read this before you flash anything)

- **Storage** is the XIAO's onboard flash (no SD card needed), formatted as
  FAT and exposed to the Origin over USB using the ESP32-S3's native
  USB-OTG peripheral in Mass Storage Class (MSC) mode.
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
- Physically: hold the **BOOT** button on the XIAO down while powering it
  on / pressing RESET, keep holding for ~3 seconds. Saved credentials are
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
- **Not supported: TEXT/MTEXT.** There's no font-rendering available on
  this converter, so any lettering is silently skipped (the status message
  after conversion tells you how many text entities were dropped). If a
  design needs real text, either convert the text to outline curves in
  your CAD tool first (in Onshape, look for "convert to sketch geometry"
  or similar on the text feature) before exporting the DXF, or skip
  conversion entirely and export/upload a finished SVG instead.
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

## The file list

Each file's row shows a checkbox, its name, size, and upload date. Check
any number of files and click "Delete selected" beneath the table to
remove them all in one go - the checkbox in the header selects/deselects
every row currently shown. You'll get one confirmation naming the file (or
the count, if you selected more than one) before anything is deleted, same
`confirm()`-dialog pattern as the overwrite warnings above. Selection only
applies to what's currently visible (this page/this search) - it doesn't
reach across a search filter or onto other pages.

The **"Uploaded" date** needs the device to have synced time over the
internet (it does this automatically once it joins your Wi-Fi - see
`config.h`'s `NTP_SERVER`/`GMT_OFFSET_SEC`/`DAYLIGHT_OFFSET_SEC` if you
want local time instead of UTC). If your network has no internet access,
or a file was uploaded before the very first sync completed, its date
just shows as "-" instead of guessing.

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
