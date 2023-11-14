/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPEG-HTTP streamer.                   #
#                                                                            #
#    This source file based on code of MJPG-Streamer.                        #
#                                                                            #
#    Copyright (C) 2005-2006  Laurent Pinchart & Michel Xhaard               #
#    Copyright (C) 2006  Gabriel A. Devenyi                                  #
#    Copyright (C) 2007  Tom Stöveken                                        #
#    Copyright (C) 2018-2023  Maxim Devaev <mdevaev@gmail.com>               #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#include "encoder.h"


typedef struct {
	struct	jpeg_destination_mgr mgr; // Default manager
	JOCTET	*buf; // Start of buffer
	us_frame_s	*frame;
} _jpeg_dest_manager_s;


static void _jpeg_set_dest_frame(j_compress_ptr jpeg, us_frame_s *frame);

static void _jpeg_write_scanlines_yuyv(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);
static void _jpeg_write_scanlines_uyvy(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);
static void _jpeg_write_scanlines_rgb565(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);
static void _jpeg_write_scanlines_rgb24(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);
static void _jpeg_write_scanlines_bgr24(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);
static void _jpeg_write_scanlines_nv12(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);
static void _jpeg_write_scanlines_nv16(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);
static void _jpeg_write_scanlines_nv24(struct jpeg_compress_struct *jpeg, const us_frame_s *frame);

static void _nv12_to_rgb24(const uint8_t *in, uint8_t *out, int width, int height);
static void _nv16_to_rgb24(const uint8_t *in, uint8_t *out, int width, int height);
static void _nv24_to_rgb24(const uint8_t *in, uint8_t *out, int width, int height);

static void _jpeg_init_destination(j_compress_ptr jpeg);
static boolean _jpeg_empty_output_buffer(j_compress_ptr jpeg);
static void _jpeg_term_destination(j_compress_ptr jpeg);


void us_cpu_encoder_compress(const us_frame_s *src, us_frame_s *dest, unsigned quality) {
	// This function based on compress_image_to_jpeg() from mjpg-streamer

	us_frame_encoding_begin(src, dest, V4L2_PIX_FMT_JPEG);

	struct jpeg_compress_struct jpeg;
	struct jpeg_error_mgr jpeg_error;

	jpeg.err = jpeg_std_error(&jpeg_error);
	jpeg_create_compress(&jpeg);

	_jpeg_set_dest_frame(&jpeg, dest);

	jpeg.image_width = src->width;
	jpeg.image_height = src->height;
	jpeg.input_components = 3;
	jpeg.in_color_space = JCS_RGB;

	jpeg_set_defaults(&jpeg);
	jpeg_set_quality(&jpeg, quality, TRUE);

	jpeg_start_compress(&jpeg, TRUE);

#	define WRITE_SCANLINES(x_format, x_func) \
		case x_format: { x_func(&jpeg, src); break; }

	switch (src->format) {
		// https://www.fourcc.org/yuv.php
		WRITE_SCANLINES(V4L2_PIX_FMT_YUYV, _jpeg_write_scanlines_yuyv);
		WRITE_SCANLINES(V4L2_PIX_FMT_UYVY, _jpeg_write_scanlines_uyvy);
		WRITE_SCANLINES(V4L2_PIX_FMT_RGB565, _jpeg_write_scanlines_rgb565);
		WRITE_SCANLINES(V4L2_PIX_FMT_NV12, _jpeg_write_scanlines_nv12);
		WRITE_SCANLINES(V4L2_PIX_FMT_NV16, _jpeg_write_scanlines_nv16);
		WRITE_SCANLINES(V4L2_PIX_FMT_BGR24, _jpeg_write_scanlines_bgr24);
		WRITE_SCANLINES(V4L2_PIX_FMT_RGB24, _jpeg_write_scanlines_rgb24);
		WRITE_SCANLINES(V4L2_PIX_FMT_NV24, _jpeg_write_scanlines_nv24);
		default: assert(0 && "Unsupported input format for CPU encoder");
	}

#	undef WRITE_SCANLINES

	jpeg_finish_compress(&jpeg);
	jpeg_destroy_compress(&jpeg);

	us_frame_encoding_end(dest);
}

static void _jpeg_set_dest_frame(j_compress_ptr jpeg, us_frame_s *frame) {
	if (jpeg->dest == NULL) {
		assert((jpeg->dest = (struct jpeg_destination_mgr *)(*jpeg->mem->alloc_small)(
			(j_common_ptr) jpeg, JPOOL_PERMANENT, sizeof(_jpeg_dest_manager_s)
		)) != NULL);
	}

	_jpeg_dest_manager_s *const dest = (_jpeg_dest_manager_s *)jpeg->dest;
	dest->mgr.init_destination = _jpeg_init_destination;
	dest->mgr.empty_output_buffer = _jpeg_empty_output_buffer;
	dest->mgr.term_destination = _jpeg_term_destination;
	dest->frame = frame;

	frame->used = 0;
}

#define YUV_R(_y, _, _v)	(((_y) + (359 * (_v))) >> 8)
#define YUV_G(_y, _u, _v)	(((_y) - (88 * (_u)) - (183 * (_v))) >> 8)
#define YUV_B(_y, _u, _)	(((_y) + (454 * (_u))) >> 8)
#define NORM_COMPONENT(_x)	(((_x) > 255) ? 255 : (((_x) < 0) ? 0 : (_x)))

static void _jpeg_write_scanlines_yuyv(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
	uint8_t *line_buf;
	US_CALLOC(line_buf, frame->width * 3);

	const unsigned padding = us_frame_get_padding(frame);
	const uint8_t *data = frame->data;
	unsigned z = 0;

	while (jpeg->next_scanline < frame->height) {
		uint8_t *ptr = line_buf;

		for (unsigned x = 0; x < frame->width; ++x) {
			const int y = (!z ? data[0] << 8 : data[2] << 8);
			const int u = data[1] - 128;
			const int v = data[3] - 128;

			const int r = YUV_R(y, u, v);
			const int g = YUV_G(y, u, v);
			const int b = YUV_B(y, u, v);

			*(ptr++) = NORM_COMPONENT(r);
			*(ptr++) = NORM_COMPONENT(g);
			*(ptr++) = NORM_COMPONENT(b);

			if (z++) {
				z = 0;
				data += 4;
			}
		}
		data += padding;

		JSAMPROW scanlines[1] = {line_buf};
		jpeg_write_scanlines(jpeg, scanlines, 1);
	}

	free(line_buf);
}

static void _jpeg_write_scanlines_uyvy(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
	uint8_t *line_buf;
	US_CALLOC(line_buf, frame->width * 3);

	const unsigned padding = us_frame_get_padding(frame);
	const uint8_t *data = frame->data;
	unsigned z = 0;

	while (jpeg->next_scanline < frame->height) {
		uint8_t *ptr = line_buf;

		for (unsigned x = 0; x < frame->width; ++x) {
			const int y = (!z ? data[1] << 8 : data[3] << 8);
			const int u = data[0] - 128;
			const int v = data[2] - 128;

			const int r = YUV_R(y, u, v);
			const int g = YUV_G(y, u, v);
			const int b = YUV_B(y, u, v);

			*(ptr++) = NORM_COMPONENT(r);
			*(ptr++) = NORM_COMPONENT(g);
			*(ptr++) = NORM_COMPONENT(b);

			if (z++) {
				z = 0;
				data += 4;
			}
		}
		data += padding;

		JSAMPROW scanlines[1] = {line_buf};
		jpeg_write_scanlines(jpeg, scanlines, 1);
	}

	free(line_buf);
}

static void _nv16_to_rgb24(const uint8_t *in, uint8_t *out, int width, int height) {
    int frame_size = width * height;
    int uv_offset = frame_size;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int y_index = i * width + j;
            int uv_index = uv_offset + (i * width) + (j & ~1); // For each pair of pixels, the same UV is used
            int Y = in[y_index];
            int U = in[uv_index] - 128;   // Cb component
            int V = in[uv_index + 1] - 128; // Cr component

            // YUV to RGB conversion
            int R = (298 * (Y - 16) + 409 * V + 128) >> 8;
            int G = (298 * (Y - 16) - 100 * U - 208 * V + 128) >> 8;
            int B = (298 * (Y - 16) + 516 * U + 128) >> 8;

            // Clamp values to [0, 255]
            R = R < 0 ? 0 : (R > 255 ? 255 : R);
            G = G < 0 ? 0 : (G > 255 ? 255 : G);
            B = B < 0 ? 0 : (B > 255 ? 255 : B);

            int rgb_index = y_index * 3;
            out[rgb_index] = (uint8_t)R;
            out[rgb_index + 1] = (uint8_t)G;
            out[rgb_index + 2] = (uint8_t)B;
        }
    }
}

static void _jpeg_write_scanlines_nv16(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
    const unsigned padding = us_frame_get_padding(frame);
    uint8_t *rgb;
    US_CALLOC(rgb, frame->width * frame->height * 3);
    _nv16_to_rgb24(frame->data, rgb, frame->width, frame->height);
    uint8_t *data = rgb;

    while (jpeg->next_scanline < frame->height) {
        JSAMPROW scanlines[1] = {data};
        jpeg_write_scanlines(jpeg, scanlines, 1);

        data += (frame->width * 3) + padding;
    }

    free(rgb);
}


static void _nv12_to_rgb24(const uint8_t *in, uint8_t *out, int width, int height) {
    int frame_size = width * height;
    int uv_offset = frame_size;
    
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int uv_index = (i / 2) * width + 2 * (j / 2);
            int Y = in[i * width + j];
            int U = in[uv_offset + uv_index];
            int V = in[uv_offset + uv_index + 1];

            // YUV to RGB conversion
            int C = Y - 16;
            int D = U - 128;
            int E = V - 128;
            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            // Clamp values to [0, 255]
            R = (R < 0) ? 0 : ((R > 255) ? 255 : R);
            G = (G < 0) ? 0 : ((G > 255) ? 255 : G);
            B = (B < 0) ? 0 : ((B > 255) ? 255 : B);

            int rgb_index = (i * width + j) * 3;
            out[rgb_index] = (uint8_t)R;
            out[rgb_index + 1] = (uint8_t)G;
            out[rgb_index + 2] = (uint8_t)B;
        }
    }
}
static void _nv24_to_rgb24(const uint8_t *in, uint8_t *out, int width, int height) {
    int frame_size = width * height;
    int uv_offset = frame_size;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int y_index = i * width + j;
            int uv_index = uv_offset + (2 * j) + (i * width * 2); // Adjusted for full resolution CbCr plane
            int Y = in[y_index];
            int U = in[uv_index] - 128; // Cb component
            int V = in[uv_index + 1] - 128; // Cr component

            // YUV to RGB conversion
            int R = (298 * (Y - 16) + 409 * V + 128) >> 8;
            int G = (298 * (Y - 16) - 100 * U - 208 * V + 128) >> 8;
            int B = (298 * (Y - 16) + 516 * U + 128) >> 8;

            // Clamp values to [0, 255]
            R = R < 0 ? 0 : (R > 255 ? 255 : R);
            G = G < 0 ? 0 : (G > 255 ? 255 : G);
            B = B < 0 ? 0 : (B > 255 ? 255 : B);

            int rgb_index = y_index * 3;
            out[rgb_index] = (uint8_t)R;
            out[rgb_index + 1] = (uint8_t)G;
            out[rgb_index + 2] = (uint8_t)B;
        }
    }
}



static void _jpeg_write_scanlines_nv12(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
	const unsigned padding = us_frame_get_padding(frame);
	uint8_t *rgb;
	US_CALLOC(rgb, frame->width * frame->height * 3);
	_nv12_to_rgb24(frame->data, rgb, frame->width, frame->height);
	uint8_t *data = rgb;

	while (jpeg->next_scanline < frame->height) {
		JSAMPROW scanlines[1] = {data};
		jpeg_write_scanlines(jpeg, scanlines, 1);

		data += (frame->width * 3) + padding;
	}

	free(rgb);
}

static void _jpeg_write_scanlines_nv24(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
    const unsigned padding = us_frame_get_padding(frame);
    uint8_t *rgb;
    US_CALLOC(rgb, frame->width * frame->height * 3);
    _nv24_to_rgb24(frame->data, rgb, frame->width, frame->height);
    uint8_t *data = rgb;

    while (jpeg->next_scanline < frame->height) {
        JSAMPROW scanlines[1] = {data};
        jpeg_write_scanlines(jpeg, scanlines, 1);

        data += (frame->width * 3) + padding;
    }

    free(rgb);
}

#undef NORM_COMPONENT
#undef YUV_B
#undef YUV_G
#undef YUV_R

static void _jpeg_write_scanlines_rgb565(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
	uint8_t *line_buf;
	US_CALLOC(line_buf, frame->width * 3);

	const unsigned padding = us_frame_get_padding(frame);
	const uint8_t *data = frame->data;

	while (jpeg->next_scanline < frame->height) {
		uint8_t *ptr = line_buf;

		for (unsigned x = 0; x < frame->width; ++x) {
			const unsigned int two_byte = (data[1] << 8) + data[0];

			*(ptr++) = data[1] & 248; // Red
			*(ptr++) = (uint8_t)((two_byte & 2016) >> 3); // Green
			*(ptr++) = (data[0] & 31) * 8; // Blue

			data += 2;
		}
		data += padding;

		JSAMPROW scanlines[1] = {line_buf};
		jpeg_write_scanlines(jpeg, scanlines, 1);
	}

	free(line_buf);
}

static void _jpeg_write_scanlines_bgr24(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
	uint8_t *line_buf;
	US_CALLOC(line_buf, frame->width * 3);

	const unsigned padding = us_frame_get_padding(frame);
	uint8_t *data = frame->data;

	while (jpeg->next_scanline < frame->height) {
		uint8_t *ptr = line_buf;

		// swap B and R values
		for (unsigned x = 0; x < frame->width * 3; x+=3) {
			*(ptr++) = data[x+2];
			*(ptr++) = data[x+1];
			*(ptr++) = data[x];
		}
		
		JSAMPROW scanlines[1] = {line_buf};
		jpeg_write_scanlines(jpeg, scanlines, 1);

		data += (frame->width * 3) + padding;
	}

	free(line_buf);
}

static void _jpeg_write_scanlines_rgb24(struct jpeg_compress_struct *jpeg, const us_frame_s *frame) {
	const unsigned padding = us_frame_get_padding(frame);
	uint8_t *data = frame->data;

	while (jpeg->next_scanline < frame->height) {
		JSAMPROW scanlines[1] = {data};
		jpeg_write_scanlines(jpeg, scanlines, 1);

		data += (frame->width * 3) + padding;
	}
}

#define JPEG_OUTPUT_BUFFER_SIZE ((size_t)4096)

static void _jpeg_init_destination(j_compress_ptr jpeg) {
	_jpeg_dest_manager_s *const dest = (_jpeg_dest_manager_s *)jpeg->dest;

	// Allocate the output buffer - it will be released when done with image
	assert((dest->buf = (JOCTET *)(*jpeg->mem->alloc_small)(
		(j_common_ptr) jpeg, JPOOL_IMAGE, JPEG_OUTPUT_BUFFER_SIZE * sizeof(JOCTET)
	)) != NULL);

	dest->mgr.next_output_byte = dest->buf;
	dest->mgr.free_in_buffer = JPEG_OUTPUT_BUFFER_SIZE;
}

static boolean _jpeg_empty_output_buffer(j_compress_ptr jpeg) {
	// Called whenever local jpeg buffer fills up

	_jpeg_dest_manager_s *const dest = (_jpeg_dest_manager_s *)jpeg->dest;

	us_frame_append_data(dest->frame, dest->buf, JPEG_OUTPUT_BUFFER_SIZE);

	dest->mgr.next_output_byte = dest->buf;
	dest->mgr.free_in_buffer = JPEG_OUTPUT_BUFFER_SIZE;

	return TRUE;
}

static void _jpeg_term_destination(j_compress_ptr jpeg) {
	// Called by jpeg_finish_compress after all data has been written.
	// Usually needs to flush buffer.

	_jpeg_dest_manager_s *const dest = (_jpeg_dest_manager_s *)jpeg->dest;
	const size_t final = JPEG_OUTPUT_BUFFER_SIZE - dest->mgr.free_in_buffer;

	// Write any data remaining in the buffer.
	us_frame_append_data(dest->frame, dest->buf, final);
}

#undef JPEG_OUTPUT_BUFFER_SIZE
