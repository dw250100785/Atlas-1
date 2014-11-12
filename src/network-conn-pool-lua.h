/* $%BEGINLICENSE%$
 Copyright (c) 2008, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */
#ifndef __NETWORK_CONN_POOL_LUA_H__
#define __NETWORK_CONN_POOL_LUA_H__

#include <lua.h>
#include <stdio.h>
#include "network-socket.h"
#include "network-mysqld.h"

#include "network-exports.h"

struct chassis_plugin_config {
	gchar *address;                   /**< listening address of the proxy */

	gchar **backend_addresses;        /**< read-write backends */
	gchar **read_only_backend_addresses; /**< read-only  backends */
       gchar **master_standby_addresses; 
	gint fix_bug_25371;               /**< suppress the second ERR packet of bug #25371 */

	gint profiling;                   /**< skips the execution of the read_query() function */
	
	gchar *lua_script;                /**< script to load at the start the connection */

	gint pool_change_user;            /**< don't reset the connection, when a connection is taken from the pool
					       - this safes a round-trip, but we also don't cleanup the connection
					       - another name could be "fast-pool-connect", but that's too friendly
					       */

	gint start_proxy;

	gchar **client_ips;
       GHashTable *ip_table[2];
       gint iptable_index;

	gchar **lvs_ips;
	GHashTable *lvs_table;

	gchar **tables;
	GHashTable *dt_table;
       GHashTable *rule_table;

	gchar **pwds;
	GHashTable *pwd_table[2];
       gint pwdtable_index;

	network_mysqld_con *listen_con;

	FILE *sql_log;
	gchar *sql_log_type;

	gchar *charset;
       gint check_time;
       gint connect_times;
       gchar **fsql;
       GPtrArray *reg_array;

       gint sql_log_slow_ms;
       gint max_connections;
       gint connection_expire_time;
};

NETWORK_API int network_connection_pool_getmetatable(lua_State *L);

NETWORK_API int network_connection_pool_lua_add_connection(network_mysqld_con *con, int is_write_sql);
NETWORK_API int wrr_ro(network_mysqld_con *con);
NETWORK_API int idle_rw(network_mysqld_con* con);
NETWORK_API int idle_ro(network_mysqld_con* con);
NETWORK_API int change_standby_to_master(network_backends_t *bs);
NETWORK_API network_socket *network_connection_pool_lua_swap(network_mysqld_con *con, int backend_ndx, int need_keep_conn, int *err);
NETWORK_API void network_conn_available_handle(int G_GNUC_UNUSED event_fd, short G_GNUC_UNUSED events, void* user_data);
#endif
