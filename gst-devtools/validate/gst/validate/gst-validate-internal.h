/* GStreamer
 * Copyright (C) 2013 Thiago Santos <thiago.sousa.santos@collabora.com>
 *
 * validate.c - Validate generic functions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_VALIDATE_INTERNAL_H__
#define __GST_VALIDATE_INTERNAL_H__

#include <gst/gst.h>
#include "gst-validate-scenario.h"
#include "gst-validate-monitor.h"

GST_DEBUG_CATEGORY_EXTERN (gstvalidate_debug);
#define GST_CAT_DEFAULT gstvalidate_debug

extern GRegex *newline_regex;


/* If an action type is 1 (TRUE) we also consider it is a config to keep backward compatibility */
#define IS_CONFIG_ACTION_TYPE(type) (((type) & GST_VALIDATE_ACTION_TYPE_CONFIG) || ((type) == TRUE))

GST_EXPORT GType _gst_validate_action_type_type;

void init_scenarios (void);

/* FIXME 2.0 Remove that as this is only for backward compatibility
 * as we used to have to print actions in the action execution function
 * and this is done by the scenario itself now */
GST_EXPORT gboolean _action_check_and_set_printed (GstValidateAction *action);
GST_EXPORT gboolean gst_validate_action_is_subaction (GstValidateAction *action);
GST_EXPORT void _priv_validate_override_registry_deinit (void);

G_GNUC_INTERNAL GstValidateMonitor * gst_validate_get_monitor (GObject *object);
G_GNUC_INTERNAL void gst_validate_init_runner (void);
G_GNUC_INTERNAL void gst_validate_deinit_runner (void);

#endif
