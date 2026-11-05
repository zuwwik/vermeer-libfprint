/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gio/gio.h>
typedef struct _VermeerIllumination VermeerIllumination;
VermeerIllumination *vermeer_illumination_new (GError **error);
void vermeer_illumination_free (VermeerIllumination *ui);
void vermeer_illumination_begin (VermeerIllumination *ui);
void vermeer_illumination_end (VermeerIllumination *ui);
gboolean vermeer_illumination_wait (VermeerIllumination *ui, gboolean present,
                                   GCancellable *cancellable, gint64 deadline,
                                   GError **error);
