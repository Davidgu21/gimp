/* LIC 0.14 -- image filter plug-in for GIMP
 * Copyright (C) 1996 Tom Bech
 *
 * E-mail: tomb@gimp.org
 * You can contact the original GIMP authors at gimp@xcf.berkeley.edu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * In other words, you can't sue me for whatever happens while using this ;)
 *
 * Changes (post 0.10):
 * -> 0.11: Fixed a bug in the convolution kernels (Tom).
 * -> 0.12: Added Quartic's bilinear interpolation stuff (Tom).
 * -> 0.13 Changed some UI stuff causing trouble with the 0.60 release, added
 *         the (GIMP) tags and changed random() calls to rand() (Tom)
 * -> 0.14 Ported to 0.99.11 (Tom)
 *
 * This plug-in implements the Line Integral Convolution (LIC) as described in
 * Cabral et al. "Imaging vector fields using line integral convolution" in the
 * Proceedings of ACM SIGGRAPH 93. Publ. by ACM, New York, NY, USA. p. 263-270.
 * (See http://www8.cs.umu.se/kurser/TDBD13/VT00/extra/p263-cabral.pdf)
 *
 * Some of the code is based on code by Steinar Haugen (thanks!), the Perlin
 * noise function is practically ripped as is :)
 */

#include "config.h"

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

#include "libgimp/stdplugins-intl.h"


/************/
/* Typedefs */
/************/

#define numx    40              /* Pseudo-random vector grid size */
#define numy    40

#define PLUG_IN_PROC   "plug-in-lic"
#define PLUG_IN_BINARY "van-gogh-lic"
#define PLUG_IN_ROLE   "gimp-van-gogh-lic"

typedef enum
{
  LIC_HUE,
  LIC_SATURATION,
  LIC_BRIGHTNESS
} LICEffectChannel;


typedef struct _Lic      Lic;
typedef struct _LicClass LicClass;

struct _Lic
{
  GimpPlugIn parent_instance;
};

struct _LicClass
{
  GimpPlugInClass parent_class;
};


#define LIC_TYPE  (lic_get_type ())
#define LIC (obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), LIC_TYPE, Lic))

GType                   lic_get_type         (void) G_GNUC_CONST;

static GList          * lic_query_procedures (GimpPlugIn           *plug_in);
static GimpProcedure  * lic_create_procedure (GimpPlugIn           *plug_in,
                                              const gchar          *name);

static GimpValueArray * lic_run              (GimpProcedure        *procedure,
                                              GimpRunMode           run_mode,
                                              GimpImage            *image,
                                              gint                  n_drawables,
                                              GimpDrawable        **drawables,
                                              GimpProcedureConfig  *config,
                                              gpointer              run_data);
static void           lic_scale_entry_update (GimpLabelSpin        *entry,
                                              gdouble              *value);


G_DEFINE_TYPE (Lic, lic, GIMP_TYPE_PLUG_IN)

GIMP_MAIN (LIC_TYPE)
DEFINE_STD_SET_I18N


static void
lic_class_init (LicClass *klass)
{
  GimpPlugInClass *plug_in_class = GIMP_PLUG_IN_CLASS (klass);

  plug_in_class->query_procedures = lic_query_procedures;
  plug_in_class->create_procedure = lic_create_procedure;
  plug_in_class->set_i18n         = STD_SET_I18N;
}

static void
lic_init (Lic *lic)
{
}


/*****************************/
/* Global variables and such */
/*****************************/

static gdouble G[numx][numy][2];

typedef struct
{
  gdouble  filtlen;
  gdouble  noisemag;
  gdouble  intsteps;
  gdouble  minv;
  gdouble  maxv;
  gint     effect_channel;
  gint     effect_operator;
  gint     effect_convolve;
  gint32   effect_image_id;
} LicValues;

static LicValues licvals;

static gdouble l      = 10.0;
static gdouble dx     =  2.0;
static gdouble dy     =  2.0;
static gdouble minv   = -2.5;
static gdouble maxv   =  2.5;
static gdouble isteps = 20.0;

static gboolean source_drw_has_alpha = FALSE;

static gint    effect_width, effect_height;
static gint    border_x, border_y, border_w, border_h;

static GtkWidget *dialog;

/************************/
/* Convenience routines */
/************************/

static void
peek (GeglBuffer *buffer,
      gint        x,
      gint        y,
      GimpRGB    *color)
{
  gegl_buffer_sample (buffer, x, y, NULL,
                      color, babl_format ("R'G'B'A double"),
                      GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);
}

static void
poke (GeglBuffer *buffer,
      gint        x,
      gint        y,
      GimpRGB    *color)
{
  gegl_buffer_set (buffer, GEGL_RECTANGLE (x, y, 1, 1), 0,
                   babl_format ("R'G'B'A double"), color,
                   GEGL_AUTO_ROWSTRIDE);
}

static gint
peekmap (const guchar *image,
         gint          x,
         gint          y)
{
  while (x < 0)
    x += effect_width;
  x %= effect_width;

  while (y < 0)
    y += effect_height;
  y %= effect_height;

  return (gint) image[x + effect_width * y];
}

/*************/
/* Main part */
/*************/

/***************************************************/
/* Compute the derivative in the x and y direction */
/* We use these convolution kernels:               */
/*     |1 0 -1|     |  1   2   1|                  */
/* DX: |2 0 -2| DY: |  0   0   0|                  */
/*     |1 0 -1|     | -1  -2  -1|                  */
/* (It's a variation of the Sobel kernels, really)  */
/***************************************************/

static gint
gradx (const guchar *image,
       gint          x,
       gint          y)
{
  gint val = 0;

  val = val +     peekmap (image, x-1, y-1);
  val = val -     peekmap (image, x+1, y-1);

  val = val + 2 * peekmap (image, x-1, y);
  val = val - 2 * peekmap (image, x+1, y);

  val = val +     peekmap (image, x-1, y+1);
  val = val -     peekmap (image, x+1, y+1);

  return val;
}

static gint
grady (const guchar *image,
       gint          x,
       gint          y)
{
  gint val = 0;

  val = val +     peekmap (image, x-1, y-1);
  val = val + 2 * peekmap (image, x,   y-1);
  val = val +     peekmap (image, x+1, y-1);

  val = val -     peekmap (image, x-1, y+1);
  val = val - 2 * peekmap (image, x,   y+1);
  val = val -     peekmap (image, x+1, y+1);

  return val;
}

/************************************/
/* A nice 2nd order cubic spline :) */
/************************************/

static gdouble
cubic (gdouble t)
{
  gdouble at = fabs (t);

  return (at < 1.0) ? at * at * (2.0 * at - 3.0) + 1.0 : 0.0;
}

static gdouble
omega (gdouble u,
       gdouble v,
       gint    i,
       gint    j)
{
  while (i < 0)
    i += numx;

  while (j < 0)
    j += numy;

  i %= numx;
  j %= numy;

  return cubic (u) * cubic (v) * (G[i][j][0]*u + G[i][j][1]*v);
}

/*************************************************************/
/* The noise function (2D variant of Perlins noise function) */
/*************************************************************/

static gdouble
noise (gdouble x,
       gdouble y)
{
  gint i, sti = (gint) floor (x / dx);
  gint j, stj = (gint) floor (y / dy);

  gdouble sum = 0.0;

  /* Calculate the gdouble sum */
  /* ======================== */

  for (i = sti; i <= sti + 1; i++)
    for (j = stj; j <= stj + 1; j++)
      sum += omega ((x - (gdouble) i * dx) / dx,
                    (y - (gdouble) j * dy) / dy,
                    i, j);

  return sum;
}

/*************************************************/
/* Generates pseudo-random vectors with length 1 */
/*************************************************/

static void
generatevectors (void)
{
  gdouble alpha;
  gint i, j;
  GRand *gr;

  gr = g_rand_new();

  for (i = 0; i < numx; i++)
    {
      for (j = 0; j < numy; j++)
        {
          alpha = g_rand_double_range (gr, 0, 2) * G_PI;
          G[i][j][0] = cos (alpha);
          G[i][j][1] = sin (alpha);
        }
    }

  g_rand_free (gr);
}

/* A simple triangle filter */
/* ======================== */

static gdouble
filter (gdouble u)
{
  gdouble f = 1.0 - fabs (u) / l;

  return (f < 0.0) ? 0.0 : f;
}

/******************************************************/
/* Compute the Line Integral Convolution (LIC) at x,y */
/******************************************************/

static gdouble
lic_noise (gint    x,
           gint    y,
           gdouble vx,
           gdouble vy)
{
  gdouble i = 0.0;
  gdouble f1 = 0.0, f2 = 0.0;
  gdouble u, step = 2.0 * l / isteps;
  gdouble xx = (gdouble) x, yy = (gdouble) y;
  gdouble c, s;

  /* Get vector at x,y */
  /* ================= */

  c = vx;
  s = vy;

  /* Calculate integral numerically */
  /* ============================== */

  f1 = filter (-l) * noise (xx + l * c , yy + l * s);

  for (u = -l + step; u <= l; u += step)
    {
      f2 = filter (u) * noise ( xx - u * c , yy - u * s);
      i += (f1 + f2) * 0.5 * step;
      f1 = f2;
    }

  i = (i - minv) / (maxv - minv);

  i = CLAMP (i, 0.0, 1.0);

  i = (i / 2.0) + 0.5;

  return i;
}

static void
getpixel (GeglBuffer *buffer,
          GimpRGB    *p,
          gdouble     u,
          gdouble     v)
{
  register gint x1, y1, x2, y2;
  gint width, height;
  static GimpRGB pp[4];

  width  = border_w;
  height = border_h;

  x1 = (gint)u;
  y1 = (gint)v;

  if (x1 < 0)
    x1 = width - (-x1 % width);
  else
    x1 = x1 % width;

  if (y1 < 0)
    y1 = height - (-y1 % height);
  else
    y1 = y1 % height;

  x2 = (x1 + 1) % width;
  y2 = (y1 + 1) % height;

  peek (buffer, x1, y1, &pp[0]);
  peek (buffer, x2, y1, &pp[1]);
  peek (buffer, x1, y2, &pp[2]);
  peek (buffer, x2, y2, &pp[3]);

  if (source_drw_has_alpha)
    *p = gimp_bilinear_rgba (u, v, pp);
  else
    *p = gimp_bilinear_rgb (u, v, pp);
}

static void
lic_image (GeglBuffer *buffer,
           gint        x,
           gint        y,
           gdouble     vx,
           gdouble     vy,
           GimpRGB    *color)
{
  gdouble u, step = 2.0 * l / isteps;
  gdouble xx = (gdouble) x, yy = (gdouble) y;
  gdouble c, s;
  GimpRGB col = { 0, 0, 0, 0 };
  GimpRGB col1, col2, col3;

  /* Get vector at x,y */
  /* ================= */

  c = vx;
  s = vy;

  /* Calculate integral numerically */
  /* ============================== */

  getpixel (buffer, &col1, xx + l * c, yy + l * s);

  if (source_drw_has_alpha)
    gimp_rgba_multiply (&col1, filter (-l));
  else
    gimp_rgb_multiply (&col1, filter (-l));

  for (u = -l + step; u <= l; u += step)
    {
      getpixel (buffer, &col2, xx - u * c, yy - u * s);

      if (source_drw_has_alpha)
        {
          gimp_rgba_multiply (&col2, filter (u));

          col3 = col1;
          gimp_rgba_add (&col3, &col2);
          gimp_rgba_multiply (&col3, 0.5 * step);
          gimp_rgba_add (&col, &col3);
        }
      else
        {
          gimp_rgb_multiply (&col2, filter (u));

          col3 = col1;
          gimp_rgb_add (&col3, &col2);
          gimp_rgb_multiply (&col3, 0.5 * step);
          gimp_rgb_add (&col, &col3);
        }
      col1 = col2;
    }
  if (source_drw_has_alpha)
    gimp_rgba_multiply (&col, 1.0 / l);
  else
    gimp_rgb_multiply (&col, 1.0 / l);
  gimp_rgb_clamp (&col);

  *color = col;
}

static guchar *
rgb_to_hsl (GimpDrawable     *drawable,
            LICEffectChannel  effect_channel)
{
  GeglBuffer *buffer;
  guchar     *themap, data[4];
  gint        x, y;
  GimpRGB     color;
  GimpHSL     color_hsl;
  gdouble     val = 0.0;
  glong       maxc, index = 0;
  GRand      *gr;

  gr = g_rand_new ();

  maxc = gimp_drawable_get_width (drawable) * gimp_drawable_get_height (drawable);

  buffer = gimp_drawable_get_buffer (drawable);

  themap = g_new (guchar, maxc);

  for (y = 0; y < border_h; y++)
    {
      for (x = 0; x < border_w; x++)
        {
          data[3] = 255;

          gegl_buffer_sample (buffer, x, y, NULL,
                              data, babl_format ("R'G'B'A u8"),
                              GEGL_SAMPLER_NEAREST, GEGL_ABYSS_NONE);

          gimp_rgba_set_uchar (&color, data[0], data[1], data[2], data[3]);
          gimp_rgb_to_hsl (&color, &color_hsl);

          switch (effect_channel)
            {
            case LIC_HUE:
              val = color_hsl.h * 255;
              break;
            case LIC_SATURATION:
              val = color_hsl.s * 255;
              break;
            case LIC_BRIGHTNESS:
              val = color_hsl.l * 255;
              break;
            }

          /* add some random to avoid unstructured areas. */
          val += g_rand_double_range (gr, -1.0, 1.0);

          themap[index++] = (guchar) CLAMP0255 (RINT (val));
        }
    }

  g_object_unref (buffer);

  g_rand_free (gr);

  return themap;
}


static void
compute_lic (GimpDrawable *drawable,
             const guchar *scalarfield,
             gboolean      rotate)
{
  GeglBuffer *src_buffer;
  GeglBuffer *dest_buffer;
  gint        xcount, ycount;
  GimpRGB     color;
  gdouble     vx, vy, tmp;

  src_buffer  = gimp_drawable_get_buffer (drawable);
  dest_buffer = gimp_drawable_get_shadow_buffer (drawable);

  for (ycount = 0; ycount < border_h; ycount++)
    {
      for (xcount = 0; xcount < border_w; xcount++)
        {
          /* Get derivative at (x,y) and normalize it */
          /* ============================================================== */

          vx = gradx (scalarfield, xcount, ycount);
          vy = grady (scalarfield, xcount, ycount);

          /* Rotate if needed */
          if (rotate)
            {
              tmp = vy;
              vy = -vx;
              vx = tmp;
            }

          tmp = sqrt (vx * vx + vy * vy);
          if (tmp >= 0.000001)
            {
              tmp = 1.0 / tmp;
              vx *= tmp;
              vy *= tmp;
            }

          /* Convolve with the LIC at (x,y) */
          /* ============================== */

          if (licvals.effect_convolve == 0)
            {
              peek (src_buffer, xcount, ycount, &color);

              tmp = lic_noise (xcount, ycount, vx, vy);

              if (source_drw_has_alpha)
                gimp_rgba_multiply (&color, tmp);
              else
                gimp_rgb_multiply (&color, tmp);
            }
          else
            {
              lic_image (src_buffer, xcount, ycount, vx, vy, &color);
            }

          poke (dest_buffer, xcount, ycount, &color);
        }

      gimp_progress_update ((gfloat) ycount / (gfloat) border_h);
    }

  g_object_unref (src_buffer);
  g_object_unref (dest_buffer);

  gimp_progress_update (1.0);
}

static void
compute_image (GimpDrawable *drawable)
{
  GimpDrawable *effect_image;
  guchar       *scalarfield = NULL;

  /* Get some useful info on the input drawable */
  /* ========================================== */
  if (! gimp_drawable_mask_intersect (drawable,
                                      &border_x, &border_y,
                                      &border_w, &border_h))
    return;

  gimp_progress_init (_("Van Gogh (LIC)"));

  if (licvals.effect_convolve == 0)
    generatevectors ();

  if (licvals.filtlen < 0.1)
    licvals.filtlen = 0.1;

  l = licvals.filtlen;
  dx = dy = licvals.noisemag;
  minv = licvals.minv / 10.0;
  maxv = licvals.maxv / 10.0;
  isteps = licvals.intsteps;

  source_drw_has_alpha = gimp_drawable_has_alpha (drawable);

  effect_image = gimp_drawable_get_by_id (licvals.effect_image_id);

  effect_width =  gimp_drawable_get_width  (effect_image);
  effect_height = gimp_drawable_get_height (effect_image);

  switch (licvals.effect_channel)
    {
    case 0:
      scalarfield = rgb_to_hsl (effect_image, LIC_HUE);
      break;
    case 1:
      scalarfield = rgb_to_hsl (effect_image, LIC_SATURATION);
      break;
    case 2:
      scalarfield = rgb_to_hsl (effect_image, LIC_BRIGHTNESS);
      break;
    }

  compute_lic (drawable, scalarfield, licvals.effect_operator);

  g_free (scalarfield);

  /* Update image */
  /* ============ */

  gimp_drawable_merge_shadow (drawable, TRUE);
  gimp_drawable_update (drawable, border_x, border_y, border_w, border_h);

  gimp_displays_flush ();
}

/**************************/
/* Below is only UI stuff */
/**************************/

static gboolean
effect_image_constrain (GimpImage *image,
                        GimpItem  *item,
                        gpointer   data)
{
  return gimp_drawable_is_rgb (GIMP_DRAWABLE (item));
}

static gboolean
create_main_dialog (void)
{
  GtkWidget *vbox;
  GtkWidget *hbox;
  GtkWidget *frame;
  GtkWidget *grid;
  GtkWidget *combo;
  GtkWidget *scale;
  gint       row;
  gboolean   run;

  gimp_ui_init (PLUG_IN_BINARY);

  dialog = gimp_dialog_new (_("Van Gogh (LIC)"), PLUG_IN_ROLE,
                            NULL, 0,
                            gimp_standard_help_func, PLUG_IN_PROC,

                            _("_Cancel"), GTK_RESPONSE_CANCEL,
                            _("_OK"),     GTK_RESPONSE_OK,

                            NULL);

  gimp_dialog_set_alternative_button_order (GTK_DIALOG (dialog),
                                           GTK_RESPONSE_OK,
                                           GTK_RESPONSE_CANCEL,
                                           -1);

  gimp_window_set_transient (GTK_WINDOW (dialog));

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
  gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                      vbox, TRUE, TRUE, 0);
  gtk_widget_show (vbox);

  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
  gtk_widget_show (hbox);

  frame = gimp_int_radio_group_new (TRUE, _("Effect Channel"),
                                    G_CALLBACK (gimp_radio_button_update),
                                    &licvals.effect_channel, NULL,
                                    licvals.effect_channel,

                                    _("_Hue"),        0, NULL,
                                    _("_Saturation"), 1, NULL,
                                    _("_Brightness"), 2, NULL,

                                    NULL);
  gtk_box_pack_start (GTK_BOX (hbox), frame, FALSE, FALSE, 0);
  gtk_widget_show (frame);

  frame = gimp_int_radio_group_new (TRUE, _("Effect Operator"),
                                    G_CALLBACK (gimp_radio_button_update),
                                    &licvals.effect_operator, NULL,
                                    licvals.effect_operator,

                                    _("_Derivative"), 0, NULL,
                                    _("_Gradient"),   1, NULL,

                                    NULL);
  gtk_box_pack_start (GTK_BOX (hbox), frame, FALSE, FALSE, 0);
  gtk_widget_show (frame);

  frame = gimp_int_radio_group_new (TRUE, _("Convolve"),
                                    G_CALLBACK (gimp_radio_button_update),
                                    &licvals.effect_convolve, NULL,
                                    licvals.effect_convolve,

                                    _("_With white noise"),  0, NULL,
                                    _("W_ith source image"), 1, NULL,

                                    NULL);
  gtk_box_pack_start (GTK_BOX (hbox), frame, FALSE, FALSE, 0);
  gtk_widget_show (frame);

  /* Effect image menu */
  grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
  gtk_box_pack_start (GTK_BOX (vbox), grid, FALSE, FALSE, 0);
  gtk_widget_show (grid);

  combo = gimp_drawable_combo_box_new (effect_image_constrain, NULL, NULL);
  gimp_int_combo_box_connect (GIMP_INT_COMBO_BOX (combo),
                              licvals.effect_image_id,
                              G_CALLBACK (gimp_int_combo_box_get_active),
                              &licvals.effect_image_id, NULL);

  gimp_grid_attach_aligned (GTK_GRID (grid), 0, 0,
                            _("_Effect image:"), 0.0, 0.5, combo, 2);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
  gtk_box_pack_start (GTK_BOX (vbox), grid, FALSE, FALSE, 0);
  gtk_widget_show (grid);

  row = 0;

  scale = gimp_scale_entry_new (_("_Filter length:"), licvals.filtlen, 0.1, 64, 1);
  gimp_label_spin_set_increments (GIMP_LABEL_SPIN (scale), 1.0, 8.0);
  g_signal_connect (scale, "value-changed",
                    G_CALLBACK (lic_scale_entry_update),
                    &licvals.filtlen);
  gtk_grid_attach (GTK_GRID (grid), scale, 0, row++, 3, 1);
  gtk_widget_show (scale);

  scale = gimp_scale_entry_new (_("_Noise magnitude:"), licvals.noisemag, 1, 5, 1);
  gimp_label_spin_set_increments (GIMP_LABEL_SPIN (scale), 0.1, 1.0);
  g_signal_connect (scale, "value-changed",
                    G_CALLBACK (lic_scale_entry_update),
                    &licvals.noisemag);
  gtk_grid_attach (GTK_GRID (grid), scale, 0, row++, 3, 1);
  gtk_widget_show (scale);

  scale = gimp_scale_entry_new (_("In_tegration steps:"), licvals.intsteps, 1, 40, 1);
  g_signal_connect (scale, "value-changed",
                    G_CALLBACK (lic_scale_entry_update),
                    &licvals.intsteps);
  gtk_grid_attach (GTK_GRID (grid), scale, 0, row++, 3, 1);
  gtk_widget_show (scale);

  scale = gimp_scale_entry_new (_("_Minimum value:"), licvals.minv, -100, 0, 1);
  g_signal_connect (scale, "value-changed",
                    G_CALLBACK (lic_scale_entry_update),
                    &licvals.minv);
  gtk_grid_attach (GTK_GRID (grid), scale, 0, row++, 3, 1);
  gtk_widget_show (scale);

  scale = gimp_scale_entry_new (_("M_aximum value:"),
                                      licvals.maxv, 0, 100, 1);
  g_signal_connect (scale, "value-changed",
                    G_CALLBACK (lic_scale_entry_update),
                    &licvals.maxv);
  gtk_grid_attach (GTK_GRID (grid), scale, 0, row++, 3, 1);
  gtk_widget_show (scale);

  gtk_widget_show (dialog);

  run = (gimp_dialog_run (GIMP_DIALOG (dialog)) == GTK_RESPONSE_OK);

  gtk_widget_destroy (dialog);

  return run;
}

/*************************************/
/* Set parameters to standard values */
/*************************************/

static void
set_default_settings (void)
{
  licvals.filtlen          = 5;
  licvals.noisemag         = 2;
  licvals.intsteps         = 25;
  licvals.minv             = -25;
  licvals.maxv             = 25;
  licvals.effect_channel   = 2;
  licvals.effect_operator  = 1;
  licvals.effect_convolve  = 1;
  licvals.effect_image_id  = -1;
}

static GList *
lic_query_procedures (GimpPlugIn *plug_in)
{
  return g_list_append (NULL, g_strdup (PLUG_IN_PROC));
}

static GimpProcedure *
lic_create_procedure (GimpPlugIn  *plug_in,
                      const gchar *name)
{
  GimpProcedure *procedure = NULL;

  if (! strcmp (name, PLUG_IN_PROC))
    {
      procedure = gimp_image_procedure_new (plug_in, name,
                                            GIMP_PDB_PROC_TYPE_PLUGIN,
                                            lic_run, NULL, NULL);

      gimp_procedure_set_image_types (procedure, "RGB*");
      gimp_procedure_set_sensitivity_mask (procedure,
                                           GIMP_PROCEDURE_SENSITIVE_DRAWABLE);

      gimp_procedure_set_menu_label (procedure, _("_Van Gogh (LIC)..."));
      gimp_procedure_add_menu_path (procedure, "<Image>/Filters/Artistic");

      gimp_procedure_set_documentation (procedure,
                                        _("Special effects that nobody "
                                          "understands"),
                                        "No help yet",
                                        name);
      gimp_procedure_set_attribution (procedure,
                                      "Tom Bech & Federico Mena Quintero",
                                      "Tom Bech & Federico Mena Quintero",
                                      "Version 0.14, September 24 1997");
    }

  return procedure;
}

static GimpValueArray *
lic_run (GimpProcedure        *procedure,
         GimpRunMode           run_mode,
         GimpImage            *image,
         gint                  n_drawables,
         GimpDrawable        **drawables,
         GimpProcedureConfig  *config,
         gpointer              run_data)
{
  GimpDrawable *drawable;

  gegl_init (NULL, NULL);

  if (n_drawables != 1)
    {
      GError *error = NULL;

      g_set_error (&error, GIMP_PLUG_IN_ERROR, 0,
                   _("Procedure '%s' only works with one drawable."),
                   PLUG_IN_PROC);

      return gimp_procedure_new_return_values (procedure,
                                               GIMP_PDB_CALLING_ERROR,
                                               error);
    }
  else
    {
      drawable = drawables[0];
    }

  /* Set default values */
  /* ================== */

  set_default_settings ();

  /* Possibly retrieve data */
  /* ====================== */

  gimp_get_data (PLUG_IN_PROC, &licvals);

  if (! gimp_item_id_is_valid (licvals.effect_image_id))
    licvals.effect_image_id = -1;

  /* Make sure that the drawable is RGBA or RGB color */
  /* ================================================ */

  if (gimp_drawable_is_rgb (drawable))
    {
      switch (run_mode)
        {
        case GIMP_RUN_INTERACTIVE:
          if (! create_main_dialog ())
            {
              return gimp_procedure_new_return_values (procedure,
                                                       GIMP_PDB_CANCEL,
                                                       NULL);
            }

          compute_image (drawable);

          gimp_set_data (PLUG_IN_PROC, &licvals, sizeof (LicValues));
          break;

        case GIMP_RUN_WITH_LAST_VALS:
          compute_image (drawable);
          break;

        default:
          break;
        }
    }
  else
    {
      return gimp_procedure_new_return_values (procedure,
                                               GIMP_PDB_EXECUTION_ERROR,
                                               NULL);
    }

  return gimp_procedure_new_return_values (procedure, GIMP_PDB_SUCCESS, NULL);
}

static void
lic_scale_entry_update (GimpLabelSpin *entry,
                        gdouble       *value)
{
  *value = gimp_label_spin_get_value (entry);
}
