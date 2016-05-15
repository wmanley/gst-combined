/* GStreamer
 *
 * Copyright (C) 2013 Collabora Ltd.
 *  Author: Thiago Sousa Santos <thiago.sousa.santos@collabora.com>
 *
 * gst-validate-monitor-report.c - Validate report/issues functions
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>              /* fprintf */
#include <glib/gstdio.h>
#include <errno.h>

#include <string.h>
#include "gst-validate-i18n-lib.h"
#include "gst-validate-internal.h"

#include "gst-validate-report.h"
#include "gst-validate-reporter.h"
#include "gst-validate-monitor.h"
#include "gst-validate-scenario.h"

static GstClockTime _gst_validate_report_start_time = 0;
static GstValidateDebugFlags _gst_validate_flags = 0;
static GHashTable *_gst_validate_issues = NULL;
static FILE **log_files = NULL;


GRegex *newline_regex = NULL;

GST_DEBUG_CATEGORY_STATIC (gst_validate_report_debug);
#undef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT gst_validate_report_debug

#define GST_VALIDATE_REPORT_SHADOW_REPORTS_LOCK(r)			\
  G_STMT_START {					\
  (g_mutex_lock (&((GstValidateReport *) r)->shadow_reports_lock));		\
  } G_STMT_END

#define GST_VALIDATE_REPORT_SHADOW_REPORTS_UNLOCK(r)			\
  G_STMT_START {					\
  (g_mutex_unlock (&((GstValidateReport *) r)->shadow_reports_lock));		\
  } G_STMT_END

G_DEFINE_BOXED_TYPE (GstValidateReport, gst_validate_report,
    (GBoxedCopyFunc) gst_validate_report_ref,
    (GBoxedFreeFunc) gst_validate_report_unref);

static GstValidateIssue *
gst_validate_issue_ref (GstValidateIssue * issue)
{
  g_return_val_if_fail (issue != NULL, NULL);

  g_atomic_int_inc (&issue->refcount);

  return issue;
}

static void
gst_validate_issue_unref (GstValidateIssue * issue)
{
  if (G_UNLIKELY (g_atomic_int_dec_and_test (&issue->refcount))) {
    g_free (issue->summary);
    g_free (issue->description);

    /* We are using an string array for area and name */
    g_strfreev (&issue->area);

    g_slice_free (GstValidateIssue, issue);
  }
}


G_DEFINE_BOXED_TYPE (GstValidateIssue, gst_validate_issue,
    (GBoxedCopyFunc) gst_validate_issue_ref,
    (GBoxedFreeFunc) gst_validate_issue_unref);

GstValidateIssueId
gst_validate_issue_get_id (GstValidateIssue * issue)
{
  return issue->issue_id;
}

/**
 * gst_validate_issue_new:
 * @issue_id: The ID of the issue, should be a GQuark
 * @summary: A summary of the issue
 * @description: A more complete of what the issue is about
 * @default_level: The level at which the issue will be reported by default
 *
 * Returns: (transfer full): The newly created #GstValidateIssue
 */
GstValidateIssue *
gst_validate_issue_new (GstValidateIssueId issue_id, const gchar * summary,
    const gchar * description, GstValidateReportLevel default_level)
{
  GstValidateIssue *issue = g_slice_new (GstValidateIssue);
  gchar **area_name = g_strsplit (g_quark_to_string (issue_id), "::", 2);

  g_return_val_if_fail (area_name[0] != NULL && area_name[1] != 0 &&
      area_name[2] == NULL, NULL);

  issue->issue_id = issue_id;
  issue->summary = g_strdup (summary);
  issue->description = g_strdup (description);
  issue->default_level = default_level;
  issue->area = area_name[0];
  issue->name = area_name[1];

  g_free (area_name);
  return issue;
}

void
gst_validate_issue_set_default_level (GstValidateIssue * issue,
    GstValidateReportLevel default_level)
{
  GST_INFO ("Setting issue %s::%s default level to %s",
      issue->area, issue->name,
      gst_validate_report_level_get_name (default_level));

  issue->default_level = default_level;
}

/**
 * gst_validate_issue_register:
 * @issue: (transfer none): The #GstValidateIssue to register
 *
 * Registers @issue in the issue type system
 */
void
gst_validate_issue_register (GstValidateIssue * issue)
{
  g_return_if_fail (g_hash_table_lookup (_gst_validate_issues,
          (gpointer) gst_validate_issue_get_id (issue)) == NULL);

  g_hash_table_insert (_gst_validate_issues,
      (gpointer) gst_validate_issue_get_id (issue), issue);
}

#define REGISTER_VALIDATE_ISSUE(lvl,id,sum,desc)			\
  gst_validate_issue_register (gst_validate_issue_new (id, \
						       sum, desc, GST_VALIDATE_REPORT_LEVEL_##lvl))
static void
gst_validate_report_load_issues (void)
{
  g_return_if_fail (_gst_validate_issues == NULL);

  _gst_validate_issues = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) gst_validate_issue_unref);

  REGISTER_VALIDATE_ISSUE (WARNING, BUFFER_BEFORE_SEGMENT,
      _("buffer was received before a segment"),
      _("in push mode, a segment event must be received before a buffer"));
  REGISTER_VALIDATE_ISSUE (ISSUE, BUFFER_IS_OUT_OF_SEGMENT,
      _("buffer is out of the segment range"),
      _("buffer being pushed is out of the current segment's start-stop "
          " range. Meaning it is going to be discarded downstream without "
          "any use"));
  REGISTER_VALIDATE_ISSUE (WARNING, BUFFER_TIMESTAMP_OUT_OF_RECEIVED_RANGE,
      _("buffer timestamp is out of the received buffer timestamps' range"),
      _("a buffer leaving an element should have its timestamps in the range "
          "of the received buffers timestamps. i.e. If an element received "
          "buffers with timestamps from 0s to 10s, it can't push a buffer with "
          "with a 11s timestamp, because it doesn't have data for that"));
  REGISTER_VALIDATE_ISSUE (WARNING, WRONG_BUFFER,
      _("Received buffer does not correspond to wanted one."),
      _("When checking playback of a file against a MediaInfo file"
          " all buffers coming into the decoders might be checked"
          " and should have the exact expected metadatas and hash of the"
          " content"));
  REGISTER_VALIDATE_ISSUE (CRITICAL, WRONG_FLOW_RETURN,
      _("flow return from pad push doesn't match expected value"),
      _("flow return from a 1:1 sink/src pad element is as simple as "
          "returning what downstream returned. For elements that have multiple "
          "src pads, flow returns should be properly combined"));
  REGISTER_VALIDATE_ISSUE (ISSUE, BUFFER_AFTER_EOS,
      _("buffer was received after EOS"),
      _("a pad shouldn't receive any more buffers after it gets EOS"));
  REGISTER_VALIDATE_ISSUE (WARNING, FLOW_ERROR_WITHOUT_ERROR_MESSAGE,
      _("GST_FLOW_ERROR returned without posting an ERROR on the bus"),
      _("Element MUST post a GST_MESSAGE_ERROR with GST_ELEMENT_ERROR before"
          " returning GST_FLOW_ERROR"));

  REGISTER_VALIDATE_ISSUE (ISSUE, CAPS_IS_MISSING_FIELD,
      _("caps is missing a required field for its type"),
      _("some caps types are expected to contain a set of basic fields. "
          "For example, raw video should have 'width', 'height', 'framerate' "
          "and 'pixel-aspect-ratio'"));
  REGISTER_VALIDATE_ISSUE (WARNING, CAPS_FIELD_HAS_BAD_TYPE,
      _("caps field has an unexpected type"),
      _("some common caps fields should always use the same expected types"));
  REGISTER_VALIDATE_ISSUE (WARNING, CAPS_EXPECTED_FIELD_NOT_FOUND,
      _("caps expected field wasn't present"),
      _("a field that should be present in the caps wasn't found. "
          "Fields sets on a sink pad caps should be propagated downstream "
          "when it makes sense to do so"));
  REGISTER_VALIDATE_ISSUE (CRITICAL, GET_CAPS_NOT_PROXYING_FIELDS,
      _("getcaps function isn't proxying downstream fields correctly"),
      _("elements should set downstream caps restrictions on its caps when "
          "replying upstream's getcaps queries to avoid upstream sending data"
          " in an unsupported format"));
  REGISTER_VALIDATE_ISSUE (CRITICAL, CAPS_FIELD_UNEXPECTED_VALUE,
      _("a field in caps has an unexpected value"),
      _("fields set on a sink pad should be propagated downstream via "
          "set caps"));

  REGISTER_VALIDATE_ISSUE (WARNING, EVENT_NEWSEGMENT_NOT_PUSHED,
      _("new segment event wasn't propagated downstream"),
      _("segments received from upstream should be pushed downstream"));
  REGISTER_VALIDATE_ISSUE (WARNING, SERIALIZED_EVENT_WASNT_PUSHED_IN_TIME,
      _("a serialized event received should be pushed in the same 'time' "
          "as it was received"),
      _("serialized events should be pushed in the same order they are "
          "received and serialized with buffers. If an event is received after"
          " a buffer with timestamp end 'X', it should be pushed right after "
          "buffers with timestamp end 'X'"));
  REGISTER_VALIDATE_ISSUE (ISSUE, EOS_HAS_WRONG_SEQNUM,
      _("EOS events that are part of the same pipeline 'operation' should "
          "have the same seqnum"),
      _("when events/messages are created from another event/message, "
          "they should have their seqnums set to the original event/message "
          "seqnum"));
  REGISTER_VALIDATE_ISSUE (ISSUE, FLUSH_START_HAS_WRONG_SEQNUM,
      _
      ("FLUSH_START events that are part of the same pipeline 'operation' should "
          "have the same seqnum"),
      _("when events/messages are created from another event/message, "
          "they should have their seqnums set to the original event/message "
          "seqnum"));
  REGISTER_VALIDATE_ISSUE (ISSUE, FLUSH_STOP_HAS_WRONG_SEQNUM,
      _
      ("FLUSH_STOP events that are part of the same pipeline 'operation' should "
          "have the same seqnum"),
      _("when events/messages are created from another event/message, "
          "they should have their seqnums set to the original event/message "
          "seqnum"));
  REGISTER_VALIDATE_ISSUE (ISSUE, SEGMENT_HAS_WRONG_SEQNUM,
      _("SEGMENT events that are part of the same pipeline 'operation' should "
          "have the same seqnum"),
      _("when events/messages are created from another event/message, "
          "they should have their seqnums set to the original event/message "
          "seqnum"));
  REGISTER_VALIDATE_ISSUE (CRITICAL, SEGMENT_HAS_WRONG_START,
      _("A segment doesn't have the proper time value after an ACCURATE seek"),
      _("If a seek with the ACCURATE flag was accepted, the following segment "
          "should have a time value corresponding exactly to the requested start "
          "seek time"));
  REGISTER_VALIDATE_ISSUE (WARNING, EVENT_SERIALIZED_OUT_OF_ORDER,
      _("a serialized event received should be pushed in the same order "
          "as it was received"),
      _("serialized events should be pushed in the same order they are "
          "received."));
  REGISTER_VALIDATE_ISSUE (WARNING, EVENT_NEW_SEGMENT_MISMATCH,
      _("a new segment event has different value than the received one"),
      _("when receiving a new segment, an element should push an equivalent"
          "segment downstream"));
  REGISTER_VALIDATE_ISSUE (WARNING, EVENT_FLUSH_START_UNEXPECTED,
      _("received an unexpected flush start event"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, EVENT_FLUSH_STOP_UNEXPECTED,
      _("received an unexpected flush stop event"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, EVENT_CAPS_DUPLICATE,
      _("received the same caps twice"), NULL);

  REGISTER_VALIDATE_ISSUE (CRITICAL, EVENT_SEEK_NOT_HANDLED,
      _("seek event wasn't handled"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, EVENT_SEEK_RESULT_POSITION_WRONG,
      _("position after a seek is wrong"), NULL);

  REGISTER_VALIDATE_ISSUE (WARNING, EVENT_EOS_WITHOUT_SEGMENT,
      _("EOS received without segment event before"),
      _("A segment event should always be sent before data flow"
          " EOS being some kind of data flow, there is no exception"
          " in that regard"));

  REGISTER_VALIDATE_ISSUE (CRITICAL, STATE_CHANGE_FAILURE,
      _("state change failed"), NULL);

  REGISTER_VALIDATE_ISSUE (WARNING, FILE_SIZE_INCORRECT,
      _("resulting file size wasn't within the expected values"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, FILE_DURATION_INCORRECT,
      _("resulting file duration wasn't within the expected values"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, FILE_SEEKABLE_INCORRECT,
      _("resulting file wasn't seekable or not seekable as expected"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, FILE_PROFILE_INCORRECT,
      _("resulting file stream profiles didn't match expected values"), NULL);
  REGISTER_VALIDATE_ISSUE (ISSUE, FILE_TAG_DETECTION_INCORRECT,
      _("detected tags are different than expected ones"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, FILE_FRAMES_INCORRECT,
      _("resulting file frames are not as expected"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, FILE_NO_STREAM_INFO,
      _("the discoverer could not determine the stream info"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, FILE_NO_STREAM_ID,
      _("the discoverer found a stream that had no stream ID"), NULL);


  REGISTER_VALIDATE_ISSUE (CRITICAL, ALLOCATION_FAILURE,
      _("a memory allocation failed during Validate run"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, MISSING_PLUGIN,
      _("a gstreamer plugin is missing and prevented Validate from running"),
      NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, WARNING_ON_BUS,
      _("We got a WARNING message on the bus"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, ERROR_ON_BUS,
      _("We got an ERROR message on the bus"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, QUERY_POSITION_SUPERIOR_DURATION,
      _("Query position reported a value superior than what query duration "
          "returned"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, QUERY_POSITION_OUT_OF_SEGMENT,
      _("Query position reported a value outside of the current expected "
          "segment"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, SCENARIO_NOT_ENDED,
      _("All the actions were not executed before the program stopped"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, SCENARIO_ACTION_TIMEOUT,
      _("The execution of an action timed out"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, SCENARIO_FILE_MALFORMED,
      _("The scenario file was malformed"), NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, SCENARIO_ACTION_EXECUTION_ERROR,
      _("The execution of an action did not properly happen"), NULL);
  REGISTER_VALIDATE_ISSUE (ISSUE, SCENARIO_ACTION_EXECUTION_ISSUE,
      _("An issue happend during the execution of a scenario"), NULL);
  REGISTER_VALIDATE_ISSUE (WARNING, G_LOG_WARNING, _("We got a g_log warning"),
      NULL);
  REGISTER_VALIDATE_ISSUE (CRITICAL, G_LOG_CRITICAL,
      _("We got a g_log critical issue"), NULL);
  REGISTER_VALIDATE_ISSUE (ISSUE, G_LOG_ISSUE, _("We got a g_log issue"), NULL);
}

void
gst_validate_report_init (void)
{
  const gchar *var, *file_env;
  const GDebugKey keys[] = {
    {"fatal_criticals", GST_VALIDATE_FATAL_CRITICALS},
    {"fatal_warnings", GST_VALIDATE_FATAL_WARNINGS},
    {"fatal_issues", GST_VALIDATE_FATAL_ISSUES},
    {"print_issues", GST_VALIDATE_PRINT_ISSUES},
    {"print_warnings", GST_VALIDATE_PRINT_WARNINGS},
    {"print_criticals", GST_VALIDATE_PRINT_CRITICALS}
  };

  GST_DEBUG_CATEGORY_INIT (gst_validate_report_debug, "gstvalidatereport",
      GST_DEBUG_FG_YELLOW, "Gst validate reporting");

  if (_gst_validate_report_start_time == 0) {
    _gst_validate_report_start_time = gst_util_get_timestamp ();

    /* init the debug flags */
    var = g_getenv ("GST_VALIDATE");
    if (var && strlen (var) > 0) {
      _gst_validate_flags =
          g_parse_debug_string (var, keys, G_N_ELEMENTS (keys));
    }

    gst_validate_report_load_issues ();
  }

  file_env = g_getenv ("GST_VALIDATE_FILE");
  if (file_env != NULL && *file_env != '\0') {
    gint i;
    gchar **wanted_files;
    wanted_files = g_strsplit (file_env, G_SEARCHPATH_SEPARATOR_S, 0);

    /* FIXME: Make sure it is freed in the deinit function when that is
     * implemented */
    log_files =
        g_malloc0 (sizeof (FILE *) * (g_strv_length (wanted_files) + 1));
    for (i = 0; i < g_strv_length (wanted_files); i++) {
      FILE *log_file;

      if (g_strcmp0 (wanted_files[i], "stderr") == 0) {
        log_file = stderr;
      } else if (g_strcmp0 (wanted_files[i], "stdout") == 0)
        log_file = stdout;
      else {
        log_file = g_fopen (wanted_files[i], "w");
      }

      if (log_file == NULL) {
        g_printerr ("Could not open log file '%s' for writing: %s\n", file_env,
            g_strerror (errno));
        log_file = stderr;
      }

      log_files[i] = log_file;
    }

    g_strfreev (wanted_files);
  } else {
    log_files = g_malloc0 (sizeof (FILE *) * 2);
    log_files[0] = stdout;
  }

#ifndef GST_DISABLE_GST_DEBUG
  if (!newline_regex)
    newline_regex =
        g_regex_new ("\n", G_REGEX_OPTIMIZE | G_REGEX_MULTILINE, 0, NULL);
#endif
}

GstValidateIssue *
gst_validate_issue_from_id (GstValidateIssueId issue_id)
{
  return g_hash_table_lookup (_gst_validate_issues, (gpointer) issue_id);
}

/* TODO how are these functions going to work with extensions */
const gchar *
gst_validate_report_level_get_name (GstValidateReportLevel level)
{
  switch (level) {
    case GST_VALIDATE_REPORT_LEVEL_CRITICAL:
      return "critical";
    case GST_VALIDATE_REPORT_LEVEL_WARNING:
      return "warning";
    case GST_VALIDATE_REPORT_LEVEL_ISSUE:
      return "issue";
    case GST_VALIDATE_REPORT_LEVEL_IGNORE:
      return "ignore";
    default:
      return "unknown";
  }
}

GstValidateReportLevel
gst_validate_report_level_from_name (const gchar * issue_name)
{
  if (g_strcmp0 (issue_name, "critical") == 0)
    return GST_VALIDATE_REPORT_LEVEL_CRITICAL;

  else if (g_strcmp0 (issue_name, "warning") == 0)
    return GST_VALIDATE_REPORT_LEVEL_WARNING;

  else if (g_strcmp0 (issue_name, "issue") == 0)
    return GST_VALIDATE_REPORT_LEVEL_ISSUE;

  else if (g_strcmp0 (issue_name, "ignore") == 0)
    return GST_VALIDATE_REPORT_LEVEL_IGNORE;

  return GST_VALIDATE_REPORT_LEVEL_UNKNOWN;
}

gboolean
gst_validate_report_should_print (GstValidateReport * report)
{
  if ((!(_gst_validate_flags & GST_VALIDATE_PRINT_ISSUES) &&
          !(_gst_validate_flags & GST_VALIDATE_PRINT_WARNINGS) &&
          !(_gst_validate_flags & GST_VALIDATE_PRINT_CRITICALS))) {
    return TRUE;
  }

  if ((report->level <= GST_VALIDATE_REPORT_LEVEL_ISSUE &&
          _gst_validate_flags & GST_VALIDATE_PRINT_ISSUES) ||
      (report->level <= GST_VALIDATE_REPORT_LEVEL_WARNING &&
          _gst_validate_flags & GST_VALIDATE_PRINT_WARNINGS) ||
      (report->level <= GST_VALIDATE_REPORT_LEVEL_CRITICAL &&
          _gst_validate_flags & GST_VALIDATE_PRINT_CRITICALS)) {

    return TRUE;
  }

  return FALSE;
}

gboolean
gst_validate_report_check_abort (GstValidateReport * report)
{
  if ((report->level <= GST_VALIDATE_REPORT_LEVEL_ISSUE &&
          _gst_validate_flags & GST_VALIDATE_FATAL_ISSUES) ||
      (report->level <= GST_VALIDATE_REPORT_LEVEL_WARNING &&
          _gst_validate_flags & GST_VALIDATE_FATAL_WARNINGS) ||
      (report->level <= GST_VALIDATE_REPORT_LEVEL_CRITICAL &&
          _gst_validate_flags & GST_VALIDATE_FATAL_CRITICALS)) {

    return TRUE;
  }

  return FALSE;
}

GstValidateIssueId
gst_validate_report_get_issue_id (GstValidateReport * report)
{
  return gst_validate_issue_get_id (report->issue);
}

GstValidateReport *
gst_validate_report_new (GstValidateIssue * issue,
    GstValidateReporter * reporter, const gchar * message)
{
  GstValidateReport *report = g_slice_new0 (GstValidateReport);

  report->refcount = 1;
  report->issue = issue;
  report->reporter = reporter;  /* TODO should we ref? */
  report->message = g_strdup (message);
  g_mutex_init (&report->shadow_reports_lock);
  report->timestamp =
      gst_util_get_timestamp () - _gst_validate_report_start_time;
  report->level = issue->default_level;
  report->reporting_level = GST_VALIDATE_SHOW_UNKNOWN;

  return report;
}

void
gst_validate_report_unref (GstValidateReport * report)
{
  g_return_if_fail (report != NULL);

  if (G_UNLIKELY (g_atomic_int_dec_and_test (&report->refcount))) {
    g_free (report->message);
    g_list_free_full (report->shadow_reports,
        (GDestroyNotify) gst_validate_report_unref);
    g_list_free_full (report->repeated_reports,
        (GDestroyNotify) gst_validate_report_unref);
    g_mutex_clear (&report->shadow_reports_lock);
    g_slice_free (GstValidateReport, report);
  }
}

GstValidateReport *
gst_validate_report_ref (GstValidateReport * report)
{
  g_return_val_if_fail (report != NULL, NULL);

  g_atomic_int_inc (&report->refcount);

  return report;
}

void
gst_validate_printf (gpointer source, const gchar * format, ...)
{
  va_list var_args;

  va_start (var_args, format);
  gst_validate_printf_valist (source, format, var_args);
  va_end (var_args);
}

static gboolean
_append_value (GQuark field_id, const GValue * value, GString * string)
{
  gchar *val_str = NULL;

  if (g_strcmp0 (g_quark_to_string (field_id), "sub-action") == 0)
    return TRUE;

  if (G_VALUE_TYPE (value) == GST_TYPE_CLOCK_TIME)
    val_str = g_strdup_printf ("%" GST_TIME_FORMAT,
        GST_TIME_ARGS (g_value_get_uint64 (value)));
  else
    val_str = gst_value_serialize (value);

  g_string_append (string, g_quark_to_string (field_id));
  g_string_append_len (string, "=", 1);
  g_string_append (string, val_str);
  g_string_append_len (string, " ", 1);

  g_free (val_str);

  return TRUE;
}

/**
 * gst_validate_print_action:
 * @action: (allow-none): The source object to log
 * @message: The message to print out in the GstValidate logging system
 *
 * Print @message to the GstValidate logging system
 */
void
gst_validate_print_action (GstValidateAction * action, const gchar * message)
{
  GString *string = NULL;

  if (message == NULL) {
    gint nrepeats;

    string = g_string_new (NULL);

    if (gst_validate_action_is_subaction (action))
      g_string_append_printf (string, "(subaction)");

    if (gst_structure_get_int (action->structure, "repeat", &nrepeats))
      g_string_append_printf (string, "(%d/%d)", action->repeat, nrepeats);

    g_string_append_printf (string, " %s",
        gst_structure_get_name (action->structure));

    g_string_append_len (string, ": ", 2);
    gst_structure_foreach (action->structure,
        (GstStructureForeachFunc) _append_value, string);
    g_string_append_len (string, "\n", 1);
    message = string->str;
  }

  gst_validate_printf (action, "%s", message);

  if (string)
    g_string_free (string, TRUE);
}

static void
print_action_parametter (GString * string, GstValidateActionType * type,
    GstValidateActionParameter * param)
{
  gint nw = 0;

  gchar *desc, *tmp;
  gchar *param_head = g_strdup_printf ("    %s", param->name);
  gchar *tmp_head = g_strdup_printf ("\n %-30s : %s",
      param_head, "something");


  while (tmp_head[nw] != ':')
    nw++;

  g_free (tmp_head);

  tmp = g_strdup_printf ("\n%*s", nw + 1, " ");

  if (g_strcmp0 (param->description, "")) {
    desc =
        g_regex_replace (newline_regex, param->description,
        -1, 0, tmp, 0, NULL);
  } else {
    desc = g_strdup_printf ("No description");
  }

  g_string_append_printf (string, "\n %-30s : %s", param_head, desc);
  g_free (desc);

  if (param->possible_variables) {
    gchar *tmp1 = g_strdup_printf ("\n%*s", nw + 4, " ");
    desc =
        g_regex_replace (newline_regex,
        param->possible_variables, -1, 0, tmp1, 0, NULL);
    g_string_append_printf (string, "%sPossible variables:%s%s", tmp,
        tmp1, desc);

    g_free (tmp1);
  }

  if (param->types) {
    gchar *tmp1 = g_strdup_printf ("\n%*s", nw + 4, " ");
    desc = g_regex_replace (newline_regex, param->types, -1, 0, tmp1, 0, NULL);
    g_string_append_printf (string, "%sPossible types:%s%s", tmp, tmp1, desc);

    g_free (tmp1);
  }

  if (!param->mandatory) {
    g_string_append_printf (string, "%sDefault: %s", tmp, param->def);
  }

  g_string_append_printf (string, "%s%s", tmp,
      param->mandatory ? "Mandatory." : "Optional.");

  g_free (tmp);
  g_free (param_head);
}

void
gst_validate_printf_valist (gpointer source, const gchar * format, va_list args)
{
  gint i;
  GString *string = g_string_new (NULL);

  if (source) {
    if (*(GType *) source == GST_TYPE_VALIDATE_ACTION) {
      GstValidateAction *action = (GstValidateAction *) source;

      if (_action_check_and_set_printed (action))
        goto out;

      g_string_printf (string, "Executing ");

    } else if (*(GType *) source == GST_TYPE_VALIDATE_ACTION_TYPE) {
      gint i;
      gchar *desc, *tmp;
      gboolean has_parameters = FALSE;

      GstValidateActionParameter playback_time_param = {
        .name = "playback-time",
        .description =
            "The playback time at which the action " "will be executed",
        .mandatory = FALSE,
        .types = "double,string",
        .possible_variables =
            "position: The current position in the stream\n"
            "duration: The duration of the stream",
        .def = "0.0"
      };

      GstValidateActionType *type = GST_VALIDATE_ACTION_TYPE (source);

      g_string_printf (string, "\nAction type:");
      g_string_append_printf (string, "\n  Name: %s", type->name);
      g_string_append_printf (string, "\n  Implementer namespace: %s",
          type->implementer_namespace);

      if (IS_CONFIG_ACTION_TYPE (type->flags))
        g_string_append_printf (string,
            "\n    Is config action (meaning it will be executing right "
            "at the begining of the execution of the pipeline)");


      tmp = g_strdup_printf ("\n    ");
      desc =
          g_regex_replace (newline_regex, type->description, -1, 0, tmp, 0,
          NULL);
      g_string_append_printf (string, "\n\n  Description: \n    %s", desc);
      g_free (desc);
      g_free (tmp);

      if (!IS_CONFIG_ACTION_TYPE (type->flags))
        print_action_parametter (string, type, &playback_time_param);

      if (type->parameters) {
        has_parameters = TRUE;
        g_string_append_printf (string, "\n\n  Parametters:");
        for (i = 0; type->parameters[i].name; i++) {
          print_action_parametter (string, type, &type->parameters[i]);
        }

      }

      if ((type->flags & GST_VALIDATE_ACTION_TYPE_CAN_BE_OPTIONAL)) {
        has_parameters = TRUE;
        g_string_append_printf (string, "\n     %-26s : %s", "optional",
            "Don't raise an error if this action hasn't been executed of failed");
        g_string_append_printf (string, "\n     %-28s %s", "",
            "Possible types:");
        g_string_append_printf (string, "\n     %-31s %s", "", "boolean");
        g_string_append_printf (string, "\n     %-28s %s", "",
            "Default: false");
      }

      if (!has_parameters)
        g_string_append_printf (string, "\n\n  No Parameters");
    } else if (GST_IS_VALIDATE_REPORTER (source) &&
        gst_validate_reporter_get_name (source)) {
      g_string_printf (string, "\n%s --> ",
          gst_validate_reporter_get_name (source));
    } else if (GST_IS_OBJECT (source)) {
      g_string_printf (string, "\n%s --> ", GST_OBJECT_NAME (source));
    } else if (G_IS_OBJECT (source)) {
      g_string_printf (string, "\n<%s@%p> --> ", G_OBJECT_TYPE_NAME (source),
          source);
    }
  }

  g_string_append_vprintf (string, format, args);

  if (!newline_regex)
    newline_regex =
        g_regex_new ("\n", G_REGEX_OPTIMIZE | G_REGEX_MULTILINE, 0, NULL);

#ifndef GST_DISABLE_GST_DEBUG
  {
    gchar *str;

    str = g_regex_replace (newline_regex, string->str, string->len, 0,
        "", 0, NULL);

    if (source)
      GST_INFO ("%s", str);
    else
      GST_DEBUG ("%s", str);

    g_free (str);
  }
#endif

  for (i = 0; log_files[i]; i++) {
    fprintf (log_files[i], "%s", string->str);
    fflush (log_files[i]);
  }

out:
  g_string_free (string, TRUE);
}

gboolean
gst_validate_report_set_master_report (GstValidateReport * report,
    GstValidateReport * master_report)
{
  GList *tmp;
  gboolean add_shadow_report = TRUE;

  if (master_report->reporting_level >= GST_VALIDATE_SHOW_MONITOR)
    return FALSE;

  report->master_report = master_report;

  GST_VALIDATE_REPORT_SHADOW_REPORTS_LOCK (master_report);
  for (tmp = master_report->shadow_reports; tmp; tmp = tmp->next) {
    GstValidateReport *shadow_report = (GstValidateReport *) tmp->data;
    if (report->reporter == shadow_report->reporter) {
      add_shadow_report = FALSE;
      break;
    }
  }
  if (add_shadow_report)
    master_report->shadow_reports =
        g_list_append (master_report->shadow_reports,
        gst_validate_report_ref (report));
  GST_VALIDATE_REPORT_SHADOW_REPORTS_UNLOCK (master_report);

  return TRUE;
}

void
gst_validate_report_print_level (GstValidateReport * report)
{
  gst_validate_printf (NULL, "%10s : %s\n",
      gst_validate_report_level_get_name (report->level),
      report->issue->summary);
}

void
gst_validate_report_print_detected_on (GstValidateReport * report)
{
  GList *tmp;

  gst_validate_printf (NULL, "%*s Detected on <%s",
      12, "", gst_validate_reporter_get_name (report->reporter));
  for (tmp = report->shadow_reports; tmp; tmp = tmp->next) {
    GstValidateReport *shadow_report = (GstValidateReport *) tmp->data;
    gst_validate_printf (NULL, ", %s",
        gst_validate_reporter_get_name (shadow_report->reporter));
  }
  gst_validate_printf (NULL, ">\n");
}

void
gst_validate_report_print_details (GstValidateReport * report)
{
  if (report->message)
    gst_validate_printf (NULL, "%*s Details : %s\n", 12, "", report->message);
}

void
gst_validate_report_print_description (GstValidateReport * report)
{
  if (report->issue->description)
    gst_validate_printf (NULL, "%*s Description : %s\n", 12, "",
        report->issue->description);
}

void
gst_validate_report_printf (GstValidateReport * report)
{
  GList *tmp;

  gst_validate_report_print_level (report);
  gst_validate_report_print_detected_on (report);
  gst_validate_report_print_details (report);

  for (tmp = report->repeated_reports; tmp; tmp = tmp->next) {
    gst_validate_report_print_details (report);
  }

  gst_validate_report_print_description (report);
  gst_validate_printf (NULL, "\n");
}

void
gst_validate_report_set_reporting_level (GstValidateReport * report,
    GstValidateReportingDetails level)
{
  report->reporting_level = level;
}

void
gst_validate_report_add_repeated_report (GstValidateReport * report,
    GstValidateReport * repeated_report)
{
  report->repeated_reports =
      g_list_append (report->repeated_reports,
      gst_validate_report_ref (repeated_report));
}
