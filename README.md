cyanrip
=======
Rips and encodes standard audio CDs with the least effort required from user. Cross platform and very accurate.

Features
--------
 * Automatic tag lookup from the musicbrainz database
 * Encoded and muxed via FFmpeg (currently supports flac, opus, mp3, tta, wavpack, alac, vorbis and aac)
 * Drive offset compensation and error recovery via cd-paranoia
 * Full pregap handling
 * Able to encode to multiple formats in parallel
 * Able to embed in cover images to mp3, flac, aac and opus (both in mp4)
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

Arguments are optional. By default cyanrip will rip all tracks from the default CD drive, output to flac only, enables all cd-paranoia error checking and performs a musicbrainz lookup.

| Argument             | Description                                                                      |
|----------------------|----------------------------------------------------------------------------------|
|                      | **Ripping options**                                                              |
| -d *string*          | The path or name for a specific device, otherwise uses the default device        |
| -s *int*             | Specifies the CD drive offset in samples (same as EAC, default is 0)             |
| -r *int*             | Specifies how many times to retry reading a frame if it fails, (default is 25)   |
| -S *int*             | Sets the drive speed if possible (default is unset, usually maximum)             |
| -p *number*=*string* | Specifies what to do with the pregap, sytax is described below                   |
| -O                   | Overread into lead-in/lead-out areas, if unsupported by drive may freeze ripping |
|                      | **Metadata options**                                                             |
| -I                   | Only print CD metadata and information, will not rip or eject the CD             |
| -a *string*          | Album metadata, syntax is described below                                        |
| -t *number*=*string* | Track metadata, syntax is described below                                        |
| -c *path*            | Sets cover image to embed into each track, syntax is described below             |
| -n                   | Disable MusicBrainz lookup, needs to be set for unknown discs                    |
|                      | **Output options**                                                               |
| -l *list*            | Comma separated list of track numbers to rip, (default is it rips all)           |
| -D *string*          | Base folder name to which to rip into, default is the album name                 |
| -o *list*            | Comma separated list of output formats, "help" to list all, default is flac      |
| -b *int*             | Bitrate in kbps for lossy formats                                                |
|                      | **Misc. options**                                                                |
| -E                   | Eject CD tray if ripping has been successfully completed                         |
| -V                   | Print version                                                                    |
| -h                   | Print usage (this)                                                               |


Metadata
--------

In case the Musicbrainz database doesn't contain the disc information, you can manually add metadata via the -a argument for album metadata and -t argument for track metadata:

-a album="Name":album_artist="Artist":date="2018":random_tag="Value"

-t 1=artist="Track Artist":lyrics="Name":random_tag="Value"

All key=value pair tags must be separated by *:*. For track tags, the syntax is -t track_number=key=value:key=value. You need to specify the -t argument separately for each track.

The precedence of tags is Track tags > Album tags > Musicbrainz tags.


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

To embed cover art for the whole album, either specify it with the -c *path* parameter, or add the cover_art=*path* tag to the album metadata.

To specify the cover art for a single track, specify it with the cover_art=*path* tag in the track's metadata. Metadata precedence is as specified above.

The cover_art tag containing the path will not be encoded.
