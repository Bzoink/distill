# Media playback plan

Status: implemented for the desktop shell. Audio plays inside the reader.
Video does not.

## Decision

**Audio plays in the reader, on every target.** Play/pause, stop, volume,
elapsed time, duration, and a position handle that follows playback and can be
dragged.

**Video is never decoded.** A direct video file gets a **Download** action and
nothing else. An embed — a YouTube or Vimeo iframe, an `<object>`, an
`og:video` reference — has no file to download, so it keeps the external
hand-off the reader already performs, with the URL shown as copyable text
beside it.

No autoplay, on either. Nothing is fetched until the user asks for it. No
multimedia dependency reaches the extraction engine.

## Why audio and not video

The original scope cut put audio and video together because both are "media."
They do not cost the same thing.

**Video's cost is synchronisation and paint.** A/V sync, frame timing tied to
the display's refresh, and decoded frames blitted into the mmap'd surface while
the UI stays responsive. That cost is different on every render target and does
not exist at all on the TUI, where there is nothing to blit into. Above it sits
the container and codec pile — MP4/WebM, H.264/VP9/AV1, HLS/DASH manifests,
DRM — which is where FFmpeg-sized dependencies come from. Every part of that is
real, and none of it is work this project wants.

**Audio has neither.** No sync problem, no paint path, no relationship to the
display at all. A ring buffer and a decode callback. The format explosion does
not happen either: web video is a dozen live combinations, but audio in a
*reader* context is a direct MP3 nearly every time.

Then the asymmetry that actually decides it. **Audio is the only medium whose
output path is identical on desktop, framebuffer and TUI.** ALSA does not care
whether a window manager exists. Video's output path differs on all three
targets and is impossible on one. So the medium that is cheapest to do natively
is also the one where handing off fails worst: a framebuffer console may have
no `xdg-open` association worth the name, and a TUI reached over SSH that
spawns `mpv` plays the sound on the wrong machine. That is not a degraded
experience, it is a broken one, and it lands precisely on the targets this
project is heading toward.

Handing video off, by contrast, costs the user one application switch on a
desktop that already has a video player, and is the only available answer for
an embed on any target.

## Amendment required to PLAN.md

This plan contradicts [PLAN.md](PLAN.md) §2 of *What this deliberately isn't*,
which reads "no in-app playback." That section's own rule is that a reversed
decision must carry its corrected reasoning, so the cut is reworded rather than
deleted. **Nothing here proceeds until PLAN.md carries this text:**

> 2. **Not a media engine.** No WebGL, no video pipeline, no WebAssembly, no
> in-app video playback. Text, structure, images and audio are the target. A
> page that embeds video is still content worth surfacing: we extract what
> metadata is available — title, thumbnail, duration, source URL — and present
> it as a card offering a download for a direct file or an external hand-off
> for an embed, rather than decoding it ourselves.
>
> *(Corrected. The original cut lumped audio in with video because both are
> "media"; they have different costs. Video's cost is A/V sync, frame timing
> and a per-target paint path, on top of a container/codec stack. Audio's is a
> ring buffer and a decoder, and it is the one medium whose output path is
> identical on desktop, framebuffer and TUI. The external hand-off that makes
> video cheap makes audio useless exactly where this project is going: a
> framebuffer console has nowhere to hand off to, and an SSH'd TUI hands off to
> the wrong machine. Audio playback is therefore in scope, scoped to direct
> MP3/WAV/FLAC, one stream at a time, no autoplay. Video's cut stands
> unchanged, and the media-presentation model — nothing fetched until
> activated — governs audio too.)*

## What the reader offers

The action follows what the reference **is**, not what medium it belongs to.
This is the load-bearing distinction in the whole plan.

| Reference | Primary action | Secondary |
|---|---|---|
| Audio (`<audio>`, `<source>`, JSON-LD enclosure) | **Play** | Download, Open externally |
| Direct video file (`<video src>`, `<source>`) | **Download** | Open externally |
| Embed (iframe, `object`, `og:video` metadata) | **Open externally** | — (URL shown as copyable text) |
| Image | Viewer mode, as today | Save |

An embed is never offered a download, because there is no file behind it. A
direct file is never offered playback. Getting this classification wrong is how
the reader ends up promising a download that cannot happen.

## What the code does today

An accounting, because the first draft of this plan assumed capabilities that
do not exist and proposed work that is already done.

**Missing entirely:**

- **Audio.** The string does not appear anywhere in `src/`. `media_json` in
  [src/extraction.inc](../extraction.inc) collects `<img>` only; `videos_json`
  collects `video`, `iframe`, `embed`, `object`. No `<audio>`, no enclosure
  links.
- **`<source>` children.** `videos_json` reads the `src` *attribute* only, so
  the common `<video><source src="..."></video>` yields an empty URL and a card
  with a dead action.
- **The direct/embed distinction.** `kind` is the tag name. Nothing tells the
  template whether a file exists behind the reference.
- **`duration` and `mime`.** Not extracted for anything.
- **The `Audio` constructor.** Not in the JS surface, so a page whose player
  does `new Audio(src)` throws mid-settle. This may be costing *extraction*, not
  just playback.

**Already present, and not to be rebuilt:**

- **External hand-off.** `open_external` in [src/app.c](../app.c) spawns
  `xdg-open` with an argument vector and no shell, gated by
  `external_uri_allowed`, covered by `--self-test`. The first draft's proposed
  "safe argument-vector launch" is this, already written.
- **Scheme validation at the point of use.** `safe_uri` in the engine and
  `external_uri_allowed` in the shell both allowlist. A raw URL does not reach a
  process launch unguarded today.
- **Lazy media fetch.** `--fetch-media` in the engine, driven by
  `fetch_media_file` in the shell.

**Present but not sufficient:**

- `fetch_media_file` is a blocking `g_spawn_sync` of a one-shot engine. Correct
  for a thumbnail, unusable for a podcast episode or a video file that needs
  progress and cancellation. This is the real cost in the download work.
- The reader's media links carry the URL inline as `distill-media:<url>`.
  Moving to opaque page-local IDs is a *migration* of existing behaviour, not a
  new feature, and it is defence in depth rather than a fix for a live hole.

**Dropped from the first draft:** the configurable **Video player** setting and
its missing-executable handling. `xdg-open` already covers the embed case, and
with video reduced to downloads there is nothing left for a configured player
to do. This also keeps the media work clear of `settings.ini`, which is a
GLib `GKeyFile` in the GTK shell and therefore unavailable to the framebuffer
and TUI targets under PLAN.md's no-GLib rule.

## Architecture

### Which process owns the device

`build/distill` is one-shot: it is re-spawned per navigation and exits after
emitting its artifact. **Playback therefore cannot live there.** The media core
belongs to the long-lived shell process, whichever shell that is — the GTK app
today, the framebuffer and TUI shells later. All three link it. The one-shot
engine's only role in media is what it already does: fetch a validated URI to a
file and exit.

This is the opposite of what the first draft implied, and it matters for the
build: the GTK shell links miniaudio, it does not merely adapt something the
other targets own.

### Normalized media records

One record shape for every front end, owned by the current page:

```text
MediaItem {
    id          stable page-local identifier
    kind        audio | video | image | embed
    direct      true if a fetchable file stands behind the URL
    url         validated absolute URI
    title, alt, poster, mime, duration
}
```

The URL and declared MIME type are hints, never a decision. Classification is
structural: an `iframe`, `object`, or `og:video` reference is an `embed`
regardless of what its URL looks like, and an embed is never handed to the
decoder.

`direct` decides the card's actions, so it is part of the extraction JSON
contract and gated like the rest of it.

### Actions

`distill-play:<id>`, `distill-stop:<id>`, `distill-download:<id>`,
`distill-open:<id>`, resolved against the current page's registry and
re-validated at the point of use. The reader template presents labels and hit
targets; it never owns decoding or process launching. Every front end
dispatches the same actions into the same core.

### One download core, two destinations

Audio's "fetch to a private temp file, then decode" and video's "fetch to a
file the user chose" are **the same mechanism**, differing only in destination
and in what happens on completion. Build it once:

- asynchronous spawn of the existing `--fetch-media` path, replacing the
  blocking call;
- progress and cancellation;
- write to a temporary file in the destination directory, `rename` into place
  only after the transfer completes;
- HTTP error and disk-full reporting, partial-file cleanup on every exit path;
- a safe initial filename from `Content-Disposition`, the URL path, or the
  title, with the user choosing the final destination for an explicit download.

Fetching to a file first also keeps range requests, redirects, signed URLs and
network retries out of the audio device's problem space entirely. A streaming
source can replace it later without the UI state machine noticing.

### The audio core

`src/media_audio.c` / `.h`, with no miniaudio types in the public header:

```text
MediaAudioState {
    stopped | loading | playing | paused | finished | error
    item_id
    position_frames, duration_frames, sample_rate
    volume
    error_code, error_message
}

media_audio_open(path)    media_audio_seek(seconds)
media_audio_play()        media_audio_set_volume(value)
media_audio_pause()       media_audio_snapshot(state)
media_audio_stop()        media_audio_close()
```

One decoder, one device, one stream globally. Navigation stops playback and
releases the device deterministically.

**The threading contract, which is the part that bites.** miniaudio's data
callback runs on a real-time audio thread the shell does not control. Inside
it: no allocation, no locking, no front-end call of any kind. Position and
state are published as atomics; the front end reads them with
`media_audio_snapshot` on a main-loop tick and never blocks on decode or device
startup. A seek moves the decoder under a `seeking` guard so that a redraw
cannot recursively trigger another seek.

At end of stream the core transitions to `finished` and resets to the
beginning. It does not start again.

### Front-end controls

Behaviour is shared; presentation is each front end's own.

- **TUI:** `Space` play/pause, `s` stop, left/right seek, `[`/`]` volume, a
  text progress bar redrawn on the tick.
- **Framebuffer:** the same keys, plus pointer drag on the position bar.
- **GTK:** a control bar over the same core. An adapter, never the owner.

With a known duration the handle is proportional; without one, show elapsed
time and an indeterminate bar until the source reports a duration.

## Library decision

**miniaudio** for the audio core: single-source C, no required external
dependency, decoding and playback together, and backends for ALSA, PulseAudio,
Core Audio, WASAPI, OSS, sndio and JACK plus a null backend. It compiles into
the C23 build and stays behind our own API.

- [miniaudio repository and feature list](https://github.com/mackron/miniaudio)
- [miniaudio decoder and playback manual](https://miniaud.io/docs/manual/)

Stock decoders — **WAV, MP3 and FLAC** — are the whole initial format set. MP3
covers essentially all real page audio; WAV and FLAC come free with the
library. Vorbis or Opus only when a live page demands one, through an explicit
decoder adapter. AAC/M4A is not promised until a decoder and a distribution
story exist.

Because it is a static single-source library, this adds no shared object and
leaves PLAN.md's ≤ 8 MB framebuffer footprint budget untouched.

### Alternatives considered

- **dr_libs** — the same decoders without the device layer. Worth switching to
  only if binary size or decoder control starts to matter; miniaudio's device
  abstraction is the part that is hard to replace.
- **PortAudio / libao** — output abstractions that do not decode, and that add
  an installed dependency.
- **FFmpeg / libav** — capable and far too large, and pointless when video is
  never decoded.
- **libmpv / libVLC** — right for embedding a real video player, wrong for a
  small audio core. Out of the dependency graph.
- **PL_MPEG** — small and C, but MPEG-1/MP2 only, with deliberately weak error
  reporting. It cannot play ordinary web media and must never be presented as
  though it could.

Audio must reach an OS backend somewhere; miniaudio confines that to one small
API and lets each target choose. The media core and its controls stay
platform-neutral.

## Delivery phases

Extraction first. Nothing can be played, downloaded or classified before the
records describe reality, and the extraction work is valuable on its own.

### Phase A — see the media that is actually there

1. Stub `Audio` in the JS surface as a no-op that records the URL it is handed.
   Pure content fidelity: it stops player scripts throwing mid-settle, and it
   reports how common JS-driven audio really is before anything is decoded.
2. Extract `<audio>`, read `<source>` children for both audio and video, and
   pick up JSON-LD enclosures using the existing JSON-LD machinery.
3. Add `direct`, `mime` and `duration` to the media records; classify embeds
   structurally.
4. Give the reader cards the actions from the table above, still dispatching to
   the existing hand-off.

**Done when** every media reference carries an action that can actually
succeed, no embed offers a download, and the new fields are pinned in the
golden files.

Declarative sources only. A player built in JavaScript is source-site UI and is
out of scope on the same grounds as a `div`-based date picker.

### Phase B — the download core

1. Replace the blocking `fetch_media_file` spawn with an asynchronous one.
2. Progress, cancellation, atomic rename, partial-file cleanup, HTTP and
   disk-full reporting.
3. Wire **Download** for audio, direct video and images.

**Done when** video is useful with no decoder in the process, and a cancelled
or failed download leaves nothing behind.

### Phase C — the audio core

1. Vendor miniaudio in an isolated third-party directory, backends selected per
   target.
2. The C23 wrapper: one decoder, one device, file input from Phase B.
3. Play/pause/stop/seek/volume/position/EOS/error transitions, with the
   threading contract above.
4. Deterministic tests and local WAV/MP3/FLAC fixtures.

**Done when** audio plays from a shell with a moving position handle, and
navigation or exit releases the device every time.

### Phase D — front ends

GTK first as the available test surface, then the framebuffer and TUI controls
as those shells arrive. One implementation behind all three.

### Phase E — none

There is no direct-video phase. If external playback ever proves insufficient,
that is a new decision requiring its own amendment to PLAN.md §2, not a
continuation of this one.

## Implementation status

**Landed.** Audio extraction (`<audio>`, `<source>` children, JSON-LD
enclosures), the `direct`/`embed` split and the `mime`/`duration`/`playable`
fields, the `Audio` JS stub and its `script_audio` diagnostic, the reader cards
and their actions, the C23 audio core over miniaudio with its deterministic
tests, the shared asynchronous transfer with cancellation and atomic rename,
`--fetch-kind` on the engine, and the GTK transport bar. Gated by
`37-audio-media` and `38-video-direct`, with goldens, and by
`distill-media-audio-test` in ctest.

**Deliberately not done yet**, so that the next person is not misled:

- **Opaque page-local action IDs.** The actions carry the URL inline, in the
  same shape as the `distill-media:` scheme that was already there, and are
  re-validated at the point of use by `external_uri_allowed` in the shell and
  `safe_uri` in the engine. The registry remains worth having as defence in
  depth; it is a migration of existing behaviour rather than a fix for a live
  hole, and it should move all four schemes at once or none.
- **Byte-level download progress.** There is a transfer row with a Cancel
  button, and a transfer that fails or is cancelled leaves nothing behind, but
  the engine writes the file itself and reports no progress, so the row is
  indeterminate. Reporting real progress means the engine emitting it.
- **The image viewer's blocking fetch.** `fetch_media_file` still spawns
  synchronously. The new asynchronous transfer sits beside it rather than
  replacing it; moving the viewer over is a separate change with its own
  visible behaviour.
- **The end-of-content image gallery inside `<main>`.** Video cards now render
  whether or not the page has blocks outside `<main>`; images still do not.
  Same gap, same fix, but it belongs to the media-reference work rather than
  to this plan.
- **Framebuffer and TUI controls.** Those shells do not exist yet. The core is
  built so that they link it directly; GTK is already an adapter over it, not
  its owner.

## Testing and gates

Phase A changes the extraction JSON, which `corpus/golden/` pins and
`python3 corpus/check.py --extract` gates. That makes corpus work part of the
deliverable, not follow-up:

- A new isolating fixture — `37-audio-media` — carrying an `<audio>` element
  with `<source>` children, asserting post-JS as the corpus requires. Validate
  it with `--baseline` before trusting any failure it produces.
- A second fixture, or an extension of `14-video`, covering a `<video>` with
  `<source>` children and no `src` attribute — the case that is silently broken
  today.
- `corpus/golden/14-video.json` changes when `direct`, `mime` and `duration`
  arrive. Regenerate deliberately and read the diff.
- `corpus/serve.py` needs audio MIME types in `guess_type` to serve the
  fixtures honestly.
- Both `--extract` and `--render` must run: the default mode checks only the
  post-JS DOM, and the card actions live in the stages it does not touch.

For the audio core:

- **Drive the decoder synchronously in tests.** Design the core so a test can
  pull frames with no device at all. The null backend paces itself in wall
  clock, which makes it a smoke test, not a source of deterministic position
  assertions.
- Cover zero-duration and unknown-duration input, very short files, malformed
  files, seek past end, repeated play/stop, and device-open failure.
- Cover redirects, signed URLs, cancellation and partial cleanup separately
  from decoding — they are download concerns, not audio concerns.
- No test may depend on a live media site or on a sound device existing.

Per CLAUDE.md, each fix lands with a fixture that fails against the previous
binary.

## Packaging

The extraction engine builds without any media output code. Every shell links
the media core. Backends are chosen per target — PulseAudio or ALSA on Linux,
the native backend elsewhere — without the C23 API changing. Documentation
records the formats that actually play and states plainly that video is
downloaded or handed off, never decoded.

## Non-goals

- Decoding anything inside QuickJS or the extraction engine.
- In-app video, in any form.
- Scraping or simulating YouTube/Vimeo players, or reconstructing adaptive
  streams.
- DRM, subtitles, playlists, equalizers, waveform rendering, transcoding,
  gapless playback, or more than one stream at a time.
- Autoplay, or any fetch that is not the direct result of a user action.
