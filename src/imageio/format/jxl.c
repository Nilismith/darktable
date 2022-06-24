/*
    This file is part of darktable,
    Copyright (C) 2021-2022 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "imageio/format/imageio_format_api.h"

#include "jxl/encode.h"
#include "jxl/resizable_parallel_runner.h"

DT_MODULE(1)

typedef struct dt_imageio_jxl_t
{
  dt_imageio_module_data_t global;
  int bpp;
  int quality;
  int original;
  int effort;
  int tier;
} dt_imageio_jxl_t;

typedef struct dt_imageio_jxl_gui_data_t
{
  // Int (0:8b, 1:10b, 2:12b, 3:16b, 4:half, 5:float)
  GtkWidget *bpp;
  // Int (0-100): the quality of the image, roughly corresponding to JPEG quality (100 is lossless)
  GtkWidget *quality;
  // Bool: whether to encode using the original color profile or the internal XYB one
  GtkWidget *original;
  // Int (1-9): effort with which to encode output; higher is slower (default is 7)
  GtkWidget *effort;
  // Int (0-4): higher value favors decoding speed vs quality (default is 0)
  GtkWidget *tier;
} dt_imageio_jxl_gui_data_t;


void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_t, bpp, int);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_t, quality, int);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_t, original, int);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_t, effort, int);

  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_jxl_t, tier, int);
#endif
}

void cleanup(dt_imageio_module_format_t *self)
{
}


const char *mime(dt_imageio_module_data_t *data)
{
  return "image/jxl";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "jxl";
}

int dimension(struct dt_imageio_module_format_t *self, struct dt_imageio_module_data_t *data, uint32_t *width,
              uint32_t *height)
{
  // The maximum dimensions supported by jxl images
  *width = 1073741823U;
  *height = 1073741823U;
  return 1;
}

int bpp(dt_imageio_module_data_t *data)
{
  return 32; /* always request float */
}


int write_image(struct dt_imageio_module_data_t *data, const char *filename, const void *in_tmp,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename, void *exif, int exif_len,
                int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  // Return error code by default
  int ret = 1;

  float *pixels = NULL;
  uint8_t *out_buf = NULL;
  FILE *out_file = NULL;
  uint8_t *icc_buf = NULL;
  uint8_t *exif_buf = NULL;
  char *xmp_string = NULL;

#define LIBJXL_ASSERT(code)                                                                                       \
  {                                                                                                               \
    if((JxlEncoderStatus)code != JXL_ENC_SUCCESS)                                                                 \
    {                                                                                                             \
      JxlEncoderError err = JxlEncoderGetError(encoder);                                                          \
      dt_print(DT_DEBUG_IMAGEIO, "[jxl] libjxl call failed with err %d (src/imageio/format/jxl.c#L%d)\n", err,    \
               __LINE__);                                                                                         \
      goto end;                                                                                                   \
    }                                                                                                             \
  }

#define JXL_FAIL(msg, ...)                                                                                        \
  {                                                                                                               \
    dt_print(DT_DEBUG_IMAGEIO, "[jxl] " msg "\n", ##__VA_ARGS__);                                                 \
    goto end;                                                                                                     \
  }

  const dt_imageio_jxl_t *params = (dt_imageio_jxl_t *)data;
  const uint32_t width = (uint32_t)params->global.width;
  const uint32_t height = (uint32_t)params->global.height;

  JxlEncoder *encoder = JxlEncoderCreate(NULL);

  const uint32_t num_threads = JxlResizableParallelRunnerSuggestThreads(width, height);
  void *runner = JxlResizableParallelRunnerCreate(NULL);
  if(!runner) JXL_FAIL("could not create resizable parallel runner");
  JxlResizableParallelRunnerSetThreads(runner, num_threads);
  LIBJXL_ASSERT(JxlEncoderSetParallelRunner(encoder, JxlResizableParallelRunner, runner));

  // Automatically freed when we destroy the encoder
  JxlEncoderFrameSettings *frame_settings = JxlEncoderFrameSettingsCreate(encoder, NULL);

  // Set encoder basic info
  JxlBasicInfo basic_info;
  JxlEncoderInitBasicInfo(&basic_info);
  basic_info.xsize = width;
  basic_info.ysize = height;
  switch(params->bpp)
  {
    case 0:
      basic_info.bits_per_sample = 8;
      basic_info.exponent_bits_per_sample = 0;
      break;
    case 1:
      basic_info.bits_per_sample = 10;
      basic_info.exponent_bits_per_sample = 0;
      break;
    case 2:
      basic_info.bits_per_sample = 12;
      basic_info.exponent_bits_per_sample = 0;
      break;
    case 3:
      basic_info.bits_per_sample = 16;
      basic_info.exponent_bits_per_sample = 0;
      break;
    case 4:
      basic_info.bits_per_sample = 16;
      basic_info.exponent_bits_per_sample = 5;
      break;
    default:
      basic_info.bits_per_sample = 32;
      basic_info.exponent_bits_per_sample = 8;
      break;
  }
  // Lossless only makes sense for integer modes
  if(basic_info.exponent_bits_per_sample == 0 && params->quality == 100)
  {
    // Must preserve original profile for lossless mode
    basic_info.uses_original_profile = JXL_TRUE;
    LIBJXL_ASSERT(JxlEncoderSetFrameDistance(frame_settings, 0.0f));
    LIBJXL_ASSERT(JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE));
  }
  else
  {
    basic_info.uses_original_profile = params->original == FALSE ? JXL_FALSE : JXL_TRUE;
    float distance = params->quality >= 30 ? 0.1f + (100 - params->quality) * 0.09f
                                           : 6.4f + powf(2.5f, (30 - params->quality) / 5.0f) / 6.25f;
    LIBJXL_ASSERT(JxlEncoderSetFrameDistance(frame_settings, distance));
  }

  LIBJXL_ASSERT(JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT, params->effort));

  LIBJXL_ASSERT(
      JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_DECODING_SPEED, params->tier));

  // Try the settings with default codestream level 5 first, upgrade otherwise
  if(JXL_ENC_ERROR == JxlEncoderSetBasicInfo(encoder, &basic_info))
  {
    LIBJXL_ASSERT(JxlEncoderSetCodestreamLevel(encoder, 10));
    LIBJXL_ASSERT(JxlEncoderSetBasicInfo(encoder, &basic_info));
  }

  // Determine and set the encoder color space
  const dt_colorspaces_color_profile_t *output_profile
      = dt_colorspaces_get_output_profile(imgid, over_type, over_filename);
  const cmsHPROFILE out_profile = output_profile->profile;
  // Previous call will give us a more accurate color profile type
  // (not what the user requested in the export menu but what the image is actually using)
  over_type = output_profile->type;

  // If possible we want libjxl to save the color encoding in its own format, rather
  // than as an ICC binary blob which is possible.
  // If we are unable to find the required color encoding data for libjxl we will
  // just fallback to providing an ICC blob (and hope we can at least do that!).
  bool write_color_natively = true;

  JxlColorEncoding color_encoding;
  color_encoding.color_space = JXL_COLOR_SPACE_RGB;
  // If not explicitly set in the export menu, use the intent of the actual output profile
  if(pipe->icc_intent >= DT_INTENT_PERCEPTUAL && pipe->icc_intent < DT_INTENT_LAST)
    color_encoding.rendering_intent = (JxlRenderingIntent)pipe->icc_intent;
  else
    color_encoding.rendering_intent = (JxlRenderingIntent)cmsGetHeaderRenderingIntent(out_profile);

  // Attempt to find and set the known white point, primaries and transfer function.
  // If we can't find any of these we fall back to an ICC binary blob.
  switch(over_type)
  {
    case DT_COLORSPACE_SRGB:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_SRGB;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
      break;
    case DT_COLORSPACE_LIN_REC709:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_SRGB;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
      break;
    case DT_COLORSPACE_LIN_REC2020:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_2100;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
      break;
    // TODO: enable when JXL_PRIMARIES_XYZ are added to libjxl
    //  case DT_COLORSPACE_XYZ:
    //    color_encoding.white_point = JXL_WHITE_POINT_E;
    //    color_encoding.primaries = JXL_PRIMARIES_XYZ;
    //    color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
    //    break;
    case DT_COLORSPACE_REC709:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_SRGB;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_709;
      break;
    case DT_COLORSPACE_PQ_REC2020:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_2100;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
      break;
    case DT_COLORSPACE_HLG_REC2020:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_2100;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
      break;
    case DT_COLORSPACE_PQ_P3:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_P3;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
      break;
    case DT_COLORSPACE_HLG_P3:
      color_encoding.white_point = JXL_WHITE_POINT_D65;
      color_encoding.primaries = JXL_PRIMARIES_P3;
      color_encoding.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
      break;
    default:
      write_color_natively = false;
      break;
  }

  if(write_color_natively)
  {
    JxlEncoderSetColorEncoding(encoder, &color_encoding);
  }
  else
  {
    // If we didn't manage to write the color encoding natively we need to fallback to ICC
    dt_print(DT_DEBUG_IMAGEIO, "[jxl] could not generate color encoding structure, falling back to ICC\n");

    cmsUInt32Number icc_size = 0;
    // First find the size of the ICC buffer
    if(!cmsSaveProfileToMem(out_profile, NULL, &icc_size)) JXL_FAIL("error finding ICC data length");
    if(icc_size > 0) icc_buf = g_malloc(icc_size);
    if(!icc_buf) JXL_FAIL("could not allocate ICC buffer of size %u", icc_size);

    // Fill the ICC buffer
    if(!cmsSaveProfileToMem(out_profile, icc_buf, &icc_size)) JXL_FAIL("error writing ICC data");

    LIBJXL_ASSERT(JxlEncoderSetICCProfile(encoder, icc_buf, icc_size));
  }

  // We assume that the user wants the JXL image in a BMFF container.
  // JXL images can be stored without any container so they are smaller, but
  // this removes the possibility of storing extra metadata like Exif and XMP.
  LIBJXL_ASSERT(JxlEncoderUseBoxes(encoder));

  JxlPixelFormat pixel_format = { 3, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0 };

  // Fix pixel stride
  const size_t pixels_size = width * height * 3 * sizeof(float);
  pixels = g_malloc(pixels_size);
  if(!pixels) JXL_FAIL("could not allocate output pixel buffer of size %zu", pixels_size);
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(in_tmp, pixels, width, height) schedule(simd      \
                                                                                                        : static) \
    collapse(2)
#endif
  for(uint32_t y = 0; y < height; ++y)
  {
    for(uint32_t x = 0; x < width; ++x)
    {
      const float *in_pixel = (const float *)in_tmp + 4 * ((y * width) + x);
      float *out_pixel = pixels + 3 * ((y * width) + x);

      out_pixel[0] = in_pixel[0];
      out_pixel[1] = in_pixel[1];
      out_pixel[2] = in_pixel[2];
    }
  }

  LIBJXL_ASSERT(JxlEncoderAddImageFrame(frame_settings, &pixel_format, pixels, pixels_size));

  // Add the Exif data if it exists
  if(exif && exif_len > 6)
  {
    // Prepend the 4 byte (zero) offset to the blob before writing
    // (as required in the equivalent HEIF/JPEG XS Exif box specs).
    // Also skip the 6-byte "Exif\000\000" JPEG APP1 header
    // dt_exif_read_blob() assumes.
    exif_buf = g_malloc0(exif_len - 6 + 4);
    if(!exif_buf) JXL_FAIL("could not allocate Exif buffer of size %zu", (size_t)(exif_len - 6 + 4));
    memmove(exif_buf + 4, exif + 6, exif_len - 6);
    // Exiv2 doesn't support compressed boxes
    LIBJXL_ASSERT(JxlEncoderAddBox(encoder, "Exif", exif_buf, exif_len - 6 + 4, JXL_FALSE));
  }

  /* TODO: workaround; remove when exiv2 implements JXL BMFF write support and update flags() */
  xmp_string = dt_exif_xmp_read_string(imgid);
  size_t xmp_len;
  if(xmp_string && (xmp_len = strlen(xmp_string)) > 0)
  {
    // Exiv2 doesn't support compressed boxes
    LIBJXL_ASSERT(JxlEncoderAddBox(encoder, "xml ", (const uint8_t *)xmp_string, xmp_len, JXL_FALSE));
  }

  // No more image frames nor metadata boxes to add
  JxlEncoderCloseInput(encoder);

  // Write the image codestream to a buffer, starting with a chunk of 64 KiB.
  // TODO: Can we better estimate what the optimal size of chunks is for this image?
  size_t chunk_size = 1 << 16;
  size_t out_len = chunk_size;
  out_buf = g_malloc(out_len);
  if(!out_buf) JXL_FAIL("could not allocate codestream buffer of size %zu", out_len);
  uint8_t *out_cur = out_buf;
  size_t out_avail = out_len;

  JxlEncoderStatus out_status = JXL_ENC_NEED_MORE_OUTPUT;
  while(out_status == JXL_ENC_NEED_MORE_OUTPUT)
  {
    out_status = JxlEncoderProcessOutput(encoder, &out_cur, &out_avail);

    if(out_status == JXL_ENC_NEED_MORE_OUTPUT)
    {
      const size_t offset = out_cur - out_buf;
      if(chunk_size < 1 << 20) chunk_size *= 2;
      out_len += chunk_size;
      out_buf = g_realloc(out_buf, out_len);
      if(!out_buf)
      {
        JXL_FAIL("could not reallocate codestream buffer to size %zu", out_len);
        goto end;
      }
      out_cur = out_buf + offset;
      out_avail = out_len - offset;
    }
  }
  LIBJXL_ASSERT(out_status);
  // Update actual length of codestream written
  out_len = out_cur - out_buf;

  // Write codestream contents to file
  out_file = g_fopen(filename, "wb");
  if(!out_file) JXL_FAIL("could not open output file `%s'", filename);

  if(fwrite(out_buf, sizeof(uint8_t), out_len, out_file) != out_len)
    JXL_FAIL("could not write bytes to `%s'", filename);

  // Finally, successful write: set to success code
  ret = 0;

end:
  if(runner) JxlResizableParallelRunnerDestroy(runner);
  if(encoder) JxlEncoderDestroy(encoder);
  if(out_file) fclose(out_file);
  g_free(pixels);
  g_free(icc_buf);
  g_free(exif_buf);
  g_free(xmp_string);
  g_free(out_buf);

  return ret;
}

int levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

int flags(dt_imageio_module_data_t *data)
{
  /*
   * As of exiv2 0.27.5 there is no write support for the JXL BMFF format,
   * so we do not return the XMP supported flag currently.
   * Once exiv2 write support is there, the flag can be returned, and the
   * direct XMP embedding workaround using JxlEncoderAddBox("xml ") above
   * can be removed.
   */
  return 0; /* FORMAT_FLAGS_SUPPORT_XMP; */
}


size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_jxl_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_jxl_t *d = g_malloc0(sizeof(dt_imageio_jxl_t));

  if(!d) return NULL;

  d->bpp = dt_conf_get_int("plugins/imageio/format/jxl/bpp");

  d->quality = dt_conf_get_int("plugins/imageio/format/jxl/quality");

  d->original = dt_conf_get_bool("plugins/imageio/format/jxl/original") & 1;

  d->effort = dt_conf_get_int("plugins/imageio/format/jxl/effort");

  d->tier = dt_conf_get_int("plugins/imageio/format/jxl/tier");

  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  g_free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;

  const dt_imageio_jxl_t *d = (dt_imageio_jxl_t *)params;
  dt_imageio_jxl_gui_data_t *g = (dt_imageio_jxl_gui_data_t *)self->gui_data;

  int bpp = d->bpp;
  if(bpp < 0)
    bpp = 0;
  else if(bpp > 5)
    bpp = 5;
  dt_bauhaus_combobox_set(g->bpp, bpp);

  int quality = d->quality;
  if(quality < 0)
    quality = 0;
  else if(quality > 100)
    quality = 100;
  dt_bauhaus_slider_set(g->quality, quality);

  int original = d->original;
  dt_bauhaus_combobox_set(g->original, original & 1);

  int effort = d->effort;
  if(effort < 1)
    effort = 1;
  else if(effort > 9)
    effort = 9;
  dt_bauhaus_slider_set(g->effort, effort);

  int tier = d->tier;
  if(tier < 0)
    tier = 0;
  else if(tier > 4)
    tier = 4;
  dt_bauhaus_slider_set(g->tier, tier);

  return 0;
}


const char *name()
{
  return _("JPEG XL");
}

static void bpp_changed(GtkWidget *bpp, dt_imageio_module_format_t *self)
{
  const int bpp_enum = dt_bauhaus_combobox_get(bpp);
  dt_conf_set_int("plugins/imageio/format/jxl/bpp", bpp_enum);

  dt_imageio_jxl_gui_data_t *g = (dt_imageio_jxl_gui_data_t *)self->gui_data;
  const int quality_val = (int)dt_bauhaus_slider_get(g->quality);

  if(bpp_enum < 4 && quality_val == 100)
  {
    dt_bauhaus_combobox_set(g->original, 1);
    gtk_widget_set_sensitive(g->original, FALSE);
  }
  else
    gtk_widget_set_sensitive(g->original, TRUE);
}

static void quality_changed(GtkWidget *quality, dt_imageio_module_format_t *self)
{
  const int quality_val = (int)dt_bauhaus_slider_get(quality);
  dt_conf_set_int("plugins/imageio/format/jxl/quality", quality_val);

  dt_imageio_jxl_gui_data_t *g = (dt_imageio_jxl_gui_data_t *)self->gui_data;
  const int bpp_enum = dt_bauhaus_combobox_get(g->bpp);

  if(bpp_enum < 4 && quality_val == 100)
  {
    dt_bauhaus_combobox_set(g->original, 1);
    gtk_widget_set_sensitive(g->original, FALSE);
  }
  else
    gtk_widget_set_sensitive(g->original, TRUE);
}

static void original_changed(GtkWidget *original, dt_imageio_module_format_t *self)
{
  dt_conf_set_bool("plugins/imageio/format/jxl/original", dt_bauhaus_combobox_get(original));
}

static void effort_changed(GtkWidget *effort, dt_imageio_module_format_t *self)
{
  dt_conf_set_int("plugins/imageio/format/jxl/effort", (int)dt_bauhaus_slider_get(effort));
}

static void tier_changed(GtkWidget *tier, dt_imageio_module_format_t *self)
{
  dt_conf_set_int("plugins/imageio/format/jxl/tier", (int)dt_bauhaus_slider_get(tier));
}

void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_jxl_gui_data_t *gui = g_malloc0(sizeof(dt_imageio_jxl_gui_data_t));
  if(!gui) return;
  self->gui_data = gui;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->widget = box;

  // bits per sample combobox
  GtkWidget *bpp = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_combobox_add(bpp, _("8 bit"));
  dt_bauhaus_combobox_add(bpp, _("10 bit"));
  dt_bauhaus_combobox_add(bpp, _("12 bit"));
  dt_bauhaus_combobox_add(bpp, _("16 bit"));
  dt_bauhaus_combobox_add(bpp, _("16 bit (half)"));
  dt_bauhaus_combobox_add(bpp, _("32 bit (float)"));
  const int bpp_enum = dt_conf_get_int("plugins/imageio/format/jxl/bpp");
  dt_bauhaus_combobox_set(bpp, bpp_enum);
  dt_bauhaus_widget_set_label(bpp, NULL, N_("bit depth"));
  g_signal_connect(G_OBJECT(bpp), "value-changed", G_CALLBACK(bpp_changed), self);
  gtk_box_pack_start(GTK_BOX(box), bpp, TRUE, TRUE, 0);
  gui->bpp = bpp;

  // quality slider
  GtkWidget *quality
      = dt_bauhaus_slider_new_with_range(NULL, dt_confgen_get_int("plugins/imageio/format/jxl/quality", DT_MIN),
                                         dt_confgen_get_int("plugins/imageio/format/jxl/quality", DT_MAX), 1,
                                         dt_confgen_get_int("plugins/imageio/format/jxl/quality", DT_DEFAULT), 0);
  const int quality_val = dt_conf_get_int("plugins/imageio/format/jxl/quality");
  dt_bauhaus_slider_set(quality, quality_val);
  dt_bauhaus_widget_set_label(quality, NULL, _("quality"));
  gtk_widget_set_tooltip_text(quality, _("the quality of the output image\n0-29 = very lossy\n30-99 = JPEG "
                                         "quality comparable\n100 = lossless (integer bith depth only)"));
  g_signal_connect(G_OBJECT(quality), "value-changed", G_CALLBACK(quality_changed), self);
  gtk_box_pack_start(GTK_BOX(box), quality, TRUE, TRUE, 0);
  gui->quality = quality;

  // encoding color profile combobox
  GtkWidget *original = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_combobox_add(original, _("internal"));
  dt_bauhaus_combobox_add(original, _("original"));
  dt_bauhaus_combobox_set_default(original,
                                  dt_confgen_get_bool("plugins/imageio/format/jxl/original", DT_DEFAULT) & 1);
  if(bpp_enum < 4 && quality_val == 100)
  {
    dt_bauhaus_combobox_set(original, 1);
    gtk_widget_set_sensitive(original, FALSE);
  }
  else
    dt_bauhaus_combobox_set(original, dt_conf_get_bool("plugins/imageio/format/jxl/original") & 1);
  dt_bauhaus_widget_set_label(original, NULL, N_("encoding color profile"));
  g_signal_connect(G_OBJECT(original), "value-changed", G_CALLBACK(original_changed), self);
  gtk_box_pack_start(GTK_BOX(box), original, TRUE, TRUE, 0);
  gui->original = original;

  // encoding effort slider
  GtkWidget *effort
      = dt_bauhaus_slider_new_with_range(NULL, dt_confgen_get_int("plugins/imageio/format/jxl/effort", DT_MIN),
                                         dt_confgen_get_int("plugins/imageio/format/jxl/effort", DT_MAX), 1,
                                         dt_confgen_get_int("plugins/imageio/format/jxl/effort", DT_DEFAULT), 0);
  dt_bauhaus_slider_set(effort, dt_conf_get_int("plugins/imageio/format/jxl/effort"));
  dt_bauhaus_widget_set_label(effort, NULL, _("encoding effort"));
  gtk_widget_set_tooltip_text(effort, _("the effort used to encode the image, higher efforts will have "
                                        "better results at the expense of longer encode times"));
  g_signal_connect(G_OBJECT(effort), "value-changed", G_CALLBACK(effort_changed), self);
  gtk_box_pack_start(GTK_BOX(box), effort, TRUE, TRUE, 0);
  gui->effort = effort;

  // decoding speed (tier) slider
  GtkWidget *tier
      = dt_bauhaus_slider_new_with_range(NULL, dt_confgen_get_int("plugins/imageio/format/jxl/tier", DT_MIN),
                                         dt_confgen_get_int("plugins/imageio/format/jxl/tier", DT_MAX), 1,
                                         dt_confgen_get_int("plugins/imageio/format/jxl/tier", DT_DEFAULT), 0);
  dt_bauhaus_slider_set(tier, dt_conf_get_int("plugins/imageio/format/jxl/tier"));
  dt_bauhaus_widget_set_label(tier, NULL, _("decoding speed"));
  gtk_widget_set_tooltip_text(tier, _("the preffered decoding speed with some sacrifice of quality"));
  g_signal_connect(G_OBJECT(tier), "value-changed", G_CALLBACK(tier_changed), self);
  gtk_box_pack_start(GTK_BOX(box), tier, TRUE, TRUE, 0);
  gui->tier = tier;
}

void gui_cleanup(dt_imageio_module_format_t *self)
{
  g_free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self)
{
  dt_imageio_jxl_gui_data_t *gui = (dt_imageio_jxl_gui_data_t *)self->gui_data;

  const int bpp = dt_confgen_get_int("plugins/imageio/format/jxl/bpp", DT_DEFAULT);
  dt_bauhaus_combobox_set(gui->bpp, bpp);

  const int quality = dt_confgen_get_int("plugins/imageio/format/jxl/quality", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->quality, quality);

  const int original = dt_confgen_get_bool("plugins/imageio/format/jxl/original", DT_DEFAULT);
  dt_bauhaus_combobox_set(gui->original, original & 1);

  const int effort = dt_confgen_get_int("plugins/imageio/format/jxl/effort", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->effort, effort);

  const int tier = dt_confgen_get_int("plugins/imageio/format/jxl/tier", DT_DEFAULT);
  dt_bauhaus_slider_set(gui->tier, tier);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
