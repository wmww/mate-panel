/*
 * panel-snap-env.c: Tools related to running inside a snap
 *
 * Copyright (C) 2019 William Wold
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Authors:
 *	William Wold <wm@wmww.sh>
 */

#include <gio/gio.h>

#include "panel-snap-env.h"

struct EnvVar
{
	const char *name;
	const char *value; // NULL meanse variable is to be unset
};

gboolean has_initialized = FALSE;
static GList *variable_list = NULL;

static void
add_variable (const char *name, const char *value)
{
	struct EnvVar *var;

	var = g_new(struct EnvVar, 1);
	var->name = name;
	var->value = value;
	variable_list = g_list_prepend (variable_list, var);
}

static void
add_variables_from_string (char *line)
{
	while (*line) {
		char *next_line;

		next_line = strstr (line, "\n");
		if (next_line)
			next_line++;
		else
			next_line = line + strlen (line);

		if (g_str_has_prefix (line, "export ")) {
			char *name_start, *name_end, *value_start, *value_end;

			name_start = strstr(line, " ") + 1;
			g_return_if_fail (name_start >= line && name_start <= next_line);
			name_end = strstr(name_start, "=");
			g_return_if_fail (name_end >= line && name_end <= next_line);
			value_start = name_end + 1;
			g_return_if_fail (value_start >= line && value_start <= next_line);
			value_end = strstr(value_start, "\n");
			if (!value_end)
				value_end = next_line;
			g_return_if_fail (value_end >= line && value_end <= next_line);

			add_variable (g_strndup (name_start, name_end - name_start),
				      g_strndup (value_start, value_end - value_start));
		} else if (g_str_has_prefix (line, "unset ")) {
			char *name_start, *name_end;

			name_start = strstr(line, " ") + 1;
			g_return_if_fail (name_start >= line && name_start < next_line);
			name_end = strstr(name_start, "\n");
			if (!name_end)
				name_end = next_line;
			g_return_if_fail (name_end >= line && name_end <= next_line);

			add_variable (g_strndup (name_start, name_end - name_start),
				      NULL);
		} else if (next_line - line > 1) {
			char *current_line;

			current_line = g_strndup (line, next_line - line);
			g_warning ("Could not process line \"%s\"", current_line);
			g_free (current_line);
		}

		line = next_line;
	}
}

void
panel_snap_env_init (gchar *snap_out_binary_path)
{
	gboolean command_success;
	gchar *commang_args[] = {
		snap_out_binary_path,
		"--script",
		NULL};
	gchar *command_stdout = NULL;
	gchar *command_stderr = NULL;
	gint command_exit_status;
	GError *error = NULL;

	g_return_if_fail (!has_initialized);

	command_success = g_spawn_sync (NULL,
					commang_args,
					NULL,
					G_SPAWN_DEFAULT,
					NULL,
					NULL,
					&command_stdout,
					&command_stderr,
					&command_exit_status,
					&error);

	if (!command_success) {
		g_warning ("%s", error->message);
	} else if (command_exit_status != EXIT_SUCCESS) {
		g_warning ("Command returned exit code %d: %s", command_exit_status, command_stderr);
	} else {
		add_variables_from_string (command_stdout);
	}

	g_free (command_stdout);
	g_free (command_stderr);
	if (error)
		g_error_free (error);

	has_initialized = TRUE;
}


static gchar *
panel_snap_env_get_snap_out_path ()
{
	const gchar *snap_path;
	const gchar *snap_out_path;
	int ret_len;
	gchar *ret;

	snap_path = g_getenv ("SNAP");
	if (!snap_path)
		return NULL;

	snap_out_path = "/bin/snap-out";
	ret_len = strlen(snap_path) + strlen(snap_out_path) + 1;
	ret = g_strnfill (ret_len, 0);
	g_strlcat (ret, snap_path, ret_len);
	g_strlcat (ret, snap_out_path, ret_len);
	return ret;
}

void
panel_snap_env_setup_launch_context_if_needed (GAppLaunchContext *context)
{
	GList *l;

	if (!has_initialized) {
		gchar *snap_out_binary_path = panel_snap_env_get_snap_out_path ();
		if (snap_out_binary_path) {
			panel_snap_env_init (snap_out_binary_path);
			g_free (snap_out_binary_path);
		} else {
			// We're probably not inside a snap'
			return;
		}
	}

	for (l = variable_list; l != NULL; l = l->next)
	{
		struct EnvVar *var;

		var = l->data;
		if (var->value) {
			g_app_launch_context_setenv (context,
						     var->name,
						     var->value);
		} else {
			g_app_launch_context_unsetenv (context,
						       var->name);
		}
	}
}
