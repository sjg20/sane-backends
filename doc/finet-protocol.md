# The fi-8000 series network protocol (PSIP)

This describes the HTTP interface of the Ricoh (formerly Fujitsu/PFU)
fi-8000 series document scanners, as used by the `finet` SANE backend.
There is no public documentation: everything here was worked out by
watching the vendor's PaperStream IP Net driver talk to an fi-8950 and
by experiment against that scanner. Field names are the scanner's own.
Anything marked *unverified* is a reasonable reading of the traffic that
has not been confirmed by experiment.

## Transport

* Plain HTTP on port 80. No TLS.
* Every request carries `Authorization: Bearer ***CLIENTACCESSTOKEN***`,
  literally that string. It is not a real credential.
* Two endpoints, modelled on Google's Privet API:
  * `GET /api/privet/info` describes the device.
  * `POST /api/privet/session` carries a JSON command. Replies are JSON.
* Images are fetched with a plain `GET` of the URI that `readImageBlock`
  returns (for example `/image/image_0.jpeg`); the body is a JPEG.
* The scanner serialises requests: two connections asking at once gain
  nothing, each request waits for the other. One connection is enough.

## Discovery

Scanners do not announce themselves, but they answer a UDP broadcast
probe on port 52217. The probe is 32 bytes: the magic `fiCH`, the
sender's IPv4 address at offset 8 and MAC at offset 12, and the bytes
`ff 02` at offset 23; the scanner ignores the rest. The reply starts with
the same magic, has the scanner's IPv4 address at offset 16 and a name
such as `fi-8950-CLAC001933` (model and serial) as a C string at
offset 40. Replies arrive within a second or so.

## Device information

`GET /api/privet/info` returns, among a lot else (counters, EEPROM
contents, power settings):

| field | meaning |
|---|---|
| `deviceName` | for example `fi-8950-CLAC001933` |
| `model` | `fi-8950` |
| `serialNumber` | |
| `deviceState` | `noSession`, `ready` (session open), `capturing`, ... |
| `inUse`, `id` | whether another client holds the scanner, and who (`other user`) |

## Commands

The body of every `POST /api/privet/session` is

```json
{"commandId": "<any string>", "commandTimeOut": "<seconds>",
 "method": "<name>", "parameters": { ... }}
```

The reply echoes `commandId` and `method` and has a top-level `status`,
normally `"success"`, plus `results`. Other statuses seen: `"noImage"`
(from `readImageBlock`, see below) and `"busyPsip"` (from `createSession`
when a session is already open: sessions are exclusive, and one left
open by a crashed client blocks the scanner until it times out or is
closed by id). Most replies include a `session` object with the
`sessionId`, its `state` and a `revision` counter.

`commandTimeOut` is largely ignored; in particular `readImageBlock`
holds for about ten seconds regardless of it.

### createSession

```json
{"connect": "PSIP"}
```

Returns `results.session.sessionId` (a decimal string, for example
`"1788673358"`), which every later command must carry as `sessionId`.
The vendor driver also passes a `sendTo` list; it is not needed.

### getSession

```json
{"operationCode": 1, "sessionId": "..."}
```

Returns the session `state` and `deviceStatus`. The useful parts:

* `deviceStatus.sensor`: `hopperEmpty`, `adfCover`, `pick`, `top`,
  `imp_cover`, each `"on"`/`"off"`. `hopperEmpty` says whether there is
  paper to feed; `pick` and `top` show a sheet in the paper path.
* `imageBlocks`: an array of the block numbers of every captured image
  the scanner is holding that has not been released yet. Its length is
  the number of images waiting to be fetched (less any the client has
  fetched but not released). Empty when there is nothing waiting.
* `errors`: `code` and `device.code`/`message`; `code` `"038032"` appears
  while images are pending and is not an error.
* `deviceStatus.alarm.consumables`: roller wear flags.

`operationCode` 2 was also seen in the traffic (*unverified*: a longer
form of the same).

### sendTask

```json
{"sessionId": "...", "task": {"actions": {"streams": {"sources": {
  "feedControls": { ... }, "pixelFormats": { ... }, "readControls": { ... }
}}}}}
```

Sets up the scan. Each section holds groups of attributes, every one of
the form `{"attribute": "<name>", "values": {"value": "<string>"}}`;
numbers are sent as strings. The scanner echoes the whole task back with
every attribute it knows filled in, which is the only way to learn its
vocabulary: unknown attributes are silently ignored, not rejected.

Lengths are in **1/1200 inch**. The fi-8950 bed is 12 by 17 inches, so
the full window is 14400 by 20400.

`feedControls`:

| group / attribute | values | notes |
|---|---|---|
| `numberOfSheets` / `sheetCounts` | `0` = until the hopper is empty, or a count | with a count the feeder stops after that many sheets |
| `doubleFeed` / `overlap` | `enable`/`disable` | ultrasonic multifeed detection |
| `doubleFeed` / `length` | `enable`/`disable` | length-based detection |
| `doubleFeed` / `response` | `recovery` | what to do on a multifeed |
| `doubleFeed` / `deviceSpecification` | `disable` | |
| `doubleFeed` / `iOMFLength` | `0` | |
| `background` / `bgColor` | `white`/`black` | colour of the backing behind the sheet |
| `prePick` / `prePickControl` | `enable`/`disable` | feed the next sheet to the pick position early |

`pixelFormats` attributes:

| attribute | values | notes |
|---|---|---|
| `resolution` | dpi, 50..600 | |
| `width`, `height` | 1/1200 in | the scan window |
| `offsetWidth`, `offsetHeight` | 1/1200 in | window origin |
| `paperWidth`, `paperLength` | 1/1200 in | the paper size the feeder expects; the window is capped by it |
| `automaticSize` | `enable`/`disable` | scanner-side cropping to the detected sheet; see below |
| `automaticDeskew` | `enable`/`disable` | |
| `compression` | `jpeg` | the only format seen; the scanner always delivers JPEG |
| `jpegQuality` | 1..100 | |
| `jpgSubSampling` | `422` | |
| `overscan` | `on`/`off` | |
| `endOfPageDetection` | `on`/`off` | |
| `dropoutColor` | for example `green` | *unverified*: the vendor driver's task with this set produced a greyscale JPEG; without it, colour |

`readControls`:

| group / attribute | values | notes |
|---|---|---|
| `imageCacheMode` / `imageCacheMode` | `scannerMemory` | images are held in the scanner until released |
| `imageTransferMethod` / `imageTransferMethod` | `alternate` | front and back of each sheet are delivered as consecutive blocks |

The scanner always scans both sides of every sheet and always delivers
colour JPEG at the requested resolution. Single-sided, greyscale and
lineart output are the client's job: drop the unwanted side, convert
when decoding.

### startCapturing

```json
{"ignore_mf_detection": "false", "restartCapturing": "false",
 "sessionId": "..."}
```

Starts the feeder. The reply's `results.session.detected` is `"success"`
or an error code (for example `038008` when another client holds the
device); the session `state` becomes `capturing`. Give it a long
`commandTimeOut` (the backend uses 35 s): it does not return until the
feeder has picked the first sheet, and it fails when the hopper is
empty.

### readImageBlock

```json
{"imageBlockNum": N, "withMetadata": "enable",
 "duplexMetadata": "disable", "sessionId": "..."}
```

Blocks are numbered from 1 and must be read in order. With
`imageTransferMethod` `alternate`, odd blocks are fronts and even blocks
the backs of the same sheets. The reply's `results.metadata`:

* `address`: `uri` to `GET` for the JPEG, `imageNumber` and
  `sheetNumber` (both from 0), `source` (`front`/`back`).
* `image`: `pixelWidth`, `pixelHeight`, `resolution`, `size` (bytes of
  the JPEG; check the download against it), `paperWidth`, `paperHeight`,
  `pixelOffsetX/Y`, `adjustPixelWidth/Height`, `noAdjustPixelWidth/Height`,
  `requestDriverCropDeskew`, `requestDriverLUT`.
* `imageBlock`: `imagePart`, `moreParts` (`"true"` if the image is split
  over several blocks; never seen for the sizes used here, and not
  supported by the backend).
* `status`: `detected`, `endOfScan`, `multifeed`.

With `readControls` / `devidedSize` set in the task (the vendor driver
sends 12), `readImageBlock` needs an `imagePartNum` as well (from 1)
and answers `noImage` without it. The image may then come in parts,
each with its own URI, the last named `/image/<n>_end.jpeg`; `moreParts`
says whether another follows. At the sizes seen here (up to 1 MB) every
image is a single `_end` part, so the value does not split them, but the
scanner hands images over about a quarter faster in this mode (about
300 against 400 ms per side in back-to-back trials), so the backend
always asks for it.

With `duplexMetadata` `enable`, `metadata` is instead a list of two
entries, the front and back of the sheet this block belongs to. Blocks
are still one per side, so consecutive blocks return the same pair; it
saves nothing. `withMetadata` `disable` drops the metadata altogether
(the URI is then `/image/image_<block-1>.jpeg`).

If no image is ready the scanner holds the request for about ten
seconds (whatever `commandTimeOut` says) and then answers with top-level
`"status": "noImage"` and empty metadata. This is the normal answer at
the end of the paper, and also while the scanner is still working on
the next sheet: check `hopperEmpty` to tell them apart.

Timing, measured on an fi-8950 at 300 dpi colour duplex: while the
feeder is running, the front of each sheet is handed over about 400 to
490 ms after it is asked for and the back at once, whether the window
is A4 or the full bed, so delivery runs at the scanner's own cadence of
roughly 2 sheets a second and cannot be hurried from the client. The
feeder itself runs faster than that, so the scanner accumulates
finished images (tens of them over a long batch), which `imageBlocks`
shows. Once the feeder is stopped the waiting images come out in about
25 to 45 ms each. Fetching an image is then: `readImageBlock` ~25 ms,
`GET` ~25 ms for 800 KB, `releaseImageBlocks` ~16 ms.

### releaseImageBlocks

```json
{"imageBlockNum": N, "sessionId": "..."}
```

Frees a block's image in the scanner. Several blocks may be outstanding
at once, and they are listed in `imageBlocks` until released.

### stopCapturing

```json
{"pauseScanning": "true" | "false", "sessionId": "..."}
```

Stops the feeder at once. What happens to the images the scanner is
holding depends on `pauseScanning`:

* `"true"`: the images stay and can still be read with
  `readImageBlock`; sheets already in the paper path finish and are
  added. The session state becomes `draining`, then `capturing` again.
  This is how a batch is stopped early without losing the sheets that
  have gone through.
* `"false"`: the images are discarded.

After a pause, send `stopCapturing` with `"false"` before closing.

### closeSession

```json
{"sessionId": "..."}
```

Ends the session; `deviceState` returns to `noSession`. Always close
the session, or the next `createSession` gets `busyPsip`. A session left
open by a dead client can be closed by anyone who knows its id.

## A scan, end to end

```
createSession                  -> sessionId
getSession                     -> hopperEmpty off?
sendTask                       (sheetCounts 0 for the whole hopper)
startCapturing                 -> detected success
loop:
  readImageBlock N             -> uri, size   (noImage: hopper empty, or wait)
  GET uri                      -> JPEG
  (decode / store)
  releaseImageBlocks N
  N = N + 1
stopCapturing pauseScanning false
closeSession
```

To stop early without losing sheets: `stopCapturing` with
`pauseScanning` `"true"`, keep reading until `imageBlocks` is empty and
the `pick`/`top` sensors are off, then `stopCapturing` `"false"` and
`closeSession`.

## Auto-size and the black backing

The scanner's own `automaticSize` is not reliable enough to use: with a
white backing a white page has no edge to find and the full window
comes back; with a black backing it crops inconsistently and pads the
result with white. The backend therefore sends `automaticSize`
`disable` with the full-bed window and `bgColor` `black`, and crops the
page out of the black frame itself after decoding (see
`content_bbox()` in `finet.c`). The cost is that the scanner encodes
and sends the whole bed (18 MP at 300 dpi) rather than the page; its
delivery cadence turns out not to depend on the image size, so this
does not slow scanning, only the decode.

## What does not make it faster

Measured on an fi-8950 (rated 150 ppm / 300 ipm) with 7 by 9 inch sheets
at 300 dpi colour duplex, reading as fast as possible with no decoding.
The scanner delivers about 240 to 280 ms per side (roughly 220 to 250
ipm) whatever the task says; the spread between runs is about 20 ms:

* resolution 200 instead of 300: no change (so it is not pixel-bound)
* `jpegQuality` 50, `jpgSubSampling` 420, `compression` none (ignored:
  JPEG still comes), `dropoutColor` green: no change (not encode-bound)
* window 7.5 in instead of the 17 in bed: no change
* `imageCacheMode` `pcMemory`: no change; `none`: breaks the scan
* two connections fetching at once: no change (requests are serialised)
* releasing blocks late, or not at all during the run: no change (so
  releasing is not flow control)
* a persistent connection instead of one per request: no change
* `duplexMetadata` `enable`, `withMetadata` `disable`: no change
* `paperProtection`, `soundJam`, `stapleDetection`, `paperProtection3`
  all `disable`: no change
* `moireRemoval`, `sRGBPattern`, `skewCorrection` `disable`: no change
* `highSpeedMode` `on`: slower (320 ms) and the JPEGs triple in size
* `prePickControl` `enable`: the feeder runs continuously and delivery
  improves a little (about 10%); the backend enables it by default
* `endOfPageDetection` `on`: about 10% faster and smaller images
* `devidedSize` `12` (with `imagePartNum` in `readImageBlock`): about
  25% faster; the backend uses it

So the feeder can outrun the scanner's own image pipeline: over a long
batch tens of sheets are fed but not yet delivered, and they come out
after the feeder stops. Nothing in the protocol shows that backlog: the
sheet counters in `/api/privet/info` only advance when the session is
closed, and `imageBlocks` lists finished images only.

## What the vendor driver does (captured 2026-09-08)

A PaperStream Capture batch of 74 sheets at 200 dpi colour duplex,
captured on the PC running it, uses exactly the sequence above on one
keep-alive connection: `createSession`, `getSession` polls, `sendTask`,
`startCapturing`, then `readImageBlock` / `GET` / `releaseImageBlocks`
per side, with `getSession` every few sides. The scanner never answered
`noImage`. Between the application's own pauses the images came at a
median of 223 ms per side, the same cadence the backend sees. Its task
differs from the backend's in: `automaticSize` `enable` with `bgColor`
`black`, `overscan` `on`, `automaticDeskew` `enable`, `prePickControl`
`enable`, `doubleFeed` `overlap` `disable` with `response` `notify`,
`verticalLine` `disable`, and three attributes of unknown meaning with
values `rgb24`, `devidedSize` and `feeder`. Image URIs were of the form
`/image/<n>_end.jpeg`.

## Things not known

* The attribute that selects single-sided scanning, if any: the
  scanner scans both sides regardless, and the backend drops the
  unwanted one.
* Whether `dropoutColor` really selects greyscale output, and what the
  scanner can do besides JPEG.
* Any way to make the scanner deliver faster than its own cadence
  while the feeder runs.
* The meaning of most of the `getSession` and `/info` fields not listed
  above; they are echoed but unused.
