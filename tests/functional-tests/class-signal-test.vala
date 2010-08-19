/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

using Tracker;
using Tracker.Sparql;

struct Event {
	int subject_id;
	int pred_id;
	int object_id;
}

[DBus (name = "org.freedesktop.Tracker1.Resources")]
private interface Resources : GLib.Object {
	[DBus (name = "ClassSignal")]
	public signal void class_signal (string class_name, Event[] deletes, Event[] inserts);
}

public class TestApp {
	static DBus.Connection dbus_connection;
	static Resources resources_object;
	int res = -1;
	MainLoop loop;
	bool initialized = false;
	Sparql.Connection signal_con;
	Sparql.Connection con;

	public TestApp ()
	requires (!initialized) {
		try {
			con = Tracker.Sparql.Connection.get();

			// Switch between kinds of query connections here:
			signal_con = con;

			dbus_connection = DBus.Bus.get (DBus.BusType.SESSION);
			resources_object = (Resources) dbus_connection.get_object ("org.freedesktop.Tracker1",
			                                                           "/org/freedesktop/Tracker1/Resources",
			                                                           "org.freedesktop.Tracker1.Resources");

			resources_object.class_signal.connect (on_class_signal_received);

		} catch (Sparql.Error e) {
			warning ("Could not connect to D-Bus service: %s", e.message);
			initialized = false;
			res = -1;
			return;
		} catch (DBus.Error e) {
			warning ("Could not connect to D-Bus service: %s", e.message);
			initialized = false;
			res = -1;
			return;
		}
		initialized = true;
	}

	// Query looks like this:
	// SELECT ?t { ?r a nmm:MusicPiece; nie:title ?t .
	//             FILTER (tracker:id (?r) IN (id1, id2, id3))
	// }

	private StringBuilder build_title_query (string class_name, Event[] ids) {
		bool first = true;
		StringBuilder builder = new StringBuilder ("SELECT ?r ?t { ?r a <");
		builder.append (class_name);
		builder.append (">; nie:title ?t . FILTER (tracker:id (?r) IN (");
		foreach (Event event in ids) {
			if (first)
				builder.append_printf ("%d", event.subject_id);
			else
				builder.append_printf (" , %d", event.subject_id);
			first = false;
		}
		builder.append (")) }");
		return builder;
	}

	private int iter_cursor (string kind, Cursor cursor) {
		try {
			while (cursor.next()) {
				int i;
				print ("%s", kind);
				for (i = 0; i < cursor.n_columns; i++)
					print ("%s%s%s", i != 0 ? ", nie:title '":"<", cursor.get_string (i),
					                 i != 0 ? "'":">");
				print ("\n");
			}
		} catch (GLib.Error e) {
			warning ("Couldn't iterate query results: %s", e.message);
			res = -1;
			return -1;
		}
		return (0);
	}

	private async void on_class_signal_received_async (string dels_query, string ins_query) {
		try {
			Sparql.Cursor cursor1, cursor2;

			cursor1 = yield signal_con.query_async (dels_query);
			cursor2 = yield signal_con.query_async (ins_query);

			res = iter_cursor ("delete of: ", cursor1);
			if (res != -1)
				res = iter_cursor ("insert of: ", cursor2);

		} catch (GLib.Error e) {
			warning ("Couldn't iterate query results: %s", e.message);
			res = -1;
		}
	}

	private void on_class_signal_received (string class_name, Event[] deletes, Event[] inserts) {
		string dels_qry = build_title_query (class_name, deletes).str;
		string ins_qry = build_title_query (class_name, deletes).str;

		on_class_signal_received_async (dels_qry, ins_qry);
	}

	private void insert_data () {
		int i;

		for (i = 0; i< 100; i++) {
			string upqry = "DELETE { <%d> a rdfs:Resource } INSERT { <%d> a nmm:MusicPiece ; nie:title 'title %d' }".printf(i, i, i);
			con.update_async (upqry);
		}
	}

	private bool in_mainloop () {
		insert_data ();

		return false;
	}

	public int run () {
		loop = new MainLoop (null, false);
		Idle.add (in_mainloop);
		loop.run ();
		return res;
	}
}

int main (string[] args) {
	TestApp app = new TestApp ();

	return app.run ();
}