/*
 * Copyright © 2014 Patrick Griffis
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <string.h>
#include "config.h"

#include "gnotificationbackend.h"

#include "gapplication.h"
#include "giomodule-priv.h"
#include "gnotification-private.h"
#include "gsocketclient.h"
#include "giostream.h"

#define G_TYPE_GNTP_NOTIFICATION_BACKEND  (g_gntp_notification_backend_get_type ())
#define G_GNTP_NOTIFICATION_BACKEND(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_GNTP_NOTIFICATION_BACKEND, GGNTPNotificationBackend))

#define GNTP_HOST "localhost"
#define GNTP_PORT 23053

/*
 * GNTP (Growl Network Transport Protocol) backend
 *
 * Note that this backend does not support more advanced
 * growl features such as sending notifications over the network
 * or password support.
 *
 */

typedef struct _GGNTPNotificationBackend GGNTPNotificationBackend;
typedef GNotificationBackendClass GGNTPNotificationBackendClass;

struct _GGNTPNotificationBackend
{
  GNotificationBackend parent;
  gboolean is_registered;
  gboolean is_registering;
  GSList *notifications;
};

typedef struct
{
  GGNTPNotificationBackend *backend;
  const gchar *title;
  const gchar *text;
  gboolean urgent;
} GGNTPNotification;

GType g_gntp_notification_backend_get_type (void);

G_DEFINE_TYPE_WITH_CODE (GGNTPNotificationBackend, g_gntp_notification_backend, G_TYPE_NOTIFICATION_BACKEND,
                          _g_io_modules_ensure_extension_points_registered ();
  g_io_extension_point_implement (G_NOTIFICATION_BACKEND_EXTENSION_POINT_NAME,
                                 g_define_type_id, "gntp", 0))

static void
gntp_notification_free (gpointer data)
{
  GGNTPNotification *notification = (GGNTPNotification*)data;
  
  if (notification->title)
    g_free ((gchar*)notification->title);
  if (notification->text)
    g_free ((gchar*)notification->text);
  if (notification)
    g_free (notification);
}

static GGNTPNotification *
gntp_notification_new (GGNTPNotificationBackend *backend, GNotification *notification)
{
  GGNTPNotification *ret;

  ret = (GGNTPNotification*)g_malloc0 (sizeof(GGNTPNotification));
  ret->title = g_strdup (g_notification_get_title (notification));
  ret->text = g_strdup (g_notification_get_body (notification));
  ret->urgent = g_notification_get_urgent (notification);
  ret->backend = backend;

  return ret;
}

static inline void
gntp_write (GOutputStream *output, gchar *text)
{
  gchar formatted_text[2048];

  g_snprintf (formatted_text, sizeof(formatted_text), "%s\r\n", text);
  
  g_output_stream_write (output, formatted_text, strlen(formatted_text), NULL, NULL);
}

static void
gntp_notify_callback (GObject *source, GAsyncResult *res, gpointer userdata)
{
  GGNTPNotification *notification = (GGNTPNotification*)userdata;
  GApplication *app = G_NOTIFICATION_BACKEND(notification->backend)->application;
  GSocketConnection *conn;
  GOutputStream *output;
  GError *error = NULL;
  gchar app_str[512];
  gchar notify_title[512];
  gchar notify_text[1024];

  conn = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT(source), res, &error);
  if (error)
  {
    g_warning ("Could not connect to GNTP service.");
    g_error_free (error);
    goto notify_cleanup;
  }

  output = g_io_stream_get_output_stream (G_IO_STREAM(conn));

  g_snprintf (app_str, sizeof(app_str), "Application-Name: %s", g_application_get_application_id (app));
  g_snprintf (notify_title, sizeof(notify_title), "Notification-Title: %s", notification->title);
  g_snprintf (notify_text, sizeof(notify_text), "Notification-Text: %s", notification->text);
  if (notification->icon)
      g_snprintf (notify_text, sizeof(notify_text), "Notification-Text: %s", notification->text);

  gntp_write (output, "GNTP/1.0 NOTIFY NONE");
  gntp_write (output, app_str);
  gntp_write (output, "Notification-Name: Notification");
  gntp_write (output, notify_title);
  gntp_write (output, notify_text);
  if (notification->urgent)
    gntp_write (output, "Notification-Priority: 2");
  /* TODO: Icon */
  gntp_write (output, "");

  g_output_stream_close (output, NULL, NULL);
  /* Don't currently check if the service doesn't like our message, we don't plan on resending */

notify_cleanup:
  g_object_unref (source);
  gntp_notification_free (notification);
}

static void
gntp_notify (GGNTPNotification *notification)
{
  GSocketClient *client;
  
  client = g_socket_client_new ();
  g_socket_client_connect_to_host_async (client, GNTP_HOST, GNTP_PORT, NULL,
                                    gntp_notify_callback, notification);
}

static void
gntp_register_callback (GObject *source, GAsyncResult *res, gpointer userdata)
{
  GGNTPNotificationBackend *backend = G_GNTP_NOTIFICATION_BACKEND(userdata);
  GApplication *app = G_NOTIFICATION_BACKEND(userdata)->application;
  GSocketConnection *conn;
  GOutputStream *output;
  GInputStream *input;
  gchar input_buf[12]; /* Just enough to see its ok */
  gchar app_str[512];
  GError *error = NULL;
  
  conn = g_socket_client_connect_to_host_finish (G_SOCKET_CLIENT(source), res, &error);
  backend->is_registering = FALSE;
  if (error)
  {
    g_warning ("Could not connect to GNTP service.");
    g_error_free (error);
    g_object_unref (source);
    return;
  }

  output = g_io_stream_get_output_stream (G_IO_STREAM(conn));

  g_snprintf (app_str, sizeof(app_str), "Application-Name: %s",
            g_application_get_application_id (app));

  gntp_write (output, "GNTP/1.0 REGISTER NONE");
  gntp_write (output, app_str);
  /* TODO: Application icon */
  gntp_write (output, "Notifications-Count: 1");
  gntp_write (output, "");
  gntp_write (output, "Notification-Name: Notification");
  gntp_write (output, "Notification-Enabled: True");
  gntp_write (output, "");
  
  g_output_stream_close (output, NULL, NULL);

  /* Verify registration was OK */
  input = g_io_stream_get_input_stream (G_IO_STREAM(conn));
  g_input_stream_read (input, input_buf, sizeof(input_buf), NULL, &error);
  if (error == NULL && g_strstr_len (input_buf, sizeof(input_buf), "-OK") != NULL)
  {
    backend->is_registered = TRUE;

    /* Send any queued up notifications */
    while (backend->notifications)
    {
      GGNTPNotification *notification = backend->notifications->data;
      
      gntp_notify (notification); /* Notification is freed in the callback */

      backend->notifications = g_slist_next (backend->notifications);
    }
  }
  else
  {
    g_warning ("GNTP registration failed.");
    g_warning (error->message);
    g_error_free (error);
  }

  g_input_stream_close (input, NULL, NULL);
  g_object_unref (source);
}

/* TODO: The default action can be supported but it requires
 * running a server for the callback to connect to. */

static void
g_gntp_notification_backend_dispose (GObject *object)
{
  GGNTPNotificationBackend *backend = G_GNTP_NOTIFICATION_BACKEND (object);

  if (backend->notifications)
    g_slist_free_full (backend->notifications, gntp_notification_free);

  G_OBJECT_CLASS (g_gntp_notification_backend_parent_class)->dispose (object);
}

static gboolean
g_gntp_notification_backend_is_supported (void)
{
  /* To avoid an unnecessary synchronous call to check for
   * the growl daemon, this function always succeeds. A
   * warning will be printed when sending the first notification fails.
   */
  return TRUE;
}

static void
g_gntp_notification_backend_send_notification (GNotificationBackend *backend,
                                              const gchar          *id,
                                              GNotification        *notification)
{
  GGNTPNotificationBackend *self = G_GNTP_NOTIFICATION_BACKEND(backend);
  GGNTPNotification *gntp_notification = gntp_notification_new (self, notification);
  GSocketClient *client;

  if (!self->is_registered)
  {
    /* Queue for later */
    self->notifications = g_slist_append (self->notifications, gntp_notification);

    if (!self->is_registering)
    {
      self->is_registering = TRUE;
      client = g_socket_client_new ();
      g_socket_client_connect_to_host_async (client, GNTP_HOST, GNTP_PORT, NULL,
                                        gntp_register_callback, self);
    }
  }
  else
  {
    gntp_notify (gntp_notification);
  }
}

static void
g_gntp_notification_backend_withdraw_notification (GNotificationBackend *backend,
                                                  const gchar          *id)
{
  /* Not implemented */
}

static void
g_gntp_notification_backend_init (GGNTPNotificationBackend *backend)
{
  backend->is_registered = FALSE;
  backend->is_registering = FALSE;
  backend->notifications = NULL;
}

static void
g_gntp_notification_backend_class_init (GGNTPNotificationBackendClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GNotificationBackendClass *backend_class = G_NOTIFICATION_BACKEND_CLASS (class);

  object_class->dispose = g_gntp_notification_backend_dispose;

  backend_class->is_supported = g_gntp_notification_backend_is_supported;
  backend_class->send_notification = g_gntp_notification_backend_send_notification;
  backend_class->withdraw_notification = g_gntp_notification_backend_withdraw_notification;
}
