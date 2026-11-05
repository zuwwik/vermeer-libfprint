/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <unistd.h>
#include "../libfprint/drivers/goodixvermeer/illumination.c"

static const char mock_xml[] =
  "<node><interface name='org.freedesktop.login1.Manager'>"
  "<method name='GetUser'><arg type='u' direction='in'/><arg type='o' direction='out'/></method>"
  "</interface><interface name='org.freedesktop.login1.User'>"
  "<property name='Display' type='(so)' access='read'/></interface>"
  "<interface name='org.freedesktop.login1.Session'>"
  "<property name='Active' type='b' access='read'/><property name='Remote' type='b' access='read'/>"
  "</interface></node>";
static gint active = 1, remote, no_display;
static GMutex ready_lock;
static GCond ready_cond;
static gboolean ready;
static GMainLoop *mock_loop;
static char *address;
static void mock_call (GDBusConnection *bus, const char *sender, const char *path,
                       const char *interface, const char *method, GVariant *params,
                       GDBusMethodInvocation *invocation, gpointer unused)
{
  (void)bus;(void)sender;(void)path;(void)interface;(void)unused;
  g_assert_cmpstr (method, ==, "GetUser");
  guint32 uid; g_variant_get (params, "(u)", &uid);
  g_assert_cmpuint (uid, ==, getuid ());
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(o)", "/mock/user"));
}
static GVariant *mock_property (GDBusConnection *bus, const char *sender, const char *path,
                                const char *interface, const char *property, GError **error, gpointer unused)
{
  (void)bus;(void)sender;(void)path;(void)interface;(void)error;(void)unused;
  if (g_str_equal (property, "Display")) return g_variant_new ("(so)",
      g_atomic_int_get (&no_display) ? "" : "test", g_atomic_int_get (&no_display) ? "/" : "/mock/session");
  if (g_str_equal (property, "Active")) return g_variant_new_boolean (g_atomic_int_get (&active));
  if (g_str_equal (property, "Remote")) return g_variant_new_boolean (g_atomic_int_get (&remote));
  g_assert_not_reached ();
}
static const GDBusInterfaceVTable mock_vtable = { .method_call = mock_call, .get_property = mock_property };
static gpointer mock_thread (gpointer unused)
{
  (void)unused;
  g_autoptr(GMainContext) context = g_main_context_new ();
  g_main_context_push_thread_default (context);
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync (address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_assert_no_error (error);
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml (mock_xml, &error);
  g_assert_no_error (error);
  const char *paths[] = { "/org/freedesktop/login1", "/mock/user", "/mock/session" };
  for (guint i = 0; i < 3; i++) {
    g_assert_cmpuint (g_dbus_connection_register_object (bus, paths[i], info->interfaces[i],
                                                        &mock_vtable, NULL, NULL, &error), >, 0);
    g_assert_no_error (error);
  }
  g_autoptr(GVariant) owned = g_dbus_connection_call_sync (bus, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new ("(su)", "org.freedesktop.login1", 0), G_VARIANT_TYPE ("(u)"), 0, 1000, NULL, &error);
  g_assert_no_error (error); g_assert_nonnull (owned);
  g_mutex_lock (&ready_lock); mock_loop = g_main_loop_new (context, FALSE); ready = TRUE;
  g_cond_signal (&ready_cond); g_mutex_unlock (&ready_lock);
  g_main_loop_run (mock_loop);
  g_main_loop_unref (mock_loop);
  g_dbus_connection_close_sync (bus, NULL, NULL);
  g_main_context_pop_thread_default (context);
  return NULL;
}
struct call_result { gboolean done; GError *error; };
static void call_done (GObject *object, GAsyncResult *result, gpointer data)
{
  struct call_result *call = data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &call->error);
  call->done = TRUE;
}
static void contact_call (GDBusConnection *client, VermeerIllumination *ui,
                          guint64 generation, gboolean present, gboolean accepted)
{
  struct call_result call = {};
  g_dbus_connection_call (client, g_dbus_connection_get_unique_name (ui->bus), OBJECT_PATH,
      INTERFACE, "Contact", g_variant_new ("(tb)", generation, present), G_VARIANT_TYPE_UNIT,
      0, 3000, NULL, call_done, &call);
  while (!call.done) g_main_context_iteration (NULL, TRUE);
  if (accepted) g_assert_no_error (call.error);
  else g_assert_error (call.error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED);
  g_clear_error (&call.error);
}
struct snapshot_result { gboolean done; guint64 generation; };
static void snapshot_done (GObject *object, GAsyncResult *result, gpointer data)
{
  struct snapshot_result *snapshot = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (object), result, &error);
  g_assert_no_error (error);
  g_autoptr(GVariant) value = NULL;
  g_variant_get (reply, "(v)", &value);
  snapshot->generation = g_variant_get_uint64 (value);
  snapshot->done = TRUE;
}
static void assert_snapshot (GDBusConnection *client, VermeerIllumination *ui, guint64 expected)
{
  struct snapshot_result snapshot = {};
  g_dbus_connection_call (client, g_dbus_connection_get_unique_name (ui->bus), OBJECT_PATH,
      "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", INTERFACE, "ActiveGeneration"),
      G_VARIANT_TYPE ("(v)"), 0, 1000, NULL, snapshot_done, &snapshot);
  while (!snapshot.done) g_main_context_iteration (NULL, TRUE);
  g_assert_cmpuint (snapshot.generation, ==, expected);
}
static guint scan_events;
static guint64 scan_generation;
static void scan_signal (GDBusConnection *bus, const char *sender, const char *path,
                         const char *interface, const char *signal, GVariant *params, gpointer data)
{
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal; (void)data;
  g_variant_get (params, "(t)", &scan_generation); scan_events++;
}
static gboolean signal_timeout (gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_autoptr(GTestDBus) test_bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (test_bus);
  address = g_strdup (g_test_dbus_get_bus_address (test_bus));
  g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", address, TRUE);
  GThread *thread = g_thread_new ("mock-login1", mock_thread, NULL);
  g_mutex_lock (&ready_lock);
  while (!ready) g_cond_wait (&ready_cond, &ready_lock);
  g_mutex_unlock (&ready_lock);
  g_autoptr(GError) error = NULL;
  VermeerIllumination *ui = vermeer_illumination_new (&error);
  g_assert_no_error (error); g_assert_nonnull (ui);
  g_autoptr(GDBusConnection) client = g_dbus_connection_new_for_address_sync (address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_assert_no_error (error);
  assert_snapshot (client, ui, 0);
  contact_call (client, ui, 123, TRUE, FALSE);
  vermeer_illumination_begin (ui);
  guint64 generation = ui->generation;
  assert_snapshot (client, ui, generation);
  contact_call (client, ui, generation, TRUE, TRUE);
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  g_assert_true (vermeer_illumination_wait (ui, TRUE, cancellable,
      g_get_monotonic_time () + G_TIME_SPAN_SECOND, &error)); g_assert_no_error (error);
  guint subscription = g_dbus_connection_signal_subscribe (client,
      g_dbus_connection_get_unique_name (ui->bus), INTERFACE, "ScanComplete", OBJECT_PATH,
      NULL, G_DBUS_SIGNAL_FLAGS_NONE, scan_signal, NULL, NULL);
  vermeer_illumination_sample_done (ui);
  gboolean timed_out = FALSE;
  guint timer = g_timeout_add (1000, signal_timeout, &timed_out);
  while (!scan_events && !timed_out) g_main_context_iteration (NULL, TRUE);
  if (!timed_out) g_source_remove (timer);
  g_assert_cmpuint (scan_events, ==, 1); g_assert_cmpuint (scan_generation, ==, generation);
  g_assert_cmpuint (ui->generation, ==, generation);
  g_dbus_connection_signal_unsubscribe (client, subscription);
  g_atomic_int_set (&active, 0); contact_call (client, ui, generation, TRUE, FALSE);
  g_atomic_int_set (&active, 1); g_atomic_int_set (&remote, 1);
  contact_call (client, ui, generation, TRUE, FALSE);
  g_atomic_int_set (&remote, 0); g_atomic_int_set (&no_display, 1);
  contact_call (client, ui, generation, TRUE, FALSE);
  g_atomic_int_set (&no_display, 0);
  contact_call (client, ui, generation+1, TRUE, FALSE);
  contact_call (client, ui, generation, FALSE, TRUE);
  g_assert_true (vermeer_illumination_wait (ui, FALSE, cancellable,
      g_get_monotonic_time () + G_TIME_SPAN_SECOND, &error)); g_assert_no_error (error);
  g_cancellable_cancel (cancellable);
  g_assert_false (vermeer_illumination_wait (ui, TRUE, cancellable,
      g_get_monotonic_time () + G_TIME_SPAN_SECOND, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error (&error);
  vermeer_illumination_end (ui);
  assert_snapshot (client, ui, 0);
  contact_call (client, ui, generation, FALSE, TRUE);
  contact_call (client, ui, generation, TRUE, FALSE);
  vermeer_illumination_free (ui);
  g_main_loop_quit (mock_loop); g_thread_join (thread);
  g_dbus_connection_close_sync (client, NULL, NULL);
  /* The process-local system-bus cache closes at exit. */
  g_free (address);
  g_print ("Active local session, stale generation, release races and cancellation: OK\n");
  return 0;
}
