REM Mixed-mode CD: two audio tracks followed by a data track with a pregap
FILE "trailing.bin" BINARY
  TRACK 01 AUDIO
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    INDEX 01 00:02:00
  TRACK 03 MODE1/2352
    INDEX 00 00:04:00
    INDEX 01 00:05:00
