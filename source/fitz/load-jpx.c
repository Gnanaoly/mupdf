#include "mupdf/fitz.h"

#if FZ_ENABLE_JPX

typedef struct fz_jpxd_s fz_jpxd;
typedef struct stream_block_s stream_block;

static void
jpx_ycc_to_rgb(fz_context *ctx, fz_pixmap *pix, int cbsign, int crsign)
{
	int w = pix->w;
	int h = pix->h;
	int stride = pix->stride;
	int x, y;

	for (y = 0; y < h; y++)
	{
		unsigned char * row = &pix->samples[stride * y];
		for (x = 0; x < w; x++)
		{
			int ycc[3];
			ycc[0] = row[x * 3 + 0];
			ycc[1] = row[x * 3 + 1];
			ycc[2] = row[x * 3 + 2];

			/* consciously skip Y */
			if (cbsign)
				ycc[1] -= 128;
			if (crsign)
				ycc[2] -= 128;

			row[x * 3 + 0] = fz_clampi((double)ycc[0] + 1.402 * ycc[2], 0, 255);
			row[x * 3 + 1] = fz_clampi((double)ycc[0] - 0.34413 * ycc[1] - 0.71414 * ycc[2], 0, 255);
			row[x * 3 + 2] = fz_clampi((double)ycc[0] + 1.772 * ycc[1], 0, 255);
		}
	}
}

#ifdef HAVE_LURATECH

#include <lwf_jp2.h>

#define MAX_COLORS 4
#define MAX_ALPHAS 1
#define MAX_COMPONENTS (MAX_COLORS + MAX_ALPHAS)

#define HAS_PALETTE(cs) ( \
	(cs) == cJP2_Colorspace_Palette_Gray || \
	(cs) == cJP2_Colorspace_Palette_RGBa || \
	(cs) == cJP2_Colorspace_Palette_RGB_YCCa || \
	(cs) == cJP2_Colorspace_Palette_CIE_LABa || \
	(cs) == cJP2_Colorspace_Palette_ICCa || \
	(cs) == cJP2_Colorspace_Palette_CMYKa)

struct fz_jpxd_s
{
	fz_pixmap *pix;
	JP2_Palette_Params *palette;
	JP2_Property_Value width;
	JP2_Property_Value height;
	fz_colorspace *cs;
	int expand_indexed;
	unsigned long xres;
	unsigned long yres;
	JP2_Property_Value hstep[MAX_COMPONENTS];
	JP2_Property_Value vstep[MAX_COMPONENTS];
	JP2_Property_Value bpss[MAX_COMPONENTS];
	JP2_Property_Value signs[MAX_COMPONENTS];
};

struct stream_block_s
{
	unsigned char *data;
	size_t size;
};

static void * JP2_Callback_Conv
jpx_alloc(long size, JP2_Callback_Param param)
{
	fz_context *ctx = (fz_context *) param;
	return fz_malloc(ctx, size);
}

static JP2_Error JP2_Callback_Conv
jpx_free(void *ptr, JP2_Callback_Param param)
{
	fz_context *ctx = (fz_context *) param;
	fz_free(ctx, ptr);
	return cJP2_Error_OK;
}

static unsigned long JP2_Callback_Conv
jpx_read(unsigned char *pucData,
		unsigned long ulPos, unsigned long ulSize,
		JP2_Callback_Param param)
{
	stream_block *sb = (stream_block *) param;

	if (ulPos >= sb->size)
		return 0;

	ulSize = (unsigned long)fz_minz(ulSize, sb->size - ulPos);
	memcpy(pucData, &sb->data[ulPos], ulSize);
	return ulSize;
}

static JP2_Error JP2_Callback_Conv
jpx_write(unsigned char * pucData, short sComponent, unsigned long ulRow,
		unsigned long ulStart, unsigned long ulNum, JP2_Callback_Param param)
{
	fz_jpxd *state = (fz_jpxd *) param;
	JP2_Property_Value hstep, vstep;
	unsigned char *row;
	int w, h, n, entires, expand;
	JP2_Property_Value x, y, i, bps, sign;
	JP2_Property_Value k;

	w = state->pix->w;
	h = state->pix->h;
	n = state->pix->n;

	if (ulRow >= (unsigned long)h || ulStart >= (unsigned long)w || sComponent >= n)
		return cJP2_Error_OK;

	ulNum = fz_mini(ulNum, w - ulStart);
	hstep = state->hstep[sComponent];
	vstep = state->vstep[sComponent];
	bps = state->bpss[sComponent];
	sign = state->signs[sComponent];

	palette = state->palette ? state->palette->ppulPalette : NULL;
	entries = state->palette ? state->palette->ulEntries : 1;
	expand = state->expand_indexed;

	row = state->pix->samples +
		state->pix->stride * ulRow * vstep +
		n * ulStart * hstep +
		sComponent;

	for (y = 0; ulRow * vstep + y < (JP2_Property_Value)h && y < vstep; y++)
	{
		unsigned char *p = row;

		for (i = 0; i < ulNum; i++)
		{
			for (x = 0; (ulStart + i) * hstep + x < (JP2_Property_Value)w && x < hstep; x++)
			{
				if (palette)
				{
						unsigned char v = fz_clampi(pucData[i], 0, entries - 1);

						if (expand)
						{
							for (k = 0; k < n; k++)
								p[k] = palette[k][v];
						}
						else
							*p = v;
				}
				else
				{
					if (bps > 8)
					{
						unsigned int v = (pucData[2 * i + 1] << 8) | pucData[2 * i + 0];
						v &= (1 << bps) - 1;
						v -= sign;
						*p = v >> (bps - 8);
					}
					else if (bps == 8)
					{
						unsigned int v = pucData[i];
						v &= (1 << bps) - 1;
						v -= sign;
						*p = v;
					}
					else
					{
						unsigned int v = pucData[i];
						v &= (1 << bps) - 1;
						v -= sign;
						*p = v << (8 - bps);
					}
				}

				p += n;
			}
		}

		row += state->pix->stride;
	}

	return cJP2_Error_OK;
}

static fz_pixmap *
jpx_read_image(fz_context *ctx, fz_jpxd *state, unsigned char *data, size_t size, fz_colorspace *defcs, int onlymeta)
{
	JP2_Decomp_Handle doc;
	JP2_Channel_Def_Params *chans = NULL;
	JP2_Error err;
	int colors, alphas, prealphas;
	JP2_Property_Value k;
	JP2_Colorspace colorspace;
	JP2_Property_Value nchans;
	JP2_Property_Value widths[MAX_COMPONENTS];
	JP2_Property_Value heights[MAX_COMPONENTS];
	stream_block sb;

	memset(state, 0x00, sizeof (fz_jpxd));

	sb.data = data;
	sb.size = size;

	fz_try(ctx)
	{
		err = JP2_Decompress_Start(&doc,
				jpx_alloc, (JP2_Callback_Param) ctx,
				jpx_free, (JP2_Callback_Param) ctx,
				jpx_read, (JP2_Callback_Param) &sb);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot open image: %d", (int) err);

#if defined(JP2_LICENSE_NUM_1) && defined(JP2_LICENSE_NUM_2)
		err = JP2_Document_SetLicense(doc, JP2_LICENSE_NUM_1, JP2_LICENSE_NUM_2);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot set license: %d", (int) err);
#endif

		err = JP2_Decompress_GetProp(doc, cJP2_Prop_Extern_Colorspace, (unsigned long *) &colorspace, -1, -1);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get colorspace: %d", (int) err);

		err = JP2_Decompress_GetChannelDefs(doc, &chans, &nchans);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get channel definitions: %d", (int) err);

		colors = 0;
		alphas = 0;
		prealphas = 0;
		for (k = 0; k < nchans; k++)
		{
			switch (chans[k].ulType)
			{
			case cJP2_Channel_Type_Color: colors++; break;
			case cJP2_Channel_Type_Opacity: alphas++; break;
			case cJP2_Channel_Type_Opacity_Pre: prealphas++; break;
			}
		}

		if (prealphas> 0)
			alphas = prealphas;
		colors = fz_clampi(colors, 0, MAX_COLORS);
		alphas = fz_clampi(alphas, 0, MAX_ALPHAS);

		nchans = colors + alphas;

		if (HAS_PALETTE(colorspace))
		{
			err = JP2_Decompress_GetProp(doc, cJP2_Prop_Width, &state->width, -1, 0);
			if (err != cJP2_Error_OK)
				fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get width for palette indicies: %d", (int) err);
			err = JP2_Decompress_GetProp(doc, cJP2_Prop_Height, &state->height, -1, 0);
			if (err != cJP2_Error_OK)
				fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get height for palette indicies: %d", (int) err);

			for (k = 0; k < nchans; k++)
			{
				widths[k] = state->width;
				heights[k] = state->height;
			}
		}
		else
		{
			for (k = 0; k < nchans; k++)
			{
				err = JP2_Decompress_GetProp(doc, cJP2_Prop_Width, &widths[k], -1, k);
				if (err != cJP2_Error_OK)
					fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get width for component %ld: %d", k, (int) err);
				err = JP2_Decompress_GetProp(doc, cJP2_Prop_Height, &heights[k], -1, k);
				if (err != cJP2_Error_OK)
					fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get height for component %ld: %d", k, (int) err);

				state->width = fz_maxi(state->width, widths[k]);
				state->height = fz_maxi(state->height, heights[k]);
			}
		}

		err = JP2_Decompress_GetResolution(doc, &state->yres, &state->xres, NULL,
				cJP2_Resolution_Dots_Per_Inch, cJP2_Resolution_Capture);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get resolution: %d", (int) err);

		if (state->xres == 0 || state->yres == 0)
			state->xres = state->yres = 72;

		if (defcs)
		{
			if (fz_colorspace_n(ctx, defcs) == nchans)
			{
				state->cs = defcs;
			}
			else
			{
				fz_warn(ctx, "jpx file (%lu) and dict colorspace (%d, %s) do not match", nchans, fz_colorspace_n(ctx, defcs), fz_colorspace_name(ctx, defcs));
				defcs = NULL;
			}
		}

		if (!defcs)
		{
			switch (colors)
			{
			case 4: state->cs = fz_device_cmyk(ctx); break;
			case 3: if (colorspace == cJP2_Colorspace_CIE_LABa)
					state->cs = fz_device_lab(ctx);
				else
					state->cs = fz_device_rgb(ctx);
				break;
			case 1: state->cs = fz_device_gray(ctx); break;
			case 0: if (alphas == 1)
				{
					/* alpha only images are rendered as grayscale */
					state->cs = fz_device_gray(ctx);
					colors = 1;
					alphas = 0;
					break;
				}
				/* fallthrough */
			default: fz_throw(ctx, FZ_ERROR_GENERIC, "unsupported number of components: %lu", nchans);
			}
		}
	}
	fz_catch(ctx)
	{
		JP2_Decompress_End(doc);
		fz_rethrow(ctx);
	}

	if (onlymeta)
	{
		JP2_Decompress_End(doc);
		return NULL;
	}

	state->pix = fz_new_pixmap(ctx, state->cs, state->width, state->height, alphas);
	fz_clear_pixmap_with_value(ctx, state->pix, 0);

	fz_try(ctx)
	{
		if (HAS_PALETTE(colorspace))
		{
			if (!fz_colorspace_is_indexed(ctx, state->cs))
				state->expand_indexed = 1;

			err = JP2_Decompress_GetPalette(doc, &state->palette);
			if (err != cJP2_Error_OK)
				fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get indexed palette: %d", (int) err);

			/* no available sample file */
			for (k = 0; k < state->palette->ulChannels; k++)
				if (state->palette->pucSignedSample[k])
					fz_throw(ctx, FZ_ERROR_GENERIC, "signed palette components not yet supported");
		}

		for (k = 0; k < nchans; k++)
		{
			state->hstep[k] = (state->width + (widths[k] - 1)) / widths[k];
			state->vstep[k] = (state->height + (heights[k] - 1)) / heights[k];

			if (HAS_PALETTE(colorspace))
			{
				state->bpss[k] = state->palette->pucBitsPerSample[k];
				state->signs[k] = state->palette->pucSignedSample[k];
			}
			else
			{
				err = JP2_Decompress_GetProp(doc, cJP2_Prop_Bits_Per_Sample, &state->bpss[k], -1, k);
				if (err != cJP2_Error_OK)
					fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get bits per sample for compomment %d: %d", k, (int) err);
				err = JP2_Decompress_GetProp(doc, cJP2_Prop_Signed_Samples, &state->signs[k], -1, k);
				if (err != cJP2_Error_OK)
					fz_throw(ctx, FZ_ERROR_GENERIC, "cannot get signed for compomment %d: %d", k, (int) err);
			}
			if (state->signs[k])
				state->signs[k] = 1 << (state->bpss[k] - 1);
		}

		err = JP2_Decompress_SetProp(doc, cJP2_Prop_Output_Parameter, (JP2_Property_Value) state);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot set write callback userdata: %d", (int) err);
		err = JP2_Decompress_SetProp(doc, cJP2_Prop_Output_Function, (JP2_Property_Value) jpx_write);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot set write callback: %d", (int) err);

		err = JP2_Decompress_Image(doc);
		if (err != cJP2_Error_OK)
			fz_throw(ctx, FZ_ERROR_GENERIC, "cannot decode image: %d", (int) err);

		if (colorspace == cJP2_Colorspace_RGB_YCCa)
			jpx_ycc_to_rgb(ctx, state->pix, !state->signs[1], !state->signs[2]);

		if (state->pix->alpha && ! (HAS_PALETTE(colorspace) && !state->expand_indexed))
		{
			if (alphas > 0 && prealphas == 0)
			{
				state->pix = fz_ensure_pixmap_is_additive(ctx, state->pix);
				fz_premultiply_pixmap(ctx, state->pix);
			}
		}
	}
	fz_always(ctx)
		JP2_Decompress_End(doc);
	fz_catch(ctx)
	{
		fz_drop_pixmap(ctx, state->pix);
		fz_rethrow(ctx);
	}

	return state->pix;
}

fz_pixmap *
fz_load_jpx(fz_context *ctx, unsigned char *data, size_t size, fz_colorspace *defcs)
{
	fz_jpxd state = { 0 };

	return jpx_read_image(ctx, &state, data, size, defcs, 0);
}

void
fz_load_jpx_info(fz_context *ctx, unsigned char *data, size_t size, int *wp, int *hp, int *xresp, int *yresp, fz_colorspace **cspacep)
{
	fz_jpxd state = { 0 };

	jpx_read_image(ctx, &state, data, size, NULL, 1);

	*cspacep = state.cs;
	*wp = state.width;
	*hp = state.height;
	*xresp = state.xres;
	*yresp = state.yres;
}

#else /* HAVE_LURATECH */

#define OPJ_STATIC
#define OPJ_HAVE_INTTYPES_H
#if !defined(_MSC_VER) || _MSC_VER >= 1600
#define OPJ_HAVE_STDINT_H
#endif
#define USE_JPIP

#include <openjpeg.h>

struct fz_jpxd_s
{
	int width;
	int height;
	fz_colorspace *cs;
	int xres;
	int yres;
};

struct stream_block_s
{
	unsigned char *data;
	OPJ_SIZE_T size;
	OPJ_SIZE_T pos;
};

/* OpenJPEG does not provide a safe mechanism to intercept
 * allocations. In the latest version all allocations go
 * though opj_malloc etc, but no context is passed around.
 *
 * In order to ensure that allocations throughout mupdf
 * are done consistently, we implement opj_malloc etc as
 * functions that call down to fz_malloc etc. These
 * require context variables, so we lock and unlock around
 * calls to openjpeg. Any attempt to call through
 * without setting these will be detected.
 *
 * It is therefore vital that any fz_lock/fz_unlock
 * handlers are shared between all the fz_contexts in
 * use at a time.
 */

/* Potentially we can write different versions
 * of get_context and set_context for different
 * threading systems.
 */

static fz_context *opj_secret = NULL;

static void set_opj_context(fz_context *ctx)
{
	opj_secret = ctx;
}

static fz_context *get_opj_context()
{
	return opj_secret;
}

void opj_lock(fz_context *ctx)
{
	fz_lock(ctx, FZ_LOCK_FREETYPE);

	set_opj_context(ctx);
}

void opj_unlock(fz_context *ctx)
{
	set_opj_context(NULL);

	fz_unlock(ctx, FZ_LOCK_FREETYPE);
}

void *opj_malloc(size_t size)
{
	fz_context *ctx = get_opj_context();

	assert(ctx != NULL);

	return fz_malloc_no_throw(ctx, size);
}

void *opj_calloc(size_t n, size_t size)
{
	fz_context *ctx = get_opj_context();

	assert(ctx != NULL);

	return fz_calloc_no_throw(ctx, n, size);
}

void *opj_realloc(void *ptr, size_t size)
{
	fz_context *ctx = get_opj_context();

	assert(ctx != NULL);

	return fz_resize_array_no_throw(ctx, ptr, 1, size);
}

void opj_free(void *ptr)
{
	fz_context *ctx = get_opj_context();

	assert(ctx != NULL);

	fz_free(ctx, ptr);
}

void * opj_aligned_malloc(size_t size)
{
	uint8_t *ptr;
	int off;

	if (size == 0)
		return NULL;

	size += 16 + sizeof(uint8_t);
	ptr = opj_malloc(size);
	if (ptr == NULL)
		return NULL;
	off = 16-(((int)(intptr_t)ptr) & 15);
	ptr[off-1] = off;
	return ptr + off;
}

void opj_aligned_free(void* ptr_)
{
	uint8_t *ptr = (uint8_t *)ptr_;
	uint8_t off;
	if (ptr == NULL)
		return;

	off = ptr[-1];
	opj_free((void *)(((unsigned char *)ptr) - off));
}

#if 0
/* UNUSED currently, and moderately tricky, so deferred until required */
void * opj_aligned_realloc(void *ptr, size_t size)
{
	return opj_realloc(ptr, size);
}
#endif

static void fz_opj_error_callback(const char *msg, void *client_data)
{
	fz_context *ctx = (fz_context *)client_data;
	fz_warn(ctx, "openjpeg error: %s", msg);
}

static void fz_opj_warning_callback(const char *msg, void *client_data)
{
	fz_context *ctx = (fz_context *)client_data;
	fz_warn(ctx, "openjpeg warning: %s", msg);
}

static void fz_opj_info_callback(const char *msg, void *client_data)
{
	/* fz_warn("openjpeg info: %s", msg); */
}

static OPJ_SIZE_T fz_opj_stream_read(void * p_buffer, OPJ_SIZE_T p_nb_bytes, void * p_user_data)
{
	stream_block *sb = (stream_block *)p_user_data;
	OPJ_SIZE_T len;

	len = sb->size - sb->pos;
	if (len == 0)
		return (OPJ_SIZE_T)-1; /* End of file! */
	if (len > p_nb_bytes)
		len = p_nb_bytes;
	memcpy(p_buffer, sb->data + sb->pos, len);
	sb->pos += len;
	return len;
}

static OPJ_OFF_T fz_opj_stream_skip(OPJ_OFF_T skip, void * p_user_data)
{
	stream_block *sb = (stream_block *)p_user_data;

	if (skip > (OPJ_OFF_T)(sb->size - sb->pos))
		skip = (OPJ_OFF_T)(sb->size - sb->pos);
	sb->pos += skip;
	return sb->pos;
}

static OPJ_BOOL fz_opj_stream_seek(OPJ_OFF_T seek_pos, void * p_user_data)
{
	stream_block *sb = (stream_block *)p_user_data;

	if (seek_pos > (OPJ_OFF_T)sb->size)
		return OPJ_FALSE;
	sb->pos = seek_pos;
	return OPJ_TRUE;
}

static int
l2subfactor(fz_context *ctx, unsigned int max_w, unsigned int w)
{
	int i;

	for (i = 0; max_w != 0 && w != max_w; i++)
		max_w >>= 1;
	if (max_w == 0)
		fz_throw(ctx, FZ_ERROR_GENERIC, "image components are of incompatible dimensions");
	return i;
}

static fz_pixmap *
jpx_read_image(fz_context *ctx, fz_jpxd *state, unsigned char *data, size_t size, fz_colorspace *defcs, int onlymeta)
{
	fz_pixmap *img;
	opj_dparameters_t params;
	opj_codec_t *codec;
	opj_image_t *jpx;
	opj_stream_t *stream;
	unsigned char *p;
	OPJ_CODEC_FORMAT format;
	int a, n, w, h, depth, sgnd;
	int x, y, k, v, stride;
	stream_block sb;
	unsigned int max_w, max_h;
	int sub_w[FZ_MAX_COLORS];
	int sub_h[FZ_MAX_COLORS];
	int upsample_required = 0;

	fz_var(img);

	if (size < 2)
		fz_throw(ctx, FZ_ERROR_GENERIC, "not enough data to determine image format");

	/* Check for SOC marker -- if found we have a bare J2K stream */
	if (data[0] == 0xFF && data[1] == 0x4F)
		format = OPJ_CODEC_J2K;
	else
		format = OPJ_CODEC_JP2;

	opj_set_default_decoder_parameters(&params);
	if (fz_colorspace_is_indexed(ctx, defcs))
		params.flags |= OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG;

	codec = opj_create_decompress(format);
	opj_set_info_handler(codec, fz_opj_info_callback, ctx);
	opj_set_warning_handler(codec, fz_opj_warning_callback, ctx);
	opj_set_error_handler(codec, fz_opj_error_callback, ctx);
	if (!opj_setup_decoder(codec, &params))
	{
		opj_destroy_codec(codec);
		fz_throw(ctx, FZ_ERROR_GENERIC, "j2k decode failed");
	}

	stream = opj_stream_default_create(OPJ_TRUE);
	sb.data = data;
	sb.pos = 0;
	sb.size = size;

	opj_stream_set_read_function(stream, fz_opj_stream_read);
	opj_stream_set_skip_function(stream, fz_opj_stream_skip);
	opj_stream_set_seek_function(stream, fz_opj_stream_seek);
	opj_stream_set_user_data(stream, &sb, NULL);
	/* Set the length to avoid an assert */
	opj_stream_set_user_data_length(stream, size);

	if (!opj_read_header(stream, codec, &jpx))
	{
		opj_stream_destroy(stream);
		opj_destroy_codec(codec);
		fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to read JPX header");
	}

	if (!opj_decode(codec, stream, jpx))
	{
		opj_stream_destroy(stream);
		opj_destroy_codec(codec);
		opj_image_destroy(jpx);
		fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to decode JPX image");
	}

	opj_stream_destroy(stream);
	opj_destroy_codec(codec);

	/* jpx should never be NULL here, but check anyway */
	if (!jpx)
		fz_throw(ctx, FZ_ERROR_GENERIC, "opj_decode failed");

	n = jpx->numcomps;
	depth = jpx->comps[0].prec;
	sgnd = jpx->comps[0].sgnd;

	if (jpx->color_space == OPJ_CLRSPC_SRGB && n == 4) { n = 3; a = 1; }
	else if (jpx->color_space == OPJ_CLRSPC_SYCC && n == 4) { n = 3; a = 1; }
	else if (n == 2) { n = 1; a = 1; }
	else if (n > 4) { n = 4; a = 1; }
	else { a = 0; }

	if (defcs)
	{
		if (fz_colorspace_n(ctx, defcs) == n)
		{
			state->cs = defcs;
		}
		else
		{
			fz_warn(ctx, "jpx file and dict colorspace do not match");
			defcs = NULL;
		}
	}

	if (!defcs)
	{
		switch (n)
		{
		case 1: state->cs = fz_device_gray(ctx); break;
		case 3: state->cs = fz_device_rgb(ctx); break;
		case 4: state->cs = fz_device_cmyk(ctx); break;
		default: fz_throw(ctx, FZ_ERROR_GENERIC, "unsupported number of components: %d", n);
		}
	}

	max_w = jpx->comps[0].w;
	max_h = jpx->comps[0].h;
	for (k = 1; k < n + a; k++)
	{
		if (max_w < jpx->comps[k].w)
			max_w = jpx->comps[k].w;
		if (max_h < jpx->comps[k].w)
			max_h = jpx->comps[k].h;
		if (!jpx->comps[k].data)
		{
			opj_image_destroy(jpx);
			fz_throw(ctx, FZ_ERROR_GENERIC, "image components are missing data");
		}
		if (jpx->comps[k].prec != jpx->comps[0].prec)
		{
			opj_image_destroy(jpx);
			fz_throw(ctx, FZ_ERROR_GENERIC, "image components have different precision");
		}
	}

	for (k = 0; k < n + a; k++)
	{
		sub_w[k] = l2subfactor(ctx, max_w, jpx->comps[k].w);
		sub_h[k] = l2subfactor(ctx, max_h, jpx->comps[k].h);
		if (sub_w[k] != 0 || sub_h[k] != 0)
			upsample_required = 1;
	}

	state->width = w = (int)max_w;
	state->height = h = (int)max_h;
	state->xres = 72; /* openjpeg does not read the JPEG 2000 resc box */
	state->yres = 72; /* openjpeg does not read the JPEG 2000 resc box */

	if (onlymeta)
	{
		opj_image_destroy(jpx);
		return NULL;
	}

	img = fz_new_pixmap(ctx, state->cs, w, h, a);

	fz_try(ctx)
	{
		p = img->samples;
		if (upsample_required)
		{
			stride = img->stride;
			for (y = 0; y < h; y++)
			{
				for (k = 0; k < n + a; k++)
				{
					int sh = sub_h[k];
					int sw = sub_w[k];
					int yy = (y>>sh) * jpx->comps[k].w;
					OPJ_INT32 *data = &jpx->comps[k].data[yy];
					for (x = 0; x < w; x ++)
					{
						v = data[x>>sw];
						if (sgnd)
							v = v + (1 << (depth - 1));
						if (depth > 8)
							v = v >> (depth - 8);
						else if (depth < 8)
							v = v << (8 - depth);
						*p = v;
						p += n + a;
					}
					p += 1 - w * (n + a);
				}
				p += stride - (n + a);
			}
		}
		else
		{
			stride = img->stride - w * (n + a);
			for (y = 0; y < h; y++)
			{
				for (x = 0; x < w; x++)
				{
					for (k = 0; k < n + a; k++)
					{
						v = jpx->comps[k].data[y * w + x];
						if (sgnd)
							v = v + (1 << (depth - 1));
						if (depth > 8)
							v = v >> (depth - 8);
						else if (depth < 8)
							v = v << (8 - depth);
						*p++ = v;
					}
				}
				p += stride;
			}
		}

		if (jpx->color_space == OPJ_CLRSPC_SYCC && n == 3 && a == 0)
			jpx_ycc_to_rgb(ctx, img, 1, 1);

		if (a)
		{
			img = fz_ensure_pixmap_is_additive(ctx, img);
			fz_premultiply_pixmap(ctx, img);
		}
	}
	fz_always(ctx)
		opj_image_destroy(jpx);
	fz_catch(ctx)
	{
		fz_drop_pixmap(ctx, img);
		fz_rethrow(ctx);
	}

	return img;
}

fz_pixmap *
fz_load_jpx(fz_context *ctx, unsigned char *data, size_t size, fz_colorspace *defcs)
{
	fz_jpxd state = { 0 };
	fz_pixmap *pix;

	fz_try(ctx)
	{
		opj_lock(ctx);
		pix = jpx_read_image(ctx, &state, data, size, defcs, 0);
	}
	fz_always(ctx)
		opj_unlock(ctx);
	fz_catch(ctx)
		fz_rethrow(ctx);

	return pix;
}

void
fz_load_jpx_info(fz_context *ctx, unsigned char *data, size_t size, int *wp, int *hp, int *xresp, int *yresp, fz_colorspace **cspacep)
{
	fz_jpxd state = { 0 };

	fz_try(ctx)
	{
		opj_lock(ctx);
		jpx_read_image(ctx, &state, data, size, NULL, 1);
	}
	fz_always(ctx)
		opj_unlock(ctx);
	fz_catch(ctx)
		fz_rethrow(ctx);

	*cspacep = state.cs;
	*wp = state.width;
	*hp = state.height;
	*xresp = state.xres;
	*yresp = state.yres;
}

#endif /* HAVE_LURATECH */

#else /* FZ_ENABLE_JPX */

fz_pixmap *
fz_load_jpx(fz_context *ctx, unsigned char *data, size_t size, fz_colorspace *defcs)
{
	fz_throw(ctx, FZ_ERROR_GENERIC, "JPX support disabled");
}

void
fz_load_jpx_info(fz_context *ctx, unsigned char *data, size_t size, int *wp, int *hp, int *xresp, int *yresp, fz_colorspace **cspacep)
{
	fz_throw(ctx, FZ_ERROR_GENERIC, "JPX support disabled");
}

#endif
