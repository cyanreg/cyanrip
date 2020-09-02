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
