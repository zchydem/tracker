/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008, Nokia
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <libtracker-common/tracker-dbus.h>

#include "tracker-crawler.h"
#include "tracker-marshal.h"
#include "tracker-miner-process.h"
#include "tracker-monitor.h"

#define TRACKER_MINER_PROCESS_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_MINER_PROCESS, TrackerMinerProcessPrivate))

struct TrackerMinerProcessPrivate {
	TrackerMonitor *monitor;
	TrackerCrawler *crawler;

	/* File queues for indexer */
	guint		item_queues_handler_id;

	GQueue         *items_created;
	GQueue         *items_updated;
	GQueue         *items_deleted;
	GQueue         *items_moved;

	GList          *directories;
	GList          *current_directory;

	GList          *devices;
	GList          *current_device;

	GTimer	       *timer;

	/* Status */
	gboolean        been_started;
	gboolean	interrupted;

	gboolean	finished_directories;
	gboolean	finished_devices;

	/* Statistics */
	guint		total_directories_found;
	guint		total_directories_ignored;
	guint		total_files_found;
	guint		total_files_ignored;

	guint		directories_found;
	guint		directories_ignored;
	guint		files_found;
	guint		files_ignored;
};

typedef struct {
	gchar    *path;
	gboolean  recurse;
} DirectoryData;

enum {
	QUEUE_NONE,
	QUEUE_CREATED,
	QUEUE_UPDATED,
	QUEUE_DELETED,
	QUEUE_MOVED
};

enum {
	CHECK_FILE,
	CHECK_DIRECTORY,
	PROCESS_FILE,
	MONITOR_DIRECTORY,
	FINISHED,
	LAST_SIGNAL
};

static void           process_finalize             (GObject             *object);
static gboolean       process_defaults             (TrackerMinerProcess *process,
						    GFile               *file);
static void           miner_started                (TrackerMiner        *miner);
static DirectoryData *directory_data_new           (const gchar         *path,
						    gboolean             recurse);
static void           directory_data_free          (DirectoryData       *dd);
static void           monitor_item_created_cb      (TrackerMonitor      *monitor,
						    GFile               *file,
						    gboolean             is_directory,
						    gpointer             user_data);
static void           monitor_item_updated_cb      (TrackerMonitor      *monitor,
						    GFile               *file,
						    gboolean             is_directory,
						    gpointer             user_data);
static void           monitor_item_deleted_cb      (TrackerMonitor      *monitor,
						    GFile               *file,
						    gboolean             is_directory,
						    gpointer             user_data);
static void           monitor_item_moved_cb        (TrackerMonitor      *monitor,
						    GFile               *file,
						    GFile               *other_file,
						    gboolean             is_directory,
						    gboolean             is_source_monitored,
						    gpointer             user_data);
static gboolean       crawler_process_file_cb      (TrackerCrawler      *crawler,
						    GFile               *file,
						    gpointer             user_data);
static gboolean       crawler_process_directory_cb (TrackerCrawler      *crawler,
						    GFile               *file,
						    gpointer             user_data);
static void           crawler_finished_cb          (TrackerCrawler      *crawler,
						    guint                directories_found,
						    guint                directories_ignored,
						    guint                files_found,
						    guint                files_ignored,
						    gpointer             user_data);
static void           process_continue             (TrackerMinerProcess *process);
static void           process_next                 (TrackerMinerProcess *process);
static void           process_directories_next     (TrackerMinerProcess *process);
static void           process_directories_start    (TrackerMinerProcess *process);
static void           process_directories_stop     (TrackerMinerProcess *process);

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_ABSTRACT_TYPE (TrackerMinerProcess, tracker_miner_process, TRACKER_TYPE_MINER)

static void
tracker_miner_process_class_init (TrackerMinerProcessClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
        TrackerMinerProcessClass *process_class = TRACKER_MINER_PROCESS_CLASS (klass);
        TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);

	object_class->finalize = process_finalize;

	if (0) {
		process_class->check_file         = process_defaults;
		process_class->check_directory    = process_defaults;
		process_class->monitor_directory  = process_defaults;
	}

        miner_class->started = miner_started;

	/*
	  miner_class->stopped = miner_crawler_stopped;
	  miner_class->paused  = miner_crawler_paused;
	  miner_class->resumed = miner_crawler_resumed;
	*/

	signals[CHECK_FILE] =
		g_signal_new ("check-file",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerProcessClass, check_file),
			      NULL, NULL,
			      tracker_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN, 1, G_TYPE_FILE);
	signals[CHECK_DIRECTORY] =
		g_signal_new ("check-directory",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerProcessClass, check_directory),
			      NULL, NULL,
			      tracker_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN, 1, G_TYPE_FILE);
	signals[PROCESS_FILE] =
		g_signal_new ("process-file",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerProcessClass, process_file),
			      NULL, NULL,
			      tracker_marshal_BOOLEAN__OBJECT_OBJECT,
			      G_TYPE_BOOLEAN, 2, G_TYPE_FILE, TRACKER_TYPE_SPARQL_BUILDER);
	signals[MONITOR_DIRECTORY] =
		g_signal_new ("monitor-directory",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerProcessClass, monitor_directory),
			      NULL, NULL,
			      tracker_marshal_BOOLEAN__OBJECT,
			      G_TYPE_BOOLEAN, 1, G_TYPE_FILE);
	signals[FINISHED] =
		g_signal_new ("finished",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerMinerProcessClass, finished),
			      NULL, NULL,
			      tracker_marshal_VOID__UINT_UINT_UINT_UINT,
			      G_TYPE_NONE,
			      4,
			      G_TYPE_UINT,
			      G_TYPE_UINT,
			      G_TYPE_UINT,
			      G_TYPE_UINT);

	g_type_class_add_private (object_class, sizeof (TrackerMinerProcessPrivate));
}

static void
tracker_miner_process_init (TrackerMinerProcess *object)
{
	TrackerMinerProcessPrivate *priv;

	object->private = TRACKER_MINER_PROCESS_GET_PRIVATE (object);

	priv = object->private;

	/* For each module we create a TrackerCrawler and keep them in
	 * a hash table to look up.
	 */
	priv->items_created = g_queue_new ();
	priv->items_updated = g_queue_new ();
	priv->items_deleted = g_queue_new ();
	priv->items_moved = g_queue_new ();

	/* Set up the crawlers now we have config and hal */
	priv->crawler = tracker_crawler_new ();

	g_signal_connect (priv->crawler, "process-file",
			  G_CALLBACK (crawler_process_file_cb),
			  object);
	g_signal_connect (priv->crawler, "process-directory",
			  G_CALLBACK (crawler_process_directory_cb),
			  object);
	g_signal_connect (priv->crawler, "finished",
			  G_CALLBACK (crawler_finished_cb),
			  object);

	/* Set up the monitor */
	priv->monitor = tracker_monitor_new ();

	g_message ("Disabling monitor events until we have crawled the file system");
	tracker_monitor_set_enabled (priv->monitor, FALSE);

	g_signal_connect (priv->monitor, "item-created",
			  G_CALLBACK (monitor_item_created_cb),
			  object);
	g_signal_connect (priv->monitor, "item-updated",
			  G_CALLBACK (monitor_item_updated_cb),
			  object);
	g_signal_connect (priv->monitor, "item-deleted",
			  G_CALLBACK (monitor_item_deleted_cb),
			  object);
	g_signal_connect (priv->monitor, "item-moved",
			  G_CALLBACK (monitor_item_moved_cb),
			  object);
}

static void
process_finalize (GObject *object)
{
	TrackerMinerProcessPrivate *priv;

	priv = TRACKER_MINER_PROCESS_GET_PRIVATE (object);

	if (priv->timer) {
		g_timer_destroy (priv->timer);
	}

	if (priv->item_queues_handler_id) {
		g_source_remove (priv->item_queues_handler_id);
		priv->item_queues_handler_id = 0;
	}

	if (priv->crawler) {
		guint lsignals;

		lsignals = g_signal_handlers_disconnect_matched (priv->crawler,
								 G_SIGNAL_MATCH_FUNC,
								 0,
								 0,
								 NULL,
								 G_CALLBACK (crawler_process_file_cb),
								 NULL);
		lsignals = g_signal_handlers_disconnect_matched (priv->crawler,
								 G_SIGNAL_MATCH_FUNC,
								 0,
								 0,
								 NULL,
								 G_CALLBACK (crawler_process_directory_cb),
								 NULL);
		lsignals = g_signal_handlers_disconnect_matched (priv->crawler,
								 G_SIGNAL_MATCH_FUNC,
								 0,
								 0,
								 NULL,
								 G_CALLBACK (crawler_finished_cb),
								 NULL);

		g_object_unref (priv->crawler);
	}

	if (priv->monitor) {
		g_signal_handlers_disconnect_by_func (priv->monitor,
						      G_CALLBACK (monitor_item_deleted_cb),
						      object);
		g_signal_handlers_disconnect_by_func (priv->monitor,
						      G_CALLBACK (monitor_item_updated_cb),
						      object);
		g_signal_handlers_disconnect_by_func (priv->monitor,
						      G_CALLBACK (monitor_item_created_cb),
						      object);
		g_signal_handlers_disconnect_by_func (priv->monitor,
						      G_CALLBACK (monitor_item_moved_cb),
						      object);
		g_object_unref (priv->monitor);
	}

	if (priv->directories) {
		g_list_foreach (priv->directories, (GFunc) directory_data_free, NULL);
		g_list_free (priv->directories);
	}

	g_queue_foreach (priv->items_moved, (GFunc) g_object_unref, NULL);
	g_queue_free (priv->items_moved);

	g_queue_foreach (priv->items_deleted, (GFunc) g_object_unref, NULL);
	g_queue_free (priv->items_deleted);

	g_queue_foreach (priv->items_updated, (GFunc) g_object_unref, NULL);
	g_queue_free (priv->items_updated);

	g_queue_foreach (priv->items_created, (GFunc) g_object_unref, NULL);
	g_queue_free (priv->items_created);

#ifdef HAVE_HAL
	if (priv->devices) {
		g_list_foreach (priv->devices, (GFunc) g_free, NULL);
		g_list_free (priv->devices);
	}
#endif /* HAVE_HAL */

	G_OBJECT_CLASS (tracker_miner_process_parent_class)->finalize (object);
}

static gboolean 
process_defaults (TrackerMinerProcess *process,
		  GFile               *file)
{
	return TRUE;
}

static void
miner_started (TrackerMiner *miner)
{
	TrackerMinerProcess *process;

	process = TRACKER_MINER_PROCESS (miner);

	process->private->been_started = TRUE;

	process->private->interrupted = FALSE;

	process->private->finished_directories = FALSE;

	/* Disabled for now */
	process->private->finished_devices = TRUE;

	process_next (process);
}

static DirectoryData *
directory_data_new (const gchar *path,
		    gboolean     recurse)
{
	DirectoryData *dd;

	dd = g_slice_new (DirectoryData);

	dd->path = g_strdup (path);
	dd->recurse = recurse;

	return dd;
}

static void
directory_data_free (DirectoryData *dd)
{
	if (!dd) {
		return;
	}

	g_free (dd->path);
	g_slice_free (DirectoryData, dd);
}

static void
item_add_or_update (TrackerMinerProcess  *miner,
		    GFile                *file,
		    TrackerSparqlBuilder *sparql)
{
	gchar *full_sparql, *uri;

	uri = g_file_get_uri (file);

	g_debug ("Adding item '%s'", uri);

	tracker_sparql_builder_insert_close (sparql);

	full_sparql = g_strdup_printf ("DROP GRAPH <%s> %s",
		uri, tracker_sparql_builder_get_result (sparql));

	tracker_miner_execute_sparql (TRACKER_MINER (miner), full_sparql, NULL);
	g_free (full_sparql);
}

static gboolean
query_resource_exists (TrackerMinerProcess *miner,
		       const gchar         *uri)
{
	TrackerClient *client;
	gboolean   result;
	gchar     *sparql;
	GPtrArray *sparql_result;

	sparql = g_strdup_printf ("SELECT ?s WHERE { ?s a rdfs:Resource . FILTER (?s = <%s>) }",
	                          uri);

	client = tracker_miner_get_client (TRACKER_MINER (miner));
	sparql_result = tracker_resources_sparql_query (client, sparql, NULL);

	result = (sparql_result && sparql_result->len == 1);

	tracker_dbus_results_ptr_array_free (&sparql_result);
	g_free (sparql);

	return result;
}

static void
item_remove (TrackerMinerProcess *miner,
	     GFile               *file)
{
	gchar *sparql, *uri;

	uri = g_file_get_uri (file);

	g_debug ("Removing item: '%s' (Deleted from filesystem)",
		 uri);

	if (!query_resource_exists (miner, uri)) {
		g_debug ("  File does not exist anyway (uri:'%s')", uri);
		return;
	}

	/* Delete resource */
	sparql = g_strdup_printf ("DELETE { <%s> a rdfs:Resource }", uri);
	tracker_miner_execute_sparql (TRACKER_MINER (miner), sparql, NULL);
	g_free (sparql);

	/* FIXME: Should delete recursively? */
}

static GFile *
get_next_file (TrackerMinerProcess  *miner,
	       gint                 *queue)
{
	GFile *file;

	/* Deleted items first */
	file = g_queue_pop_head (miner->private->items_deleted);
	if (file) {
		*queue = QUEUE_DELETED;
		return file;
	}

	/* Created items next */
	file = g_queue_pop_head (miner->private->items_created);
	if (file) {
		*queue = QUEUE_CREATED;
		return file;
	}

	/* Updated items next */
	file = g_queue_pop_head (miner->private->items_updated);
	if (file) {
		*queue = QUEUE_UPDATED;
		return file;
	}

	/* Moved items next */
	file = g_queue_pop_head (miner->private->items_moved);
	if (file) {
		*queue = QUEUE_MOVED;
		return file;
	}

	*queue = QUEUE_NONE;
	return NULL;
}

static gboolean
item_queue_handlers_cb (gpointer user_data)
{
	TrackerSparqlBuilder *sparql;
	TrackerMinerProcess *miner;
	gboolean processed;
	GFile *file;
	gint queue;

	miner = user_data;
	sparql = tracker_sparql_builder_new_update ();
	file = get_next_file (miner, &queue);

	if (file) {
		if (queue == QUEUE_DELETED) {
			item_remove (miner, file);
		} else  {
			g_signal_emit (miner, signals[PROCESS_FILE], 0, file, sparql, &processed);

			if (processed) {
				/* Commit sparql */
				item_add_or_update (miner, file, sparql);
			}
		}

		return TRUE;
	}

	miner->private->item_queues_handler_id = 0;

	return FALSE;
}

static void
item_queue_handlers_set_up (TrackerMinerProcess *process)
{
	if (process->private->item_queues_handler_id != 0) {
		return;
	}

	process->private->item_queues_handler_id =
		g_idle_add (item_queue_handlers_cb,
			    process);
}

static gboolean
should_change_index_for_file (TrackerMinerProcess *miner,
			      GFile               *file)
{
	TrackerClient      *client;
	gboolean            uptodate;
	GPtrArray          *sparql_result;
	GFileInfo          *file_info;
	guint64             time;
	time_t              mtime;
	struct tm           t;
	gchar              *query, *uri;;

	file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	if (!file_info) {
		/* NOTE: We return TRUE here because we want to update the DB
		 * about this file, not because we want to index it.
		 */
		return TRUE;
	}

	time = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	mtime = (time_t) time;
	g_object_unref (file_info);

	uri = g_file_get_uri (file);
	client = tracker_miner_get_client (TRACKER_MINER (miner));

	gmtime_r (&mtime, &t);

	query = g_strdup_printf ("SELECT ?file { ?file nfo:fileLastModified \"%04d-%02d-%02dT%02d:%02d:%02d\" . FILTER (?file = <%s>) }",
	                         t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, uri);
	sparql_result = tracker_resources_sparql_query (client, query, NULL);

	uptodate = (sparql_result && sparql_result->len == 1);

	tracker_dbus_results_ptr_array_free (&sparql_result);

	g_free (query);
	g_free (uri);

	if (uptodate) {
		/* File already up-to-date in the database */
		return FALSE;
	}

	/* File either not yet in the database or mtime is different
	 * Update in database required
	 */
	return TRUE;
}

static gboolean
should_process_file (TrackerMinerProcess *process,
		     GFile               *file,
		     gboolean             is_dir)
{
	gboolean should_process;

	if (is_dir) {
		g_signal_emit (process, signals[CHECK_DIRECTORY], 0, file, &should_process);
	} else {
		g_signal_emit (process, signals[CHECK_FILE], 0, file, &should_process);
	}

	if (!should_process) {
		return FALSE;
	}

	/* Check whether file is up-to-date in tracker-store */
	return should_change_index_for_file (process, file);
}

static void
monitor_item_created_cb (TrackerMonitor *monitor,
			 GFile		*file,
			 gboolean	 is_directory,
			 gpointer	 user_data)
{
	TrackerMinerProcess *process;
	gboolean should_process = TRUE;
	gchar *path;

	process = user_data;
	should_process = should_process_file (process, file, is_directory);

	path = g_file_get_path (file);

	g_debug ("%s:'%s' (%s) (create monitor event or user request)",
		 should_process ? "Found " : "Ignored",
		 path,
		 is_directory ? "DIR" : "FILE");

	if (should_process) {
		if (is_directory) {
			gboolean add_monitor = TRUE;

			g_signal_emit (process, signals[MONITOR_DIRECTORY], 0, file, &add_monitor);
			
			if (add_monitor) {
				tracker_monitor_add (process->private->monitor, file);	     
			}

			/* Add to the list */
			process->private->directories = 
				g_list_append (process->private->directories, 
					       directory_data_new (path, TRUE));

			/* Make sure we are handling that list */
			process->private->finished_directories = FALSE;

			if (process->private->finished_devices) {
				process_next (process);
			}
		}

		g_queue_push_tail (process->private->items_created, 
				   g_object_ref (file));
		
		item_queue_handlers_set_up (process);
	}

	g_free (path);
}

static void
monitor_item_updated_cb (TrackerMonitor *monitor,
			 GFile		*file,
			 gboolean	 is_directory,
			 gpointer	 user_data)
{
	TrackerMinerProcess *process;
	gboolean should_process;
	gchar *path;

	process = user_data;
	should_process = should_process_file (process, file, is_directory);

	path = g_file_get_path (file);

 	g_debug ("%s:'%s' (%s) (update monitor event or user request)",
		 should_process ? "Found " : "Ignored",
		 path,
		 is_directory ? "DIR" : "FILE");

	if (should_process) {
		g_queue_push_tail (process->private->items_updated,
				   g_object_ref (file));

		item_queue_handlers_set_up (process);
	}

	g_free (path);
}

static void
monitor_item_deleted_cb (TrackerMonitor *monitor,
			 GFile		*file,
			 gboolean	 is_directory,
			 gpointer	 user_data)
{
	TrackerMinerProcess *process;
	gboolean should_process;
	gchar *path;

	process = user_data;
	should_process = should_process_file (process, file, is_directory);
	path = g_file_get_path (file);

	g_debug ("%s:'%s' (%s) (delete monitor event or user request)",
		 should_process ? "Found " : "Ignored",
		 path,
		 is_directory ? "DIR" : "FILE");

	if (should_process) {
		g_queue_push_tail (process->private->items_deleted,
				   g_object_ref (file));

		item_queue_handlers_set_up (process);
	}

#if 0
	/* FIXME: Should we do this for MOVE events too? */

	/* Remove directory from list of directories we are going to
	 * iterate if it is in there.
	 */
	l = g_list_find_custom (process->private->directories, 
				path, 
				(GCompareFunc) g_strcmp0);

	/* Make sure we don't remove the current device we are
	 * processing, this is because we do this same clean up later
	 * in process_device_next() 
	 */
	if (l && l != process->private->current_directory) {
		directory_data_free (l->data);
		process->private->directories = 
			g_list_delete_link (process->private->directories, l);
	}
#endif

	g_free (path);
}

static void
monitor_item_moved_cb (TrackerMonitor *monitor,
		       GFile	      *file,
		       GFile	      *other_file,
		       gboolean        is_directory,
		       gboolean        is_source_monitored,
		       gpointer        user_data)
{
	TrackerMinerProcess *process;

	process = user_data;

	if (!is_source_monitored) {
		gchar *path;

		path = g_file_get_path (other_file);

#ifdef FIX
		/* If the source is not monitored, we need to crawl it. */
		tracker_crawler_add_unexpected_path (process->private->crawler, path);
#endif
		g_free (path);
	} else {
		gchar *path;
		gchar *other_path;
		gboolean should_process;
		gboolean should_process_other;
		
		path = g_file_get_path (file);
		other_path = g_file_get_path (other_file);

		should_process = should_process_file (process, file, is_directory);
		should_process_other = should_process_file (process, other_file, is_directory);

		g_debug ("%s:'%s'->'%s':%s (%s) (move monitor event or user request)",
			 should_process ? "Found " : "Ignored",
			 path,
			 other_path,
			 should_process_other ? "Found " : "Ignored",
			 is_directory ? "DIR" : "FILE");
		
		if (!should_process && !should_process_other) {
			/* Do nothing */
		} else if (!should_process) {
			/* Check new file */
			if (!is_directory) {
				g_queue_push_tail (process->private->items_created, 
						   g_object_ref (other_file));
				
				item_queue_handlers_set_up (process);
			} else {
				gboolean add_monitor = TRUE;
				
				g_signal_emit (process, signals[MONITOR_DIRECTORY], 0, file, &add_monitor);
				
				if (add_monitor) {
					tracker_monitor_add (process->private->monitor, file);	     
				}

#ifdef FIX
				/* If this is a directory we need to crawl it */
				tracker_crawler_add_unexpected_path (process->private->crawler, other_path);
#endif
			}
		} else if (!should_process_other) {
			/* Delete old file */
			g_queue_push_tail (process->private->items_deleted, g_object_ref (file));
			
			item_queue_handlers_set_up (process);
		} else {
			/* Move old file to new file */
			g_queue_push_tail (process->private->items_moved, g_object_ref (file));
			g_queue_push_tail (process->private->items_moved, g_object_ref (other_file));
			
			item_queue_handlers_set_up (process);
		}
		
		g_free (other_path);
		g_free (path);
	}
}

static gboolean
crawler_process_file_cb (TrackerCrawler *crawler,
			 GFile	        *file,
			 gpointer	 user_data)
{
	TrackerMinerProcess *process;
	gboolean should_process;

	process = user_data;
	should_process = should_process_file (process, file, FALSE);

	if (should_process) {
		/* Add files in queue to our queues to send to the indexer */
		g_queue_push_tail (process->private->items_created,
				   g_object_ref (file));
		item_queue_handlers_set_up (process);
	}

	return should_process;
}

static gboolean
crawler_process_directory_cb (TrackerCrawler *crawler,
			      GFile	     *file,
			      gpointer	      user_data)
{
	TrackerMinerProcess *process;
	gboolean should_process;
	gboolean add_monitor = TRUE;

	process = user_data;
	should_process = should_process_file (process, file, TRUE);

	if (should_process) {
		/* FIXME: Do we add directories to the queue? */
		g_queue_push_tail (process->private->items_created,
				   g_object_ref (file));

		item_queue_handlers_set_up (process);
	}

	g_signal_emit (process, signals[MONITOR_DIRECTORY], 0, file, &add_monitor);

	/* Should we add? */
	if (add_monitor) {
		tracker_monitor_add (process->private->monitor, file);
	}

	return should_process;
}

static void
crawler_finished_cb (TrackerCrawler *crawler,
		     guint	     directories_found,
		     guint	     directories_ignored,
		     guint	     files_found,
		     guint	     files_ignored,
		     gpointer	     user_data)
{
	TrackerMinerProcess *process;

	process = user_data;

	/* Update stats */
	process->private->directories_found += directories_found;
	process->private->directories_ignored += directories_ignored;
	process->private->files_found += files_found;
	process->private->files_ignored += files_ignored;

	process->private->total_directories_found += directories_found;
	process->private->total_directories_ignored += directories_ignored;
	process->private->total_files_found += files_found;
	process->private->total_files_ignored += files_ignored;

	/* Proceed to next thing to process */
	process_continue (process);
}

static void
process_continue (TrackerMinerProcess *process)
{
	if (!process->private->finished_directories) {
		process_directories_next (process);
		return;
	}

#if 0
	if (!process->private->finished_devices) {
		process_device_next (process);
		return;
	}
#endif

	/* Nothing to do */
}

static void
process_next (TrackerMinerProcess *process)
{
	static gboolean shown_totals = FALSE;

	if (!process->private->finished_directories) {
		process_directories_start (process);
		return;
	}

#if 0
	if (!process->private->finished_devices) {
		process_devices_start (process);
		return;
	}
#endif

	/* Only do this the first time, otherwise the results are
	 * likely to be inaccurate. Devices can be added or removed so
	 * we can't assume stats are correct.
	 */
	if (!shown_totals) {
		shown_totals = TRUE;

		g_message ("--------------------------------------------------");
		g_message ("Total directories : %d (%d ignored)",
			   process->private->total_directories_found,
			   process->private->total_directories_ignored);
		g_message ("Total files       : %d (%d ignored)",
			   process->private->total_files_found,
			   process->private->total_files_ignored);
		g_message ("Total monitors    : %d",
			   tracker_monitor_get_count (process->private->monitor));
		g_message ("--------------------------------------------------\n");
	}

	/* Now we have finished crawling, we enable monitor events */
	g_message ("Enabling monitor events");
	tracker_monitor_set_enabled (process->private->monitor, TRUE);
}

static void
process_directories_next (TrackerMinerProcess *process)
{
	DirectoryData *dd;

	/* Don't recursively iterate the modules */
	if (!process->private->current_directory) {
		if (!process->private->finished_directories) {
			process->private->current_directory = process->private->directories;
		}
	} else {
		GList *l;

		l = process->private->current_directory;
		
		/* Now free that device so we don't recrawl it */
		if (l) {
			directory_data_free (l->data);
			
			process->private->current_directory = 
				process->private->directories = 
				g_list_delete_link (process->private->directories, l);
		}
	}

	/* If we have no further modules to iterate */
	if (!process->private->current_directory) {
		process_directories_stop (process);
		process_next (process);
		return;
	}

	dd = process->private->current_directory->data;

	tracker_crawler_start (process->private->crawler, 
			       dd->path, 
			       dd->recurse);
}

static void
process_directories_start (TrackerMinerProcess *process)
{
	g_message ("Process is starting to iterating directories");

	/* Go through dirs and crawl */
	if (!process->private->directories) {
		g_message ("No directories set up for process to handle, doing nothing");
		return;
	}

	if (process->private->timer) {
		g_timer_destroy (process->private->timer);
	}

	process->private->timer = g_timer_new ();

	process->private->finished_directories = FALSE;

	process->private->directories_found = 0;
	process->private->directories_ignored = 0;
	process->private->files_found = 0;
	process->private->files_ignored = 0;

	process_directories_next (process);
}

static void
process_directories_stop (TrackerMinerProcess *process)
{
	if (process->private->finished_directories) {
		return;
	}

	g_message ("--------------------------------------------------");
	g_message ("Process has %s iterating files",
		   process->private->interrupted ? "been stopped while" : "finished");

	process->private->finished_directories = TRUE;

	if (process->private->interrupted) {
		if (process->private->crawler) {
			tracker_crawler_stop (process->private->crawler);
		}

		if (process->private->timer) {
			g_timer_destroy (process->private->timer);
			process->private->timer = NULL;
		}
	} else {
		gdouble elapsed;
	
		if (process->private->timer) {
			g_timer_stop (process->private->timer);
			elapsed = g_timer_elapsed (process->private->timer, NULL);
		} else {
			elapsed = 0;
		}
		
		g_message ("FS time taken : %4.4f seconds",
			   elapsed);
		g_message ("FS directories: %d (%d ignored)",
			   process->private->directories_found,
			   process->private->directories_ignored);
		g_message ("FS files      : %d (%d ignored)",
			   process->private->files_found,
			   process->private->files_ignored);
	}

	g_message ("--------------------------------------------------\n");

	g_signal_emit (process, signals[FINISHED], 0,
		       process->private->total_directories_found,
		       process->private->total_directories_ignored,
		       process->private->total_files_found,
		       process->private->total_files_ignored);
}

void
tracker_miner_process_add_directory (TrackerMinerProcess *process,
				     const gchar         *path,
				     gboolean             recurse)
{
	g_return_if_fail (TRACKER_IS_PROCESS (process));
	g_return_if_fail (path != NULL);

	/* WHAT HAPPENS IF WE ADD DURING OPERATION ? */

	process->private->directories = 
		g_list_append (process->private->directories, 
			       directory_data_new (path, recurse));
}
