cyanrip
=======
Rips and encodes standard audio CDs with the least effort required from user. Cross platform.

Features
--------
 * Automatic tag lookup from the musicbrainz database
 * Encoded and muxed via FFmpeg (currently supports flac, opus, mp3, tta, wavpack, alac, vorbis and aac)
 * Drive offset compensation and error recovery via cd-paranoia
 * Able to encode to multiple formats simultaneously in parallel
 * Able to mux in cover images to mp3, flac and aac, opus (both in mp4)
 * Provides EAC CRC32, Accurip V1 and V2 checksums (doesn't check or submit them)


Compiling
---------
Complete list of dependencies:

 * FFmpeg (at least 4.0, libavcodec, libswresample, libavutil, libavformat)
 * libcdio-paranoia
 * libdiscid
 * libmusicbrainz5

All are available on any up-to-date Linux distribution's package repositories. To compile and install on any *NIX platform:

`meson build --buildtype release`

`ninja -C build`

`sudo ninja -C build install`

cyanrip can be also built and ran under Windows using MinGW


CLI
---

All arguments are entirely optional. By default cyanrip will rip all tracks from the default CD drive, output to flac only, enables all cd-paranoia error checking and performs a musicbrainz lookup.

| Argument             | Description                                                                |
|----------------------|----------------------------------------------------------------------------|
| -d *path*            | Optional device path (e.g. /dev/sr0)                                       |
| -D *path*            | Optional path to use as base folder name to rip into                       |
| -c *path*            | Path to cover image to attach to files                                     |
| -s *int*             | CD Drive offset in samples (stereo samples, same as accurip)               |
| -S *int*             | Set the drive speed (0 for default/auto)                                   |
| -o *string*          | Comma separated list of output formats (e.g. flac,opus or help to list all)|
| -b *float*           | Bitrate of lossy files in kbps                                             |
| -l *list*            | Indices of tracks to rip (e.g. 2,8,4)                                      |
| -r *int*             | Max retries to read a frame before considering it corrupt (default: 25)    |
| -a *string*          | Album metadata, in case disc info is unavailable                           |
| -t *number*=*string* | Track metadata, in case unavailable or incomplete                          |
| -E                   | Don't eject drive tray once done successfully and without interruptions    |
| -V                   | Print program version                                                      |
| -h                   | List all arguments and their description (this)                            |
| -n                   | Disable Musicbrainz lookup                                                 |


Metadata
--------

In case the Musicbrainz database doesn't contain the disc information, you can manually add metadata via the -a argument for album metadata and -t argument for track metadata:

-a album="Name":album_artist="Artist":date="2018":random_tag="Value"

-t 1=artist="Track Artist":lyrics="Name":random_tag="Value"

All key=value pair tags must be separated by *:*. For track tags, the syntax is -t track_number=key=value:key=value. You need to specify the -t argument separately for each track.

The precedence of tags is Track tags > Album tags > Musicbrainz tags.


Cover art embedding
-------------------

cyanrip supports embedding album and track cover art, in either jpeg or png formats.

To embed cover art for the whole album, either specify it with the -c *path* parameter, or add the cover_art=*path* tag to the album metadata.

To specify the cover art for a single track, specify it with the cover_art=*path* tag in the track's metadata. Metadata precedence is as specified above.

The cover_art tag containing the path will not be encoded.
