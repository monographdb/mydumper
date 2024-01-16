/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    David Ducos, Percona (david dot ducos at percona dot com)
*/
#include <mysql.h>
#include <errmsg.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include "common.h"
#include <errno.h>
#include <unistd.h>
#include "myloader.h"
//#include "myloader_jobs_manager.h"
#include "myloader_common.h"
#include "myloader_global.h"
#include "connection.h"
#include "myloader_intermediate_queue.h"
#include "myloader_process.h"
const guint err_oom = 216;
gboolean skip_definer = FALSE;
int restore_data_in_gstring_by_statement(struct thread_data *td, GString *data, gboolean is_schema, guint *query_counter)
{
  guint en=mysql_real_query(td->thrconn, data->str, data->len);
  if (en) {
    uint my_err = mysql_errno(td->thrconn);
    if (is_schema)
      g_warning("Thread %d: Error restoring %d: %s %s", td->thread_id, en, data->str, mysql_error(td->thrconn));
    else{
      g_warning("Thread %d: Error restoring %d: %s", td->thread_id, en, mysql_error(td->thrconn));
    }
    // Maybe need reconnect
    if (mysql_ping(td->thrconn)) {
      m_connect(td->thrconn);
      execute_gstring(td->thrconn, set_session);
      execute_use(td);
      if (!is_schema && commit_count > 1) {
        g_critical("Thread %d: Lost connection error", td->thread_id);
        errors++;
        return 2;
      }
    }

    if (my_err == err_oom) {
      // Keep retry if monograph is out of memory
      const guint max_retry = 64;
      guint wait_secs = 1;
      guint retry_cost = 0;
      for (guint r = 0; my_err == err_oom && r < max_retry; r++) {
        retry_cost += wait_secs;
        sleep(wait_secs);
        if (wait_secs < 128) wait_secs <<= 1;
        g_warning("Thread %d: Retrying after OOM %d", td->thread_id, r);
        g_atomic_int_inc(&(detailed_errors.retries));
        en = mysql_real_query(td->thrconn, data->str, data->len);
        my_err = en ? mysql_errno(td->thrconn) : 0;
      }
      g_info("Thread %d: retry cost total %d seconds", td->thread_id, retry_cost);
    } else {
      g_warning("Thread %d: Retrying last failed executed statement", td->thread_id);
      g_atomic_int_inc(&(detailed_errors.retries));
      en = mysql_real_query(td->thrconn, data->str, data->len);
    }
    if (en) {
      if (is_schema)
        g_critical("Thread %d: Error restoring: %s %s", td->thread_id, data->str, mysql_error(td->thrconn));
      else{
        g_critical("Thread %d: Error restoring: %s", td->thread_id, mysql_error(td->thrconn));
      }
      errors++;
      return 1;
    }
  }

  *query_counter=*query_counter+1;
  if (is_schema==FALSE) {
    if (commit_count > 1) {
      if (*query_counter == commit_count) {
        *query_counter= 0;
        if (!m_query(td->thrconn, "COMMIT", m_warning, "COMMIT failed")) {
          errors++;
          return 2;
        }
        m_query(td->thrconn, "START TRANSACTION", m_warning, "START TRANSACTION failed");
      }
    }
  }
  g_string_set_size(data, 0);
  return 0;
}

int restore_data_in_gstring(struct thread_data *td, GString *data, gboolean is_schema, guint *query_counter)
{
  int i=0;
  int r=0;
  if (data != NULL && data->len > 4){
    gchar** line=g_strsplit(data->str, ";\n", -1);
    for (i=0; i < (int)g_strv_length(line);i++){
       if (strlen(line[i])>2){
         GString *str=g_string_new(line[i]);
         g_string_append_c(str,';');
         r+=restore_data_in_gstring_by_statement(td, str, is_schema, query_counter);
         g_string_free(str,TRUE);
       }
    }
    g_strfreev(line);
  }
  return r;
}

int split_and_restore_data_in_gstring_by_statement(struct thread_data *td,
                  GString *data, gboolean is_schema, guint *query_counter, guint offset_line)
{
  char *next_line=g_strstr_len(data->str,-1,"VALUES") + 6;
  char *insert_statement_prefix=g_strndup(data->str,next_line - data->str);
  guint insert_statement_prefix_len=strlen(insert_statement_prefix);
  int r=0;
  guint tr=0,current_offset_line=offset_line-1;
  gchar *current_line=next_line;
  next_line=g_strstr_len(current_line, -1, "\n");
  GString * new_insert=g_string_sized_new(strlen(insert_statement_prefix));
  guint current_rows=0;
  do {
    current_rows=0;
    g_string_set_size(new_insert, 0);
    new_insert=g_string_append(new_insert,insert_statement_prefix);
    do {
      char *line=g_strndup(current_line, next_line - current_line);
      g_string_append(new_insert, line);
      g_free(line);
      current_rows++;
      current_line=next_line+1;
      next_line=g_strstr_len(current_line, -1, "\n");
      current_offset_line++;
    } while (current_rows < rows && next_line != NULL);
    if (new_insert->len > insert_statement_prefix_len)
      tr=restore_data_in_gstring_by_statement(td, new_insert, is_schema, query_counter);
    else
      tr=0;
    r+=tr;
    if (tr > 0){
      g_critical("Error occurs between lines: %d and %d in a splited INSERT: %s",offset_line,current_offset_line,mysql_error(td->thrconn));
    }
    offset_line=current_offset_line+1;
    current_line++; // remove trailing ,
  } while (next_line != NULL);
  g_string_free(new_insert,TRUE);
  g_free(insert_statement_prefix);
  g_string_set_size(data, 0);
  return r;

}

gboolean load_data_mutex_locate( gchar * filename , GMutex ** mutex){
  g_mutex_lock(load_data_list_mutex);
  gchar * orig_key=NULL;
  if (!g_hash_table_lookup_extended(load_data_list,filename, (gpointer*) orig_key, (gpointer*) *mutex)){
    *mutex=g_mutex_new();
    g_mutex_lock(*mutex);
    g_hash_table_insert(load_data_list,g_strdup(filename), *mutex);
    g_mutex_unlock(load_data_list_mutex);
    return TRUE;
  }
  if (orig_key!=NULL)
    g_hash_table_remove(load_data_list, orig_key);
  g_mutex_unlock(load_data_list_mutex);
  return FALSE;
}

void wait_til_data_file_is_close( gchar * filename ){
  GMutex * mutex=NULL;
  if (load_data_mutex_locate(filename, &mutex)){
    g_mutex_lock(mutex);
    // TODO we need to free filename and mutex from the hash.
  }
}

void release_load_data_as_it_is_close( gchar * filename ){
  g_mutex_lock(load_data_list_mutex);
  GMutex *mutex = g_hash_table_lookup(load_data_list,filename);
  if (mutex == NULL){
    g_hash_table_insert(load_data_list,g_strdup(filename), NULL);
  }else{
    g_mutex_unlock(mutex);
  }
  g_mutex_unlock(load_data_list_mutex);
}


int restore_data_from_file(struct thread_data *td, char *database, char *table,
                  char *filename, gboolean is_schema){
  FILE *infile=NULL;
  int r=0;
  gboolean eof = FALSE;
  guint query_counter = 0;
  GString *data = g_string_sized_new(256);
  guint line=0,preline=0;
  gchar *path = g_build_filename(directory, filename, NULL);
  infile=myl_open(path,"r");

  g_log_set_always_fatal(G_LOG_LEVEL_ERROR|G_LOG_LEVEL_CRITICAL);

  if (!infile) {
    g_critical("cannot open file %s (%d)", filename, errno);
    errors++;
    return 1;
  }
  if (!is_schema && (commit_count > 1) )
    m_query(td->thrconn, "START TRANSACTION", m_warning, "START TRANSACTION failed");
  guint tr=0;
  gchar *load_data_filename=NULL;
  gchar *load_data_fifo_filename=NULL;
  gchar *new_load_data_fifo_filename=NULL;
  while (eof == FALSE) {
    if (read_data(infile, data, &eof, &line)) {
      if (g_strrstr(&data->str[data->len >= 5 ? data->len - 5 : 0], ";\n")) {
        if ( skip_definer && g_str_has_prefix(data->str,"CREATE")){
          remove_definer(data);
        }
        if (rows > 0 && g_strrstr_len(data->str,6,"INSERT"))
          tr=split_and_restore_data_in_gstring_by_statement(td,
            data, is_schema, &query_counter,preline);
        else{
          if (g_strrstr_len(data->str,10,"LOAD DATA ")){
            GString *new_data = NULL;
            gchar *from = g_strstr_len(data->str, -1, "'");
            from++;
            gchar *to = g_strstr_len(from, -1, "'");
            load_data_filename=g_strndup(from, to-from);
            wait_til_data_file_is_close(load_data_filename);
            gchar **command=NULL;
            if (get_command_and_basename(load_data_filename, &command, &load_data_fifo_filename)){ 
              if (fifo_directory != NULL){
                new_data = g_string_new_len(data->str, from - data->str);
                g_string_append(new_data, fifo_directory);
                g_string_append_c(new_data, '/');
                g_string_append(new_data, from);
                g_string_free(data,TRUE);
                from = g_strstr_len(new_data->str, -1, "'") + 1;
                data=new_data;
                to = g_strstr_len(from, -1, "'");
              }
              guint a=0;
              for(;a<strlen(load_data_filename)-strlen(load_data_fifo_filename);a++){
                *to=' '; to--;
              }
              *to='\'';

              if (fifo_directory != NULL){
                new_load_data_fifo_filename=g_strdup_printf("%s/%s", fifo_directory, load_data_fifo_filename);
                g_free(load_data_fifo_filename);
                load_data_fifo_filename=new_load_data_fifo_filename;
              }
              g_message("load_data_fifo_filename:: %s", load_data_fifo_filename);
              if (mkfifo(load_data_fifo_filename,0666)){
                g_critical("cannot create named pipe %s (%d)", load_data_fifo_filename, errno);
              }
	      execute_file_per_thread(load_data_filename, load_data_fifo_filename, command );
              release_load_data_as_it_is_close(load_data_fifo_filename);
//              g_free(fifo_name);
            }else{
	      g_message("load_data_fifo_filename:: %s", load_data_fifo_filename);

	    }

            tr=restore_data_in_gstring_by_statement(td, data, is_schema, &query_counter);
            if (load_data_fifo_filename!=NULL) 
              m_remove(NULL, load_data_fifo_filename);
            else
              m_remove(NULL, load_data_filename);

          }else{
            tr=restore_data_in_gstring_by_statement(td, data, is_schema, &query_counter);
          }
        }
        r|= tr;
        if (tr > 0){
          // FIXME: CLI option for max_errors (and AUTO for --identifier-quote-character), test
          if (max_errors && errors > max_errors) {
            m_critical("Error occurs between lines: %d and %d on file %s: %s",preline,line,filename,mysql_error(td->thrconn));
          } else {
            g_critical("Error occurs between lines: %d and %d on file %s: %s",preline,line,filename,mysql_error(td->thrconn));
          }
        }
        g_string_set_size(data, 0);
        preline=line+1;
      }
    } else {
      g_critical("error reading file %s (%d)", filename, errno);
      errors++;
      return r;
    }
  }
  if (!is_schema && (commit_count > 1) && !m_query(td->thrconn, "COMMIT", m_warning, "COMMIT failed")) {
    g_critical("Error committing data for %s.%s from file %s: %s",
               database, table, filename, mysql_error(td->thrconn));
    errors++;
  }
  g_string_free(data, TRUE);

  g_free(load_data_filename);

/*  if (load_data_fifo_filename != NULL){
    if (remove(load_data_fifo_filename)) {
      g_warning("Thread %d: Failed to remove fifo file : %s", td->thread_id, load_data_fifo_filename);
    }else{
      g_message("Thread %d: Fifo file removed: %s", td->thread_id, load_data_fifo_filename);
    }
    remove_fifo_file(load_data_fifo_filename);
    g_free(load_data_fifo_filename);
  }
*/
  myl_close(filename, infile, TRUE);
  g_free(path);
  return r;
}

