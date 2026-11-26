// Bytes written by FFmpeg 6.1.1 (independent tool, Section 5.1.7 rule 5), taken from the TS of
//   ffmpeg -f lavfi -i testsrc2=size=320x240:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000
//          -t 1 -c:v libx264 -g 25 -c:a aac -f mpegts -mpegts_service_id 1 t.ts
// ffprobe -show_programs on that file: program_num=1, pmt_pid=4096, pcr_pid=256, streams 0x100, 0x101.
#pragma once

#include "psi_builder.hpp"

namespace testutil {

// PAT section (PID 0): program 1 -> PMT PID 4096, CRC 0x2AB104B2.
inline const Bytes kFfmpegPat = {0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
                                 0x00, 0x01, 0xf0, 0x00, 0x2a, 0xb1, 0x04, 0xb2};

// PMT section (PID 4096): PCR 256, 0x1B on 256, 0x0F on 257, CRC 0x2F44B99B.
inline const Bytes kFfmpegPmt = {0x02, 0xb0, 0x17, 0x00, 0x01, 0xc1, 0x00, 0x00, 0xe1, 0x00,
                                 0xf0, 0x00, 0x1b, 0xe1, 0x00, 0xf0, 0x00, 0x0f, 0xe1, 0x01,
                                 0xf0, 0x00, 0x2f, 0x44, 0xb9, 0x9b};

// The first PAT packet of the file: header 47 40 00 10 (PID 0, pusi, payload only, cc 0),
// pointer_field 0, kFfmpegPat, then 0xFF up to 188 bytes.
inline const Bytes kFfmpegPatPacketHead = {0x47, 0x40, 0x00, 0x10, 0x00};

// First 16 bytes of the first video packet of the file (packet 3): PID 256, pusi, cc 0, adaptation
// field of 7 bytes with random_access_indicator and PCR (flags 0x50), PCR_base 63000, extension 0,
// then the PES start code 00 00 01 e0.
inline const Bytes kFfmpegKeyFrameHead = {0x47, 0x41, 0x00, 0x30, 0x07, 0x50, 0x00, 0x00,
                                          0x7b, 0x0c, 0x7e, 0x00, 0x00, 0x00, 0x01, 0xe0};

}  // namespace testutil
