/* gnome-screenshot.c - Take a screenshot of the desktop
 *
 * Copyright (C) 2001 Jonathan Blandford <jrb@alum.mit.edu>
 * Copyright (C) 2006 Emmanuele Bassi <ebassi@gnome.org>
 * Copyright (C) 2008-2012 Cosimo Cecchi <cosimoc@gnome.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */

#include "config.h"

#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "screenshot-application.h"
#include "screenshot-area-selection.h"
#include "screenshot-config.h"
#include "screenshot-filename-builder.h"
#include "screenshot-interactive-dialog.h"
#include "screenshot-shadow.h"
#include "screenshot-utils.h"
#include "screenshot-dialog.h"

#define LAST_SAVE_DIRECTORY_KEY "last-save-directory"

/* Event callbacks */
static gboolean keyPress(GtkWidget *widget, gpointer data);
static gboolean sizeChanged(GtkWidget *widget, GtkAllocation *allocation, gpointer data);

static GtkWidget *image; /* As displayed on the screen */

G_DEFINE_TYPE(ScreenshotApplication, screenshot_application, GTK_TYPE_APPLICATION);
void destroy_widget(GtkButton *button, GtkWidget *widget){
  gtk_widget_destroy(widget);
  exit(0);
}
void showArlert()
{
  GtkWidget *window, *label, *button_ok, *grid;
  gtk_init(0, 0);
  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Problem with Pinta!!");
  gtk_window_set_default_size(GTK_WINDOW(window),400, 200);
  label = gtk_label_new("Cannot open Pinta!!!\nMay be Pinta was not installed.\nYou can search Pinta on software market.\n");
  button_ok = gtk_button_new_with_label("OK");  
  gtk_widget_set_size_request(button_ok, 70, 30);          
  g_signal_connect(window, "delete-event", G_CALLBACK(gtk_main_quit), NULL);
  g_signal_connect(button_ok, "clicked", G_CALLBACK(destroy_widget), window);
  
  grid = gtk_grid_new ();
  gtk_grid_attach(GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID (grid), button_ok, 0, 1, 1, 1);
  gtk_widget_set_size_request(button_ok, 70, 30);  
  gtk_container_add(GTK_CONTAINER(window), grid);
  gtk_widget_set_vexpand (label, TRUE);
    gtk_widget_set_hexpand (label, TRUE);
    //  gtk_widget_set_vexpand (button_ok, TRUE);
    // gtk_widget_set_hexpand (button_ok, TRUE);


  gtk_widget_show_all(window);
  gtk_main();
}



static void screenshot_save_to_file(ScreenshotApplication *self);
static void screenshot_show_interactive_dialog(ScreenshotApplication *self);

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
struct _ScreenshotApplicationPriv
{
  gchar *icc_profile_base64;
  GdkPixbuf *screenshot;

  gchar *save_uri;
  gchar *save_path;
  gboolean should_overwrite;

  ScreenshotDialog *dialog;
};

static void
save_folder_to_settings(ScreenshotApplication *self)
{
  g_autofree gchar *folder = screenshot_dialog_get_folder(self->priv->dialog);
  g_settings_set_string(screenshot_config->settings,
                        LAST_SAVE_DIRECTORY_KEY, folder);
}

static void
set_recent_entry(ScreenshotApplication *self)
{
  g_autofree gchar *app_exec = NULL;
  g_autoptr(GAppInfo) app = NULL;
  GtkRecentManager *recent;
  GtkRecentData recent_data;
  const char *exec_name = NULL;
  static char *groups[2] = {"Graphics", NULL};

  app = g_app_info_get_default_for_type("image/png", TRUE);

  if (!app)
  {
    /* return early, as this would be an useless recent entry anyway. */
    return;
  }

  recent = gtk_recent_manager_get_default();

  exec_name = g_app_info_get_executable(app);
  app_exec = g_strjoin(" ", exec_name, "%u", NULL);

  recent_data.display_name = NULL;
  recent_data.description = NULL;
  recent_data.mime_type = "image/png";
  recent_data.app_name = "GNOME Screenshot";
  recent_data.app_exec = app_exec;
  recent_data.groups = groups;
  recent_data.is_private = FALSE;

  gtk_recent_manager_add_full(recent, self->priv->save_uri, &recent_data);
}

static void
screenshot_close_interactive_dialog(ScreenshotApplication *self)
{
  ScreenshotDialog *dialog = self->priv->dialog;
  save_folder_to_settings(self);
  gtk_widget_destroy(dialog->dialog);
  g_free(dialog);
}

static void
save_pixbuf_handle_success(ScreenshotApplication *self)
{
  set_recent_entry(self);

  if (screenshot_config->interactive)
  {
    screenshot_close_interactive_dialog(self);
  }
  else
  {
    g_application_release(G_APPLICATION(self));
  }
}

static void
save_pixbuf_handle_error(ScreenshotApplication *self,
                         GError *error)
{
  if (screenshot_config->interactive)
  {
    ScreenshotDialog *dialog = self->priv->dialog;

    screenshot_dialog_set_busy(dialog, FALSE);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS) &&
        !self->priv->should_overwrite)
    {
      g_autofree gchar *folder = screenshot_dialog_get_folder(dialog);
      g_autofree gchar *folder_uri = g_path_get_basename(folder);
      g_autofree gchar *folder_name = g_uri_unescape_string(folder_uri, NULL);
      g_autofree gchar *file_name = screenshot_dialog_get_filename(dialog);
      g_autofree gchar *detail = g_strdup_printf(_("A file named “%s” already exists in “%s”"),
                                                 file_name, folder_name);

      gint response = screenshot_show_dialog(GTK_WINDOW(dialog->dialog),
                                             GTK_MESSAGE_WARNING,
                                             GTK_BUTTONS_YES_NO,
                                             _("Overwrite existing file?"),
                                             detail);

      if (response == GTK_RESPONSE_YES)
      {
        self->priv->should_overwrite = TRUE;
        screenshot_save_to_file(self);

        return;
      }
    }
    else
    {
      screenshot_show_dialog(GTK_WINDOW(dialog->dialog),
                             GTK_MESSAGE_ERROR,
                             GTK_BUTTONS_OK,
                             _("Unable to capture a screenshot"),
                             _("Error creating file. Please choose another location and retry."));
    }

    gtk_widget_grab_focus(dialog->filename_entry);
  }
  else
  {
    g_critical("Unable to save the screenshot: %s", error->message);
    if (screenshot_config->play_sound)
      screenshot_play_sound_effect("dialog-error", _("Unable to capture a screenshot"));
    g_application_release(G_APPLICATION(self));
    if (screenshot_config->file != NULL)
      exit(EXIT_FAILURE);
  }
}

static void
save_pixbuf_ready_cb(GObject *source,
                     GAsyncResult *res,
                     gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  ScreenshotApplication *self = user_data;

  gdk_pixbuf_save_to_stream_finish(res, &error);

  if (error != NULL)
  {
    save_pixbuf_handle_error(self, error);
    return;
  }

  save_pixbuf_handle_success(self);
}

static void
find_out_writable_format_by_extension(gpointer data,
                                      gpointer user_data)
{
  GdkPixbufFormat *format = (GdkPixbufFormat *)data;
  gchar **name = (gchar **)user_data;
  g_auto(GStrv) extensions = gdk_pixbuf_format_get_extensions(format);
  gchar **ptr = extensions;

  while (*ptr != NULL)
  {
    if (g_strcmp0(*ptr, *name) == 0 &&
        gdk_pixbuf_format_is_writable(format) == TRUE)
    {
      g_free(*name);
      *name = gdk_pixbuf_format_get_name(format);
      break;
    }
    ptr++;
  }
}

static gboolean
is_png(gchar *format)
{
  if (g_strcmp0(format, "png") == 0)
    return TRUE;
  else
    return FALSE;
}

static gboolean
has_profile(ScreenshotApplication *self)
{
  if (self->priv->icc_profile_base64 != NULL)
    return TRUE;
  else
    return FALSE;
}

static void
save_with_description_and_profile(ScreenshotApplication *self,
                                  GFileOutputStream *os,
                                  gchar *format)
{
  gdk_pixbuf_save_to_stream_async(self->priv->screenshot,
                                  G_OUTPUT_STREAM(os),
                                  format, NULL,
                                  save_pixbuf_ready_cb, self,
                                  "icc-profile", self->priv->icc_profile_base64,
                                  "tEXt::Software", "gnome-screenshot",
                                  NULL);
}
static void
save_with_description(ScreenshotApplication *self,
                      GFileOutputStream *os,
                      gchar *format)
{
  gdk_pixbuf_save_to_stream_async(self->priv->screenshot,
                                  G_OUTPUT_STREAM(os),
                                  format, NULL,
                                  save_pixbuf_ready_cb, self,
                                  "tEXt::Software", "gnome-screenshot",
                                  NULL);
}

static void
save_with_no_profile_or_description(ScreenshotApplication *self,
                                    GFileOutputStream *os,
                                    gchar *format)
{
  gdk_pixbuf_save_to_stream_async(self->priv->screenshot,
                                  G_OUTPUT_STREAM(os),
                                  format, NULL,
                                  save_pixbuf_ready_cb, self,
                                  NULL);
}

static void
save_file_create_ready_cb(GObject *source,
                          GAsyncResult *res,
                          gpointer user_data)
{
  ScreenshotApplication *self = user_data;
  g_autoptr(GFileOutputStream) os = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *basename = g_file_get_basename(G_FILE(source));
  g_autofree gchar *format = NULL;
  gchar *extension = g_strrstr(basename, ".");
  GSList *formats = NULL;

  if (extension == NULL)
    extension = "png";
  else
    extension++;

  format = g_strdup(extension);

  formats = gdk_pixbuf_get_formats();
  g_slist_foreach(formats,
                  find_out_writable_format_by_extension,
                  (gpointer)&format);
  g_slist_free(formats);

  if (self->priv->should_overwrite)
    os = g_file_replace_finish(G_FILE(source), res, &error);
  else
    os = g_file_create_finish(G_FILE(source), res, &error);

  if (error != NULL)
  {
    save_pixbuf_handle_error(self, error);
    return;
  }

  if (is_png(format))
  {
    if (has_profile(self))
      save_with_description_and_profile(self, os, format);
    else
      save_with_description(self, os, format);
  }
  else
  {
    save_with_no_profile_or_description(self, os, format);
  }
}

static void
screenshot_save_to_file(ScreenshotApplication *self)
{
  g_autoptr(GFile) target_file = NULL;

  if (self->priv->dialog != NULL)
    screenshot_dialog_set_busy(self->priv->dialog, TRUE);

  target_file = g_file_new_for_uri(self->priv->save_uri);

  if (self->priv->should_overwrite)
  {
    g_file_replace_async(target_file,
                         NULL, FALSE,
                         G_FILE_CREATE_NONE,
                         G_PRIORITY_DEFAULT,
                         NULL,
                         save_file_create_ready_cb, self);
  }
  else
  {
    g_file_create_async(target_file,
                        G_FILE_CREATE_NONE,
                        G_PRIORITY_DEFAULT,
                        NULL,
                        save_file_create_ready_cb, self);
  }
  g_print("->saveok: %s\n", self->priv->save_uri);
}

static void
screenshot_back(ScreenshotApplication *self)
{
  screenshot_close_interactive_dialog(self);
  screenshot_show_interactive_dialog(self);
}

static void
screenshot_save_to_clipboard(ScreenshotApplication *self) // ham luu vao clipbo
{
  GtkClipboard *clipboard;

  clipboard = gtk_clipboard_get_for_display(gdk_display_get_default(),
                                            GDK_SELECTION_CLIPBOARD);

  if (!self->priv->screenshot)
    g_print("----clipnull\n");

  gtk_clipboard_set_image(clipboard, self->priv->screenshot);
  g_print("----clipboard\n");
}
/* Callback functions */

/* Check for Control-Q and quit if it was pressed */
static gboolean
keyPress(GtkWidget *widget, gpointer data)
{
  GdkEventKey *event = (GdkEventKey *)data;

  if (event->keyval == GDK_KEY_q && (event->state & GDK_CONTROL_MASK))
  {
    gtk_main_quit();
    return FALSE;
  }
  else
    return TRUE;
}

/* If the window has been resized, that resizes the scrolledwindow,
 * and we scale the image to the dimensions of the scrolledwindow so that
 * the scrollbars disappear again. Yuk! */
static gboolean
sizeChanged(GtkWidget *widget, GtkAllocation *allocation, gpointer data)
{
  GdkPixbuf *sourcePixbuf = data; /* As read from a file */
  GdkPixbuf *imagePixbuf;         /* pixbuf of the on-screen image */

  imagePixbuf = gtk_image_get_pixbuf(GTK_IMAGE(image));
  if (imagePixbuf == NULL)
  {
    g_message("Can't get on-screen pixbuf");
    return TRUE;
  }
  /* Recreate the displayed image if the image size has changed. */
  if (allocation->width != gdk_pixbuf_get_width(imagePixbuf) ||
      allocation->height != gdk_pixbuf_get_height(imagePixbuf))
  {

    gtk_image_set_from_pixbuf(
        GTK_IMAGE(image),
        gdk_pixbuf_scale_simple(sourcePixbuf,
                                allocation->width,
                                allocation->height,
                                GDK_INTERP_BILINEAR));
    g_object_unref(imagePixbuf); /* Free the old one */
  }

  return FALSE;
}
void zoom(GtkWidget *button,ScreenshotApplication *self){
  GtkWidget *window;
    GtkWidget *viewport;
    GdkPixbuf *sourcePixbuf = NULL; /* As read from a file */
    // char filename[200]= "/home/viet132pham/Pictures/Screenshot from 2022-01-08 22-58-57.png";
    // char *filename2 = "/home/viet132pham/Pictures/tuyen.png";
    int n = 1;
  gtk_init(&n , NULL);

    /* Make pixbuf, then make image from pixbuf because
     * gtk_image_new_from_file() doesn't flag errors */
      GError *error = NULL;
    // while(1)
    // {
      sourcePixbuf = gdk_pixbuf_new_from_file(self->priv->save_path, &error);
      if (sourcePixbuf == NULL)
      {
        g_message("%s", error->message);
      }
    // }

    /* On expose/resize, the image's pixbuf will be overwritten
     * but we will still need the original image so take a copy of it */
    image = gtk_image_new_from_pixbuf(gdk_pixbuf_copy(sourcePixbuf));

    viewport = gtk_scrolled_window_new(NULL, NULL);
    /* Saying "1x1" reduces the window's minumum size from 55x55 to 42x42. */
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(viewport), 1);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(viewport), 1);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "image1-gtk3");

    /* Quit if they ask the window manager to close the window */
    g_signal_connect(G_OBJECT(window), "destroy",
                     G_CALLBACK(gtk_main_quit), NULL);
    /* Quit on control-Q. */
    g_signal_connect(window, "key-press-event", G_CALLBACK(keyPress), NULL);

    /* When the window is resized, scale the image to fit */
    g_signal_connect(viewport, "size-allocate",
                     G_CALLBACK(sizeChanged), sourcePixbuf);

    /* The image is in a scrolled window container so that the main window
     * can be resized smaller than the current image. */
    gtk_container_add(GTK_CONTAINER(viewport), image);
    gtk_container_add(GTK_CONTAINER(window), viewport);

    //gtk_window_set_resizable(GTK_WINDOW(window), 1);
    /* Open the window the same size as the image */
    gtk_window_set_default_size(GTK_WINDOW(window),
                                gdk_pixbuf_get_width(sourcePixbuf),
                                gdk_pixbuf_get_height(sourcePixbuf));

    gtk_widget_show_all(window);

    gtk_main();
}
void create_open_image(ScreenshotApplication *self){
  // GtkWidget *grid;
  GtkWidget* button;
  GtkWidget* window;
 
  gtk_init(0, 0);

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  // grid = gtk_grid_new();
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

  gtk_window_set_title(GTK_WINDOW(window), "After_save");
  
  gtk_window_set_default_size(GTK_WINDOW(window), 400, 200);
  button = gtk_button_new_with_label ( "If you want open image: Click" );
  // gtk_grid_attach ( GTK_GRID ( grid ), button, 0, 0, 1, 1 );
  gtk_container_add(GTK_CONTAINER(window),button);
  printf("\nDAY LA FILE NAME : %s\n",self->priv->save_path);
  g_signal_connect ( button,    "clicked", G_CALLBACK ( zoom ),self);
  g_signal_connect(window, "destroy", gtk_main_quit, 0);
  gtk_widget_show_all(window);
  
  gtk_main();
}

static void
screenshot_dialog_response_cb(ScreenshotResponse response,
                              ScreenshotApplication *self)
{
  char command[120];
  switch (response)
  {
  case SCREENSHOT_RESPONSE_SAVE:
    /* update to the new URI */

    g_free(self->priv->save_uri);
    self->priv->save_uri = screenshot_dialog_get_uri(self->priv->dialog);
    screenshot_save_to_file(self);
    printf("->uri: %s\n", self->priv->save_uri);
    create_open_image(self);
    break;
  case SCREENSHOT_RESPONSE_COPY:
    screenshot_save_to_clipboard(self);
    break;
  case SCREENSHOT_RESPONSE_BACK:
    screenshot_back(self);
    break;
  case SCREENSHOT_RESPONSE_EDIT:
  g_free(self->priv->save_uri);
    self->priv->save_uri = screenshot_dialog_get_uri(self->priv->dialog);
    screenshot_save_to_file(self);
    sprintf(command,"pinta \"%s\"", self->priv->save_path);
    if(screenshot_config->pinta_check == FALSE){
      showArlert();
      exit(0);
    }else{
      if (fork() == 0)
    {
      if (system(command) < 0)
      {
        exit(-1);
      }
      exit(-1);
    }
    else
    {    }

    }
    
    break;
    
    break;
  default:
    g_assert_not_reached();
    break;
  }
//  printf("Check: %d\n", check); 
//  showArlert();
  return;
}

static void
build_filename_ready_cb(GObject *source,
                        GAsyncResult *res,
                        gpointer user_data)
{
  ScreenshotApplication *self = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *save_path = screenshot_build_filename_finish(res, &error);

  if (save_path != NULL)
  {
    g_autoptr(GFile) file = g_file_new_for_path(save_path);
    self->priv->save_uri = g_file_get_uri(file);
    self->priv->save_path = g_file_get_path(file);
    g_print("path: %s\n", g_file_get_path(file));
  }
  else
    self->priv->save_uri = NULL;

  /* now release the application */
  g_application_release(G_APPLICATION(self));

  if (error != NULL)
  {
    g_critical("Impossible to find a valid location to save the screenshot: %s",
               error->message);

    if (screenshot_config->interactive)
      screenshot_show_dialog(NULL,
                             GTK_MESSAGE_ERROR,
                             GTK_BUTTONS_OK,
                             _("Unable to capture a screenshot"),
                             _("Error creating file"));
    else
    {
      if (screenshot_config->play_sound)
        screenshot_play_sound_effect("dialog-error", _("Unable to capture a screenshot"));
      if (screenshot_config->file != NULL)
        exit(EXIT_FAILURE);
    }

    return;
  }
  if (screenshot_config->play_sound)
    screenshot_play_sound_effect(screenshot_config->sound, _("Screenshot taken"));

  if (screenshot_config->interactive)
  {
    self->priv->dialog = screenshot_dialog_new(self->priv->screenshot,
                                               self->priv->save_uri,
                                               (SaveScreenshotCallback)screenshot_dialog_response_cb,
                                               self);
  }
  else
  {
    g_application_hold(G_APPLICATION(self));
    screenshot_save_to_file(self);
  }
}

static void
finish_prepare_screenshot(ScreenshotApplication *self,
                          GdkRectangle *rectangle)
{
  GdkPixbuf *screenshot;

  screenshot = screenshot_get_pixbuf(rectangle);

  if (screenshot == NULL)
  {
    g_critical("Unable to capture a screenshot of any window");

    if (screenshot_config->interactive)
      screenshot_show_dialog(NULL,
                             GTK_MESSAGE_ERROR,
                             GTK_BUTTONS_OK,
                             _("Unable to capture a screenshot"),
                             _("All possible methods failed"));
    else
    {
      if (screenshot_config->play_sound)
        screenshot_play_sound_effect("dialog-error", _("Unable to capture a screenshot"));
    }

    g_application_release(G_APPLICATION(self));
    if (screenshot_config->file != NULL)
      exit(EXIT_FAILURE);

    return;
  }

  if (screenshot_config->take_window_shot)
  {
    switch (screenshot_config->border_effect[0])
    {
    case 's': /* shadow */
      screenshot_add_shadow(&screenshot);
      break;
    case 'b': /* border */
      screenshot_add_border(&screenshot);
      break;
    case 'v': /* vintage */
      screenshot_add_vintage(&screenshot);
      break;
    case 'n': /* none */
    default:
      break;
    }
  }

  self->priv->screenshot = screenshot;
  g_print("screenshot_config->copy_to_clipboard: %d\n", screenshot_config->copy_to_clipboard);

  if (screenshot_config->copy_to_clipboard)
  {
    self->priv->save_uri = g_build_filename("file:///tmp", "temp_file_clipboard.png", NULL);
    self->priv->should_overwrite = TRUE;
    // g_application_hold(G_APPLICATION(self));
    int status = system("rm -f /tmp/temp_file_clipboard.png");
    if (status < 0)
    {
      exit(-1);
    }
    g_application_hold(G_APPLICATION(self));
    screenshot_save_to_file(self);

    // g_application_release(G_APPLICATION(self));

    // g_application_release(G_APPLICATION(self));

    // g_application_hold(G_APPLICATION(self));

    // status = system("rm /tmp/temp_file_clipboard");
    // if (status < 0)
    // {
    //   exit(-1);
    // }
    // g_application_release(G_APPLICATION(self));

    if (screenshot_config->play_sound)
      screenshot_play_sound_effect(screenshot_config->sound, _("Screenshot taken"));

    if (screenshot_config->file == NULL)
    {
      g_application_release(G_APPLICATION(self));

      return;
    }
  }

  /* FIXME: apply the ICC profile according to the preferences.
   * org.gnome.ColorManager.GetProfileForWindow() does not exist anymore,
   * so we probably need to fetch the color profile of the screen where
   * the area/window was.
   *
   * screenshot_ensure_icc_profile (window);
   */
  if (screenshot_config->file != NULL)
  {
    self->priv->save_uri = g_file_get_uri(screenshot_config->file);

    self->priv->should_overwrite = TRUE;
    screenshot_save_to_file(self);
  }
  else
    screenshot_build_filename_async(screenshot_config->save_dir, NULL, build_filename_ready_cb, self);
}

static void
rectangle_found_cb(GdkRectangle *rectangle,
                   gpointer user_data)
{
  ScreenshotApplication *self = user_data;

  if (rectangle != NULL)
  {
    finish_prepare_screenshot(self, rectangle);
  }
  else
  {
    /* user dismissed the area selection, possibly show the dialog again */
    g_application_release(G_APPLICATION(self));

    if (screenshot_config->interactive)
      screenshot_show_interactive_dialog(self);
  }
}

static gboolean
prepare_screenshot_timeout(gpointer user_data)
{
  ScreenshotApplication *self = user_data;

  if (screenshot_config->take_area_shot)
    screenshot_select_area_async(rectangle_found_cb, self);
  else
    finish_prepare_screenshot(self, NULL);

  screenshot_save_config();

  return FALSE;
}

static void
screenshot_start(ScreenshotApplication *self)
{
  guint delay = screenshot_config->delay * 1000;

  /* hold the GApplication while doing the async screenshot op */
  g_application_hold(G_APPLICATION(self));

  if (screenshot_config->take_area_shot)
    delay = 0;

  /* HACK: give time to the dialog to actually disappear.
   * We don't have any way to tell when the compositor has finished
   * re-drawing.
   */
  if (delay == 0 && screenshot_config->interactive)
    delay = 200;

  if (delay > 0)
    g_timeout_add(delay,
                  prepare_screenshot_timeout,
                  self);
  else
    g_idle_add(prepare_screenshot_timeout, self);
}

static gboolean version_arg = FALSE;

static const GOptionEntry entries[] = {
    {"clipboard", 'c', 0, G_OPTION_ARG_NONE, NULL, N_("Send the grab directly to the clipboard"), NULL},
    {"window", 'w', 0, G_OPTION_ARG_NONE, NULL, N_("Grab a window instead of the entire screen"), NULL},
    {"area", 'a', 0, G_OPTION_ARG_NONE, NULL, N_("Grab an area of the screen instead of the entire screen"), NULL},
    {"include-border", 'b', 0, G_OPTION_ARG_NONE, NULL, N_("Include the window border with the screenshot"), NULL},
    {"remove-border", 'B', 0, G_OPTION_ARG_NONE, NULL, N_("Remove the window border from the screenshot"), NULL},
    {"include-pointer", 'p', 0, G_OPTION_ARG_NONE, NULL, N_("Include the pointer with the screenshot"), NULL},
    {"delay", 'd', 0, G_OPTION_ARG_INT, NULL, N_("Take screenshot after specified delay [in seconds]"), N_("seconds")},
    {"border-effect", 'e', 0, G_OPTION_ARG_STRING, NULL, N_("Effect to add to the border (shadow, border, vintage or none)"), N_("effect")},
    {"interactive", 'i', 0, G_OPTION_ARG_NONE, NULL, N_("Interactively set options"), NULL},
    {"file", 'f', 0, G_OPTION_ARG_FILENAME, NULL, N_("Save screenshot directly to this file"), N_("filename")},
    {"version", 0, 0, G_OPTION_ARG_NONE, &version_arg, N_("Print version information and exit"), NULL},
    {NULL},
};

static gint
screenshot_application_handle_local_options(GApplication *app,
                                            GVariantDict *options)
{
  if (version_arg)
  {
    g_print("%s %s\n", g_get_application_name(), VERSION);
    exit(EXIT_SUCCESS);
  }

  /* Start headless instances in non-unique mode */
  if (!g_variant_dict_contains(options, "interactive"))
  {
    GApplicationFlags old_flags;

    old_flags = g_application_get_flags(app);
    if ((old_flags & G_APPLICATION_IS_SERVICE) == 0)
    {
      g_application_set_flags(app, old_flags | G_APPLICATION_NON_UNIQUE);
    }
  }

  return -1;
}

static gint
screenshot_application_command_line(GApplication *app,
                                    GApplicationCommandLine *command_line)
{
  ScreenshotApplication *self = SCREENSHOT_APPLICATION(app);
  gboolean clipboard_arg = FALSE;
  gboolean window_arg = FALSE;
  gboolean area_arg = FALSE;
  gboolean include_border_arg = FALSE;
  gboolean disable_border_arg = FALSE;
  gboolean include_pointer_arg = FALSE;
  gboolean interactive_arg = FALSE;
  gchar *border_effect_arg = NULL;
  guint delay_arg = 0;
  gchar *file_arg = NULL;
  GVariantDict *options;
  gint exit_status = EXIT_SUCCESS;
  gboolean res;

  options = g_application_command_line_get_options_dict(command_line);
  g_variant_dict_lookup(options, "clipboard", "b", &clipboard_arg);
  g_variant_dict_lookup(options, "window", "b", &window_arg);
  g_variant_dict_lookup(options, "area", "b", &area_arg);
  g_variant_dict_lookup(options, "include-border", "b", &include_border_arg);
  g_variant_dict_lookup(options, "remove-border", "b", &disable_border_arg);
  g_variant_dict_lookup(options, "include-pointer", "b", &include_pointer_arg);
  g_variant_dict_lookup(options, "interactive", "b", &interactive_arg);
  g_variant_dict_lookup(options, "border-effect", "&s", &border_effect_arg);
  g_variant_dict_lookup(options, "delay", "i", &delay_arg);
  g_variant_dict_lookup(options, "file", "^&ay", &file_arg);

  res = screenshot_config_parse_command_line(clipboard_arg,
                                             window_arg,
                                             area_arg,
                                             include_border_arg,
                                             disable_border_arg,
                                             include_pointer_arg,
                                             border_effect_arg,
                                             delay_arg,
                                             interactive_arg,
                                             file_arg);
  if (!res)
  {
    exit_status = EXIT_FAILURE;
    goto out;
  }

  /* interactive mode: trigger the dialog and wait for the response */
  if (screenshot_config->interactive)
    g_application_activate(app);
  else
    screenshot_start(self);

out:
  return exit_status;
}

static void
screenshot_show_interactive_dialog(ScreenshotApplication *self)
{
  screenshot_interactive_dialog_new((CaptureClickedCallback)screenshot_start, self);
}

static void
action_quit(GSimpleAction *action,
            GVariant *parameter,
            gpointer user_data)
{
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(user_data));
  gtk_widget_destroy(g_list_nth_data(windows, 0));
}

static void
action_help(GSimpleAction *action,
            GVariant *parameter,
            gpointer user_data)
{
  GList *windows = gtk_application_get_windows(GTK_APPLICATION(user_data));
  screenshot_display_help(g_list_nth_data(windows, 0));
}

static void
action_about(GSimpleAction *action,
             GVariant *parameter,
             gpointer user_data)
{
  const gchar *authors[] = {
      "Emmanuele Bassi",
      "Jonathan Blandford",
      "Cosimo Cecchi",
      NULL};

  GList *windows = gtk_application_get_windows(GTK_APPLICATION(user_data));
  gtk_show_about_dialog(GTK_WINDOW(g_list_nth_data(windows, 0)),
                        "version", VERSION,
                        "authors", authors,
                        "program-name", _("Screenshot"),
                        "comments", _("Save images of your screen or individual windows"),
                        "logo-icon-name", SCREENSHOT_ICON_NAME,
                        "translator-credits", _("translator-credits"),
                        "license-type", GTK_LICENSE_GPL_2_0,
                        "wrap-license", TRUE,
                        NULL);
}

static void
action_screen_shot(GSimpleAction *action,
                   GVariant *parameter,
                   gpointer user_data)
{
  ScreenshotApplication *self = SCREENSHOT_APPLICATION(user_data);

  screenshot_config_parse_command_line(FALSE, /* clipboard */
                                       FALSE, /* window */
                                       FALSE, /* area */
                                       FALSE, /* include border */
                                       FALSE, /* disable border */
                                       FALSE, /* include pointer */
                                       NULL,  /* border effect */
                                       0,     /* delay */
                                       FALSE, /* interactive */
                                       NULL); /* file */
  screenshot_start(self);
}

static void
action_window_shot(GSimpleAction *action,
                   GVariant *parameter,
                   gpointer user_data)
{
  ScreenshotApplication *self = SCREENSHOT_APPLICATION(user_data);

  screenshot_config_parse_command_line(FALSE, /* clipboard */
                                       TRUE,  /* window */
                                       FALSE, /* area */
                                       FALSE, /* include border */
                                       FALSE, /* disable border */
                                       FALSE, /* include pointer */
                                       NULL,  /* border effect */
                                       0,     /* delay */
                                       FALSE, /* interactive */
                                       NULL); /* file */
  screenshot_start(self);
}

static GActionEntry action_entries[] = {
    {"about", action_about, NULL, NULL, NULL},
    {"help", action_help, NULL, NULL, NULL},
    {"quit", action_quit, NULL, NULL, NULL},
    {"screen-shot", action_screen_shot, NULL, NULL, NULL},
    {"window-shot", action_window_shot, NULL, NULL, NULL}};

static void
screenshot_application_startup(GApplication *app)
{
  g_autoptr(GMenuModel) menu = NULL;
  g_autoptr(GtkBuilder) builder = NULL;
  ScreenshotApplication *self = SCREENSHOT_APPLICATION(app);

  G_APPLICATION_CLASS(screenshot_application_parent_class)->startup(app);

  screenshot_load_config();

  g_set_application_name(_("Screenshot"));
  gtk_window_set_default_icon_name(SCREENSHOT_ICON_NAME);

  g_action_map_add_action_entries(G_ACTION_MAP(self), action_entries,
                                  G_N_ELEMENTS(action_entries), self);

  builder = gtk_builder_new();
  gtk_builder_add_from_resource(builder, "/org/gnome/screenshot/screenshot-app-menu.ui", NULL);
  menu = G_MENU_MODEL(gtk_builder_get_object(builder, "app-menu"));
  gtk_application_set_app_menu(GTK_APPLICATION(app), menu);
}

static void
screenshot_application_activate(GApplication *app)
{
  GtkWindow *window;

  window = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (window != NULL)
  {
    gtk_window_present(GTK_WINDOW(window));
    return;
  }

  screenshot_config->interactive = TRUE;
  screenshot_show_interactive_dialog(SCREENSHOT_APPLICATION(app));
}

static void
screenshot_application_finalize(GObject *object)
{
  ScreenshotApplication *self = SCREENSHOT_APPLICATION(object);
  if (screenshot_config->copy_to_clipboard)
  {
    int status = system("xclip -selection clipboard -t image/png -i /tmp/temp_file_clipboard.png && rm -f  /tmp/temp_file_clipboard.png");
    // printf("\n%d\n", status);
    if (status < 0)
    {
      exit(-1);
    }
  }
  g_clear_object(&self->priv->screenshot);
  g_free(self->priv->icc_profile_base64);
  g_free(self->priv->save_uri);

  G_OBJECT_CLASS(screenshot_application_parent_class)->finalize(object);
}

static void
screenshot_application_class_init(ScreenshotApplicationClass *klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS(klass);
  GApplicationClass *aclass = G_APPLICATION_CLASS(klass);

  oclass->finalize = screenshot_application_finalize;

  aclass->handle_local_options = screenshot_application_handle_local_options;
  aclass->command_line = screenshot_application_command_line;
  aclass->startup = screenshot_application_startup;
  aclass->activate = screenshot_application_activate;

  g_type_class_add_private(klass, sizeof(ScreenshotApplicationPriv));
}

static void
screenshot_application_init(ScreenshotApplication *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, SCREENSHOT_TYPE_APPLICATION,
                                           ScreenshotApplicationPriv);

  g_application_add_main_option_entries(G_APPLICATION(self), entries);
}

GApplication *
screenshot_application_new(void)
{
  return g_object_new(SCREENSHOT_TYPE_APPLICATION,
                      "application-id", "org.gnome.Screenshot",
                      "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
                      NULL);
}
