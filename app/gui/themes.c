/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
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
 */

#include "config.h"

#include <stdlib.h>

#include <gegl.h>
#include <gtk/gtk.h>

#include "libgimpbase/gimpbase.h"
#include "libgimpconfig/gimpconfig.h"

#include "gui-types.h"

#include "config/gimpguiconfig.h"

#include "core/gimp.h"

#include "themes.h"

#include "gimp-intl.h"


/*  local function prototypes  */

static void   themes_apply_theme         (Gimp                   *gimp,
                                          GimpGuiConfig          *config);
static void   themes_list_themes_foreach (gpointer                key,
                                          gpointer                value,
                                          gpointer                data);
static gint   themes_name_compare        (const void             *p1,
                                          const void             *p2);
static void   themes_theme_change_notify (GimpGuiConfig          *config,
                                          GParamSpec             *pspec,
                                          Gimp                   *gimp);
static void   themes_theme_paths_notify  (GimpExtensionManager   *manager,
                                          GParamSpec             *pspec,
                                          Gimp                   *gimp);


/*  private variables  */

static GHashTable       *themes_hash           = NULL;
static GtkStyleProvider *themes_style_provider = NULL;


/*  public functions  */

void
themes_init (Gimp *gimp)
{
  GimpGuiConfig *config;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  config = GIMP_GUI_CONFIG (gimp->config);

  /* Check for theme extensions. */
  themes_theme_paths_notify (gimp->extension_manager, NULL, gimp);
  g_signal_connect (gimp->extension_manager, "notify::theme-paths",
                    G_CALLBACK (themes_theme_paths_notify),
                    gimp);

  themes_style_provider = GTK_STYLE_PROVIDER (gtk_css_provider_new ());

  /*  Use GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1 so theme files
   *  override default styles provided by widgets themselves.
   */
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             themes_style_provider,
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

  g_signal_connect (config, "notify::theme",
                    G_CALLBACK (themes_theme_change_notify),
                    gimp);
  g_signal_connect (config, "notify::prefer-dark-theme",
                    G_CALLBACK (themes_theme_change_notify),
                    gimp);
  g_signal_connect (config, "notify::prefer-symbolic-icons",
                    G_CALLBACK (themes_theme_change_notify),
                    gimp);
  g_signal_connect (config, "notify::override-theme-icon-size",
                    G_CALLBACK (themes_theme_change_notify),
                    gimp);
  g_signal_connect (config, "notify::custom-icon-size",
                    G_CALLBACK (themes_theme_change_notify),
                    gimp);
  g_signal_connect (config, "notify::font-relative-size",
                    G_CALLBACK (themes_theme_change_notify),
                    gimp);

  themes_theme_change_notify (config, NULL, gimp);
}

void
themes_exit (Gimp *gimp)
{
  g_return_if_fail (GIMP_IS_GIMP (gimp));

  if (themes_hash)
    {
      g_signal_handlers_disconnect_by_func (gimp->config,
                                            themes_theme_change_notify,
                                            gimp);

      g_hash_table_destroy (themes_hash);
      themes_hash = NULL;
    }

  g_clear_object (&themes_style_provider);
}

gchar **
themes_list_themes (Gimp *gimp,
                    gint *n_themes)
{

  g_return_val_if_fail (GIMP_IS_GIMP (gimp), NULL);
  g_return_val_if_fail (n_themes != NULL, NULL);

  *n_themes = g_hash_table_size (themes_hash);

  if (*n_themes > 0)
    {
      gchar **themes;
      gchar **index;

      themes = g_new0 (gchar *, *n_themes + 1);

      index = themes;

      g_hash_table_foreach (themes_hash, themes_list_themes_foreach, &index);

      qsort (themes, *n_themes, sizeof (gchar *), themes_name_compare);

      return themes;
    }

  return NULL;
}

GFile *
themes_get_theme_dir (Gimp        *gimp,
                      const gchar *theme_name)
{
  g_return_val_if_fail (GIMP_IS_GIMP (gimp), NULL);

  if (! theme_name)
    theme_name = GIMP_CONFIG_DEFAULT_THEME;

  return g_hash_table_lookup (themes_hash, theme_name);
}

GFile *
themes_get_theme_file (Gimp        *gimp,
                       const gchar *first_component,
                       ...)
{
  GimpGuiConfig *gui_config;
  GFile         *file;
  const gchar   *component;
  va_list        args;

  g_return_val_if_fail (GIMP_IS_GIMP (gimp), NULL);
  g_return_val_if_fail (first_component != NULL, NULL);

  gui_config = GIMP_GUI_CONFIG (gimp->config);

  file      = g_object_ref (themes_get_theme_dir (gimp, gui_config->theme));
  component = first_component;

  va_start (args, first_component);

  do
    {
      GFile *tmp = g_file_get_child (file, component);
      g_object_unref (file);
      file = tmp;
    }
  while ((component = va_arg (args, gchar *)));

  va_end (args);

  if (! g_file_query_exists (file, NULL))
    {
      g_object_unref (file);

      file      = g_object_ref (themes_get_theme_dir (gimp, NULL));
      component = first_component;

      va_start (args, first_component);

      do
        {
          GFile *tmp = g_file_get_child (file, component);
          g_object_unref (file);
          file = tmp;
        }
      while ((component = va_arg (args, gchar *)));

      va_end (args);
    }

  return file;
}


/*  private functions  */

static void
themes_apply_theme (Gimp          *gimp,
                    GimpGuiConfig *config)
{
  GFile         *theme_css;
  GOutputStream *output;
  GError        *error = NULL;

  g_return_if_fail (GIMP_IS_GIMP (gimp));
  g_return_if_fail (GIMP_IS_GUI_CONFIG (config));

  theme_css = gimp_directory_file ("theme.css", NULL);

  if (gimp->be_verbose)
    g_print ("Writing '%s'\n", gimp_file_get_utf8_name (theme_css));

  output = G_OUTPUT_STREAM (g_file_replace (theme_css,
                                            NULL, FALSE, G_FILE_CREATE_NONE,
                                            NULL, &error));
  if (! output)
    {
      gimp_message_literal (gimp, NULL, GIMP_MESSAGE_ERROR, error->message);
      g_clear_error (&error);
    }
  else
    {
      GFile  *theme_dir = themes_get_theme_dir (gimp, config->theme);
      GFile  *css_user;
      GSList *css_files = NULL;
      GSList *iter;

      if (theme_dir)
        {
          css_files = g_slist_prepend (css_files, g_file_get_child (theme_dir,
                                                                    "gimp.css"));
          if (config->prefer_dark_theme)
            css_files = g_slist_prepend (css_files, g_file_get_child (theme_dir,
                                                                      "gimp-dark.css"));
        }
      else
        {
          gchar *tmp;

          tmp = g_build_filename (gimp_data_directory (),
                                  "themes", "Default", "gimp.css",
                                  NULL);
          css_files = g_slist_prepend (
            css_files, g_file_new_for_path (tmp));
          g_free (tmp);

          if (config->prefer_dark_theme)
            {
              tmp = g_build_filename (gimp_data_directory (),
                                      "themes", "Default", "gimp-dark.css",
                                      NULL);
              css_files = g_slist_prepend (css_files, g_file_new_for_path (tmp));
              g_free (tmp);
            }
        }

      css_files = g_slist_prepend (
        css_files, gimp_sysconf_directory_file ("gimp.css", NULL));

      css_user  = gimp_directory_file ("gimp.css", NULL);
      css_files = g_slist_prepend (
        css_files, css_user);

      css_files = g_slist_reverse (css_files);

      g_output_stream_printf (
        output, NULL, NULL, &error,
        "/* GIMP theme.css\n"
        " *\n"
        " * This file is written on GIMP startup and on every theme change.\n"
        " * It is NOT supposed to be edited manually. Edit your personal\n"
        " * gimp.css file instead (%s).\n"
        " */\n"
        "\n",
        gimp_file_get_utf8_name (css_user));

      for (iter = css_files; ! error && iter; iter = g_slist_next (iter))
        {
          GFile *file = iter->data;

          if (g_file_query_exists (file, NULL))
            {
              gchar *path;

              path = g_file_get_uri (file);

              g_output_stream_printf (
                output, NULL, NULL, &error,
                "@import url(\"%s\");\n",
                path);

              g_free (path);
            }
        }

      if (! error)
        {
          g_output_stream_printf (
            output, NULL, NULL, &error,
            "\n"
            "* { -gtk-icon-style: %s; }\n"
            "\n"
            "%s",
            config->prefer_symbolic_icons ? "symbolic" : "regular",
            config->prefer_dark_theme ? "/* prefer-dark-theme */\n" : "");
        }

      if (! error && config->override_icon_size)
        {
          const gchar *tool_icon_size   = "large-toolbar";
          const gchar *tab_icon_size    = "small-toolbar";
          const gchar *button_icon_size = "small-toolbar";

          switch (config->custom_icon_size)
            {
            case GIMP_ICON_SIZE_SMALL:
              tool_icon_size   = "small-toolbar";
              tab_icon_size    = "small-toolbar";
              button_icon_size = "small-toolbar";
              break;
            case GIMP_ICON_SIZE_MEDIUM:
              tool_icon_size   = "large-toolbar";
              tab_icon_size    = "small-toolbar";
              button_icon_size = "small-toolbar";
              break;
            case GIMP_ICON_SIZE_LARGE:
              tool_icon_size   = "dnd";
              tab_icon_size    = "large-toolbar";
              button_icon_size = "large-toolbar";
              break;
            case GIMP_ICON_SIZE_HUGE:
              tool_icon_size   = "dialog";
              tab_icon_size    = "dnd";
              button_icon_size = "dnd";
              break;
            }

          g_output_stream_printf (
            output, NULL, NULL, &error,
            "\n"
            "* { -GimpToolPalette-tool-icon-size: %s; }"
            "\n"
            "* { -GimpDockbook-tab-icon-size: %s; }"
            "\n"
            "* { -GimpEditor-button-icon-size: %s; }",
            tool_icon_size,
            tab_icon_size,
            button_icon_size);
        }

      if (! error && config->font_relative_size != 1.0)
        g_output_stream_printf (output, NULL, NULL, &error,
                                "\n"
                                "* { font-size: %frem; }",
                                config->font_relative_size);

      if (! error)
        {
          g_output_stream_printf (
            output, NULL, NULL, &error,
            "\n\n"
            "/* end of theme.css */\n");
        }

      if (error)
        {
          GCancellable *cancellable = g_cancellable_new ();

          gimp_message (gimp, NULL, GIMP_MESSAGE_ERROR,
                        _("Error writing '%s': %s"),
                        gimp_file_get_utf8_name (theme_css), error->message);
          g_clear_error (&error);

          /* Cancel the overwrite initiated by g_file_replace(). */
          g_cancellable_cancel (cancellable);
          g_output_stream_close (output, cancellable, NULL);
          g_object_unref (cancellable);
        }
      else if (! g_output_stream_close (output, NULL, &error))
        {
          gimp_message (gimp, NULL, GIMP_MESSAGE_ERROR,
                        _("Error closing '%s': %s"),
                        gimp_file_get_utf8_name (theme_css), error->message);
          g_clear_error (&error);
        }

      g_slist_free_full (css_files, g_object_unref);
      g_object_unref (output);
    }

  g_object_unref (theme_css);
}

static void
themes_list_themes_foreach (gpointer key,
                            gpointer value,
                            gpointer data)
{
  gchar ***index = data;

  **index = g_strdup ((gchar *) key);

  (*index)++;
}

static gint
themes_name_compare (const void *p1,
                     const void *p2)
{
  return strcmp (* (char **) p1, * (char **) p2);
}

static void
themes_theme_change_notify (GimpGuiConfig *config,
                            GParamSpec    *pspec,
                            Gimp          *gimp)
{
  GFile  *theme_css;
  GError *error = NULL;

  g_object_set (gtk_settings_get_for_screen (gdk_screen_get_default ()),
                "gtk-application-prefer-dark-theme", config->prefer_dark_theme,
                NULL);

  themes_apply_theme (gimp, config);

  theme_css = gimp_directory_file ("theme.css", NULL);

  if (gimp->be_verbose)
    g_print ("Parsing '%s'\n",
             gimp_file_get_utf8_name (theme_css));

  if (! gtk_css_provider_load_from_file (GTK_CSS_PROVIDER (themes_style_provider),
                                         theme_css, &error))
    {
      g_printerr ("%s: error parsing %s: %s\n", G_STRFUNC,
                  gimp_file_get_utf8_name (theme_css), error->message);
      g_clear_error (&error);
    }

  g_object_unref (theme_css);

  gtk_style_context_reset_widgets (gdk_screen_get_default ());
}

static void
themes_theme_paths_notify (GimpExtensionManager *manager,
                           GParamSpec           *pspec,
                           Gimp                 *gimp)
{
  GimpGuiConfig *config;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  if (themes_hash)
    g_hash_table_remove_all (themes_hash);
  else
    themes_hash = g_hash_table_new_full (g_str_hash,
                                         g_str_equal,
                                         g_free,
                                         g_object_unref);

  config = GIMP_GUI_CONFIG (gimp->config);
  if (config->theme_path)
    {
      GList *path;
      GList *list;

      g_object_get (gimp->extension_manager,
                    "theme-paths", &path,
                    NULL);
      path = g_list_copy_deep (path, (GCopyFunc) g_object_ref, NULL);
      path = g_list_concat (path, gimp_config_path_expand_to_files (config->theme_path, NULL));

      for (list = path; list; list = g_list_next (list))
        {
          GFile           *dir = list->data;
          GFileEnumerator *enumerator;

          enumerator =
            g_file_enumerate_children (dir,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                       G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
                                       G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                       G_FILE_QUERY_INFO_NONE,
                                       NULL, NULL);

          if (enumerator)
            {
              GFileInfo *info;

              while ((info = g_file_enumerator_next_file (enumerator,
                                                          NULL, NULL)))
                {
                  if (! g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) &&
                      g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE) == G_FILE_TYPE_DIRECTORY)
                    {
                      GFile       *file;
                      const gchar *name;
                      gchar       *basename;

                      file = g_file_enumerator_get_child (enumerator, info);
                      name = gimp_file_get_utf8_name (file);

                      basename = g_path_get_basename (name);

                      if (gimp->be_verbose)
                        g_print ("Adding theme '%s' (%s)\n",
                                 basename, name);

                      g_hash_table_insert (themes_hash, basename, file);
                    }

                  g_object_unref (info);
                }

              g_object_unref (enumerator);
            }
        }

      g_list_free_full (path, (GDestroyNotify) g_object_unref);
    }
}
