/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "illumination.h"
#define OBJECT_PATH "/org/vermeer/Fingerprint/Illumination0"
#define INTERFACE "org.vermeer.Fingerprint.Illumination1"
struct _VermeerIllumination {
  GDBusConnection *bus;
  guint registration;
  GAsyncQueue *contacts;
  GMutex lock;
  guint64 generation;
};
struct contact { guint64 generation; gboolean present; };
static const char xml[] =
  "<node><interface name='" INTERFACE "'>"
  "<method name='Contact'><arg type='t' direction='in'/><arg type='b' direction='in'/></method>"
  "<signal name='TargetRequested'><arg type='t'/><arg type='b'/></signal>"
  "</interface></node>";
/* Only a user with an active local graphical session may report contact.
 * User-service processes need not belong to the session scope themselves.
 * These messages only trigger hardware acquisition; they are not match results.
 */
static gboolean active_local_session (GDBusConnection *bus, const char *sender)
{
  g_autoptr(GVariant) uid_reply = NULL, user_reply = NULL, display_reply = NULL;
  g_autoptr(GVariant) properties = NULL, dictionary = NULL, boxed = NULL, display = NULL;
  guint32 uid;
  const gchar *user_path, *session_name, *path;
  gboolean active = FALSE, remote = TRUE;
  uid_reply = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixUser",
      g_variant_new ("(s)", sender), G_VARIANT_TYPE ("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  if (!uid_reply) return FALSE;
  g_variant_get (uid_reply, "(u)", &uid);
  user_reply = g_dbus_connection_call_sync (bus, "org.freedesktop.login1",
      "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "GetUser",
      g_variant_new ("(u)", uid), G_VARIANT_TYPE ("(o)"),
      G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  if (!user_reply) return FALSE;
  g_variant_get (user_reply, "(&o)", &user_path);
  display_reply = g_dbus_connection_call_sync (bus, "org.freedesktop.login1", user_path,
      "org.freedesktop.DBus.Properties", "Get",
      g_variant_new ("(ss)", "org.freedesktop.login1.User", "Display"),
      G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  if (!display_reply) return FALSE;
  g_variant_get (display_reply, "(@v)", &boxed);
  display = g_variant_get_variant (boxed);
  if (!g_variant_is_of_type (display, G_VARIANT_TYPE ("(so)"))) return FALSE;
  g_variant_get (display, "(&s&o)", &session_name, &path);
  if (!*session_name || g_str_equal (path, "/")) return FALSE;
  properties = g_dbus_connection_call_sync (bus, "org.freedesktop.login1", path,
      "org.freedesktop.DBus.Properties", "GetAll",
      g_variant_new ("(s)", "org.freedesktop.login1.Session"),
      G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
  if (!properties) return FALSE;
  g_variant_get (properties, "(@a{sv})", &dictionary);
  g_variant_lookup (dictionary, "Active", "b", &active);
  g_variant_lookup (dictionary, "Remote", "b", &remote);
  return active && !remote;
}
static void method_call (GDBusConnection *bus, const gchar *sender,
                         const gchar *path, const gchar *interface,
                         const gchar *method, GVariant *parameters,
                         GDBusMethodInvocation *invocation, gpointer data)
{
  VermeerIllumination *ui = data;
  guint64 generation;
  gboolean present;
  (void)path; (void)interface;
  if (!g_str_equal (method, "Contact")) {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
        G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown illumination method"); return;
  }
  g_variant_get (parameters, "(tb)", &generation, &present);
  g_mutex_lock (&ui->lock);
  gboolean current = generation && generation == ui->generation;
  g_mutex_unlock (&ui->lock);
  if (!current && !present) {
    /* A finger release can arrive after the result ended this generation. */
    g_dbus_method_invocation_return_value (invocation, NULL); return;
  }
  if (!current || !active_local_session (bus, sender)) {
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
        G_DBUS_ERROR_ACCESS_DENIED, "No active local fingerprint interaction"); return;
  }
  struct contact *contact = g_new (struct contact, 1);
  *contact = (struct contact){ generation, present };
  /* Bounded queue, including input from a malfunctioning session UI. */
  if (g_async_queue_length (ui->contacts) >= 32) g_free (contact);
  else g_async_queue_push (ui->contacts, contact);
  g_dbus_method_invocation_return_value (invocation, NULL);
}
static const GDBusInterfaceVTable vtable = { .method_call = method_call };
VermeerIllumination *vermeer_illumination_new (GError **error)
{
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml (xml, error);
  if (!info) return NULL;
  VermeerIllumination *ui = g_new0 (VermeerIllumination, 1);
  g_mutex_init (&ui->lock);
  ui->contacts = g_async_queue_new_full (g_free);
  ui->bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (ui->bus) ui->registration = g_dbus_connection_register_object (ui->bus,
      OBJECT_PATH, info->interfaces[0], &vtable, ui, NULL, error);
  if (!ui->registration) { vermeer_illumination_free (ui); return NULL; }
  return ui;
}
void vermeer_illumination_free (VermeerIllumination *ui)
{
  if (!ui) return;
  if (ui->registration) g_dbus_connection_unregister_object (ui->bus, ui->registration);
  g_clear_object (&ui->bus);
  g_async_queue_unref (ui->contacts);
  g_mutex_clear (&ui->lock);
  g_free (ui);
}
struct notification { GDBusConnection *bus; guint64 generation; gboolean show; };
static gboolean notify_main (gpointer data)
{
  struct notification *notification = data;
  g_dbus_connection_emit_signal (notification->bus, NULL, OBJECT_PATH, INTERFACE,
      "TargetRequested", g_variant_new ("(tb)", notification->generation, notification->show), NULL);
  g_object_unref (notification->bus); g_free (notification);
  return G_SOURCE_REMOVE;
}
static void notify (VermeerIllumination *ui, guint64 generation, gboolean show)
{
  struct notification *notification = g_new (struct notification, 1);
  *notification = (struct notification){ g_object_ref (ui->bus), generation, show };
  g_main_context_invoke (NULL, notify_main, notification);
}
void vermeer_illumination_begin (VermeerIllumination *ui)
{
  g_mutex_lock (&ui->lock);
  ui->generation = MAX ((guint64)g_get_monotonic_time (), ui->generation + 1);
  guint64 generation = ui->generation;
  g_mutex_unlock (&ui->lock);
  notify (ui, generation, TRUE);
}
void vermeer_illumination_end (VermeerIllumination *ui)
{
  g_mutex_lock (&ui->lock);
  guint64 generation = ui->generation; ui->generation = 0;
  g_mutex_unlock (&ui->lock);
  notify (ui, generation, FALSE);
  struct contact *contact;
  while ((contact = g_async_queue_try_pop (ui->contacts))) g_free (contact);
}
gboolean vermeer_illumination_wait (VermeerIllumination *ui, gboolean present,
                                   GCancellable *cancellable, gint64 deadline,
                                   GError **error)
{
  while (g_get_monotonic_time () < deadline) {
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) return FALSE;
    struct contact *contact = g_async_queue_timeout_pop (ui->contacts, 100000);
    if (!contact) continue;
    g_mutex_lock (&ui->lock);
    gboolean found = contact->generation == ui->generation && contact->present == present;
    g_mutex_unlock (&ui->lock);
    g_free (contact);
    if (found) return TRUE;
  }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Fingerprint contact timed out");
  return FALSE;
}
