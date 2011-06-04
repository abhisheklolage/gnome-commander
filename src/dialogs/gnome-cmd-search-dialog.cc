/*
    GNOME Commander - A GNOME based file manager
    Copyright (C) 2001-2006 Marcus Bjurman
    Copyright (C) 2007-2011 Piotr Eljasiak

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
*/
/*
 *GNOME Search Tool
 *
 * File:  gsearchtool.c
 *
 * (C) 1998,2002 the Free Software Foundation
 *
 * Authors:    Dennis Cranston  <dennis_cranston@yahoo.com>
 *             George Lebl
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 *
 */

#include <config.h>
#include <sys/types.h>
#include <regex.h>

#include "gnome-cmd-includes.h"
#include "gnome-cmd-data.h"
#include "gnome-cmd-search-dialog.h"
#include "gnome-cmd-dir.h"
#include "gnome-cmd-file-list.h"
#include "gnome-cmd-file-selector.h"
#include "gnome-cmd-main-win.h"
#include "gnome-cmd-con-list.h"
#include "filter.h"
#include "utils.h"

using namespace std;


#if 0
static char *msgs[] = {N_("Search local directories only"),
                       N_("Files _not containing text")};
#endif


#define PBAR_MAX   50

#define SEARCH_JUMP_SIZE     4096U
#define SEARCH_BUFFER_SIZE  (SEARCH_JUMP_SIZE * 10U)

#define GNOME_SEARCH_TOOL_REFRESH_DURATION  50000


struct GnomeCmdSearchDialogClass
{
    GtkDialogClass parent_class;
};


struct SearchFileData
{
    GnomeVFSResult  result;
    gchar          *uri_str;
    GnomeVFSHandle *handle;
    gint            offset;
    guint           len;
    gchar           mem[SEARCH_BUFFER_SIZE];     // memory to search in the content of a file
};


struct SearchData
{
    struct ProtectedData
    {
        GList  *files;
        gchar  *msg;
        GMutex *mutex;

        ProtectedData(): files(0), msg(0), mutex(0)     {}
    };

    GnomeCmdSearchDialog *dialog;

    GnomeCmdDir *start_dir;                     // the directory to start searching from

    const gchar *name_pattern;                  // the pattern that file names should match to end up in the file list
    const gchar *content_pattern;               // the pattern that the content of a file should match to end up in the file list

    Filter *name_filter;
    regex_t *content_regex;
    gint context_id;                            // the context id of the status bar
    GList *match_dirs;                          // the directories which we found matching files in
    GThread *thread;
    ProtectedData pdata;
    gint update_gui_timeout_id;

    gboolean search_done;
    gboolean stopped;                           // stops the search routine if set to TRUE. This is done by the stop_button
    gboolean dialog_destroyed;                  // set when the search dialog is destroyed, also stops the search of course

    SearchData(GnomeCmdSearchDialog *dlg);

    void set_statusmsg(const gchar *msg=NULL);
    gchar *build_search_command();
    void search_dir_r(GnomeCmdDir *dir, long level);                                // searches a given directory for files that matches the criteria given by data

    gboolean name_matches(gchar *name)   {  return name_filter->match(name);  }     // determines if the name of a file matches an regexp
    gboolean content_matches(GnomeCmdFile *f);                                      // determines if the content of a file matches an regexp
    gboolean read_search_file(SearchFileData *, GnomeCmdFile *f);                   // loads a file in chunks and returns the content
    gboolean start_generic_search();
    gboolean start_local_search();

    static gboolean join_thread_func(SearchData *data);
};


struct GnomeCmdSearchDialog::Private
{
    SearchData data;                            // holds data needed by the search routines

    GtkWidget *filter_type_combo;
    GtkWidget *pattern_combo;
    GtkWidget *dir_browser;
    GtkWidget *recurse_combo;
    GtkWidget *find_text_combo;
    GtkWidget *find_text_check;
    GnomeCmdFileList *result_list;
    GtkWidget *statusbar;

    GtkWidget *case_check;
    GtkWidget *pbar;

    Private(GnomeCmdSearchDialog *dlg);
    ~Private();

    static gboolean on_list_keypressed (GtkWidget *result_list,  GdkEventKey *event, gpointer unused);
    static void on_filter_type_changed (GtkComboBox *combo, GnomeCmdSearchDialog *dialog);
    static void on_find_text_toggled (GtkToggleButton *togglebutton, GnomeCmdSearchDialog *dialog);

    static void on_dialog_show (GtkWidget *widget, GnomeCmdSearchDialog *dialog);
    static void on_dialog_hide (GtkWidget *widget, GnomeCmdSearchDialog *dialog);
    static gboolean on_dialog_delete (GtkWidget *widget, GdkEvent *event, GnomeCmdSearchDialog *dialog);
    static void on_dialog_size_allocate (GtkWidget *widget, GtkAllocation *allocation, GnomeCmdSearchDialog *dialog);
    static void on_dialog_response (GtkDialog *window, int response_id, GnomeCmdSearchDialog *dialog);
};


inline GnomeCmdSearchDialog::Private::Private(GnomeCmdSearchDialog *dlg): data(dlg)
{
    filter_type_combo = NULL;
    pattern_combo = NULL;
    dir_browser = NULL;
    recurse_combo = NULL;
    find_text_combo = NULL;
    find_text_check = NULL;
    result_list = NULL;
    statusbar = NULL;
    case_check = NULL;
    pbar = NULL;
}


inline GnomeCmdSearchDialog::Private::~Private()
{
}


inline SearchData::SearchData(GnomeCmdSearchDialog *dlg): dialog(dlg)
{
    start_dir = NULL;

    name_pattern = NULL;
    content_pattern = NULL;

    name_filter = NULL;
    content_regex = NULL;
    context_id = 0;
    match_dirs = NULL;
    thread = NULL;
    update_gui_timeout_id = 0;

    search_done = TRUE;
    stopped = TRUE;
    dialog_destroyed = FALSE;
}


G_DEFINE_TYPE (GnomeCmdSearchDialog, gnome_cmd_search_dialog, GTK_TYPE_DIALOG)


inline void free_search_file_data (SearchFileData *searchfile_data)
{
    if (searchfile_data->handle)
        gnome_vfs_close (searchfile_data->handle);

    g_free (searchfile_data->uri_str);
    g_free (searchfile_data);
}


gboolean SearchData::read_search_file(SearchFileData *searchfile_data, GnomeCmdFile *f)
{
    if (stopped)     // if the stop button was pressed, let's abort here
    {
        free_search_file_data (searchfile_data);
        return FALSE;
    }

    if (searchfile_data->len)
    {
      if ((searchfile_data->offset + searchfile_data->len) >= f->info->size)   // end, all has been read
      {
          free_search_file_data (searchfile_data);
          return FALSE;
      }

      // jump a big step backward to give the regex a chance
      searchfile_data->offset += searchfile_data->len - SEARCH_JUMP_SIZE;
      if (f->info->size < (searchfile_data->offset + (SEARCH_BUFFER_SIZE - 1)))
          searchfile_data->len = f->info->size - searchfile_data->offset;
      else
          searchfile_data->len = SEARCH_BUFFER_SIZE - 1;
    }
    else   // first time call of this function
        searchfile_data->len = MIN (f->info->size, SEARCH_BUFFER_SIZE - 1);

    searchfile_data->result = gnome_vfs_seek (searchfile_data->handle, GNOME_VFS_SEEK_START, searchfile_data->offset);
    if (searchfile_data->result != GNOME_VFS_OK)
    {
        g_warning (_("Failed to read file %s: %s"), searchfile_data->uri_str, gnome_vfs_result_to_string (searchfile_data->result));
        free_search_file_data (searchfile_data);
        return FALSE;
    }

    GnomeVFSFileSize ret;
    searchfile_data->result = gnome_vfs_read (searchfile_data->handle, searchfile_data->mem, searchfile_data->len, &ret);
    if (searchfile_data->result != GNOME_VFS_OK)
    {
        g_warning (_("Failed to read file %s: %s"), searchfile_data->uri_str, gnome_vfs_result_to_string (searchfile_data->result));
        free_search_file_data (searchfile_data);
        return FALSE;
    }

    searchfile_data->mem[searchfile_data->len] = '\0';

    return TRUE;
}


inline gboolean SearchData::content_matches(GnomeCmdFile *f)
{
    g_return_val_if_fail (f != NULL, FALSE);
    g_return_val_if_fail (f->info != NULL, FALSE);

    if (f->info->size==0)
        return FALSE;

    SearchFileData *search_file = g_new0 (SearchFileData, 1);
    search_file->uri_str = f->get_uri_str();
    search_file->result  = gnome_vfs_open (&search_file->handle, search_file->uri_str, GNOME_VFS_OPEN_READ);

    if (search_file->result != GNOME_VFS_OK)
    {
        g_warning (_("Failed to read file %s: %s"), search_file->uri_str, gnome_vfs_result_to_string (search_file->result));
        free_search_file_data (search_file);
        return FALSE;
    }

    regmatch_t match;

    while (read_search_file(search_file, f))
        if (regexec (content_regex, search_file->mem, 1, &match, 0) != REG_NOMATCH)
            return TRUE;        // stop on first match

    return FALSE;
}


inline gboolean handle_list_keypress (GnomeCmdFileList *fl, GdkEventKey *event)
{
    switch (event->keyval)
    {
        case GDK_F3:
            gnome_cmd_file_list_view (fl, -1);
            return TRUE;

        case GDK_F4:
            gnome_cmd_file_list_edit (fl);
            return TRUE;
    }

    return FALSE;
}


/*
 * callback function for 'g_list_foreach' to add default value to dropdownbox
 */
static void combo_box_insert_text (const gchar *text, GtkComboBox *widget)
{
    gtk_combo_box_append_text (widget, text);
}


inline void SearchData::set_statusmsg(const gchar *msg)
{
    gtk_statusbar_push (GTK_STATUSBAR (dialog->priv->statusbar), context_id, msg ? msg : "");
}


void SearchData::search_dir_r(GnomeCmdDir *dir, long level)
{
    if (!dir)
        return;

    if (stopped)     // if the stop button was pressed, let's abort here
        return;

    // update the search status data
    if (!dialog_destroyed)
    {
        g_mutex_lock (pdata.mutex);

        g_free (pdata.msg);
        pdata.msg = g_strdup_printf (_("Searching in: %s"), gnome_cmd_dir_get_display_path (dir));

        g_mutex_unlock (pdata.mutex);
    }

    gnome_cmd_dir_list_files (dir, FALSE);

    // let's iterate through all files
    for (GList *i=gnome_cmd_dir_get_files (dir); i; i=i->next)
    {
        if (stopped)         // if the stop button was pressed, let's abort here
            return;

        GnomeCmdFile *f = (GnomeCmdFile *) i->data;

        // if the current file is a directory, let's continue our recursion
        if (GNOME_CMD_IS_DIR (f) && level!=0)
        {
            // we don't want to go backwards or to follow symlinks
            if (!f->is_dotdot && strcmp (f->info->name, ".") != 0 && !GNOME_VFS_FILE_INFO_SYMLINK (f->info))
            {
                GnomeCmdDir *new_dir = GNOME_CMD_DIR (f);

                if (new_dir)
                {
                    gnome_cmd_dir_ref (new_dir);
                    search_dir_r(new_dir, level-1);
                    gnome_cmd_dir_unref (new_dir);
                }
            }
        }
        else                                                            // if the file is a regular one, it might match the search criteria
            if (f->info->type == GNOME_VFS_FILE_TYPE_REGULAR)
            {
                if (!name_matches(f->info->name))                       // if the name doesn't match, let's go to the next file
                    continue;

                if (dialog->defaults.default_profile.content_search && !content_matches(f))              // if the user wants to we should do some content matching here
                    continue;

                g_mutex_lock (pdata.mutex);                             // the file matched the search criteria, let's add it to the list
                pdata.files = g_list_append (pdata.files, f->ref());
                g_mutex_unlock (pdata.mutex);

                if (g_list_index (match_dirs, dir) == -1)               // also ref each directory that has a matching file
                    match_dirs = g_list_append (match_dirs, gnome_cmd_dir_ref (dir));
            }
    }
}


static gpointer perform_search_operation (SearchData *data)
{
    // unref all directories which contained matching files from last search
    if (data->match_dirs)
    {
        g_list_foreach (data->match_dirs, (GFunc) gnome_cmd_dir_unref, NULL);
        g_list_free (data->match_dirs);
        data->match_dirs = NULL;
    }

    data->search_dir_r(data->start_dir, data->dialog->defaults.default_profile.max_depth);

    // free regexps
    delete data->name_filter;
    data->name_filter = NULL;

    if (data->dialog->defaults.default_profile.content_search)
    {
        regfree (data->content_regex);
        g_free (data->content_regex);
        data->content_regex = NULL;
    }

    gnome_cmd_dir_unref (data->start_dir);      //  FIXME:  ???
    data->start_dir = NULL;

    data->search_done = TRUE;

    return NULL;
}


static gboolean update_search_status_widgets (SearchData *data)
{
    progress_bar_update (data->dialog->priv->pbar, PBAR_MAX);        // update the progress bar

    if (data->pdata.mutex)
    {
        g_mutex_lock (data->pdata.mutex);

        GList *files = data->pdata.files;
        data->pdata.files = NULL;

        data->set_statusmsg(data->pdata.msg);                       // update status bar with the latest message

        g_mutex_unlock (data->pdata.mutex);

        GnomeCmdFileList *fl = data->dialog->priv->result_list;

        for (GList *i = files; i; i = i->next)                      // add all files found since last update to the list
            fl->append_file(GNOME_CMD_FILE (i->data));

        gnome_cmd_file_list_free (files);
    }

    if (!data->search_done && !data->stopped || data->pdata.files)
        return TRUE;

    if (!data->dialog_destroyed)
    {
        int matches = data->dialog->priv->result_list->size();

        gchar *fmt = data->stopped ? ngettext("Found %d match - search aborted", "Found %d matches - search aborted", matches) :
                                    ngettext("Found %d match", "Found %d matches", matches);

        gchar *msg = g_strdup_printf (fmt, matches);

        data->set_statusmsg(msg);
        g_free (msg);

        gtk_widget_hide (data->dialog->priv->pbar);

        gtk_dialog_set_response_sensitive (*data->dialog, GnomeCmdSearchDialog::GCMD_RESPONSE_GOTO, matches>0);
        gtk_dialog_set_response_sensitive (*data->dialog, GnomeCmdSearchDialog::GCMD_RESPONSE_STOP, FALSE);
        gtk_dialog_set_response_sensitive (*data->dialog, GnomeCmdSearchDialog::GCMD_RESPONSE_FIND, TRUE);

        gtk_widget_grab_focus (*data->dialog->priv->result_list);         // set focus to result list
    }

    return FALSE;    // returning FALSE here stops the timeout callbacks
}


/*
 * This function gets called then the search-dialog is about the be destroyed.
 * The function waits for the last search-thread to finish and then frees the
 * data structure that has been shared between the search threads and the
 * main thread.
 */
gboolean SearchData::join_thread_func (SearchData *data)
{
    if (data->thread)
        g_thread_join (data->thread);

    if (data->pdata.mutex)
        g_mutex_free (data->pdata.mutex);

    return FALSE;
}


gboolean SearchData::start_generic_search()
{
    // create an re for file name matching
    name_filter = new Filter(name_pattern, dialog->defaults.default_profile.match_case, dialog->defaults.default_profile.syntax);

    // if we're going to search through file content create an re for that too
    if (dialog->defaults.default_profile.content_search)
    {
        content_regex = g_new0 (regex_t, 1);
        regcomp (content_regex, content_pattern, dialog->defaults.default_profile.match_case ? 0 : REG_ICASE);
    }

    if (!pdata.mutex)
        pdata.mutex = g_mutex_new ();

    thread = g_thread_create ((GThreadFunc) perform_search_operation, this, TRUE, NULL);

    return TRUE;
}


//  local search - using findutils

gchar *SearchData::build_search_command()
{
    gchar *file_pattern_utf8 = g_strdup (name_pattern);
    GError *error = NULL;

    switch (dialog->defaults.default_profile.syntax)
    {
        case Filter::TYPE_FNMATCH:
            if (!file_pattern_utf8 || !*file_pattern_utf8)
            {
                g_free (file_pattern_utf8);
                file_pattern_utf8 = g_strdup ("*");
            }
            else
                if (!g_utf8_strchr (file_pattern_utf8, -1, '*') && !g_utf8_strchr (file_pattern_utf8, -1, '?'))
                {
                    gchar *tmp = file_pattern_utf8;
                    file_pattern_utf8 = g_strconcat ("*", file_pattern_utf8, "*", NULL);
                    g_free (tmp);
                }
            break;

        case Filter::TYPE_REGEX:
            break;

        default:
            break;
    }

    gchar *file_pattern_locale = g_locale_from_utf8 (file_pattern_utf8, -1, NULL, NULL, &error);

    if (!file_pattern_locale)
    {
        gnome_cmd_error_message (file_pattern_utf8, error);
        g_free (file_pattern_utf8);
        return NULL;
    }

    gchar *file_pattern_quoted = quote_if_needed (file_pattern_locale);
    gchar *look_in_folder_utf8 = GNOME_CMD_FILE (start_dir)->get_real_path();
    gchar *look_in_folder_locale = g_locale_from_utf8 (look_in_folder_utf8, -1, NULL, NULL, NULL);

    if (!look_in_folder_locale)     // if for some reason a path was not returned, fallback to the user's home directory
        look_in_folder_locale = g_strconcat (g_get_home_dir (), G_DIR_SEPARATOR_S, NULL);

    gchar *look_in_folder_quoted = quote_if_needed (look_in_folder_locale);

    GString *command = g_string_sized_new (512);

    g_string_append (command, "find ");
    g_string_append (command, look_in_folder_quoted);

    if (dialog->defaults.default_profile.max_depth!=-1)
        g_string_append_printf (command, " -maxdepth %i", dialog->defaults.default_profile.max_depth+1);

    switch (dialog->defaults.default_profile.syntax)
    {
        case Filter::TYPE_FNMATCH:
            g_string_append_printf (command, " -iname '%s'", file_pattern_utf8);
            break;

        case Filter::TYPE_REGEX:
            g_string_append_printf (command, " -regextype posix-extended -iregex '.*/.*%s.*'", file_pattern_utf8);
            break;
    }

    if (dialog->defaults.default_profile.content_search)
    {
        static const gchar GREP_COMMAND[] = "grep";

        if (dialog->defaults.default_profile.match_case)
            g_string_append_printf (command, " '!' -type p -exec %s -E -q '%s' {} \\;", GREP_COMMAND, content_pattern);
        else
            g_string_append_printf (command, " '!' -type p -exec %s -E -q -i '%s' {} \\;", GREP_COMMAND, content_pattern);
    }

    g_string_append (command, " -print");

    g_free (file_pattern_utf8);
    g_free (file_pattern_locale);
    g_free (file_pattern_quoted);
    g_free (look_in_folder_utf8);
    g_free (look_in_folder_locale);
    g_free (look_in_folder_quoted);

    return g_string_free (command, FALSE);
}


static void child_command_set_pgid_cb (gpointer unused)
{
    if (setpgid (0, 0) < 0)
        g_print (_("Failed to set process group id of child %d: %s.\n"), getpid (), g_strerror (errno));
}


static gboolean handle_search_command_stdout_io (GIOChannel *ioc, GIOCondition condition, SearchData *data)
{
    gboolean broken_pipe = FALSE;

    if (condition & G_IO_IN)
    {
        GError *error = NULL;

        GString *string = g_string_new (NULL);

        GTimer *timer = g_timer_new ();
        g_timer_start (timer);

        while (!ioc->is_readable);

        do
        {
            gint status;

            if (data->stopped)
            {
                broken_pipe = TRUE;
                break;
            }

            do
            {
                status = g_io_channel_read_line_string (ioc, string, NULL, &error);

                if (status == G_IO_STATUS_EOF)
                    broken_pipe = TRUE;
                else
                    if (status == G_IO_STATUS_AGAIN)
                        while (gtk_events_pending ())
                        {
                            if (data->stopped)
                                return FALSE;

                            gtk_main_iteration ();
                        }
            }
            while (status == G_IO_STATUS_AGAIN && !broken_pipe);

            if (broken_pipe)
                break;

            if (status != G_IO_STATUS_NORMAL)
            {
                if (error)
                {
                    g_warning ("handle_search_command_stdout_io(): %s", error->message);
                    g_error_free (error);
                }
                continue;
            }

            string = g_string_truncate (string, string->len - 1);

            if (string->len <= 1)
                continue;

            gchar *utf8 = g_filename_display_name (string->str);

            GnomeCmdFile *f = gnome_cmd_file_new (utf8);

            if (f)
                data->dialog->priv->result_list->append_file(f);

            g_free (utf8);

            gulong duration;

            g_timer_elapsed (timer, &duration);

            if (duration > GNOME_SEARCH_TOOL_REFRESH_DURATION)
            {
                while (gtk_events_pending ())
                {
                    if (data->stopped)
                        return FALSE;

                    gtk_main_iteration ();
                }

                g_timer_reset (timer);
            }
        }
        while (g_io_channel_get_buffer_condition (ioc) & G_IO_IN);

        g_string_free (string, TRUE);
        g_timer_destroy (timer);
    }

    if (!(condition & G_IO_IN) || broken_pipe)
    {
        g_io_channel_shutdown (ioc, TRUE, NULL);

        data->search_done = TRUE;

        return FALSE;
    }

    return TRUE;
}


gboolean SearchData::start_local_search()
{
    gchar *command = build_search_command();

    g_return_val_if_fail (command!=NULL, FALSE);

    DEBUG ('g', "running: %s\n", command);

    GError *error = NULL;
    gchar **argv  = NULL;
    gint child_stdout;

    if (!g_shell_parse_argv (command, NULL, &argv, &error))
    {
        gnome_cmd_error_message (_("Error parsing the search command."), error);

        g_free (command);
        g_strfreev (argv);

        return FALSE;
    }

    g_free (command);

    if (!g_spawn_async_with_pipes (NULL, argv, NULL, GSpawnFlags (G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL), child_command_set_pgid_cb, NULL, NULL, NULL, &child_stdout, NULL, &error))
    {
        gnome_cmd_error_message (_("Error running the search command."), error);

        g_strfreev (argv);

        return FALSE;
    }

#ifdef G_OS_WIN32
    GIOChannel *ioc_stdout = g_io_channel_win32_new_fd (child_stdout);
#else
    GIOChannel *ioc_stdout = g_io_channel_unix_new (child_stdout);
#endif

    g_io_channel_set_encoding (ioc_stdout, NULL, NULL);
    g_io_channel_set_flags (ioc_stdout, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch (ioc_stdout, GIOCondition (G_IO_IN | G_IO_HUP), (GIOFunc) handle_search_command_stdout_io, this);

    g_io_channel_unref (ioc_stdout);
    g_strfreev (argv);

    return TRUE;
}


gboolean GnomeCmdSearchDialog::Private::on_list_keypressed(GtkWidget *result_list,  GdkEventKey *event, gpointer unused)
{
    if (GNOME_CMD_FILE_LIST (result_list)->key_pressed(event) ||
        handle_list_keypress (GNOME_CMD_FILE_LIST (result_list), event))
    {
        stop_kp (GTK_OBJECT (result_list));
        return TRUE;
    }

    return FALSE;
}


void GnomeCmdSearchDialog::Private::on_filter_type_changed(GtkComboBox *combo, GnomeCmdSearchDialog *dialog)
{
    gtk_widget_grab_focus (dialog->priv->pattern_combo);
}


void GnomeCmdSearchDialog::Private::on_dialog_show(GtkWidget *widget, GnomeCmdSearchDialog *dialog)
{
    GnomeCmdData::Selection &profile = dialog->defaults.default_profile;

    gtk_combo_box_set_active (GTK_COMBO_BOX (dialog->priv->filter_type_combo), (int) profile.syntax);
    gtk_combo_box_set_active (GTK_COMBO_BOX (dialog->priv->recurse_combo), profile.max_depth+1);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dialog->priv->find_text_check), profile.content_search);

    dialog->priv->data.start_dir = main_win->fs(ACTIVE)->get_directory();

    gchar *uri = gnome_cmd_dir_get_uri_str (dialog->priv->data.start_dir);
    gtk_file_chooser_set_current_folder_uri (GTK_FILE_CHOOSER (dialog->priv->dir_browser), uri);
    g_free (uri);
}


void GnomeCmdSearchDialog::Private::on_dialog_hide(GtkWidget *widget, GnomeCmdSearchDialog *dialog)
{
    GnomeCmdData::Selection &profile = dialog->defaults.default_profile;

    dialog->priv->result_list->remove_all_files();

    profile.syntax = (Filter::Type) gtk_combo_box_get_active (GTK_COMBO_BOX (dialog->priv->filter_type_combo));
    profile.max_depth = gtk_combo_box_get_active (GTK_COMBO_BOX (dialog->priv->recurse_combo)) - 1;
    profile.content_search = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dialog->priv->find_text_check));
}


gboolean GnomeCmdSearchDialog::Private::on_dialog_delete(GtkWidget *widget, GdkEvent *event, GnomeCmdSearchDialog *dialog)
{
    return event->type==GDK_DELETE;
}


void GnomeCmdSearchDialog::Private::on_find_text_toggled(GtkToggleButton *togglebutton, GnomeCmdSearchDialog *dialog)
{
    if (gtk_toggle_button_get_active (togglebutton))
    {
        gtk_widget_set_sensitive (dialog->priv->find_text_combo, TRUE);
        gtk_widget_set_sensitive (dialog->priv->case_check, TRUE);
        gtk_widget_grab_focus (dialog->priv->find_text_combo);
    }
    else
    {
        gtk_widget_set_sensitive (dialog->priv->find_text_combo, FALSE);
        gtk_widget_set_sensitive (dialog->priv->case_check, FALSE);
    }
}


void GnomeCmdSearchDialog::Private::on_dialog_size_allocate(GtkWidget *widget, GtkAllocation *allocation, GnomeCmdSearchDialog *dialog)
{
    dialog->defaults.width  = allocation->width;
    dialog->defaults.height = allocation->height;
}


void GnomeCmdSearchDialog::Private::on_dialog_response(GtkDialog *window, int response_id, GnomeCmdSearchDialog *dialog)
{
    switch (response_id)
    {
        case GCMD_RESPONSE_STOP:
            {
                dialog->priv->data.stopped = TRUE;
                gtk_dialog_set_response_sensitive (*dialog, GCMD_RESPONSE_STOP, FALSE);
            }
            break;

        case GCMD_RESPONSE_FIND:
            {
                SearchData &data = dialog->priv->data;

                if (data.thread)
                {
                    g_thread_join (data.thread);
                    data.thread = NULL;
                }

                data.search_done = TRUE;
                data.stopped = TRUE;
                data.dialog_destroyed = FALSE;

                data.dialog = dialog;
                data.name_pattern = gtk_combo_box_get_active_text (GTK_COMBO_BOX (dialog->priv->pattern_combo));
                data.content_pattern = gtk_combo_box_get_active_text (GTK_COMBO_BOX (dialog->priv->find_text_combo));
                dialog->defaults.default_profile.max_depth = gtk_combo_box_get_active (GTK_COMBO_BOX (dialog->priv->recurse_combo)) - 1;
                dialog->defaults.default_profile.syntax = (Filter::Type) gtk_combo_box_get_active (GTK_COMBO_BOX (dialog->priv->filter_type_combo));
                dialog->defaults.default_profile.content_search = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dialog->priv->find_text_check));
                dialog->defaults.default_profile.match_case = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dialog->priv->case_check));

                data.context_id = gtk_statusbar_get_context_id (GTK_STATUSBAR (dialog->priv->statusbar), "info");
                data.content_regex = NULL;
                data.match_dirs = NULL;

                gchar *dir_str = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (dialog->priv->dir_browser));
                GnomeVFSURI *uri = gnome_vfs_uri_new (dir_str);
                g_free (dir_str);

                dir_str = gnome_vfs_unescape_string (gnome_vfs_uri_get_path (uri), NULL);
                gchar *dir_path = g_strconcat (dir_str, G_DIR_SEPARATOR_S, NULL);
                g_free (dir_str);

                GnomeCmdCon *con = gnome_cmd_dir_get_connection (data.start_dir);

                if (strncmp(dir_path, gnome_cmd_con_get_root_path (con), con->root_path->len)!=0)
                {
                    if (!gnome_vfs_uri_is_local (uri))
                    {
                        gnome_cmd_show_message (*dialog, stringify(g_strdup_printf (_("Failed to change directory outside of %s"),
                                                                                              gnome_cmd_con_get_root_path (con))));
                        gnome_vfs_uri_unref (uri);
                        g_free (dir_path);

                        break;
                    }
                    else
                        data.start_dir = gnome_cmd_dir_new (get_home_con (), gnome_cmd_con_create_path (get_home_con (), dir_path));
                }
                else
                    data.start_dir = gnome_cmd_dir_new (con, gnome_cmd_con_create_path (con, dir_path + con->root_path->len));

                gnome_cmd_dir_ref (data.start_dir);

                gnome_vfs_uri_unref (uri);
                g_free (dir_path);

                // save default settings
                gnome_cmd_data.search_defaults.name_patterns.add(data.name_pattern);

                if (dialog->defaults.default_profile.content_search)
                {
                    gnome_cmd_data.search_defaults.content_patterns.add(data.content_pattern);
                    gnome_cmd_data.intviewer_defaults.text_patterns.add(data.content_pattern);
                }

                data.search_done = FALSE;
                data.stopped = FALSE;

                dialog->priv->result_list->remove_all_files();

                if (gnome_cmd_con_is_local (con) ? data.start_local_search() : data.start_generic_search())
                {
                    data.set_statusmsg();
                    gtk_widget_show (dialog->priv->pbar);
                    data.update_gui_timeout_id = g_timeout_add (gnome_cmd_data.gui_update_rate, (GSourceFunc) update_search_status_widgets, &data);

                    gtk_dialog_set_response_sensitive (*dialog, GCMD_RESPONSE_GOTO, FALSE);
                    gtk_dialog_set_response_sensitive (*dialog, GCMD_RESPONSE_STOP, TRUE);
                    gtk_dialog_set_response_sensitive (*dialog, GCMD_RESPONSE_FIND, FALSE);
                }
            }
            break;

        case GCMD_RESPONSE_GOTO:
            {
                GnomeCmdFile *f = dialog->priv->result_list->get_selected_file();

                if (!f)
                    break;

                gchar *fpath = f->get_path();
                gchar *dpath = g_path_get_dirname (fpath);

                GnomeCmdFileSelector *fs = main_win->fs(ACTIVE);

                if (fs->file_list()->locked)
                    fs->new_tab(f->get_parent_dir());
                else
                    fs->file_list()->goto_directory(dpath);

                fs->file_list()->focus_file(f->get_name(), TRUE);

                g_free (fpath);
                g_free (dpath);
            }

        case GTK_RESPONSE_NONE:
        case GTK_RESPONSE_DELETE_EVENT:
        case GTK_RESPONSE_CANCEL:
        case GTK_RESPONSE_CLOSE:
            gtk_widget_hide (*dialog);
            g_signal_stop_emission_by_name (dialog, "response");
            break;

        case GTK_RESPONSE_HELP:
            gnome_cmd_help_display ("gnome-commander.xml", "gnome-commander-search");
            g_signal_stop_emission_by_name (dialog, "response");
            break;

        default :
            g_assert_not_reached ();
    }
}


static void gnome_cmd_search_dialog_init (GnomeCmdSearchDialog *dialog)
{
    dialog->priv = new GnomeCmdSearchDialog::Private(dialog);

    gtk_window_set_title (*dialog, _("Search..."));
    gtk_window_set_resizable (*dialog, TRUE);
    gtk_dialog_set_has_separator (*dialog, FALSE);
    gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);
    gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (dialog)->vbox), 2);

    GtkWidget *vbox = gtk_vbox_new (FALSE, 6);
    gtk_container_set_border_width (GTK_CONTAINER (vbox), 5);
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), vbox, TRUE, TRUE, 0);

    GtkWidget *table = gtk_table_new (5, 2, FALSE);
    gtk_table_set_row_spacings (GTK_TABLE (table), 6);
    gtk_table_set_col_spacings (GTK_TABLE (table), 6);
    gtk_box_pack_start (GTK_BOX (vbox), table, FALSE, TRUE, 0);


    // search for
    dialog->priv->filter_type_combo = gtk_combo_box_new_text ();
    gtk_combo_box_append_text (GTK_COMBO_BOX (dialog->priv->filter_type_combo), _("Name matches regex:"));
    gtk_combo_box_append_text (GTK_COMBO_BOX (dialog->priv->filter_type_combo), _("Name contains:"));
    dialog->priv->pattern_combo = gtk_combo_box_entry_new_text ();
    table_add (table, dialog->priv->filter_type_combo, 0, 0, GTK_FILL);
    table_add (table, dialog->priv->pattern_combo, 1, 0, (GtkAttachOptions) (GTK_EXPAND|GTK_FILL));


    // search in
    dialog->priv->dir_browser =  gtk_file_chooser_button_new (_("Select Directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (dialog->priv->dir_browser), FALSE);
    table_add (table, create_label_with_mnemonic (*dialog, _("_Look in folder:"), dialog->priv->dir_browser), 0, 1, GTK_FILL);
    table_add (table, dialog->priv->dir_browser, 1, 1, (GtkAttachOptions) (GTK_EXPAND|GTK_FILL));


    // recurse check
    dialog->priv->recurse_combo = gtk_combo_box_new_text ();

    gtk_combo_box_append_text (GTK_COMBO_BOX (dialog->priv->recurse_combo), _("Unlimited depth"));
    gtk_combo_box_append_text (GTK_COMBO_BOX (dialog->priv->recurse_combo), _("Current directory only"));
    for (int i=1; i<=40; ++i)
    {
       gchar *item = g_strdup_printf (ngettext("%i level", "%i levels", i), i);
       gtk_combo_box_append_text (GTK_COMBO_BOX (dialog->priv->recurse_combo), item);
       g_free (item);
    }

    table_add (table, create_label_with_mnemonic (*dialog, _("Search _recursively:"), dialog->priv->recurse_combo), 0, 2, GTK_FILL);
    table_add (table, dialog->priv->recurse_combo, 1, 2, (GtkAttachOptions) (GTK_EXPAND|GTK_FILL));


    // find text
    dialog->priv->find_text_check = create_check_with_mnemonic (*dialog, _("Contains _text:"), "find_text");
    table_add (table, dialog->priv->find_text_check, 0, 3, GTK_FILL);

    dialog->priv->find_text_combo = gtk_combo_box_entry_new_text ();
    table_add (table, dialog->priv->find_text_combo, 1, 3, (GtkAttachOptions) (GTK_EXPAND|GTK_FILL));
    gtk_widget_set_sensitive (dialog->priv->find_text_combo, FALSE);

    gtk_combo_box_set_active (GTK_COMBO_BOX (dialog->priv->find_text_combo), 0);


    // case check
    dialog->priv->case_check = create_check_with_mnemonic (*dialog, _("Case sensiti_ve"), "case_check");
    gtk_table_attach (GTK_TABLE (table), dialog->priv->case_check, 1, 2, 4, 5, (GtkAttachOptions) (GTK_FILL), (GtkAttachOptions) (0), 0, 0);
    gtk_widget_set_sensitive (dialog->priv->case_check, FALSE);


    // file list
    GtkWidget *sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    dialog->priv->result_list = new GnomeCmdFileList(GnomeCmdFileList::COLUMN_NAME,GTK_SORT_ASCENDING);
    gtk_widget_set_size_request (*dialog->priv->result_list, -1, 200);
    gtk_container_add (GTK_CONTAINER (sw), *dialog->priv->result_list);
    gtk_container_set_border_width (GTK_CONTAINER (dialog->priv->result_list), 4);


    // status
    dialog->priv->statusbar = gtk_statusbar_new ();
    gtk_statusbar_set_has_resize_grip (GTK_STATUSBAR (dialog->priv->statusbar), FALSE);
    gtk_box_pack_start (GTK_BOX (vbox), dialog->priv->statusbar, FALSE, TRUE, 0);


    // progress
    dialog->priv->pbar = create_progress_bar (*dialog);
    gtk_progress_set_show_text (GTK_PROGRESS (dialog->priv->pbar), FALSE);
    gtk_progress_set_activity_mode (GTK_PROGRESS (dialog->priv->pbar), TRUE);
    gtk_progress_configure (GTK_PROGRESS (dialog->priv->pbar), 0, 0, PBAR_MAX);
    gtk_box_pack_start (GTK_BOX (dialog->priv->statusbar),dialog->priv-> pbar, FALSE, TRUE, 0);


    dialog->priv->result_list->update_style();

    gtk_widget_show_all (table);
    gtk_widget_show_all (sw);
    gtk_widget_show (dialog->priv->statusbar);
    gtk_widget_hide (dialog->priv->pbar);
    gtk_widget_show (vbox);

    g_signal_connect_swapped (gtk_bin_get_child (GTK_BIN (dialog->priv->pattern_combo)), "activate", G_CALLBACK (gtk_window_activate_default), dialog);
    g_signal_connect_swapped (gtk_bin_get_child (GTK_BIN (dialog->priv->find_text_combo)), "activate", G_CALLBACK (gtk_window_activate_default), dialog);
}


static void gnome_cmd_search_dialog_finalize (GObject *object)
{
    GnomeCmdSearchDialog *dialog = GNOME_CMD_SEARCH_DIALOG (object);
    SearchData &data = dialog->priv->data;

    if (!data.search_done)
        g_source_remove (data.update_gui_timeout_id);

    // stop and wait for search thread to exit
    data.stopped = TRUE;
    data.dialog_destroyed = TRUE;
    g_timeout_add (1, (GSourceFunc) SearchData::join_thread_func, &data);

    // unref all directories which contained matching files from last search
    if (data.pdata.mutex)
    {
        g_mutex_lock (data.pdata.mutex);
        if (data.match_dirs)
        {
            g_list_foreach (data.match_dirs, (GFunc) gnome_cmd_dir_unref, NULL);
            g_list_free (data.match_dirs);
            data.match_dirs = NULL;
        }
        g_mutex_unlock (data.pdata.mutex);
    }

    delete dialog->priv;

    G_OBJECT_CLASS (gnome_cmd_search_dialog_parent_class)->finalize (object);
}


static void gnome_cmd_search_dialog_class_init (GnomeCmdSearchDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = gnome_cmd_search_dialog_finalize;
}


GnomeCmdSearchDialog::GnomeCmdSearchDialog(GnomeCmdData::SearchConfig &cfg): defaults(cfg)
{
    gtk_window_set_default_size (*this, defaults.width, defaults.height);

    gtk_dialog_add_buttons (*this,
                            GTK_STOCK_HELP, GTK_RESPONSE_HELP,
                            GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
                            GTK_STOCK_JUMP_TO, GCMD_RESPONSE_GOTO,
                            GTK_STOCK_STOP, GCMD_RESPONSE_STOP,
                            GTK_STOCK_FIND, GCMD_RESPONSE_FIND,
                            NULL);

    gtk_dialog_set_response_sensitive (*this, GCMD_RESPONSE_GOTO, FALSE);
    gtk_dialog_set_response_sensitive (*this, GCMD_RESPONSE_STOP, FALSE);

    gtk_dialog_set_default_response (*this, GCMD_RESPONSE_FIND);


    if (!defaults.name_patterns.empty())
    {
        g_list_foreach (defaults.name_patterns.ents, (GFunc) combo_box_insert_text, priv->pattern_combo);
        gtk_combo_box_set_active (GTK_COMBO_BOX (priv->pattern_combo), 0);
    }

    if (!defaults.content_patterns.empty())
        g_list_foreach (defaults.content_patterns.ents, (GFunc) combo_box_insert_text, priv->find_text_combo);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (priv->case_check), defaults.default_profile.match_case);


    gtk_widget_grab_focus (priv->pattern_combo);

    g_signal_connect (priv->result_list, "key-press-event", G_CALLBACK (Private::on_list_keypressed), this);
    g_signal_connect (priv->filter_type_combo, "changed", G_CALLBACK (Private::on_filter_type_changed), this);
    g_signal_connect (priv->find_text_check, "toggled", G_CALLBACK (Private::on_find_text_toggled), this);

    g_signal_connect (this, "show", G_CALLBACK (Private::on_dialog_show), this);
    g_signal_connect (this, "hide", G_CALLBACK (Private::on_dialog_hide), this);
    g_signal_connect (this, "delete-event", G_CALLBACK (Private::on_dialog_delete), this);
    g_signal_connect (this, "size-allocate", G_CALLBACK (Private::on_dialog_size_allocate), this);
    g_signal_connect (this, "response", G_CALLBACK (Private::on_dialog_response), this);
}


GnomeCmdSearchDialog::~GnomeCmdSearchDialog()
{
}
