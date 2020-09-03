cyanrip
=======
Rips and encodes audio CDs with the least effort required from user. Cross platform and very accurate.

Features
--------
 * Automatic tag lookup from the MusicBrainz database
 * Encoded and muxed via FFmpeg (currently supports flac, opus, mp3, tta, wavpack, alac, vorbis and aac)
 * Drive offset compensation and error recovery via cd-paranoia
 * Full pregap handling
 * HDCD decoding
 * Multi-disc album ripping
 * Able to encode to multiple formats in parallel
 * Able to embed in cover images to mp3, flac, aac and opus (both in mp4)
 * Provides and automatically verifies EAC CRC32, AccurateRip V1 and V2 checksums


Compiling
---------
Complete list of dependencies:

 * FFmpeg (at least 4.0, libavcodec, libswresample, libavutil, libavformat, libavfilter)
 * libcdio-paranoia
 * libdiscid
 * libmusicbrainz5
 * libcurl

All are available on any up-to-date Linux distribution's package repositories. To compile and install on any *NIX platform:

`meson build`

`ninja -C build`

`sudo ninja -C build install`

cyanrip can be also built and ran under Windows using MinGW


CLI
---

Arguments are optional. By default cyanrip will rip all tracks from the default CD drive, output to flac only, enables all cd-paranoia error checking and performs a MusicBrainz lookup.

| Argument             | Description                                                                             |
|----------------------|-----------------------------------------------------------------------------------------|
|                      | **Ripping options**                                                                     |
| -d `string`          | The path or name for a specific device, otherwise uses the default device               |
| -s `int`             | Specifies the CD drive offset in samples (same as EAC, default is 0)                    |
| -r `int`             | Specifies how many times to retry reading a frame if it fails, (default is 25)          |
| -S `int`             | Sets the drive speed if possible (default is unset, usually maximum)                    |
| -p `number=string`   | Specifies what to do with the pregap, syntax is described below                         |
| -O                   | Overread into lead-in/lead-out areas, if unsupported by drive may freeze ripping        |
| -H                   | Enable HDCD decoding, read below for details                                            |
|                      | **Metadata options**                                                                    |
| -I                   | Only print CD metadata and information, will not rip or eject the CD                    |
| -a `string`          | Album metadata, syntax is described below                                               |
| -t `number=string`   | Track metadata, syntax is described below                                               |
| -R `int` or `string` | Sets the MusicBrainz release to use, either as an index starting from 1 or an ID string |
| -c `path` or `url`   | Sets cover image to embed into each track, syntax is described below                    |
| -n                   | Disables MusicBrainz lookup and ignores lack of manual metadata to continue             |
| -A                   | Disables AccurateRip database query and comparison                                      |
| -C `int/int`         | Tag multi-disc albums as such, syntax is `disc/totaldiscs`, read below                  |
|                      | **Output options**                                                                      |
| -l `list`            | Comma separated list of track numbers to rip, (default is it rips all)                  |
| -D `string`          | Base folder name to which to rip into, default is the album name                        |
| -o `list`            | Comma separated list of output formats, "help" to list all, default is flac             |
| -b `int`             | Bitrate in kbps for lossy formats                                                       |
|                      | **Misc. options**                                                                       |
| -E                   | Eject CD tray if ripping has been successfully completed                                |
| -V                   | Print version                                                                           |
| -h                   | Print usage (this)                                                                      |


Metadata
--------

In case the MusicBrainz database doesn't contain the disc information, is incomplete, or incorrect, you can manually add metadata via the -a argument for album metadata and -t argument for track metadata:

`-a album="Name":album_artist="Artist":date="2018":random_tag="Value"`

`-t 1=artist="Track Artist":lyrics="Name":random_tag="Value" -t 3=artist="Someone Else"`

All key=value pair tags must be separated by *:*. For track tags, the syntax is -t track_number=key=value:key=value. You need to specify the -t argument separately for each track.

For convenience, if any of the first 2 metadata tags of tracks are missing a key, such as with `-t 2=some_title:some_artist:key=value`, cyanrip will automatically prepend `title=` and `artist=` such that it becomes `-t title=some_title:artist=some_artist:key=value`.
A missing key in tag 1 is always considered a title while a missing key in tag 2 is always considered artist, so either can be skipped with no effect.

The precedence of tags is Track tags > Album tags > MusicBrainz tags.


Pregap handling
---------------

By default, track 1 pregap is ignored, while any other track's pregap is merged into the previous track. This is identical to EAC's default behaviour.

You can override what's done with each pregap on a per-track basis using the -p *number*=*action* argument. This argument must be specified separately for each track.

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

cyanrip supports embedding album and track cover art, in either jpeg or png formats.

To embed cover art for the whole album, either specify it with the -c *path* parameter, or add the cover_art=*path* tag to the album metadata. *path* can be a URL as well, in which case it will be downloaded once per track.

To specify the cover art for a single track, specify it with the `cover_art="path"` tag in the track's metadata. Metadata precedence is as specified above.

The cover_art tag containing the path will not be encoded.


Multi-disc albums
-----------------

cyanrip supports ripping multi-disc albums in the same output folder. To enable this manually, specify the -C argument followed by `disc/totaldiscs` (`/totaldiscs` is optional), otherwise it'll be done automatically if the MusicBrainz tags indicate so.

The track filenames will be `disc.track - title.ext`. The logfile will be `Album name CD<disc>.log`.

As well as using the -C argument, you can also specify the `disc=number:totaldiscs=number` in the album/track metadata.

If each disc has a title, you should use the `discname` tag if you're manually setting tags, which is what MusicBrainz will set if available.


HDCD decoding
-------------

cyanrip can decode and detect HDCD encoded discs. To check if a suspected disc contains HDCD audio, rip a single track using the -l argument and look at the log. A non-HDCD encoded disc will have:

```
HDCD detected: no
```

If the CD does contain HDCD audio something similar to the following will be printed:

```
HDCD detected: yes, peak_extend: never enabled, max_gain_adj: 0.0 dB, transient_filter: not detected, detectable errors: 0
```

Should a track be detected as HDCD, it would be safe to proceed decoding all of the disc. The resulting encoded files will have a bit depth of at least 24 bits.


Paranoia status count
---------------------

At the end of the ripping process, cyanrip will print and log a summary of cdparanoia's status during ripping. This can be used to estimate the disc/drive's health.

An idealized disc and drive will only log `READ: $number$` and nothing else. This happens while ripping from a file. In general `READ`/`VERIFY`/`OVERLAP` are normal and will happen when
ripping a brand new CD with a new drive. `FIXUP_EDGE`/`FIXUP_ATOM` usually happen if cdparanoia is somewhat struggling, but both are recoverable and lossless. `FIXUP_DROPPED`/`FIXUP_DUPED`
indicate more severe errors, but are still recoverable and lossless, though a hard read error will often follow. `READERR` indicates that cdparanoia gave up after all retries and
outputted zeroes for all samples it couldn't recover.
