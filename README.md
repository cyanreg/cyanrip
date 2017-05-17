cyanrip
=======
Rips and encodes standard audio CDs with the least effort required from user.

Features
--------
 * Automatic tag lookup from the musicbrainz database
 * Audio data ripping via cd-paranoia
 * Encoded and muxed via FFmpeg (currently supports flac, opus, mp3, tta, wavpack, alac, vorbis and aac)
 * Able to encode to multiple formats simultaneously
 * Able to mux in cover images to flac and mp3 files

CLI
---

All arguments are entirely optional. By default cyanrip will rip all tracks from /dev/sr0,
output to flac only, enables all cd-paranoia error checking and performs musicbrainz lookup.

|   Argument  | Description                                                  |
|:-----------:|--------------------------------------------------------------|
| -d <path>   | Optional device path (e.g. /dev/sr0)                         |
| -c <path>   | Path to cover image to attach to files                       |
| -o <string> | Comma separated list of output formats (e.g. flac,alac,opus) |
| -b <float>  | Bitrate of lossy files in kbps                               |
| -t <list>   | Numbers of tracks to rip (e.g. 2,8,4,2)                      |
| -f          | Disable CD paranoia error checking                           |
| -h          | List all arguments and their description (this)              |
| -n          | Disable Musicbrainz lookup                                   |


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
