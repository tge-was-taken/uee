#pragma once

#include <tamtypes.h>

namespace FrameDigest {

struct Result {
	u32 crc;
	int quadwordsWanted;
	int quadwordsRead;
};

// Waits for the unit to finish drawing, reads a rectangle of a frame buffer back
// over the bus and checksums the pixels.  The page is the one FRAME counts in;
// the format is the one BITBLTBUF counts in.  A rectangle rather than the whole
// buffer, because the bus stays turned around for the length of the transfer.
Result digest(u32 page, u32 format, int bufferWidth, int x, int y, int width,
              int height);

// A pixel out of the rectangle the last digest read.
u32 pixelAt(int x, int y, int width);

// The rectangle the last digest read, as a shade per sample on a sixteen step
// ramp.  A frame that came out nearly right reads as nearly right.
void printWindow(int width, int height, int step);

}
