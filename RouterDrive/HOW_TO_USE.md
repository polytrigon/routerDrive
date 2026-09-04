# Using RouterDrive

This is the day-to-day guide: how to get files onto the Origin once
RouterDrive is already built, flashed, and set up. If you haven't gotten
that far yet, start with `README.md` instead - it covers parts, Arduino
IDE setup, and first flash/bring-up. This doc assumes all of that is
already done and the device is plugged into your Origin.

## Opening RouterDrive

- **Already joined your home Wi-Fi?** Open `http://routerdrive.local/`
  from any phone/laptop on the same network. (If `.local` addresses
  don't resolve on your network, use the IP address shown in Arduino's
  Serial Monitor instead.)
- **Never set up Wi-Fi, or just reset it?** The device broadcasts its own
  hotspot, `RouterDrive-Setup`. Join it and either follow the "sign in to
  network" popup, or open `http://192.168.4.1/` yourself. From there you
  can either enter your home Wi-Fi's credentials, or skip that entirely
  and use RouterDrive straight off its own hotspot - see "Using it
  without joining Wi-Fi" in `README.md` if you want that route.

Either way you land on the same page: USB/Wi-Fi status at the top, your
file list, then the upload form.

## Uploading a design

1. Click the file picker and choose one or more `.dxf`/`.svg` files
   (mix both if you like - `.dxf` files are converted to SVG
   automatically, right in your browser).
2. Optionally pick units (mm/inches - only matters for DXF files) and a
   folder to upload into (see "Folders" below).
3. Optionally set a **Cut type**, **Tool diameter**, and **Cut depth** -
   see "Cut type, depth, and tool diameter" below. Leave Cut type on
   "unset" to skip this and upload the file exactly as converted.
4. Click **Convert & upload**.
5. If a file with that name already exists in the target folder, you'll
   get a warning before it's overwritten.

After uploading, click **Restart RouterDrive** so the Origin picks up
the new file(s) - it usually needs this even while the cable stays
plugged in. If the file list ever looks out of date on the Origin's own
screen after that, a physical unplug/replug of the USB cable is the
fallback that always works.

## Cut type, depth, and tool diameter

Above the upload button:

- **Cut type**: Outside, Inside, Pocket, On Line, Guide, or "unset."
  Whichever you pick applies to *every* line in that upload.
- **Tool diameter**: only shown for Outside/Inside/Pocket (the cut types
  that need to know your bit size to offset the toolpath correctly).
  Pick a common size from the dropdown, or "Custom..." for anything
  else.
- **Cut depth**: optional. Leave blank to not set one.

This is a whole-file setting - useful when every line in the design
should be cut the same way. For a design that needs more than one cut
type (a box cut out of a pocketed base, say), upload it first and use
the per-line editor instead - next section.

## Giving individual lines their own cut type

Say you've got a box-within-a-box design: the outer line should be cut
all the way through (Inside), and the inner one should only be a shallow
Pocket. The upload controls above can't do that - they set one cut type
for the whole file. For this, use the file list instead:

1. Upload the file first (cut type "unset" is fine, or pick whatever
   most of the lines should be - you can change any of it next).
2. In the file list, click that file's **Cut type** cell (whatever it
   currently shows - "-", a type, or "Mixed").
3. A viewer opens showing the actual file. Click a line to select it -
   it highlights in cyan. Shift-click to select more than one line at
   once if several should share the same setting.
4. In the side panel, pick a **Cut type** (plus **Tool diameter**/**Cut
   depth** if that type needs them) and click **Apply to selected**. The
   line(s) recolor to match - the small legend in the panel shows which
   color means what, so you can see at a glance what's been set and
   what's still black (not set yet).
5. Repeat steps 3-4 for any other lines that need a different setting.
6. Click **Save changes** to write it all back to the file. (Closing the
   editor without saving asks you to confirm first if you've applied
   anything.)

Once a file has more than one cut type on it, its **Cut type** column in
the file list shows **Mixed** instead of a single type - that's your
signal it's using per-line settings rather than one blanket type.

## Folders

Files can be organized into one level of folders (no folders-within-
folders). To create one: pick "+ New folder..." from either the Upload
section's folder dropdown, or the folder dropdown at the top of the file
list - either way it'll prompt you for a name.

- The file list's folder dropdown switches which folder you're viewing.
- **Delete this folder** (next to Delete/Move, only shown while viewing
  a folder) deletes the folder and everything in it - you'll get a
  confirmation first.
- Check any files, click **Move**, then pick a destination folder (or
  HOME for the root) and confirm - this actually relocates them, not a
  copy.

## Managing the file list

- Check any number of files to enable **Delete** and **Move** (both stay
  greyed out until something's checked).
- The search box filters by filename; a Prev/Next pager shows up once
  you have enough files that they don't fit on one page.
- The **Uploaded** date needs the device to have synced time over the
  internet - if it hasn't, or your network has none, you'll see "-"
  instead of a guessed date.

## Checking status and restarting

- The top of the page shows **USB connected**/**not connected** and your
  Wi-Fi network (or "setup mode" if it hasn't joined one).
- The footer on every page shows when the currently-running firmware was
  built - handy for confirming a re-flash actually took.
- **Restart RouterDrive** (bottom of the page) is the normal way to get
  the Origin to notice new/changed files. It takes a few seconds, drops
  Wi-Fi briefly, and the page reconnects on its own - no need to
  manually refresh.

## Resetting Wi-Fi credentials

Two ways to make RouterDrive forget its saved Wi-Fi and go back to
broadcasting `RouterDrive-Setup`:

- **From the web UI**: the **Forget Wi-Fi** button, under the Wi-Fi
  section once you're connected.
- **Physically, on the device itself**: the XIAO board has two small
  buttons, silkscreened **B** and **R**. Hold down **B**, then - while
  still holding it - tap **R**, and keep holding **B** for about 3
  more seconds before letting go. (Tapping **R** by itself just reboots
  the device and does *not* touch your saved Wi-Fi - **B** is the one
  that matters for actually resetting credentials.)

## Status LED

A small light on the board tells you the device's state at a glance:

| Pattern | Meaning |
|---|---|
| Solid on | Connected to your Wi-Fi and idle - ready to use |
| Slow blink | Setup/AP mode - broadcasting `RouterDrive-Setup`, waiting for Wi-Fi credentials |
| Fast blink | Busy - a file upload or delete is in progress |
| A few quick flashes, then off | Restart was just triggered (confirms the button press registered) |

It's a single fixed-color (yellow) LED, not RGB. If you'd rather it just
stayed dark, there's a small "Turn status LED off" link at the very
bottom of the page - click it again to turn it back on. The setting is
saved and survives restarts/power cycles.

## If something's not working

This guide covers the happy path. For known limitations, unresolved
issues, and what hasn't been hardware-tested yet, see `README.md`'s
"Known rough edges / things to improve next" section - and if it's not
listed there either, it's worth mentioning so it can get tracked.
