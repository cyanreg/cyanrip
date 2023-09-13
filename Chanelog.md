next
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
