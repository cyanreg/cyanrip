[cyanrip](#cyanrip)
===================
Fully featured CD ripping program able to take out most of the tedium. Fully accurate, has advanced features most rippers don't, yet has no bloat and is cross-platform.

Features
--------
 * Automatic tag lookup from the MusicBrainz database
 * Encoded and muxed via FFmpeg (currently supports flac, opus, mp3, tta, wavpack, alac, vorbis and aac)
 * Drive offset compensation and error recovery via cd-paranoia
 * [Full pregap handling](#pregap-handling)
 * [HDCD detection and decoding](#hdcd-decoding)
 * [CD Deemphasis (TOC + subcode)](#deemphasis)
 * [Multi-disc album ripping](#multi-disc-albums)
 * [ReplayGain v2 tagging](#replaygain)
 * Able to encode to multiple formats in parallel
 * [Cover image embedding](#cover-art-embedding) in mp3, flac, aac and opus
 * Automatic [cover art image downloading](#cover-art-downloading)
 * Provides and automatically verifies EAC CRC32, AccurateRip V1 and V2 checksums
 * Accurate ripping verification of partially damaged tracks
 * Automatic drive offset finding

Installation
------------
### Alpine Linux
```bash
apk add cyanrip
```

### Archlinux
```bash
pacaur -S cyanrip
```
Or use your favorite AUR installation method.

### Debian (and derivatives)
```bash
apt install cyanrip
```

### Void Linux
```bash
xbps-install -S cyanrip
```

### FreeBSD
```bash
pkg install cyanrip
```
Or via ports: `cd /usr/ports/audio/cyanrip && make install clean`.

### NixOS
```bash
nix-env -iA nixos.cyanrip
```

### Docker
```bash
docker pull ep76/cyanrip
```

### Automated Windows builds ![Windows CI](https://github.com/cyanreg/cyanrip/workflows/CI/badge.svg)
[Latest release Windows build](https://github.com/cyanreg/cyanrip/releases/latest/download/cyanrip-win64.exe)

[Latest Windows build](https://github.com/cyanreg/cyanrip/releases/download/nightly/cyanrip-win64-latest.exe)

If the latest build is broken, you can find older ones in the [nightly release page](https://github.com/cyanreg/cyanrip/releases/tag/nightly)

### Snap Store

cyanrip is available as a snap package on supported Linux distros. Install the snapd service following the [instructions](https://snapcraft.io/docs/installing-snapd).

Install cyanrip:
```bash
snap install cyanrip
```

#### Building the snap (testing only)

Currently only supported on amd64 Linux systems. Tested on Ubuntu 22.04 which is also the recommended build environment. Install snapcraft per the [instructions](https://snapcraft.io/docs/snapcraft-overview):
```bash
snap install snapcraft --classic
lxd init --auto
```

From the root of the source code directory:
```bash
snapcraft
```

Resulting snap will **not** be signed and must be installed in devmode:
```bash
snap install cyanrip_<version>_amd64.snap --devmode
```

### Compiling
Complete list of dependencies:

 * FFmpeg (at least 4.0, libavcodec, libswresample, libavutil, libavformat, libavfilter)
 * libcdio-paranoia
 * libmusicbrainz5
 * libcurl

All are available on any up-to-date Linux distribution's package repositories. To compile and install on any *NIX platform:

`meson build`

`ninja -C build`

`sudo ninja -C build install`

cyanrip can be also built and ran under Windows using MinGW


CLI
---
Arguments are optional, except `-s`. By default cyanrip will rip all tracks from the default CD drive, output to flac only, enables all cd-paranoia error checking, performs a MusicBrainz lookup, and downloads and embeds the cover art if one is found.

| Argument             | Description                                                                                 |
|----------------------|---------------------------------------------------------------------------------------------|
|                      | **Ripping options**                                                                         |
| -d `string`          | The path or name for a specific device, otherwise uses the default device                   |
| -s `int`             | Specifies the CD drive offset in samples (same as EAC, default is 0)                        |
| -r `int`             | Specifies how many times to retry a frame/ripping if it fails, (default is 10)              |
| -Z `int`             | Rips tracks until their checksums match `<int>` number of times. For very damaged CDs.      |
| -S `int`             | Sets the drive speed if possible (default is unset, usually maximum)                        |
| -p `number=string`   | Specifies what to do with the pregap, syntax is described below                             |
| -P `int`             | Sets the paranoia level to use, by default its max, 0 disables all checking completely      |
| -O                   | Overread into lead-in/lead-out areas, if unsupported by drive may freeze ripping            |
| -H                   | Enable HDCD decoding, read below for details                                                |
| -E                   | Force CD deemphasis, for CDs mastered with preemphasis without actually signalling it       |
| -W                   | Disable automatic CD deemphasis. Read [below](#deemphasis) for details.                     |
| -K                   | Disable ReplayGain tag generation. Read [replaygain](#replaygain) for details.              |
|                      | **Output options**                                                                          |
| -o `list`            | Comma separated list of output formats (encodings). Use "help" to list all. Default is flac |
| -b `int`             | Bitrate in kbps for lossy formats, 256 by default                                           |
| -D `string`          | Directory naming scheme, see [below](#naming-scheme)                                        |
| -F `string`          | File naming scheme, see [below](#naming-scheme)                                             |
| -L `string`          | Log naming scheme, see [below](#naming-scheme)                                              |
| -M `string`          | CUE file naming scheme, see [below](#naming-scheme)                                         |
| -l `list`            | Comma separated list of track numbers to rip, (default is it rips all)                      |
| -T `string`          | Filename sanitation, default is unicode, see [below](#filename-sanitation)                  |
|                      | **Metadata options**                                                                        |
| -I                   | Only print CD metadata and information, will not rip or eject the CD                        |
| -a `string`          | Album metadata, syntax is described below                                                   |
| -t `number=string`   | Track metadata, syntax is described below                                                   |
| -R `int` or `string` | Sets the MusicBrainz release to use, either as an index starting from 1 or an ID string     |
| -c `int/int`         | Tag multi-disc albums as such, syntax is `disc/totaldiscs`, read below                      |
| -C `path` or `url`   | Sets cover image to embed into each track, syntax is described below                        |
| -N                   | Disables MusicBrainz lookup and ignores lack of manual metadata to continue                 |
| -A                   | Disables AccurateRip database query and comparison                                          |
| -U                   | Disables Cover art DB database query and retrieval                                          |
| -G                   | Disables embedding of cover art images                                                      |
|                      | **Misc. options**                                                                           |
| -Q                   | Eject CD tray if ripping has been successfully completed                                    |
| -V                   | Print version                                                                               |
| -h                   | Print usage (this)                                                                          |
| -f                   | Find drive offset (requires a disc with an AccuRip DB entry)                                |


Metadata
--------
In case the MusicBrainz database doesn't contain the disc information, is incomplete, or incorrect, you can manually add metadata via the -a argument for album metadata and -t argument for track metadata:

`-a album="Name":album_artist="Artist":date="2018":random_tag="Value"`

`-t 1=artist="Track Artist":lyrics="Name":random_tag="Value" -t 3=artist="Someone Else"`

All key=value pair tags must be separated by `:`. For track tags, the syntax is `-t track_number=key=value:key=value`. You need to specify the -t argument separately for each track.

For convenience, if any of the first 2 metadata tags of tracks are missing a key, such as with `-t 2=some_title:some_artist:key=value`, cyanrip will automatically prepend `title=` and `artist=` such that it becomes `-t title=some_title:artist=some_artist:key=value`.
A missing key in tag 1 is always considered a title while a missing key in tag 2 is always considered artist, so either can be skipped with no effect.

The same goes for album tags, with `album=` and `album_artist=` being omitable.

For album tags, if either `artist` or `album_artist` are unset, their values will be mirrored if one is available.

The precedence of tags is Track tags > Album tags > MusicBrainz tags.


Output
------
The output encoding(s) can be set via the `-o` option as a comma-separated list. Currently, the following formats are available:

| Format name | Description                                     | Extension | Cover art embedding | Notes                                                     |
|-------------|-------------------------------------------------|-----------|---------------------|-----------------------------------------------------------|
| `flac`      | Standard FLAC files                             | `.flac`   | :heavy_check_mark:  | Always uses maximum compression                           |
| `tta`       | TTA (True Audio) files                          | `.tta`    | ⬜                  | Always uses maximum compression                           |
| `opus`      | Standard Opus files (in an Ogg container)       | `.opus`   | ⬜                  | VBR, use -b to adjust the bitrate, default is 256 (kbps)  |
| `aac`       | Standard AAC files                              | `.aac`    | ⬜                  | Use -b to adjust the bitrate, default is 256 (kbps)       |
| `wavpack`   | Standard lossless WavPack files                 | `.wv`     | ⬜                  | Always uses maximum compression                           |
| `alac`      | Standard ALAC files                             | `.alac`   | ⬜                  | Always uses maximum compression                           |
| `mp3`       | Standard MP3 files                              | `.mp3`    | :heavy_check_mark:  | VBR, use -b to adjust the bitrate, default is 256 (kbps)  |
| `vorbis`    | Standard Ogg/Vorbis files (in an OGG container) | `.ogg`    | ⬜                  | Use -b to adjust the bitrate, default is 256 (kbps)       |
| `wav`       | Standard WAV files                              | `.wav`    | ⬜                  | 16-bit little endian signed audio, or 32-bit in HDCD mode |
| `aac_mp4`   | Standard MP4 files (with AAC encoding)          | `.mp4`    | :heavy_check_mark:  | Use -b to adjust the bitrate, default is 256 (kbps)       |
| `opus_mp4`  | Standard MP4 files (with Opus encoding)         | `.mp4`    | :heavy_check_mark:  | Use -b to adjust the bitrate, default is 256 (kbps)       |
| `pcm`       | Raw audio, 16-bits, two channel, little-endian  | `.raw`    | ⬜                  |                                                           |

For example, to make both FLAC and MP3 files simultaneously, use `-o flac,mp3`. Encodings are done in parallel during ripping, so adding more does not slow down the process.

To adjust the directories and filenames, read the [naming scheme](#naming-scheme) section below.


Pregap handling
---------------
By default, track 1 pregap is ignored, while any other track's pregap is merged into the previous track. This is identical to EAC's default behaviour.

You can override what's done with each pregap on a per-track basis using the `-p track_number=action` argument. This argument must be specified separately for each track.

| *action* | Description                                |
|----------|--------------------------------------------|
| default  | Merge into previous track, drop on track 1 |
| drop     | Drop the pregap entirely                   |
| merge    | Merge into current track                   |
| track    | Split into a new track before the current  |

If the pregap offset isn't available for a given track, this argument will do nothing.

cyanrip guarantees that there will be no discontinuities between tracks, unless the drop action is used to delete a pregap.


Cover art embedding
-------------------
cyanrip supports embedding album and track cover art.

To embed cover art for the whole album, specify it with the `-C path` or `-C destination=path` parameter. `path` can be a URL, in which case it will be automatically downloaded.

If `destination` is a number, the cover art will be embedded only for the track with that number. Otherwise, `destination` should be a descriptor like `Front` or `Back` or `Disc`. If `destination` is omitted, `Front`, then `Back` will be used.

Cover arts which are not attached to a track will be copied to each output directory as `destination` with an autodetected extension.

If multiple cover arts are present and no track cover art is specified, only the cover art with `Front` destination will be embedded, or the first cover art specified if no `Front` exists. Otherwise, only the specified track cover art will be embedded.


Cover art downloading
---------------------
If a release ID is specified or detected, and no `Front` or `Back` cover arts were specified, a query will be made to the [Cover Art Archive](https://coverartarchive.org/) and if found, the archive cover art will be downloaded and used.


Multi-disc albums
-----------------
cyanrip supports ripping multi-disc albums in the same output folder. To enable this manually, specify the `-c` argument followed by `disc/totaldiscs` (`/totaldiscs` is optional), otherwise it'll be done automatically if the MusicBrainz tags indicate so.

The track filenames will be `disc.track - title.ext`. The logfile will be `Album name CD<disc>.log`.

As well as using the `-c` argument, you can also specify the `disc=number:totaldiscs=number` in the album/track metadata.

If each disc has a title, you should use the `discname` tag if you're manually setting tags, which is what MusicBrainz will set if available.


HDCD decoding
-------------
cyanrip can decode and detect HDCD encoded discs. To check if a suspected disc contains HDCD audio, rip a single track using the `-l 1` argument and look at the log. A non-HDCD encoded disc will have:

```
HDCD detected: no
```

If the CD does contain HDCD audio something similar to the following will be printed:

```
HDCD detected: yes, peak_extend: never enabled, max_gain_adj: 0.0 dB, transient_filter: not detected, detectable errors: 0
```

Should a track be detected as HDCD, it would be safe to proceed decoding all of the disc. The resulting encoded files will have a bit depth of at least 24 bits.


Deemphasis
----------
Old (and not so old) CDs may require deeemphasis. cyanrip is able to detect such discs (using both the TOC and track subcode) and will apply a correction filter automatically.
To check whether the CD contains emphasised audio, you can simply run `cyanrip -I` and read the printout.

```
Preemphasis:      <status> <action>
```

for each track.

`status` can be either `none detected` or `present (TOC)` or `present (subcode)`.

Action may be either `(deemphasis applied)`, which applies a deemphasis filter to the audio (default), `(deemphasis forced)` which does the same, even if no preemphasis is signalled
(can be activated via the `-E` flag, or blank, which deactivates automatic deemphasis, preserving audio data as-is (activated via the `-W` flag).

The `-E` flag can be useful for some CDs, which may have been mastered with preemphasis, but don't actually signal it via the track substream properties.

An incomplete list of releases mastered with preemphasis can be found at [this wiki page](https://www.studio-nibble.com/cd/index.php?title=Pre-emphasis_(release_list)).

ReplayGain
----------
cyanrip will automatically compute ReplayGain tags and add them to all files while ripping. Note, that this requires to hold all **compressed** audio data for the entire CD in RAM while ripping, which may be from 300 to 600 megabytes, depending on the output used. Low-power and low-RAM devices can turn this off via the `-K` switch.

The tags generated are ReplayGain 2.0 compliant, which is backwards-compatible with ReplayGain 1.0. The **true peak** value is calculated and used.


Paranoia status count
---------------------
At the end of the ripping process, cyanrip will print and log a summary of cdparanoia's status during ripping. This can be used to estimate the disc/drive's health.

An idealized disc and drive will only log `READ: $number$` and nothing else. This happens while ripping from a file. In general `READ`/`VERIFY`/`OVERLAP` are normal and will happen when
ripping a brand new CD with a new drive. `FIXUP_EDGE`/`FIXUP_ATOM` usually happen if cdparanoia is somewhat struggling, but both are recoverable and lossless. `FIXUP_DROPPED`/`FIXUP_DUPED`
indicate more severe errors, but are still recoverable and lossless, though a hard read error will often follow. `READERR` indicates that cdparanoia gave up after all retries and
outputted zeroes for all samples it couldn't recover.


Naming scheme
-------------
cyanrip supports highly flexible naming schemes via a custom syntax. You can extensively customize how all files and directories are named.

The default naming scheme for albums is `Album name [Format]` if the `releasecomment` tag is empty or `Album name (Release comment) [Format]` if it isn't.
For tracks, its `Track number - Track title.Extension` if there's a single disc in the album or `Disc Number.Track number - Track title.Extension` if there are multiple.
The log file will be named `Album name.log` in each of the output folders unless there are multiple CDs in the album, in which case it'll be `Album name CD1.log` for the first CD and so on.

If you would like something different, read on. If for a one-off you'd like to specify a different directory name, you can just use `-D Directory` and not read further.

The syntax is as follows: everything not inside `{` or `}` tags gets copied as-is unconditionally.
Everything inside `{` or `}` tags is interpreted: if what's inside the tags matches a metadata key,
the tag along with its outer brackets is replaced as-is. Otherwise only the tag brackets are removed and what's inside is copied literally.

Conditions are possible and must follow this syntax: `{if #token1# != #token2#album name is |album|}`. Condition tokens must be wrapped in `#` tags, and both must exist.
In between the 2 tokens must be either `>` or `<` or `==` or `!=`. If any of the tokens inside the `#` tags matches a metadata key, it is replaced with a value, otherwise its taken literally (so if the `album` tag exists, `#album#` resolves to the album name, otherwise to just `album`).

If the condition is a direct comparison (`==` or `!=`), then the 2 tokens are compared as strings. If arithmetic comparison is used and the 2 tokens are integers, they're compared arithmetically.

If an arithmetic comparison is used when both tokens are strings, the result of `strcmp` is used. If only one is a string, its pointer (always above 0, __unless__ the token did not match a metadata key, in which case 0) is used.

If the condition is true, everything after the last token's `#` is copied, with any metadata tags there wrapped with `|`. Otherwise, nothing is copied.

Examples are easier to understand, by default the folder value of `{album}{if #releasecomment# > #0# (|releasecomment|)} [{format}]` is used. This resolves to `Album [FLAC]` if there's nothing in the `releasecomment` key, or `Album (Release comment) [FLAC]` if there is.

The default track file name syntax is: `{if #totaldiscs# > #1#|disc|.}{track} - {title}`. So this resolves to `01 - Title.flac` if there's a single CD, or `1.01 - Title.flac` if there are more than 1 CDs and you're ripping the first one.

A useful example is to have separate directories for each disc: `-D "{album}{if #totaldiscs# > #1# CD|disc|} [{format}]" -F "{track} - {title}"`.

The ripping log name and location can be modified via the `-L` argument. By default its set to `{album}{if #totaldiscs# > #1# CD|disc|}`, which resolves to `Album name.log` for 1 CD and `Album name CD1.log` if there are multiple CDs and you're ripping the first.


Filename sanitation
-------------------
If invalid symbols are found in a file or a folder, such as `:` or `/`, the symbol is by default substituted with a unicode lookalike, such as `∶` or `∕` respectively. If this is undesirable, this can be overridden via the `-T simple` argument. This will replace all invalid symbols with `_`.
In case you're on an operating system with more liberal allowance on filenames, you can use the `-T os_unicode` option to allow symbols like `:` not supported on Windows to be passed through. Note that this will make files like these not accessible on Windows, unless renamed, so use this only if you're sure.


CUE sheet
---------
cyanrip will generate a CUE sheet from which a byte-exact duplicate of the disc can be made. By default, pregaps are kept in the CUE sheet as being appended to the previous track (except for the first track, where the pregap is dropped and signalled as silence). This is reffered to as "noncompliant" by [hydrogenaudio](https://wiki.hydrogenaud.io/index.php?title=Cue_sheet#Multiple_files_with_gaps_.28Noncompliant.29).

Custom changes in the way pregaps are handled will be reflected in the CUE file. For example, dropping a pregap will signal [silence](https://wiki.hydrogenaud.io/index.php?title=Cue_sheet#Multiple_files_with_gaps_left_out) in the CUE sheet. Appending a pregap to the track will accordingly mark the track as [having two audio indices](https://wiki.hydrogenaud.io/index.php?title=Cue_sheet#Multiple_files_with_corrected_gaps).


Links
=====
You can talk about the project and get in touch with developers on:
 - IRC: **`#cyanrip`** on the [Libera.Chat](ircs://irc.libera.chat:6697) network
  - [Libera.Chat’s guide on how to connect](https://libera.chat/guides/connect)
    - Or use the [Kiwi web client](https://kiwiirc.com/nextclient/irc.libera.chat/?#cyanrip)
