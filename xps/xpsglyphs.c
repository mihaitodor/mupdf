#include "fitz.h"
#include "muxps.h"

#include <ctype.h> /* for tolower() */

static inline int unhex(int i)
{
	if (isdigit(i))
		return i - '0';
	return tolower(i) - 'a' + 10;
}

/*
 * Some fonts in XPS are obfuscated by XOR:ing the first 32 bytes of the
 * data with the GUID in the fontname.
 */
static void
xps_deobfuscate_font_resource(xps_context *ctx, xps_part *part)
{
	byte buf[33];
	byte key[16];
	char *p;
	int i;

	p = strrchr(part->name, '/');
	if (!p)
		p = part->name;

	for (i = 0; i < 32 && *p; p++)
	{
		if (isxdigit(*p))
			buf[i++] = *p;
	}
	buf[i] = 0;

	if (i != 32)
	{
		fz_warn("cannot extract GUID from obfuscated font part name");
		return;
	}

	for (i = 0; i < 16; i++)
		key[i] = unhex(buf[i*2+0]) * 16 + unhex(buf[i*2+1]);

	for (i = 0; i < 16; i++)
	{
		part->data[i] ^= key[15-i];
		part->data[i+16] ^= key[15-i];
	}
}

static void
xps_select_best_font_encoding(fz_font *font)
{
	static struct { int pid, eid; } xps_cmap_list[] =
	{
		{ 3, 10 },		/* Unicode with surrogates */
		{ 3, 1 },		/* Unicode without surrogates */
		{ 3, 5 },		/* Wansung */
		{ 3, 4 },		/* Big5 */
		{ 3, 3 },		/* Prc */
		{ 3, 2 },		/* ShiftJis */
		{ 3, 0 },		/* Symbol */
		// { 0, * }, -- Unicode (deprecated)
		{ 1, 0 },
		{ -1, -1 },
	};

	int i, k, n, pid, eid;

	n = xps_count_font_encodings(font);
	for (k = 0; xps_cmap_list[k].pid != -1; k++)
	{
		for (i = 0; i < n; i++)
		{
			xps_identify_font_encoding(font, i, &pid, &eid);
			if (pid == xps_cmap_list[k].pid && eid == xps_cmap_list[k].eid)
			{
				xps_select_font_encoding(font, i);
				return;
			}
		}
	}

	fz_warn("could not find a suitable cmap");
}

/*
 * Parse and draw an XPS <Glyphs> element.
 *
 * Indices syntax:

 GlyphIndices	= GlyphMapping ( ";" GlyphMapping )
 GlyphMapping	= ( [ClusterMapping] GlyphIndex ) [GlyphMetrics]
 ClusterMapping = "(" ClusterCodeUnitCount [":" ClusterGlyphCount] ")"
 ClusterCodeUnitCount	= * DIGIT
 ClusterGlyphCount		= * DIGIT
 GlyphIndex		= * DIGIT
 GlyphMetrics	= "," AdvanceWidth ["," uOffset ["," vOffset]]
 AdvanceWidth	= ["+"] RealNum
 uOffset		= ["+" | "-"] RealNum
 vOffset		= ["+" | "-"] RealNum
 RealNum		= ((DIGIT ["." DIGIT]) | ("." DIGIT)) [Exponent]
 Exponent		= ( ("E"|"e") ("+"|"-") DIGIT )

 */

static char *
xps_parse_digits(char *s, int *digit)
{
	*digit = 0;
	while (*s >= '0' && *s <= '9')
	{
		*digit = *digit * 10 + (*s - '0');
		s ++;
	}
	return s;
}

static inline int is_real_num_char(int c)
{
	return (c >= '0' && c <= '9') || c == 'e' || c == 'E' || c == '+' || c == '-' || c == '.';
}

static char *
xps_parse_real_num(char *s, float *number)
{
	char buf[64];
	char *p = buf;
	while (is_real_num_char(*s))
		*p++ = *s++;
	*p = 0;
	if (buf[0])
		*number = atof(buf);
	return s;
}

static char *
xps_parse_cluster_mapping(char *s, int *code_count, int *glyph_count)
{
	if (*s == '(')
		s = xps_parse_digits(s + 1, code_count);
	if (*s == ':')
		s = xps_parse_digits(s + 1, glyph_count);
	if (*s == ')')
		s ++;
	return s;
}

static char *
xps_parse_glyph_index(char *s, int *glyph_index)
{
	if (*s >= '0' && *s <= '9')
		s = xps_parse_digits(s, glyph_index);
	return s;
}

static char *
xps_parse_glyph_metrics(char *s, float *advance, float *uofs, float *vofs)
{
	if (*s == ',')
		s = xps_parse_real_num(s + 1, advance);
	if (*s == ',')
		s = xps_parse_real_num(s + 1, uofs);
	if (*s == ',')
		s = xps_parse_real_num(s + 1, vofs);
	return s;
}

/*
 * Parse unicode and indices strings and encode glyphs.
 * Calculate metrics for positioning.
 */
static void
xps_parse_glyphs_imp(xps_context *ctx, fz_matrix ctm, fz_font *font, float size,
		float originx, float originy, int is_sideways, int bidi_level,
		char *indices, char *unicode, int is_charpath)
{
	xps_glyph_metrics mtx;
	fz_matrix tm;
	float e, f;
	float x = originx;
	float y = originy;
	char *us = unicode;
	char *is = indices;
	int un = 0;

	if (!unicode && !indices)
	{
		fz_warn("glyphs element with neither characters nor indices");
		return;
	}

	if (us)
	{
		if (us[0] == '{' && us[1] == '}')
			us = us + 2;
		un = strlen(us);
	}

	if (is_sideways)
		tm = fz_concat(fz_scale(-size, size), fz_rotate(90));
	else
		tm = fz_scale(size, -size);

	ctx->text = fz_newtext(font, tm, is_sideways);

	while ((us && un > 0) || (is && *is))
	{
		int char_code = '?';
		int code_count = 1;
		int glyph_count = 1;

		if (is && *is)
		{
			is = xps_parse_cluster_mapping(is, &code_count, &glyph_count);
		}

		if (code_count < 1)
			code_count = 1;
		if (glyph_count < 1)
			glyph_count = 1;

		/* TODO: add code chars with cluster mappings for proper text extraction */

		while (code_count--)
		{
			if (us && un > 0)
			{
				int t = xps_utf8_to_ucs(&char_code, us, un);
				us += t; un -= t;
			}
		}

		while (glyph_count--)
		{
			int glyph_index = -1;
			float u_offset = 0.0;
			float v_offset = 0.0;
			float advance;

			if (is && *is)
				is = xps_parse_glyph_index(is, &glyph_index);

			if (glyph_index == -1)
				glyph_index = xps_encode_font_char(font, char_code);

			xps_measure_font_glyph(ctx, font, glyph_index, &mtx);
			if (is_sideways)
				advance = mtx.vadv * 100.0;
			else if (bidi_level & 1)
				advance = -mtx.hadv * 100.0;
			else
				advance = mtx.hadv * 100.0;

			if (is && *is)
			{
				is = xps_parse_glyph_metrics(is, &advance, &u_offset, &v_offset);
				if (*is == ';')
					is ++;
			}

			if (bidi_level & 1)
				u_offset = -mtx.hadv * 100 - u_offset;

			u_offset = u_offset * 0.01 * size;
			v_offset = v_offset * 0.01 * size;

			if (is_sideways)
			{
				e = x + u_offset + (mtx.vorg * size);
				f = y - v_offset + (mtx.hadv * 0.5 * size);
			}
			else
			{
				e = x + u_offset;
				f = y - v_offset;
			}

			fz_addtext(ctx->text, glyph_index, char_code, e, f);

			x += advance * 0.01 * size;
		}
	}
}

void
xps_parse_glyphs(xps_context *ctx, fz_matrix ctm,
		char *base_uri, xps_resource *dict, xps_item *root)
{
	xps_item *node;
	int code;

	char *fill_uri;
	char *opacity_mask_uri;

	char *bidi_level_att;
	char *caret_stops_att;
	char *fill_att;
	char *font_size_att;
	char *font_uri_att;
	char *origin_x_att;
	char *origin_y_att;
	char *is_sideways_att;
	char *indices_att;
	char *unicode_att;
	char *style_att;
	char *transform_att;
	char *clip_att;
	char *opacity_att;
	char *opacity_mask_att;

	xps_item *transform_tag = NULL;
	xps_item *clip_tag = NULL;
	xps_item *fill_tag = NULL;
	xps_item *opacity_mask_tag = NULL;

	char *fill_opacity_att = NULL;

	xps_part *part;
	fz_font *font;

	char partname[1024];
	char *subfont;

	float font_size = 10.0;
	int subfontid = 0;
	int is_sideways = 0;
	int bidi_level = 0;

	/*
	 * Extract attributes and extended attributes.
	 */

	bidi_level_att = xps_att(root, "BidiLevel");
	caret_stops_att = xps_att(root, "CaretStops");
	fill_att = xps_att(root, "Fill");
	font_size_att = xps_att(root, "FontRenderingEmSize");
	font_uri_att = xps_att(root, "FontUri");
	origin_x_att = xps_att(root, "OriginX");
	origin_y_att = xps_att(root, "OriginY");
	is_sideways_att = xps_att(root, "IsSideways");
	indices_att = xps_att(root, "Indices");
	unicode_att = xps_att(root, "UnicodeString");
	style_att = xps_att(root, "StyleSimulations");
	transform_att = xps_att(root, "RenderTransform");
	clip_att = xps_att(root, "Clip");
	opacity_att = xps_att(root, "Opacity");
	opacity_mask_att = xps_att(root, "OpacityMask");

	for (node = xps_down(root); node; node = xps_next(node))
	{
		if (!strcmp(xps_tag(node), "Glyphs.RenderTransform"))
			transform_tag = xps_down(node);
		if (!strcmp(xps_tag(node), "Glyphs.OpacityMask"))
			opacity_mask_tag = xps_down(node);
		if (!strcmp(xps_tag(node), "Glyphs.Clip"))
			clip_tag = xps_down(node);
		if (!strcmp(xps_tag(node), "Glyphs.Fill"))
			fill_tag = xps_down(node);
	}

	fill_uri = base_uri;
	opacity_mask_uri = base_uri;

	xps_resolve_resource_reference(ctx, dict, &transform_att, &transform_tag, NULL);
	xps_resolve_resource_reference(ctx, dict, &clip_att, &clip_tag, NULL);
	xps_resolve_resource_reference(ctx, dict, &fill_att, &fill_tag, &fill_uri);
	xps_resolve_resource_reference(ctx, dict, &opacity_mask_att, &opacity_mask_tag, &opacity_mask_uri);

	/*
	 * Check that we have all the necessary information.
	 */

	if (!font_size_att || !font_uri_att || !origin_x_att || !origin_y_att) {
		fz_warn("missing attributes in glyphs element");
		return;
	}

	if (!indices_att && !unicode_att)
		return; /* nothing to draw */

	if (is_sideways_att)
		is_sideways = !strcmp(is_sideways_att, "true");

	if (bidi_level_att)
		bidi_level = atoi(bidi_level_att);

	/*
	 * Find and load the font resource
	 */

	xps_absolute_path(partname, base_uri, font_uri_att, sizeof partname);
	subfont = strrchr(partname, '#');
	if (subfont)
	{
		subfontid = atoi(subfont + 1);
		*subfont = 0;
	}

	font = xps_hash_lookup(ctx->font_table, partname);
	if (!font)
	{
		part = xps_read_part(ctx, partname);
		if (!part) {
			fz_warn("cannot find font resource part '%s'", partname);
			return;
		}

		/* deobfuscate if necessary */
		if (strstr(part->name, ".odttf"))
			xps_deobfuscate_font_resource(ctx, part);
		if (strstr(part->name, ".ODTTF"))
			xps_deobfuscate_font_resource(ctx, part);

		code = fz_newfontfrombuffer(&font, part->data, part->size, subfontid);
		if (code) {
			fz_catch(code, "cannot load font resource '%s'", partname);
			xps_free_part(ctx, part);
			return;
		}

		xps_select_best_font_encoding(font);

		xps_hash_insert(ctx, ctx->font_table, part->name, font);

		/* NOTE: we kept part->name in the hashtable and part->data in the font */
		xps_free(ctx, part);
	}

	/*
	 * Set up graphics state.
	 */

	if (transform_att || transform_tag)
	{
		fz_matrix transform;
		if (transform_att)
			xps_parse_render_transform(ctx, transform_att, &transform);
		if (transform_tag)
			xps_parse_matrix_transform(ctx, transform_tag, &transform);
		ctm = fz_concat(transform, ctm);
	}

	if (clip_att || clip_tag)
	{
		ctx->path = fz_newpath();
		if (clip_att)
			xps_parse_abbreviated_geometry(ctx, clip_att);
		if (clip_tag)
			xps_parse_path_geometry(ctx, dict, clip_tag, 0);
		xps_clip(ctx, ctm);
	}

	font_size = atof(font_size_att);

	xps_begin_opacity(ctx, ctm, opacity_mask_uri, dict, opacity_att, opacity_mask_tag);

	/*
	 * If it's a solid color brush fill/stroke do a simple fill
	 */

	if (fill_tag && !strcmp(xps_tag(fill_tag), "SolidColorBrush"))
	{
		fill_opacity_att = xps_att(fill_tag, "Opacity");
		fill_att = xps_att(fill_tag, "Color");
		fill_tag = NULL;
	}

	if (fill_att)
	{
		float samples[32];
		fz_colorspace *colorspace;

		xps_parse_color(ctx, base_uri, fill_att, &colorspace, samples);
		if (fill_opacity_att)
			samples[0] = atof(fill_opacity_att);
		xps_set_color(ctx, colorspace, samples);

		xps_parse_glyphs_imp(ctx, ctm, font, font_size,
				atof(origin_x_att), atof(origin_y_att),
				is_sideways, bidi_level,
				indices_att, unicode_att, 0);

		ctx->dev->filltext(ctx->dev->user, ctx->text, ctm,
			ctx->colorspace, ctx->color, ctx->alpha);
		fz_freetext(ctx->text);
		ctx->text = nil;
	}

	/*
	 * If it's a visual brush or image, use the charpath as a clip mask to paint brush
	 */

	if (fill_tag)
	{
		xps_parse_glyphs_imp(ctx, ctm, font, font_size,
				atof(origin_x_att), atof(origin_y_att),
				is_sideways, bidi_level, indices_att, unicode_att, 1);
		xps_parse_brush(ctx, ctm, fill_uri, dict, fill_tag);
	}

	xps_end_opacity(ctx, opacity_mask_uri, dict, opacity_att, opacity_mask_tag);

	if (clip_att || clip_tag)
		ctx->dev->popclip(ctx->dev->user);
}
