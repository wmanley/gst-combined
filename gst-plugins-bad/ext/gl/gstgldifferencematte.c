/*
 * GStreamer
 * Copyright (C) 2008 Filippo Argiolas <filippo.argiolas@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-gldifferencematte.
 *
 * Saves a background frame and replace it with a pixbuf.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 videotestsrc ! glupload ! gldifferencemate location=backgroundimagefile ! glimagesink
 * ]|
 * FBO (Frame Buffer Object) and GLSL (OpenGL Shading Language) are required.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <png.h>

#include "gstgldifferencematte.h"
#include "effects/gstgleffectssources.h"

#if PNG_LIBPNG_VER >= 10400
#define int_p_NULL         NULL
#define png_infopp_NULL    NULL
#endif

#define GST_CAT_DEFAULT gst_gl_differencematte_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_gl_differencematte_debug, "gldifferencematte", 0, "gldifferencematte element");

G_DEFINE_TYPE_WITH_CODE (GstGLDifferenceMatte, gst_gl_differencematte,
    GST_TYPE_GL_FILTER, DEBUG_INIT);

static void gst_gl_differencematte_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_gl_differencematte_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

static gboolean gst_gl_differencematte_filter_texture (GstGLFilter * filter,
    guint in_tex, guint out_tex);

static gboolean gst_gl_differencematte_loader (GstGLFilter * filter);

enum
{
  PROP_0,
  PROP_LOCATION,
};


/* init resources that need a gl context */
static void
gst_gl_differencematte_init_gl_resources (GstGLFilter * filter)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (filter);
  GstGLContext *context = GST_GL_BASE_FILTER (filter)->context;
  const GstGLFuncs *gl = context->gl_vtable;
  GError *error = NULL;
  gint i;

  for (i = 0; i < 4; i++) {
    gl->GenTextures (1, &differencematte->midtexture[i]);
    gl->BindTexture (GL_TEXTURE_2D, differencematte->midtexture[i]);
    gl->TexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
        GST_VIDEO_INFO_WIDTH (&filter->out_info),
        GST_VIDEO_INFO_HEIGHT (&filter->out_info),
        0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  if (!(differencematte->identity_shader =
          gst_gl_shader_new_default (context, &error))) {
    GST_ELEMENT_ERROR (differencematte, RESOURCE, NOT_FOUND, ("%s",
            "Failed to compile identity shader"), ("%s", error->message));
    return;
  }

  if (!(differencematte->shader[0] =
          gst_gl_shader_new_link_with_stages (context, &error,
              gst_glsl_stage_new_default_vertex (context),
              gst_glsl_stage_new_with_string (context, GL_FRAGMENT_SHADER,
                  GST_GLSL_VERSION_NONE,
                  GST_GLSL_PROFILE_ES | GST_GLSL_PROFILE_COMPATIBILITY,
                  difference_fragment_source), NULL))) {
    GST_ELEMENT_ERROR (differencematte, RESOURCE, NOT_FOUND, ("%s",
            "Failed to compile difference shader"), ("%s", error->message));
    return;
  }

  if (!(differencematte->shader[1] =
          gst_gl_shader_new_link_with_stages (context, &error,
              gst_glsl_stage_new_default_vertex (context),
              gst_glsl_stage_new_with_string (context, GL_FRAGMENT_SHADER,
                  GST_GLSL_VERSION_NONE,
                  GST_GLSL_PROFILE_ES | GST_GLSL_PROFILE_COMPATIBILITY,
                  hconv7_fragment_source_gles2), NULL))) {
    GST_ELEMENT_ERROR (differencematte, RESOURCE, NOT_FOUND, ("%s",
            "Failed to compile convolution shader"), ("%s", error->message));
    return;
  }

  if (!(differencematte->shader[2] =
          gst_gl_shader_new_link_with_stages (context, &error,
              gst_glsl_stage_new_default_vertex (context),
              gst_glsl_stage_new_with_string (context, GL_FRAGMENT_SHADER,
                  GST_GLSL_VERSION_NONE,
                  GST_GLSL_PROFILE_ES | GST_GLSL_PROFILE_COMPATIBILITY,
                  vconv7_fragment_source_gles2), NULL))) {
    GST_ELEMENT_ERROR (differencematte, RESOURCE, NOT_FOUND, ("%s",
            "Failed to compile convolution shader"), ("%s", error->message));
    return;
  }

  if (!(differencematte->shader[3] =
          gst_gl_shader_new_link_with_stages (context, &error,
              gst_glsl_stage_new_default_vertex (context),
              gst_glsl_stage_new_with_string (context, GL_FRAGMENT_SHADER,
                  GST_GLSL_VERSION_NONE,
                  GST_GLSL_PROFILE_ES | GST_GLSL_PROFILE_COMPATIBILITY,
                  texture_interp_fragment_source), NULL))) {
    GST_ELEMENT_ERROR (differencematte, RESOURCE, NOT_FOUND, ("%s",
            "Failed to compile interpolation shader"), ("%s", error->message));
    return;
  }

  /* FIXME: this should really be per shader */
  filter->draw_attr_position_loc =
      gst_gl_shader_get_attribute_location (differencematte->shader[2],
      "a_position");
  filter->draw_attr_texture_loc =
      gst_gl_shader_get_attribute_location (differencematte->shader[2],
      "a_texcoord");
}

/* free resources that need a gl context */
static void
gst_gl_differencematte_reset_gl_resources (GstGLFilter * filter)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (filter);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;
  gint i;

  gl->DeleteTextures (1, &differencematte->savedbgtexture);
  gl->DeleteTextures (1, &differencematte->newbgtexture);
  for (i = 0; i < 4; i++) {
    if (differencematte->identity_shader) {
      gst_object_unref (differencematte->identity_shader);
      differencematte->identity_shader = NULL;
    }

    if (differencematte->shader[i]) {
      gst_object_unref (differencematte->shader[i]);
      differencematte->shader[i] = NULL;
    }
    if (differencematte->midtexture[i]) {
      gl->DeleteTextures (1, &differencematte->midtexture[i]);
      differencematte->midtexture[i] = 0;
    }
  }
  differencematte->location = NULL;
  differencematte->pixbuf = NULL;
  differencematte->savedbgtexture = 0;
  differencematte->newbgtexture = 0;
  differencematte->bg_has_changed = FALSE;
}

static void
gst_gl_differencematte_class_init (GstGLDifferenceMatteClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);
  gobject_class->set_property = gst_gl_differencematte_set_property;
  gobject_class->get_property = gst_gl_differencematte_get_property;

  GST_GL_FILTER_CLASS (klass)->filter_texture =
      gst_gl_differencematte_filter_texture;
  GST_GL_FILTER_CLASS (klass)->display_init_cb =
      gst_gl_differencematte_init_gl_resources;
  GST_GL_FILTER_CLASS (klass)->display_reset_cb =
      gst_gl_differencematte_reset_gl_resources;

  g_object_class_install_property (gobject_class,
      PROP_LOCATION,
      g_param_spec_string ("location",
          "Background image location",
          "Background image location", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_metadata (element_class,
      "Gstreamer OpenGL DifferenceMatte", "Filter/Effect/Video",
      "Saves a background frame and replace it with a pixbuf",
      "Filippo Argiolas <filippo.argiolas@gmail.com>");

  GST_GL_BASE_FILTER_CLASS (klass)->supported_gl_api =
      GST_GL_API_OPENGL | GST_GL_API_OPENGL3 | GST_GL_API_GLES2;
}

static void
gst_gl_differencematte_init (GstGLDifferenceMatte * differencematte)
{
  differencematte->shader[0] = NULL;
  differencematte->shader[1] = NULL;
  differencematte->shader[2] = NULL;
  differencematte->shader[3] = NULL;
  differencematte->location = NULL;
  differencematte->pixbuf = NULL;
  differencematte->savedbgtexture = 0;
  differencematte->newbgtexture = 0;
  differencematte->bg_has_changed = FALSE;

  fill_gaussian_kernel (differencematte->kernel, 7, 30.0);
}

static void
gst_gl_differencematte_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_free (differencematte->location);
      differencematte->bg_has_changed = TRUE;
      differencematte->location = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_differencematte_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, differencematte->location);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
init_pixbuf_texture (GstGLContext * context, gpointer data)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (data);
  GstGLFilter *filter = GST_GL_FILTER (data);
  const GstGLFuncs *gl = context->gl_vtable;
  guint internal_format =
      gst_gl_sized_gl_format_from_gl_format_type (context, GL_RGBA,
      GL_UNSIGNED_BYTE);

  gl->DeleteTextures (1, &differencematte->newbgtexture);
  gl->GenTextures (1, &differencematte->newbgtexture);
  gl->BindTexture (GL_TEXTURE_2D, differencematte->newbgtexture);
  gl->TexImage2D (GL_TEXTURE_2D, 0, internal_format,
      (gint) differencematte->pbuf_width, (gint) differencematte->pbuf_height,
      0, GL_RGBA, GL_UNSIGNED_BYTE, differencematte->pixbuf);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (differencematte->savedbgtexture == 0) {
    gl->GenTextures (1, &differencematte->savedbgtexture);
    gl->BindTexture (GL_TEXTURE_2D, differencematte->savedbgtexture);
    gl->TexImage2D (GL_TEXTURE_2D, 0, internal_format,
        GST_VIDEO_INFO_WIDTH (&filter->out_info),
        GST_VIDEO_INFO_HEIGHT (&filter->out_info),
        0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
}

static void
gst_gl_differencematte_diff (gint width, gint height, guint texture,
    gpointer stuff)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (stuff);
  GstGLFilter *filter = GST_GL_FILTER (stuff);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;

  gst_gl_shader_use (differencematte->shader[0]);

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, texture);

  gst_gl_shader_set_uniform_1i (differencematte->shader[0], "current", 0);

  gl->ActiveTexture (GL_TEXTURE1);
  gl->BindTexture (GL_TEXTURE_2D, differencematte->savedbgtexture);

  gst_gl_shader_set_uniform_1i (differencematte->shader[0], "saved", 1);

  gst_gl_filter_draw_texture (filter, texture, width, height);
}

static void
gst_gl_differencematte_hblur (gint width, gint height, guint texture,
    gpointer stuff)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (stuff);
  GstGLFilter *filter = GST_GL_FILTER (stuff);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;

  gst_gl_shader_use (differencematte->shader[1]);

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, texture);

  gst_gl_shader_set_uniform_1i (differencematte->shader[1], "tex", 0);

  gst_gl_shader_set_uniform_1fv (differencematte->shader[1], "kernel", 7,
      differencematte->kernel);
  gst_gl_shader_set_uniform_1f (differencematte->shader[1], "gauss_width",
      width);

  gst_gl_filter_draw_texture (filter, texture, width, height);
}

static void
gst_gl_differencematte_vblur (gint width, gint height, guint texture,
    gpointer stuff)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (stuff);
  GstGLFilter *filter = GST_GL_FILTER (stuff);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;

  gst_gl_shader_use (differencematte->shader[2]);

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, texture);

  gst_gl_shader_set_uniform_1i (differencematte->shader[2], "tex", 0);

  gst_gl_shader_set_uniform_1fv (differencematte->shader[2], "kernel", 7,
      differencematte->kernel);
  gst_gl_shader_set_uniform_1f (differencematte->shader[2], "gauss_height",
      height);

  gst_gl_filter_draw_texture (filter, texture, width, height);
}

static void
gst_gl_differencematte_interp (gint width, gint height, guint texture,
    gpointer stuff)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (stuff);
  GstGLFilter *filter = GST_GL_FILTER (stuff);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;

  gst_gl_shader_use (differencematte->shader[3]);

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, texture);

  gst_gl_shader_set_uniform_1i (differencematte->shader[3], "blend", 0);

  gl->ActiveTexture (GL_TEXTURE1);
  gl->BindTexture (GL_TEXTURE_2D, differencematte->newbgtexture);

  gst_gl_shader_set_uniform_1i (differencematte->shader[3], "base", 1);

  gl->ActiveTexture (GL_TEXTURE2);
  gl->BindTexture (GL_TEXTURE_2D, differencematte->midtexture[2]);

  gst_gl_shader_set_uniform_1i (differencematte->shader[3], "alpha", 2);

  gst_gl_filter_draw_texture (filter, texture, width, height);
}

static void
gst_gl_differencematte_identity (gint width, gint height, guint texture,
    gpointer stuff)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (stuff);
  GstGLFilter *filter = GST_GL_FILTER (differencematte);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;

  gst_gl_shader_use (differencematte->identity_shader);

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, texture);

  gst_gl_shader_set_uniform_1i (differencematte->identity_shader, "tex", 0);

  gst_gl_filter_draw_texture (filter, texture, width, height);
}

static gboolean
gst_gl_differencematte_filter_texture (GstGLFilter * filter, guint in_tex,
    guint out_tex)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (filter);

  differencematte->intexture = in_tex;

  if (differencematte->bg_has_changed && (differencematte->location != NULL)) {

    if (!gst_gl_differencematte_loader (filter))
      differencematte->pixbuf = NULL;

    /* if loader failed then context is turned off */
    gst_gl_context_thread_add (GST_GL_BASE_FILTER (filter)->context,
        init_pixbuf_texture, differencematte);

    /* save current frame, needed to calculate difference between
     * this frame and next ones */
    gst_gl_filter_render_to_target (filter, TRUE, in_tex,
        differencematte->savedbgtexture,
        gst_gl_differencematte_identity, differencematte);

    if (differencematte->pixbuf) {
      free (differencematte->pixbuf);
      differencematte->pixbuf = NULL;
    }

    differencematte->bg_has_changed = FALSE;
  }

  if (differencematte->savedbgtexture != 0) {
    gst_gl_filter_render_to_target (filter, TRUE, in_tex,
        differencematte->midtexture[0], gst_gl_differencematte_diff,
        differencematte);
    gst_gl_filter_render_to_target (filter, FALSE,
        differencematte->midtexture[0], differencematte->midtexture[1],
        gst_gl_differencematte_hblur, differencematte);
    gst_gl_filter_render_to_target (filter, FALSE,
        differencematte->midtexture[1], differencematte->midtexture[2],
        gst_gl_differencematte_vblur, differencematte);
    gst_gl_filter_render_to_target (filter, TRUE, in_tex, out_tex,
        gst_gl_differencematte_interp, differencematte);
  } else {
    gst_gl_filter_render_to_target (filter, TRUE, in_tex, out_tex,
        gst_gl_differencematte_identity, differencematte);
  }

  return TRUE;
}

static void
user_warning_fn (png_structp png_ptr, png_const_charp warning_msg)
{
  g_warning ("%s\n", warning_msg);
}

#define LOAD_ERROR(msg) { GST_WARNING ("unable to load %s: %s", differencematte->location, msg); return FALSE; }

static gboolean
gst_gl_differencematte_loader (GstGLFilter * filter)
{
  GstGLDifferenceMatte *differencematte = GST_GL_DIFFERENCEMATTE (filter);

  png_structp png_ptr;
  png_infop info_ptr;
  guint sig_read = 0;
  png_uint_32 width = 0;
  png_uint_32 height = 0;
  gint bit_depth = 0;
  gint color_type = 0;
  gint interlace_type = 0;
  png_FILE_p fp = NULL;
  guint y = 0;
  guchar **rows = NULL;
  gint filler;

  if (!GST_GL_BASE_FILTER (filter)->context)
    return TRUE;

  if ((fp = fopen (differencematte->location, "rb")) == NULL)
    LOAD_ERROR ("file not found");

  png_ptr = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if (png_ptr == NULL) {
    fclose (fp);
    LOAD_ERROR ("failed to initialize the png_struct");
  }

  png_set_error_fn (png_ptr, NULL, NULL, user_warning_fn);

  info_ptr = png_create_info_struct (png_ptr);
  if (info_ptr == NULL) {
    fclose (fp);
    png_destroy_read_struct (&png_ptr, png_infopp_NULL, png_infopp_NULL);
    LOAD_ERROR ("failed to initialize the memory for image information");
  }

  png_init_io (png_ptr, fp);

  png_set_sig_bytes (png_ptr, sig_read);

  png_read_info (png_ptr, info_ptr);

  png_get_IHDR (png_ptr, info_ptr, &width, &height, &bit_depth, &color_type,
      &interlace_type, int_p_NULL, int_p_NULL);

  if (color_type == PNG_COLOR_TYPE_RGB) {
    filler = 0xff;
    png_set_filler (png_ptr, filler, PNG_FILLER_AFTER);
    color_type = PNG_COLOR_TYPE_RGB_ALPHA;
  }

  if (color_type != PNG_COLOR_TYPE_RGB_ALPHA) {
    fclose (fp);
    png_destroy_read_struct (&png_ptr, png_infopp_NULL, png_infopp_NULL);
    LOAD_ERROR ("color type is not rgb");
  }

  differencematte->pbuf_width = width;
  differencematte->pbuf_height = height;

  differencematte->pixbuf =
      (guchar *) malloc (sizeof (guchar) * width * height * 4);

  rows = (guchar **) malloc (sizeof (guchar *) * height);

  for (y = 0; y < height; ++y)
    rows[y] = (guchar *) (differencematte->pixbuf + y * width * 4);

  png_read_image (png_ptr, rows);

  free (rows);

  png_read_end (png_ptr, info_ptr);
  png_destroy_read_struct (&png_ptr, &info_ptr, png_infopp_NULL);
  fclose (fp);

  return TRUE;
}