0.9.4-rc1
=========
 - New option parser: long options, typed values, saner errors
 - Option values out of their range, overflowing, or NaN, are refused rather than clamped
 - CUE sheet-only mode, without ripping anything (-J)
 - Rip log checksum verification (-Y)
 - Label and catalog number tagging from MusicBrainz
 - The MusicBrainz recording ID is tagged as musicbrainz_trackid too, as
   taggers and players expect it
 - -R takes releases the disc isn't attached to, e.g. for discs not on
   MusicBrainz yet, or for pseudo-releases
 - Discs and tracks without an MCN or ISRC are no longer tagged with zeroes
 - -S sets the speed of drives which don't advertise it, rather than refusing to
 - MusicBrainz submission URLs are printed as a QR code (needs libqrencode)
 - AcoustID fingerprinting (needs libchromaprint): fingerprints are submitted
   when the disc is accurately ripped and on MusicBrainz, and tagged otherwise
 - Embedded cover art is now typed as a front cover, so file managers thumbnail it
 - Automatic deemphasis of preemphasized discs now actually happens
 - CUE sheets reference their files relative to the sheet
 - Quotes and line breaks in metadata no longer break CUE sheet lines
 - CUE sheets of discs with split off pregaps number their tracks right, and
   give a split off hidden track as track 1's INDEX 00 rather than as track 0
 - Naming schemes: stricter parsing, whitespace trimming, filename collision warnings
 - Naming schemes are checked before ripping, and condition tokens matching no
   key compare as written
 - Naming schemes with braces inside braces are refused, rather than mangled
 - Absolute Windows paths in naming schemes keep their drive letter
 - The OS specific sanitation methods replace colons on macOS too
 - Merged pregaps get the right INDEX 01 in CUE sheets, and the log gives
   the bounds of pregap tracks and of the tracks they were split from
 - The first track's pregap on discs not starting at track 1 is handled
   like track 1's, rather than written to memory before the track list
 - Data tracks are ripped to raw MODE1/2352 binaries and referenced from the CUE sheet
 - Data tracks no longer leave stray files
 - Data track sectors are checked against their EDC, which catches damaged
   reads the drive doesn't report
 - CUE sheets give CD-ROM XA data tracks, as on Enhanced CDs, as MODE2/2352
 - Mixed mode CDs no longer lose 152 seconds of the last audio track
 - DiscID, CDDB ID and the MusicBrainz submission URL now match libdiscid on
   discs with data tracks and on discs not starting at track 1
 - AccurateRip lookups now find discs with data tracks, and each track is
   checked against its own database entry; the AccurateRip ID is logged
 - Fixed a crash on overlong filenames
 - Fixed crashes on unexpected AccurateRip replies
 - Refusing to rip without an offset, and finding no offset with -f, exit with an error
 - FFmpeg 8.0+ compatibility

0.9.3
=====
 - FFmpeg 7.0 compatibility
 - Added an option to specify the image size for cover lookup
 - When only printing info, do not require an offset to be specified with -I
 - Fix default pregap action setting for tracks after the first
 - Better error handling

0.9.2
=====
 - ReplayGain 2.0 scanning and tagging
 - Preemphasis detection via both TOC and subchannel
 - Automatic deemphasis
 - CUE file writing
 - Repeat ripping mode for affirmation or badly damaged discs
 - Tagging improvements (setting the media_type tag)
 - Logfile reorganization and checksumming
 - Windows compatibility improvements
 - Migration to new FFmpeg 6.0 APIs

0.9.0
=====
 - Improve MusicBrainz query result handling and detect stub releases
 - For unknown discs, add an ID to the album name
 - Better error reporting when opening logfile
 - Fix crash when MCN is missing
 - Silence warning when writing cover art to a file
 - Fix compilation with FFmpeg 6.0

0.8.1
=====
 - __No need to rerip anything.__
 - Fix Musicbrainz album name setting.

0.8.0
=====
 - __No need to rerip anything.__
 - ETA printout
 - Minor bugfixes
 - Big endian fixes
 - Default bitrate for lossy files set to 256kbps
 - Fix minor compilation warnings
 - Fix compilation warnings with FFmpeg 5.0

0.7
===
 - __No need to rerip anything.__
 - Automated CD drive offset finding
 - Verification of partially damaged tracks
 - Tagging usability improvements
 - Even faster ripping
 - Arbitrary directory/file structure
 - Automatic cover art image downloading
 - ...and more

0.6.0
=====
 - __No change in actual audio data ripped, rerip if you want to verify with accurip.__
 - Fill disc count and disc number from musicbrainz.
 - Able to choose the MusicBrainz release to use for albums with multiple releases.
 - Tag improvements (discname is set when available for multi-disc releases).
 - Fix accurip v1 and v2 checksums (were calculated incorrectly). EAC CRC has always been correct.
 - Fix some minor and one large memory leak.

0.5.2
=====
 - __No need to rerip anything.__
 - Fix encoding while ripping from a real drive (broken by 0.5.0).

0.5.1
=====
 - __No need to rerip anything.__
 - Reduce FFmpeg library version requirements

0.5.0
=====
 - __No need to rerip anything.__
 - Rewritten audio muxing
       * Now properly sets the time base in all cases
 - Rewritten encoding code
 - Rewritten FIFO code
       * No longer deadlocks
 - Rewritten build system

Previous versions
=================
No history.
