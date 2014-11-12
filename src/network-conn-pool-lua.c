/* $%BEGINLICENSE%$
 Copyright (c) 2008, 2009, Oracle and/or its affiliates. All rights reserved.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_FILIO_H
/**
 * required for FIONREAD on solaris
 */
#include <sys/filio.h>
#endif

#ifndef _WIN32
#include <sys/ioctl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define ioctlsocket ioctl
#endif

#include <errno.h>
#include <lua.h>

#include "lua-env.h"
#include "glib-ext.h"

#include "network-mysqld.h"
#include "network-mysqld-packet.h"
#include "chassis-event-thread.h"
#include "network-mysqld-lua.h"

#include "network-conn-pool.h"
#include "network-conn-pool-lua.h"

/**
 * lua wrappers around the connection pool
 */

#define C(x) x, sizeof(x) - 1
#define S(x) x->str, x->len

/**
 * handle the events of a idling server connection in the pool 
 *
 * make sure we know about connection close from the server side
 * - wait_timeout
 */
static void network_mysqld_con_idle_handle(int event_fd, short events, void *user_data) {
	network_connection_pool_entry *pool_entry = user_data;
	network_connection_pool *pool             = pool_entry->pool;

	if (events == EV_READ) {
		int b = -1;

		/**
		 * @todo we have to handle the case that the server really sent us something
		 *        up to now we just ignore it
		 */
		if (ioctlsocket(event_fd, FIONREAD, &b)) {
			g_critical("ioctl(%d, FIONREAD, ...) failed: %s", event_fd, g_strerror(errno));
		} else if (b != 0) {
			g_critical("ioctl(%d, FIONREAD, ...) said there is something to read, oops: %d", event_fd, b);
		} else {
			/* the server decided to close the connection (wait_timeout, crash, ... )
			 *
			 * remove us from the connection pool and close the connection */
		

			network_connection_pool_remove(pool, pool_entry); // not in lua, so lock like lua_lock
		}
	}
}

/**
 * move the con->server into connection pool and disconnect the 
 * proxy from its backend 
 */
int network_connection_pool_lua_add_connection(network_mysqld_con *con, int is_write_sql) {
	network_connection_pool_entry *pool_entry = NULL;
	network_mysqld_con_lua_t *st = con->plugin_con_state;
       guint thread_id;

	/* con-server is already disconnected, got out */
	if (!con->server) return 0;

    /* TODO bug fix */
    /* when mysql return unkonw packet, response is null, insert the socket into pool cause segment fault. */
    /* ? should init socket->challenge  ? */
    /* if response is null, conn has not been authed, use an invalid username. */
    if(!con->server->response)
    {
        g_warning("%s: (remove) remove socket from pool, response is NULL, src is %s, dst is %s",
            G_STRLOC, con->server->src->name->str, con->server->dst->name->str);

        con->server->response = network_mysqld_auth_response_new();
        g_string_assign_len(con->server->response->username, C("mysql_proxy_invalid_user"));
    }

	/* the server connection is still authed */
	con->server->is_authed = 1;

	/* insert the server socket into the connection pool */
       if(is_write_sql == 0) {
              network_connection_pool* pool = chassis_event_thread_pool(st->backend);
              pool_entry = network_connection_pool_add(pool, con->server);
       }else {
              network_connection_pool* pool = chassis_event_thread_secondpool(st->backend);
              pool_entry = network_connection_pool_time_add(pool, con->server, con);
       }

	if (pool_entry) {
		event_set(&(con->server->event), con->server->fd, EV_READ, network_mysqld_con_idle_handle, pool_entry);
		chassis_event_add_local(con->srv, &(con->server->event)); /* add a event, but stay in the same thread */
	}
	
	g_atomic_int_dec_and_test(&(st->backend->connected_clients));
	st->backend = NULL;
	st->backend_ndx = -1;
	con->server = NULL;
       thread_id = chassis_event_thread_index_get();
       chassis_event_thread_t *thread = g_ptr_array_index(con->srv->threads, thread_id);
       if(thread->block_con_queue->length) {
              if (write(thread->con_write_fd, "", 1) != 1) g_message("%s:pipes - write error: %s", G_STRLOC, g_strerror(errno));
       }
	
       return 0;
}

network_socket *self_connect(network_mysqld_con *con, network_backend_t *backend) {
	//1. connect DB
	network_socket *sock = network_socket_new();
	network_address_copy(sock->dst, backend->addr);
	if (-1 == (sock->fd = socket(sock->dst->addr.common.sa_family, sock->socket_type, 0))) {
		g_critical("%s.%d: socket(%s) failed: %s (%d)", __FILE__, __LINE__, sock->dst->name->str, g_strerror(errno), errno);
		network_socket_free(sock);
		return NULL;
	}
	if (-1 == (connect(sock->fd, &sock->dst->addr.common, sock->dst->len))) {
		g_message("%s.%d: connecting to backend (%s) failed, marking it as down for ...", __FILE__, __LINE__, sock->dst->name->str);
		network_socket_free(sock);
		if (backend->state != BACKEND_STATE_OFFLINE) backend->state = BACKEND_STATE_DOWN;
		return NULL;
	}

	//2. read handshake���ص��ǻ�ȡ20���ֽڵ������
	off_t to_read = NET_HEADER_SIZE;
	guint offset = 0;
	guchar header[NET_HEADER_SIZE];
	while (to_read > 0) {
		gssize len = recv(sock->fd, header + offset, to_read, 0);
		if (len == -1 || len == 0) {
			network_socket_free(sock);
			return NULL;
		}
		offset += len;
		to_read -= len;
	}

	to_read = header[0] + (header[1] << 8) + (header[2] << 16);
	offset = 0;
	GString *data = g_string_sized_new(to_read);
	while (to_read > 0) {
		gssize len = recv(sock->fd, data->str + offset, to_read, 0);
		if (len == -1 || len == 0) {
			network_socket_free(sock);
			g_string_free(data, TRUE);
			return NULL;
		}
		offset += len;
		to_read -= len;
	}
	data->len = offset;

	network_packet packet;
	packet.data = data;
	packet.offset = 0;
	network_mysqld_auth_challenge *challenge = network_mysqld_auth_challenge_new();
	network_mysqld_proto_get_auth_challenge(&packet, challenge);

	//3. ����response
	GString *response = g_string_sized_new(20);
        GString *hashed_password = NULL;
        hashed_password = g_hash_table_lookup(con->config->pwd_table[con->config->pwdtable_index], con->client->response->username->str);
	if (hashed_password) {
		network_mysqld_proto_password_scramble(response, S(challenge->challenge), S(hashed_password));
	} else {
		network_socket_free(sock);
		g_string_free(data, TRUE);
		network_mysqld_auth_challenge_free(challenge);
		g_string_free(response, TRUE);
		return NULL;
	}

	//4. send auth
	off_t to_write = 58 + con->client->response->username->len;
	offset = 0;
	g_string_truncate(data, 0);
	char tmp[] = {to_write - 4, 0, 0, 1, 0x85, 0xa6, 3, 0, 0, 0, 0, 1, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	g_string_append_len(data, tmp, 36);
	g_string_append_len(data, con->client->response->username->str, con->client->response->username->len);
	g_string_append_len(data, "\0\x14", 2);
	g_string_append_len(data, response->str, 20);
	g_string_free(response, TRUE);
	while (to_write > 0) {
		gssize len = send(sock->fd, data->str + offset, to_write, 0);
		if (len == -1) {
			network_socket_free(sock);
			g_string_free(data, TRUE);
			network_mysqld_auth_challenge_free(challenge);
			return NULL;
		}
		offset += len;
		to_write -= len;
	}

	//5. read auth result
	to_read = NET_HEADER_SIZE;
	offset = 0;
	while (to_read > 0) {
		gssize len = recv(sock->fd, header + offset, to_read, 0);
		if (len == -1 || len == 0) {
			network_socket_free(sock);
			g_string_free(data, TRUE);
			network_mysqld_auth_challenge_free(challenge);
			return NULL;
		}
		offset += len;
		to_read -= len;
	}

	to_read = header[0] + (header[1] << 8) + (header[2] << 16);
	offset = 0;
	g_string_truncate(data, 0);
	g_string_set_size(data, to_read);
	while (to_read > 0) {
		gssize len = recv(sock->fd, data->str + offset, to_read, 0);
		if (len == -1 || len == 0) {
			network_socket_free(sock);
			g_string_free(data, TRUE);
			network_mysqld_auth_challenge_free(challenge);
			return NULL;
		}
		offset += len;
		to_read -= len;
	}
	data->len = offset;

	if (data->str[0] != MYSQLD_PACKET_OK) {
		network_socket_free(sock);
		g_string_free(data, TRUE);
		network_mysqld_auth_challenge_free(challenge);
		return NULL;
	}
	g_string_free(data, TRUE);

	//6. set non-block
	network_socket_set_non_blocking(sock);
	network_socket_connect_setopts(sock);	//�˾��Ƿ���Ҫ���Ƿ�Ӧ�÷��ڵ�1��ĩβ��

	sock->challenge = challenge;
	sock->response = network_mysqld_auth_response_copy(con->client->response);

	return sock;
}

/**
 * swap the server connection with a connection from
 * the connection pool
 *
 * we can only switch backends if we have a authed connection in the pool.
 *
 * @return NULL if swapping failed
 *         the new backend on success
 */
network_socket* network_connection_pool_lua_swap(network_mysqld_con *con, int backend_ndx, int need_keep_conn, int *err) {
	network_backend_t *backend = NULL;
	network_socket *send_sock;
       network_connection_pool *pool, *second_pool;
	network_mysqld_con_lua_t *st = con->plugin_con_state;
	chassis_private *g = con->srv->priv;

	/*
	 * we can only change to another backend if the backend is already
	 * in the connection pool and connected
	 */

	backend = network_backends_get(g->backends, backend_ndx);
	if (!backend) return NULL;


	/**
	 * get a connection from the pool which matches our basic requirements
	 * - username has to match
	 * - default_db should match
	 */
		
#ifdef DEBUG_CONN_POOL
	g_debug("%s: (swap) check if we have a connection for this user in the pool '%s'", G_STRLOC, con->client->response ? con->client->response->username->str: "empty_user");
#endif
       if(need_keep_conn == 0) {
              pool = chassis_event_thread_pool(backend);
              send_sock = network_connection_pool_get(pool);
              if(send_sock == NULL) {
                     second_pool = chassis_event_thread_secondpool(backend);
                     send_sock = network_expiretime_connection_pool_get(second_pool, con);
              }
       }else {
              second_pool = chassis_event_thread_secondpool(backend);
              send_sock = network_connection_secondpool_get(second_pool, con);
              if(send_sock == NULL) g_message("%s:the connection need keep, but it's not in the pool now.", G_STRLOC);
       }
	if (NULL == send_sock ) {
		/**
		 * no connections in the pool
		 */
              if ((con->config->max_connections <= 0) || (backend->connected_clients < con->config->max_connections)) {
                     if (NULL == (send_sock = self_connect(con, backend))) {
                            st->backend_ndx = -1;
                            return NULL;
                     }
              } else {
                     st->backend_ndx = -1;
                     *err = -1;
                     return NULL;
              }
	}

	/* the backend is up and cool, take and move the current backend into the pool */
#ifdef DEBUG_CONN_POOL
	g_debug("%s: (swap) added the previous connection to the pool", G_STRLOC);
#endif

	/* connect to the new backend */
	st->backend = backend;
	g_atomic_int_inc(&(st->backend->connected_clients));
	st->backend_ndx = backend_ndx;

	return send_sock;
}

void network_conn_available_handle(int G_GNUC_UNUSED event_fd, short G_GNUC_UNUSED events, void* user_data) {
       int err = 0;
       char ping[1];
       network_mysqld_con_lua_t *st;
       injection *inj;
       GString *packet;

       chassis *chas = user_data;
       guint index = chassis_event_thread_index_get();
       chassis_event_thread_t *thread = g_ptr_array_index(chas->threads, index);
       if (read(thread->con_read_fd, ping, 1) != 1) g_message("%s:pipes - read error,error message:%s", G_STRLOC, g_strerror(errno));
       
       network_mysqld_con *con = g_queue_pop_head(thread->block_con_queue);
       if(con == NULL) return;
       network_socket* sock = network_connection_pool_lua_swap(con, con->backend_ndx, 0, &err);
       if(sock == NULL) {
              g_queue_push_tail(thread->block_con_queue, con);
              return;
       }
       con->server = sock;
       st = con->plugin_con_state;
       inj = g_queue_peek_head(st->injected.queries);
       con->resultset_is_needed = inj->resultset_is_needed; /* let the lua-layer decide if we want to buffer the result or not */

       network_mysqld_queue_reset(con->server);
       network_mysqld_queue_append(con->server, con->server->send_queue, S(inj->query));

       while ((packet = g_queue_pop_head(con->client->recv_queue->chunks))) g_string_free(packet, TRUE);
       con->state = CON_STATE_SEND_QUERY;
       network_mysqld_con_reset_command_response_state(con);

       network_mysqld_con_handle(-1, 0, con);
}

int wrr_ro(network_mysqld_con *con) {
       guint i, j;
       network_backends_t* backends = con->srv->priv->backends;
       g_wrr_poll* rwsplit = backends->global_wrr;
       guint ndx_num = network_backends_count(backends);
       if (rwsplit->next_ndx >= ndx_num)
              rwsplit->next_ndx = 0; 
       // set max weight if no init
       if (rwsplit->max_weight == 0) { 
              for(i = 0; i < ndx_num; ++i) {
                     network_backend_t* backend = network_backends_get(backends, i);
                     if (backend == NULL || backend->state != BACKEND_STATE_UP) continue;
                     if (rwsplit->max_weight < backend->weight) {
                            rwsplit->max_weight = backend->weight;
                            rwsplit->cur_weight = backend->weight;
                     }    
              }    
       }    

       guint max_weight = rwsplit->max_weight;
       gint cur_weight = rwsplit->cur_weight;
       guint next_ndx   = rwsplit->next_ndx;

       // get backend index by slave wrr
       gint ndx = -1;
       for(i = 0; i < ndx_num; ++i) {
              network_backend_t* backend = network_backends_get(backends, next_ndx);
              if (backend == NULL) goto next;

              network_connection_pool* pool = chassis_event_thread_pool(backend);
              if (pool == NULL) goto next;

              if (backend->type == BACKEND_TYPE_RO && backend->weight >= cur_weight) {
                     if(backend->state == BACKEND_STATE_UP) {
                            ndx = next_ndx;
                     }else {
                            for(j = 0; j < ndx_num; ++j) {
                                   network_backend_t* b = network_backends_get(backends, j);
                                   if(b == NULL) continue;
                                   network_connection_pool* p = chassis_event_thread_pool(b);
                                   if(p == NULL) continue;
                                   if(b->type == BACKEND_TYPE_RO && b->state == BACKEND_STATE_UP) ndx = j;
                            }
                     }
              }
next:
              if (next_ndx >= ndx_num - 1) { 
                     --cur_weight;
                     next_ndx = 0; 

                     if (cur_weight <= 0) cur_weight = max_weight;
              } else {
                     ++next_ndx;
              }    

              if (ndx != -1) break;
       }    
       rwsplit->cur_weight = cur_weight;
       rwsplit->next_ndx = next_ndx;
       return ndx; 
}

int idle_rw(network_mysqld_con* con) {
       int ret = -1;
       guint i;

       network_backends_t* backends = con->srv->priv->backends;

       guint count = network_backends_count(backends);
       for (i = 0; i < count; ++i) {
              network_backend_t* backend = network_backends_get(backends, i);
              if (backend == NULL) continue;

              network_connection_pool* pool = chassis_event_thread_pool(backend);
              if (pool == NULL) continue;

              if (backend->type == BACKEND_TYPE_RW && backend->state == BACKEND_STATE_UP) {
                     ret = i;
                     break;
              }
       }

       return ret;
}

int idle_ro(network_mysqld_con* con) {
       int max_conns = -1;
       guint i;

       network_backends_t* backends = con->srv->priv->backends;

       guint count = network_backends_count(backends);
       for(i = 0; i < count; ++i) {
              network_backend_t* backend = network_backends_get(backends, i);
              if (backend == NULL) continue;

              network_connection_pool* pool = chassis_event_thread_pool(backend);
              if (pool == NULL) continue;

              if (backend->type == BACKEND_TYPE_RO && backend->state == BACKEND_STATE_UP) {
                     if (max_conns == -1 || backend->connected_clients < max_conns) {
                            max_conns = backend->connected_clients;
                     }    
              }    
       }    

       return max_conns;
}

int change_standby_to_master(network_backends_t *bs) {
       int i;
       network_backend_t *standby, *item, *m_item, *s_item, *temp_backend;
       standby = network_get_backend_by_type(bs, BACKEND_TYPE_SY);
       if (standby != NULL && standby->state == BACKEND_STATE_UP) {
              g_mutex_lock(bs->backends_mutex);
              for (i = 0; i < bs->backends->len; i++) {
                     item = g_ptr_array_index(bs->backends, i);
                     if (item->type == BACKEND_TYPE_RW) {
                            item->type = BACKEND_TYPE_SY;
                            item->state = BACKEND_STATE_DOWN;
                            m_item = item;
                     } else if (item->type == BACKEND_TYPE_SY) {
                            item->type = BACKEND_TYPE_RW;
                            s_item = item;
                            temp_backend = g_ptr_array_index(bs->backends, 0);
                            bs->backends->pdata[0] = item;
                            bs->backends->pdata[i] = temp_backend;
                     }
              }
              g_mutex_unlock(bs->backends_mutex);
              g_message("%s:master(%s) is down, change standby(%s) to master.", G_STRLOC, m_item->addr->name->str, s_item->addr->name->str);
       }else {
              g_message("%s:have no standby master or standby master down,can't change master to standby",G_STRLOC);
              return -1;
       }
       return 0;
}
