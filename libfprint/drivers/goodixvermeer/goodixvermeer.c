/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define FP_COMPONENT "goodixvermeer"
#include "drivers_api.h"
#include "goodixvermeer.h"
#include <errno.h>
#include <string.h>

#define GROUP 0x564d0002u
#define NAMESPACE "/vermeer-linux/"
#define PANEL "/sys/bus/mipi-dsi/devices/ae94000.dsi.0/fingerprint_hbm"
#define FIRMWARE "/usr/lib/firmware/goodix/gfvermeer.elf"
#define ENROLL_STAGES 15

enum work_operation { OPEN, CLOSE, LIST, DELETE, ENROLL, IDENTIFY };
struct work { enum work_operation operation; guint32 finger; };
struct result { struct vfp_finger fingers[VFP_MAX_FINGERS]; size_t count; guint32 finger; int retry; };
struct progress { FpiDeviceGoodixVermeer *self; gint stage; GError *retry; };
G_DEFINE_TYPE (FpiDeviceGoodixVermeer, fpi_device_goodixvermeer, FP_TYPE_DEVICE)
static const FpIdEntry id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_MISC, .misc_name = "goodix_fp",
    .misc_compatible = "xiaomi,vermeer-fingerprint" },
  { .udev_types = 0 }
};
static GError *native_error (int status)
{
  if (status < 0) return g_error_new (G_IO_ERROR, g_io_error_from_errno (-status),
                                     "Vermeer fingerprint: %s", g_strerror (-status));
  if (status == 1015) return fpi_device_error_new (FP_DEVICE_ERROR_DATA_DUPLICATE);
  if (status == 1007) return fpi_device_error_new (FP_DEVICE_ERROR_DATA_FULL);
  if (status == 1049) return fpi_device_error_new (FP_DEVICE_ERROR_DATA_NOT_FOUND);
  return fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Vermeer TA returned %d", status);
}
static bool cancelled (void *data) { return g_cancellable_is_cancelled (data); }
static gboolean progress_main (gpointer data)
{
  struct progress *p = data;
  fpi_device_enroll_progress (FP_DEVICE (p->self), p->stage, NULL, g_steal_pointer (&p->retry));
  g_object_unref (p->self); g_free (p); return G_SOURCE_REMOVE;
}
static void report_progress (FpiDeviceGoodixVermeer *self, gint stage, GError *retry)
{
  struct progress *p = g_new (struct progress, 1);
  *p = (struct progress){ g_object_ref (self), stage, retry };
  g_main_context_invoke (NULL, progress_main, p);
}
static gboolean interactive (FpiDeviceGoodixVermeer *self, gboolean enroll,
                             GCancellable *cancellable, struct result *result,
                             GError **error)
{
  int status = enroll ? vfp_begin_enrollment (self->native, UINT64_C(0x564d000200000001)) :
                        vfp_begin_authentication (self->native);
  gboolean success = FALSE;
  gint stage = 0;
  gint64 deadline = g_get_monotonic_time () + (enroll ? 120 : 30) * G_TIME_SPAN_SECOND;
  if (status) { g_propagate_error (error, native_error (status)); goto out; }
  vermeer_illumination_begin (self->ui);
  for (guint attempt = 0; attempt < 40; attempt++) {
    struct vfp_sample sample = {0};
    if (!vermeer_illumination_wait (self->ui, TRUE, cancellable, deadline, error)) goto out;
    status = vfp_sample (self->native, &sample, cancelled, cancellable);
    fp_dbg ("%s sample status=%d remaining=%u", enroll ? "Enrollment" : "Authentication", status, status ? 0 : sample.remaining);
    if (sample.cleanup_failed) { g_propagate_error (error, native_error (status)); goto out; }
    if (!enroll && (status == 0 || status == 1008 || status == 1141)) {
      result->finger = status == 0 ? sample.finger_id : 0;
      success = TRUE; goto out;
    }
    if (!status && enroll) {
      if (sample.remaining > ENROLL_STAGES) {
        g_propagate_error (error, native_error (-EPROTO)); goto out;
      }
      stage = MAX (stage, ENROLL_STAGES - sample.remaining);
      if (!sample.remaining) {
        status = vfp_save (self->native, sample.finger_id);
        if (status) { g_propagate_error (error, native_error (status)); goto out; }
        result->finger = sample.finger_id;
        report_progress (self, stage, NULL);
        success = TRUE; goto out;
      }
      report_progress (self, stage, NULL);
    } else if (status == 1011 || (enroll && (status == 1141 || status == 1047))) {
      if (enroll) report_progress (self, stage, fpi_device_retry_new (
          status == 1047 ? FP_DEVICE_RETRY_REMOVE_FINGER : FP_DEVICE_RETRY_GENERAL));
      else { result->retry = status; success = TRUE; goto out; }
    } else { g_propagate_error (error, native_error (status)); goto out; }
    if (!vermeer_illumination_wait (self->ui, FALSE, cancellable, deadline, error)) goto out;
  }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Fingerprint attempt limit reached");
out:
  vermeer_illumination_end (self->ui);
  status = vfp_cancel (self->native);
  if (status && !*error) { g_propagate_error (error, native_error (status)); success = FALSE; }
  return success;
}
static void worker (GTask *task, gpointer object, gpointer task_data, GCancellable *cancellable)
{
  FpiDeviceGoodixVermeer *self = object;
  struct work *work = task_data;
  g_autoptr(GError) error = NULL;
  int status = 0;
  struct result *result = NULL;
  if (g_task_return_error_if_cancelled (task)) return;
  if (work->operation == OPEN) {
    const char *firmware = g_getenv ("LIBFPRINT_VERMEER_FIRMWARE");
    status = vfp_open (&self->native, firmware ? firmware : FIRMWARE, PANEL, true);
  } else if (work->operation == CLOSE) {
    status = vfp_close (self->native); self->native = NULL;
  } else {
    status = vfp_select_group (self->native, GROUP, NAMESPACE);
    if (!status && work->operation == DELETE) status = vfp_remove (self->native, work->finger);
    if (!status && work->operation == LIST) {
      result = g_new0 (struct result, 1);
      status = vfp_list (self->native, result->fingers, &result->count);
    }
    if (!status && (work->operation == ENROLL || work->operation == IDENTIFY)) {
      result = g_new0 (struct result, 1);
      /* An empty device cannot match. fprintd also identifies before enrollment
       * to check duplicates; the TA returns BAD_PARAMS for an empty template set.
       */
      if (work->operation == IDENTIFY) {
        struct vfp_finger fingers[VFP_MAX_FINGERS]; size_t count = 0;
        status = vfp_list (self->native, fingers, &count);
        if (status) { g_free (result); g_task_return_error (task, native_error (status)); return; }
        if (!count) { g_task_return_pointer (task, result, g_free); return; }
      }
      if (!interactive (self, work->operation == ENROLL, cancellable, result, &error)) {
        g_free (result); g_task_return_error (task, g_steal_pointer (&error)); return;
      }
    }
  }
  if (status) { g_free (result); g_task_return_error (task, native_error (status)); }
  else if (result) g_task_return_pointer (task, result, g_free);
  else g_task_return_boolean (task, TRUE);
}
static void start (FpDevice *device, enum work_operation operation, guint32 finger, GAsyncReadyCallback done)
{
  g_autoptr(GTask) task = g_task_new (device,
      operation == OPEN || operation == CLOSE ? NULL : fpi_device_get_cancellable (device), done, NULL);
  struct work *work = g_new (struct work, 1);
  *work = (struct work){ operation, finger };
  g_task_set_task_data (task, work, g_free);
  g_task_run_in_thread (task, worker);
}
static gboolean parse_print (FpPrint *print, guint32 *finger)
{
  g_autoptr(GVariant) data = NULL;
  guint32 group;
  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(uu)"))) return FALSE;
  g_variant_get (data, "(uu)", &group, finger);
  return group == GROUP && *finger != 0;
}
static void set_print (FpPrint *print, guint32 finger)
{
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", g_variant_new ("(uu)", GROUP, finger), NULL);
}
static FpPrint *make_print (FpDevice *device, guint32 finger)
{ FpPrint *print = fp_print_new (device); set_print (print, finger); return print; }
static void open_done (GObject *o, GAsyncResult *r, gpointer unused)
{
  (void)unused; g_autoptr(GError) error = NULL;
  g_task_propagate_boolean (G_TASK (r), &error);
  if (error) { vermeer_illumination_free (FPI_DEVICE_GOODIXVERMEER (o)->ui); FPI_DEVICE_GOODIXVERMEER (o)->ui = NULL; }
  fpi_device_open_complete (FP_DEVICE (o), g_steal_pointer (&error));
}
static void close_done (GObject *o, GAsyncResult *r, gpointer unused)
{
  (void)unused; g_autoptr(GError) error = NULL;
  g_task_propagate_boolean (G_TASK (r), &error);
  vermeer_illumination_free (FPI_DEVICE_GOODIXVERMEER (o)->ui); FPI_DEVICE_GOODIXVERMEER (o)->ui = NULL;
  fpi_device_close_complete (FP_DEVICE (o), g_steal_pointer (&error));
}
static void list_done (GObject *o, GAsyncResult *r, gpointer unused)
{
  (void)unused; g_autoptr(GError) error = NULL;
  g_autofree struct result *result = g_task_propagate_pointer (G_TASK (r), &error);
  GPtrArray *prints = NULL;
  if (result) {
    prints = g_ptr_array_new_with_free_func (g_object_unref);
    for (size_t i = 0; i < result->count; i++)
      g_ptr_array_add (prints, g_object_ref_sink (make_print (FP_DEVICE (o), result->fingers[i].finger_id)));
  }
  fpi_device_list_complete (FP_DEVICE (o), prints, g_steal_pointer (&error));
}
static void delete_done (GObject *o, GAsyncResult *r, gpointer unused)
{
  (void)unused; g_autoptr(GError) error = NULL;
  g_task_propagate_boolean (G_TASK (r), &error);
  fpi_device_delete_complete (FP_DEVICE (o), g_steal_pointer (&error));
}
static void enroll_done (GObject *o, GAsyncResult *r, gpointer unused)
{
  (void)unused; g_autoptr(GError) error = NULL;
  g_autofree struct result *result = g_task_propagate_pointer (G_TASK (r), &error);
  FpPrint *print = NULL;
  if (result) { fpi_device_get_enroll_data (FP_DEVICE (o), &print); set_print (print, result->finger); g_object_ref (print); }
  fpi_device_enroll_complete (FP_DEVICE (o), print, g_steal_pointer (&error));
}
static void identify_done (GObject *o, GAsyncResult *r, gpointer unused)
{
  (void)unused; g_autoptr(GError) error = NULL;
  g_autofree struct result *result = g_task_propagate_pointer (G_TASK (r), &error);
  if (result && result->retry) {
    fpi_device_identify_report (FP_DEVICE (o), NULL, NULL,
                               fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
  } else if (result) {
    GPtrArray *gallery; FpPrint *match = NULL, *scan = NULL;
    fpi_device_get_identify_data (FP_DEVICE (o), &gallery);
    if (result->finger) {
      scan = g_object_ref_sink (make_print (FP_DEVICE (o), result->finger));
      for (guint i = 0; i < gallery->len; i++) {
        guint32 finger;
        if (parse_print (g_ptr_array_index (gallery, i), &finger) && finger == result->finger) {
          match = g_ptr_array_index (gallery, i); break;
        }
      }
    }
    fpi_device_identify_report (FP_DEVICE (o), match, scan, NULL);
    g_clear_object (&scan);
  }
  fpi_device_identify_complete (FP_DEVICE (o), g_steal_pointer (&error));
}
static void device_open (FpDevice *d)
{
  g_autoptr(GError) error = NULL;
  FpiDeviceGoodixVermeer *self = FPI_DEVICE_GOODIXVERMEER (d);
  self->ui = vermeer_illumination_new (&error);
  if (!self->ui) { fpi_device_open_complete (d, g_steal_pointer (&error)); return; }
  start (d, OPEN, 0, open_done);
}
static void device_close (FpDevice *d) { start (d, CLOSE, 0, close_done); }
static void device_list (FpDevice *d) { start (d, LIST, 0, list_done); }
static void device_enroll (FpDevice *d) { start (d, ENROLL, 0, enroll_done); }
static void device_identify (FpDevice *d) { start (d, IDENTIFY, 0, identify_done); }
static void device_delete (FpDevice *d)
{
  FpPrint *print; guint32 finger;
  fpi_device_get_delete_data (d, &print);
  if (!parse_print (print, &finger)) {
    fpi_device_delete_complete (d, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID)); return;
  }
  start (d, DELETE, finger, delete_done);
}
static void device_cancel (FpDevice *d) { (void)d; /* The only worker observes GCancellable. */ }
static void device_probe (FpDevice *d) { fpi_device_probe_complete (d, "vermeer-gfvermeer-v3", NULL, NULL); }
static void fpi_device_goodixvermeer_init (FpiDeviceGoodixVermeer *self) { (void)self; }
static void fpi_device_goodixvermeer_class_init (FpiDeviceGoodixVermeerClass *klass)
{
  FpDeviceClass *device = FP_DEVICE_CLASS (klass);
  device->id = FP_COMPONENT;
  device->full_name = "Vermeer Goodix optical fingerprint sensor";
  device->type = FP_DEVICE_TYPE_UDEV; device->id_table = id_table;
  device->scan_type = FP_SCAN_TYPE_PRESS; device->nr_enroll_stages = ENROLL_STAGES;
  device->temp_hot_seconds = -1;
  device->probe = device_probe; device->open = device_open; device->close = device_close;
  device->list = device_list; device->delete = device_delete;
  device->enroll = device_enroll; device->identify = device_identify; device->cancel = device_cancel;
  fpi_device_class_auto_initialize_features (device);
}
