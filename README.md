cyanrip
=======
Rips and encodes standard audio CDs with the least effort required from user.

Features
--------
 * Automatic tag lookup from the musicbrainz database
 * Encoded and muxed via FFmpeg (currently supports flac, opus, mp3, tta, wavpack, alac, vorbis and aac)
 * Drive offset compensation, error recovery via cd-paranoia
 * Able to encode to multiple formats simultaneously
 * Able to mux in cover images to mp3 and flac (soon in latest ffmpeg git) files


Compiling
---------
Complete list of dependencies:

 * FFmpeg (3.0 or newer)
 * libcdio-paranoia
 * libdiscid
 * libmusicbrainz5

All are available on any up-to-date Linux distribution's package repositories. To compile and install:

`./bootstrap.py`

`./waf configure`

`./waf build`

`./waf install`


CLI
---

All arguments are entirely optional. By default cyanrip will rip all tracks from /dev/sr0,
output to flac only, enables all cd-paranoia error checking and performs musicbrainz lookup.

|   Argument  | Description                                                                |
|-------------|----------------------------------------------------------------------------|
| -d *path*   | Optional device path (e.g. /dev/sr0)                                       |
| -c *path*   | Path to cover image to attach to files                                     |
| -s *int*    | CD Drive offset in samples                                                 |
| -S *int*    | Set the drive speed (0 for default/auto)                                   |
| -o *string* | Comma separated list of output formats (e.g. flac,opus or help to list all)|
| -b *float*  | Bitrate of lossy files in kbps                                             |
| -t *list*   | Numbers of tracks to rip (e.g. 2,8,4,2 or 0 to print CD info only)         |
| -r *int*    | Max retries to read a frame before considering it corrupt                  |
| -f          | Disable CD paranoia error checking (for speed)                             |
| -h          | List all arguments and their description (this)                            |
| -n          | Disable Musicbrainz lookup                                                 |
