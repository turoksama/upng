/*
uPNG -- derived from LodePNG version 20100808

Copyright (c) 2005-2010 Lode Vandevenne
Copyright (c) 2010 Sean Middleditch

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

		1. The origin of this software must not be misrepresented; you must not
		claim that you wrote the original software. If you use this software
		in a product, an acknowledgment in the product documentation would be
		appreciated but is not required.

		2. Altered source versions must be plainly marked as such, and must not be
		misrepresented as being the original software.

		3. This notice may not be removed or altered from any source
		distribution.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "upng.h"

#define MAKE_BYTE(b) ((b) & 0xFF)
#define MAKE_DWORD(a,b,c,d) ((MAKE_BYTE(a) << 24) | (MAKE_BYTE(b) << 16) | (MAKE_BYTE(c) << 8) | MAKE_BYTE(d))
#define MAKE_DWORD_PTR(p) MAKE_DWORD((p)[0], (p)[1], (p)[2], (p)[3])

#define CHUNK_IHDR MAKE_DWORD('I','H','D','R')
#define CHUNK_IDAT MAKE_DWORD('I','D','A','T')
#define CHUNK_IEND MAKE_DWORD('I','E','N','D')

#define SET_ERROR(upng,code) do { (upng)->error = (code); (upng)->error_line = __LINE__; } while (0)

#define upng_chunk_length(chunk) MAKE_DWORD_PTR(chunk)
#define upng_chunk_type(chunk) MAKE_DWORD_PTR((chunk) + 4)
#define upng_chunk_critical(chunk) (((chunk)[4] & 32) == 0)

extern	upng_error	uz_inflate			(upng_t* decoder, unsigned char* out, unsigned long outsize, const unsigned char* in, unsigned long insize);

/*Paeth predicter, used by PNG filter type 4*/
static int paeth_predictor(int a, int b, int c)
{
	int p = a + b - c;
	int pa = p > a ? p - a : a - p;
	int pb = p > b ? p - b : b - p;
	int pc = p > c ? p - c : c - p;

	if (pa <= pb && pa <= pc)
		return a;
	else if (pb <= pc)
		return b;
	else
		return c;
}

static upng_error unfilter_scanline(unsigned char *recon, const unsigned char *scanline, const unsigned char *precon, unsigned long bytewidth, unsigned char filterType, unsigned long length)
{
	/*
	   For PNG filter method 0
	   unfilter a PNG image scanline by scanline. when the pixels are smaller than 1 byte, the filter works byte per byte (bytewidth = 1)
	   precon is the previous unfiltered scanline, recon the result, scanline the current one
	   the incoming scanlines do NOT include the filtertype byte, that one is given in the parameter filterType instead
	   recon and scanline MAY be the same memory address! precon must be disjoint.
	 */

	unsigned long i;
	switch (filterType) {
	case 0:
		for (i = 0; i < length; i++)
			recon[i] = scanline[i];
		break;
	case 1:
		for (i = 0; i < bytewidth; i++)
			recon[i] = scanline[i];
		for (i = bytewidth; i < length; i++)
			recon[i] = scanline[i] + recon[i - bytewidth];
		break;
	case 2:
		if (precon)
			for (i = 0; i < length; i++)
				recon[i] = scanline[i] + precon[i];
		else
			for (i = 0; i < length; i++)
				recon[i] = scanline[i];
		break;
	case 3:
		if (precon) {
			for (i = 0; i < bytewidth; i++)
				recon[i] = scanline[i] + precon[i] / 2;
			for (i = bytewidth; i < length; i++)
				recon[i] = scanline[i] + ((recon[i - bytewidth] + precon[i]) / 2);
		} else {
			for (i = 0; i < bytewidth; i++)
				recon[i] = scanline[i];
			for (i = bytewidth; i < length; i++)
				recon[i] = scanline[i] + recon[i - bytewidth] / 2;
		}
		break;
	case 4:
		if (precon) {
			for (i = 0; i < bytewidth; i++)
				recon[i] = (unsigned char)(scanline[i] + paeth_predictor(0, precon[i], 0));
			for (i = bytewidth; i < length; i++)
				recon[i] = (unsigned char)(scanline[i] + paeth_predictor(recon[i - bytewidth], precon[i], precon[i - bytewidth]));
		} else {
			for (i = 0; i < bytewidth; i++)
				recon[i] = scanline[i];
			for (i = bytewidth; i < length; i++)
				recon[i] = (unsigned char)(scanline[i] + paeth_predictor(recon[i - bytewidth], 0, 0));
		}
		break;
	default:
		return UPNG_EUNSUPPORTED;	/*error: unexisting filter type given */
	}
	return UPNG_EOK;
}

static upng_error unfilter(unsigned char *out, const unsigned char *in, unsigned w, unsigned h, unsigned bpp)
{
	/*
	   For PNG filter method 0
	   this function unfilters a single image (e.g. without interlacing this is called once, with Adam7 it's called 7 times)
	   out must have enough bytes allocated already, in must have the scanlines + 1 filtertype byte per scanline
	   w and h are image dimensions or dimensions of reduced image, bpp is bpp per pixel
	   in and out are allowed to be the same memory address!
	 */

	unsigned y;
	unsigned char *prevline = 0;

	unsigned long bytewidth = (bpp + 7) / 8;	/*bytewidth is used for filtering, is 1 when bpp < 8, number of bytes per pixel otherwise */
	unsigned long linebytes = (w * bpp + 7) / 8;

	for (y = 0; y < h; y++) {
		unsigned long outindex = linebytes * y;
		unsigned long inindex = (1 + linebytes) * y;	/*the extra filterbyte added to each row */
		unsigned char filterType = in[inindex];

		unsigned error = unfilter_scanline(&out[outindex], &in[inindex + 1], prevline,
						  bytewidth, filterType, linebytes);
		if (error)
			return error;

		prevline = &out[outindex];
	}

	return UPNG_EOK;
}

static void remove_padding_bits(unsigned char *out, const unsigned char *in, unsigned long olinebits, unsigned long ilinebits, unsigned h)
{
	/*
	   After filtering there are still padding bpp if scanlines have non multiple of 8 bit amounts. They need to be removed (except at last scanline of (Adam7-reduced) image) before working with pure image buffers for the Adam7 code, the color convert code and the output to the user.
	   in and out are allowed to be the same buffer, in may also be higher but still overlapping; in must have >= ilinebits*h bpp, out must have >= olinebits*h bpp, olinebits must be <= ilinebits
	   also used to move bpp after earlier such operations happened, e.g. in a sequence of reduced images from Adam7
	   only useful if (ilinebits - olinebits) is a value in the range 1..7
	 */
	unsigned y;
	unsigned long diff = ilinebits - olinebits;
	unsigned long obp = 0, ibp = 0;	/*bit pointers */
	for (y = 0; y < h; y++) {
		unsigned long x;
		for (x = 0; x < olinebits; x++) {
			unsigned char bit = (unsigned char)((in[(ibp) >> 3] >> (7 - ((ibp) & 0x7))) & 1);
			ibp++;

			if (bit == 0)
				out[(obp) >> 3] &= (unsigned char)(~(1 << (7 - ((obp) & 0x7))));
			else
				out[(obp) >> 3] |= (1 << (7 - ((obp) & 0x7)));
			++obp;
		}
		ibp += diff;
	}
}

/*out must be buffer big enough to contain full image, and in must contain the full decompressed data from the IDAT chunks*/
static upng_error post_process_scanlines(unsigned char *out, unsigned char *in, const upng_t* info_png)
{
	unsigned bpp = upng_get_bpp(info_png);
	unsigned w = info_png->width;
	unsigned h = info_png->height;
	upng_error error = 0;
	if (bpp == 0)
		return UPNG_EUNSUPPORTED;

	if (bpp < 8 && w * bpp != ((w * bpp + 7) / 8) * 8) {
		error = unfilter(in, in, w, h, bpp);
		if (error)
			return error;
		remove_padding_bits(out, in, w * bpp, ((w * bpp + 7) / 8) * 8, h);
	} else
		error = unfilter(out, in, w, h, bpp);	/*we can immediatly filter into the out buffer, no other steps needed */

	return error;
}

/*read the information from the header and store it in the upng_Info. return value is error*/
upng_error upng_inspect(upng_t* upng, const unsigned char *in, unsigned long inlength)
{
	/* ensure we have valid input */
	if (inlength == 0 || in == NULL) {
		SET_ERROR(upng, UPNG_ENOTPNG);
		return upng->error;
	}

	/* minimum length of a valid PNG file is 29 bytes
	 * FIXME: verify this against the specification, or
	 * better against the actual code below */
	if (inlength < 29) {
		SET_ERROR(upng, UPNG_ENOTPNG);
		return upng->error;
	}

	/* check that PNG header matches expected value */
	if (in[0] != 137 || in[1] != 80 || in[2] != 78 || in[3] != 71 || in[4] != 13 || in[5] != 10 || in[6] != 26 || in[7] != 10) {
		SET_ERROR(upng, UPNG_ENOTPNG);
		return upng->error;
	}

	/* check that the first chunk is the IHDR chunk */
	if (MAKE_DWORD_PTR(in + 12) != CHUNK_IHDR) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* read the values given in the header */
	upng->width = MAKE_DWORD_PTR(in + 16);
	upng->height = MAKE_DWORD_PTR(in + 20);
	upng->color_depth = in[24];
	upng->color_type = (upng_color)in[25];

	/* determine our color format */
	upng->format = upng_get_format(upng);
	if (upng->format == UPNG_BADFORMAT) {
		SET_ERROR(upng, UPNG_EUNSUPPORTED);
		return upng->error;
	}

	/* check that the compression method (byte 27) is 0 (only allowed value in spec) */
	if (in[26] != 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* check that the compression method (byte 27) is 0 (only allowed value in spec) */
	if (in[27] != 0) {
		SET_ERROR(upng, UPNG_EMALFORMED);
		return upng->error;
	}

	/* check that the compression method (byte 27) is 0 (spec allows 1, but uPNG does not support it) */
	if (in[28] != 0) {
		SET_ERROR(upng, UPNG_EUNSUPPORTED);
		return upng->error;
	}

	return upng->error;
}

/*read a PNG, the result will be in the same color type as the PNG (hence "generic")*/
upng_error upng_decode(upng_t* upng, const unsigned char *in, unsigned long size)
{
	const unsigned char *chunk;
	unsigned char* compressed;
	unsigned char* inflated;
	unsigned long compressed_size = 0, compressed_index = 0;
	unsigned long inflated_size;
	upng_error error;

	/* cannot work on an empty input */
	if (size == 0 || in == 0) {
		SET_ERROR(upng, UPNG_ENOTPNG);
		return upng->error;
	}

	/* release old result, if any */
	if (upng->buffer != 0) {
		free(upng->buffer);
		upng->buffer = 0;
		upng->size = 0;
	}

	/* parse the main header */
	upng_inspect(upng, in, size);
	if (upng->error != UPNG_EOK)
		return upng->error;

	/* first byte of the first chunk after the header */
	chunk = in + 33;

	/* scan through the chunks, finding the size of all IDAT chunks, and also
	 * verify general well-formed-ness */
	while (chunk < in + size) {
		unsigned long length;
		const unsigned char *data;	/*the data in the chunk */

		/* make sure chunk header is not larger than the total compressed */
		if ((unsigned long)(chunk - in + 12) > size) {
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* get length; sanity check it */
		length = upng_chunk_length(chunk);
		if (length > INT_MAX) {
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* make sure chunk header+paylaod is not larger than the total compressed */
		if ((unsigned long)(chunk - in + length + 12) > size) {
			SET_ERROR(upng, UPNG_EMALFORMED);
			return upng->error;
		}

		/* get pointer to payload */
		data = chunk + 8;

		/* parse chunks */
		if (upng_chunk_type(chunk) == CHUNK_IDAT) {
			compressed_size += length;
		} else if (upng_chunk_type(chunk) == CHUNK_IEND) {
			break;
		} else if (upng_chunk_critical(chunk)) {
			SET_ERROR(upng, UPNG_EUNSUPPORTED);
			return upng->error;
		}

		chunk += upng_chunk_length(chunk) + 12;
	}

	/* allocate enough space for the (compressed and filtered) image data */
	compressed = (unsigned char*)malloc(compressed_size);
	if (compressed == NULL) {
		SET_ERROR(upng, UPNG_ENOMEM);
		return upng->error;
	}

	/* scan through the chunks again, this time copying the values into
	 * our compressed.  there's no reason to validate anything a second time. */
	chunk = in + 33;
	while (chunk < in + size) {
		unsigned long length;
		const unsigned char *data;	/*the data in the chunk */

		length = upng_chunk_length(chunk);
		data = chunk + 8;

		/* parse chunks */
		if (upng_chunk_type(chunk) == CHUNK_IDAT) {
			memcpy(compressed + compressed_index, data, length);
			compressed_index += length;
		} else if (upng_chunk_type(chunk) == CHUNK_IEND) {
			break;
		}

		chunk += upng_chunk_length(chunk) + 12;
	}

	/* allocate space to store inflated (but still filtered) data */
	inflated_size = ((upng->width * (upng->height * upng_get_bpp(upng) + 7)) / 8) + upng->height;
	inflated = (unsigned char*)malloc(inflated_size);
	if (inflated == NULL) {
		free(compressed);
		SET_ERROR(upng, UPNG_ENOMEM);
		return upng->error;
	}

	/* decompress image data */
	error = uz_inflate(upng, inflated, inflated_size, compressed, compressed_size);
	if (error != UPNG_EOK) {
		free(compressed);
		free(inflated);
		return upng->error;
	}

	/* free the compressed compressed data */
	free(compressed);

	/* allocate final image buffer */
	upng->size = (upng->height * upng->width * upng_get_bpp(upng) + 7) / 8;
	upng->buffer = (unsigned char*)malloc(upng->size);
	if (upng->buffer == NULL) {
		free(inflated);
		upng->size = 0;
		SET_ERROR(upng, UPNG_ENOMEM);
		return upng->error;
	}

	/* unfilter scanlines */
	error = post_process_scanlines(upng->buffer, inflated, upng);
	if (error != UPNG_EOK) {
		free(inflated);
		free(upng->buffer);
		upng->buffer = NULL;
		upng->size = 0;
		SET_ERROR(upng, error);
		return upng->error;
	}

	/* free the inflated, filtered data */
	free(inflated);

	return UPNG_EOK;
}

upng_error upng_decode_file(upng_t* upng, const char *filename)
{
	unsigned char *buffer;
	FILE *file;
	long size;

	file = fopen(filename, "rb");
	if (!file)
		return UPNG_ENOTFOUND;

	/*get filesize: */
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);

	/*read contents of the file into the vector */
	buffer = (unsigned char *)malloc((unsigned long)size);
	if (buffer == 0) {
		fclose(file);
		return UPNG_ENOMEM;
	}

	fread(buffer, 1, (unsigned long)size, file);
	fclose(file);

	upng_decode(upng, buffer, size);

	free(buffer);

	return upng->error;
}

upng_t* upng_new(void)
{
	upng_t* upng;

	upng = (upng_t*)malloc(sizeof(upng_t));
	if (upng == NULL) {
		return NULL;
	}

	upng->buffer = NULL;
	upng->size = 0;

	upng->width = upng->height = 0;

	upng->color_type = UPNG_RGBA;
	upng->color_depth = 8;
	upng->format = UPNG_RGBA_8888;

	upng->error = UPNG_EOK;
	upng->error_line = 0;

	return upng;
}

void upng_free(upng_t* upng)
{
	/* deallocate image buffer */
	if (upng->buffer != NULL) {
		free(upng->buffer);
	}

	/* deallocate struct itself */
	free(upng);
}

upng_error upng_get_error(const upng_t* upng)
{
	return upng->error;
}

unsigned upng_get_error_line(const upng_t* upng)
{
	return upng->error_line;
}

unsigned upng_get_width(const upng_t* upng)
{
	return upng->width;
}

unsigned upng_get_height(const upng_t* upng)
{
	return upng->height;
}

unsigned upng_get_bpp(const upng_t* upng)
{
	switch (upng->color_type) {
	case UPNG_GREY:
		return upng->color_depth;
	case UPNG_RGB:
		return upng->color_depth * 3;
	case UPNG_GREY_ALPHA:
		return upng->color_depth * 2;
	case UPNG_RGBA:
		return upng->color_depth * 4;
	default:
		return 0;
	}
}

upng_format upng_get_format(const upng_t* upng)
{
	switch (upng->color_type) {
	case UPNG_GREY:
		switch (upng->color_depth) {
		case 1:
			return UPNG_G_1;
		case 2:
			return UPNG_G_2;
		case 4:
			return UPNG_G_4;
		case 8:
			return UPNG_G_8;
		default:
			return UPNG_BADFORMAT;
		}
	case UPNG_RGB:
		if (upng->color_depth == 8) {
			return UPNG_RGB_888;
		} else {
			return UPNG_BADFORMAT;
		}
	case UPNG_GREY_ALPHA:
		switch (upng->color_depth) {
		case 1:
			return UPNG_GA_1;
		case 2:
			return UPNG_GA_2;
		case 4:
			return UPNG_GA_4;
		case 8:
			return UPNG_GA_8;
		default:
			return UPNG_BADFORMAT;
		}
	case UPNG_RGBA:
		if (upng->color_depth == 8) {
			return UPNG_RGBA_8888;
		} else {
			return UPNG_BADFORMAT;
		}
	default:
		return 0;
	}
}

const unsigned char* upng_get_buffer(const upng_t* upng)
{
	return upng->buffer;
}

unsigned upng_get_size(const upng_t* upng)
{
	return upng->size;
}
