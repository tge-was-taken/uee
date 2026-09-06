#include "framedigest.h"

#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

namespace FrameDigest {
namespace {

volatile u32 *const gifChcr = (volatile u32 *)0x1000A000;
volatile u32 *const gifMadr = (volatile u32 *)0x1000A010;
volatile u32 *const gifQwc = (volatile u32 *)0x1000A020;
volatile u32 *const vifChcr = (volatile u32 *)0x10009000;
volatile u32 *const vifMadr = (volatile u32 *)0x10009010;
volatile u32 *const vifQwc = (volatile u32 *)0x10009020;
volatile u32 *const vifStat = (volatile u32 *)0x10003C00;
volatile u32 *const vifFbrst = (volatile u32 *)0x10003C10;
volatile u32 *const dmaCtrl = (volatile u32 *)0x1000E000;
volatile u32 *const gifCtrl = (volatile u32 *)0x10003000;
volatile u64 *const busdir = (volatile u64 *)0x12001040;
volatile u64 *const csr = (volatile u64 *)0x12001000;

const u32 blocksPerPage = 32;
const u32 finish = 1 << 1;

u8 *pixels = 0;
u32 pixelBytes = 0;

// The bus is turned around to read, and a wait that never ends leaves the
// machine with no way back, so every spin here gives up.
const int spinLimit = 20000000;

void sendToGif(const u64 *packet, u32 quadwords) {
	SyncDCache((void *)packet, (u8 *)packet + quadwords * 16);
	*dmaCtrl = 1;
	*gifMadr = (u32)packet;
	*gifQwc = quadwords;
	*gifChcr = (1 << 8);
	for (int spins = 0; spins < spinLimit && (*gifChcr & (1 << 8)); ++spins) {
		continue;
	}
	*gifChcr = 0;
}

// Nloop, end of packet, and one register descriptor, which is address plus
// data.  The count of descriptors goes in nreg; the descriptor itself is the
// second half of the tag.
void writeTag(u64 *out, u32 loops) {
	out[0] = (u64)loops | (1ULL << 15) | (1ULL << 60);
	out[1] = 0xE;
}

void waitForDrawing() {
	static u64 packet[4] __attribute__((aligned(16)));

	*csr = finish;
	writeTag(packet, 1);
	packet[2] = 1;
	packet[3] = 0x61;
	sendToGif(packet, 2);

	for (int spins = 0; spins < spinLimit && (*csr & finish) == 0; ++spins) {
		continue;
	}
	*csr = finish;
}

u32 crc32(const u8 *data, u32 length) {
	u32 value = 0xFFFFFFFF;
	for (u32 i = 0; i < length; ++i) {
		value ^= data[i];
		for (int bit = 0; bit < 8; ++bit) {
			value = (value >> 1) ^ (0xEDB88320 & -(value & 1));
		}
	}
	return ~value;
}

}

Result digest(u32 page, u32 format, int bufferWidth, int x, int y, int width,
              int height) {
	static const u32 bytesPerPixel[] = {4, 3, 2};
	const u32 wanted = (u32)width * (u32)height * bytesPerPixel[format & 3];
	const u32 quadwords = wanted / 16;

	if (pixelBytes < wanted) {
		free(pixels);
		pixels = (u8 *)memalign(64, wanted);
		pixelBytes = wanted;
	}
	memset(pixels, 0, wanted);

	waitForDrawing();

	{
		static u64 packet[10] __attribute__((aligned(16)));
		writeTag(packet, 4);
		packet[2] = (u64)(page * blocksPerPage) |
		            ((u64)(bufferWidth / 64) << 16) | ((u64)format << 24);
		packet[3] = 0x50;
		packet[4] = (u64)x | ((u64)y << 16);
		packet[5] = 0x51;
		packet[6] = (u64)width | ((u64)height << 32);
		packet[7] = 0x52;
		packet[8] = 1;
		packet[9] = 0x53;
		sendToGif(packet, 5);
	}

	*busdir = 1;
	*vifStat = 1 << 23;

	*dmaCtrl = 1;
	*vifMadr = (u32)pixels;
	*vifQwc = quadwords;
	*vifChcr = (1 << 8);
	for (int spins = 0; spins < spinLimit && (*vifChcr & (1 << 8)); ++spins) {
		continue;
	}
	const int left = (int)*vifQwc;
	*vifChcr = 0;
	SyncDCache(pixels, pixels + wanted);

	*vifStat = 0;
	*vifFbrst = 1;
	*busdir = 0;
	*gifCtrl = 1;

	Result result;
	result.crc = crc32(pixels, wanted);
	result.quadwordsWanted = (int)quadwords;
	result.quadwordsRead = (int)quadwords - left;
	return result;
}

void printWindow(int width, int height, int step) {
	static const char ramp[] = " .:-=+co*O8%#@WM";

	for (int y = 0; y < height; y += step) {
		printf("    ");
		for (int x = 0; x < width; x += step) {
			const u32 pixel = pixelAt(x, y, width);
			const u32 r = pixel & 0xFF;
			const u32 g = (pixel >> 8) & 0xFF;
			const u32 b = (pixel >> 16) & 0xFF;
			printf("%c", ramp[((r * 77 + g * 151 + b * 28) >> 8) >> 4]);
		}
		printf("\n");
	}
}

u32 pixelAt(int x, int y, int width) {
	const u8 *pixel = pixels + ((u32)y * (u32)width + (u32)x) * 3;
	return (u32)pixel[0] | ((u32)pixel[1] << 8) | ((u32)pixel[2] << 16);
}

}
