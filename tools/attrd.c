/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>






#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/cluster.h>

#include <crm/common/xml.h>
#include <crm/common/msg.h>
#include <attrd.h>

#define OPTARGS	"hV"
#if SUPPORT_HEARTBEAT
ll_cluster_t	*attrd_cluster_conn;
#endif

GMainLoop*  mainloop = NULL;
char *attrd_uname = NULL;
char *attrd_uuid = NULL;
gboolean need_shutdown = FALSE;

GHashTable *attr_hash = NULL;	//グローバルなattrd内のハッシュテーブル
cib_t *cib_conn = NULL;

typedef struct attr_hash_entry_s 	//ハッシュテーブルエントリ構造
{
		char *uuid;
		char *id;
		char *set;
		char *section;

		char *value;
		char *stored_value;

		int  timeout;
		char *dampen;
		guint  timer_id;
		
} attr_hash_entry_t;

//ハッシュ解放処理
static void
free_hash_entry(gpointer data)
{
	attr_hash_entry_t *entry = data;
	if (entry == NULL) {
		return;
	}	
	crm_free(entry->id);
	crm_free(entry->set);
	crm_free(entry->dampen);
	crm_free(entry->section);
	crm_free(entry->uuid);
	crm_free(entry->value);
	crm_free(entry->stored_value);
	crm_free(entry);
}

void attrd_local_callback(xmlNode * msg);
gboolean attrd_timer_callback(void *user_data);
gboolean attrd_trigger_update(attr_hash_entry_t *hash_entry);
void attrd_perform_update(attr_hash_entry_t *hash_entry);
//attrdのシャットダウン処理
static void
attrd_shutdown(int nsig)
{
	need_shutdown = TRUE;
	crm_info("Exiting");
	if (mainloop != NULL && g_main_is_running(mainloop)) {
		g_main_quit(mainloop);
	} else {
		exit(0);
	}
}

static void
usage(const char* cmd, int exit_status)
{
	FILE* stream;

	stream = exit_status ? stderr : stdout;

	fprintf(stream, "usage: %s [-srkh] [-c configure file]\n", cmd);
/* 	fprintf(stream, "\t-d\tsets debug level\n"); */
/* 	fprintf(stream, "\t-s\tgets daemon status\n"); */
/* 	fprintf(stream, "\t-r\trestarts daemon\n"); */
/* 	fprintf(stream, "\t-k\tstops daemon\n"); */
/* 	fprintf(stream, "\t-h\thelp message\n"); */
	fflush(stream);

	exit(exit_status);
}
//attrd接続クライアント情報
typedef struct attrd_client_s 
{
		char  *id;
		char  *name;
		
		IPC_Channel *channel;
		GCHSource   *source;
} attrd_client_t;
//dampenタイマー停止処理(タイマーのキャンセル)
static void
stop_attrd_timer(attr_hash_entry_t *hash_entry) 
{
	if(hash_entry != NULL && hash_entry->timer_id != 0) {
		//存在するハッシュエントリで、タイマーが設定済みの場合には、タイマーを削除する
		crm_debug_2("Stopping %s timer", hash_entry->id);
		g_source_remove(hash_entry->timer_id);
		hash_entry->timer_id = 0;
	}
}

static gboolean
attrd_ipc_callback(IPC_Channel *client, gpointer user_data)
{
	int lpc = 0;
	xmlNode *msg = NULL;
	attrd_client_t *curr_client = (attrd_client_t*)user_data;
	gboolean stay_connected = TRUE;
	
	crm_debug_2("Invoked: %s", curr_client->id);

	while(IPC_ISRCONN(client)) {
		if(client->ops->is_message_pending(client) == 0) {
			break;
		}
		
		msg = xmlfromIPC(client, MAX_IPC_DELAY);
		if (msg == NULL) {
		    break;
		}

		lpc++;
		
		crm_debug_2("Processing msg from %s", curr_client->id);
		crm_log_xml(LOG_DEBUG_3, __PRETTY_FUNCTION__, msg);
		
		attrd_local_callback(msg);	//ローカルコールバック処理の実行

		free_xml(msg);
		msg = NULL;

		if(client->ch_status != IPC_CONNECT) {
			break;
		}
	}
	
	crm_debug_2("Processed %d messages", lpc);
	if (client->ch_status != IPC_CONNECT) {
		stay_connected = FALSE;
	}

	return stay_connected;
}

static void
attrd_connection_destroy(gpointer user_data)
{
	attrd_client_t *client = user_data;
	
	/* cib_process_disconnect */

	if(client == NULL) {
		return;
	}

	if(client->source != NULL) {
		crm_debug_4("Deleting %s (%p) from mainloop",
			    client->name, client->source);
		G_main_del_IPC_Channel(client->source); 
		client->source = NULL;
	}
	
	crm_debug_3("Destroying %s (%p)", client->name, client);
	crm_free(client->name);
	crm_free(client->id);
	crm_free(client);
	crm_debug_4("Freed the cib client");

	return;
}
//クライアント接続処理
static gboolean
attrd_connect(IPC_Channel *channel, gpointer user_data)
{
	attrd_client_t *new_client = NULL;
	crm_debug_3("Connecting channel");

	if(channel == NULL) {
		crm_err("Channel was NULL");
		return FALSE;

	} else if(channel->ch_status != IPC_CONNECT) {
		crm_err("Channel was disconnected");
		return FALSE;		
	} else if(need_shutdown) {
		crm_info("Ignoring connection request during shutdown");
		return FALSE;		
	}
	

	crm_malloc0(new_client, sizeof(attrd_client_t));
	new_client->channel = channel;
	
	crm_debug_3("Created channel %p for channel %s",
		    new_client, new_client->id);
	
/* 		channel->ops->set_recv_qlen(channel, 100); */
/* 		channel->ops->set_send_qlen(channel, 400); */
	//クライアント接続のコールバックと切断コールバックを登録
	new_client->source = G_main_add_IPC_Channel(
		G_PRIORITY_DEFAULT, channel, FALSE, attrd_ipc_callback,
		new_client, attrd_connection_destroy);
	
	crm_debug_3("Client %s connected", new_client->id);
	
	return TRUE;
}
//ハッシュエントリのロギング
static void
log_hash_entry(int level, attr_hash_entry_t *entry, const char *text) 
{
	do_crm_log(level, "%s", text);
	do_crm_log(level, "Set:     %s", entry->section);
	do_crm_log(level, "Name:    %s", entry->id);
	do_crm_log(level, "Value:   %s", entry->value);
	do_crm_log(level, "Timeout: %s", entry->dampen);
}
//ハッシュエントリの検索処理
static attr_hash_entry_t *
find_hash_entry(xmlNode * msg) 
{
	const char *value = NULL;
	//受信メッセージから属性名を取得
	const char *attr  = crm_element_value(msg, F_ATTRD_ATTRIBUTE);
	attr_hash_entry_t *hash_entry = NULL;

	if(attr == NULL) {
		//属性名がＮＵＬＬの場合は何もしない
		crm_info("Ignoring message with no attribute name");
		return NULL;
	}
	//属性名でグローバルハッシュテーブルを検索
	hash_entry = g_hash_table_lookup(attr_hash, attr);

	if(hash_entry == NULL) {	
		/* create one and add it */
		//存在しない属性名の場合は、ハッシュテーブルにその属性名を追加する
		crm_info("Creating hash entry for %s", attr);
		crm_malloc0(hash_entry, sizeof(attr_hash_entry_t));
		hash_entry->id = crm_strdup(attr);

		g_hash_table_insert(attr_hash, hash_entry->id, hash_entry);
		hash_entry = g_hash_table_lookup(attr_hash, attr);
		CRM_CHECK(hash_entry != NULL, return NULL);
	}
	//受信メッセージのattr_setを取得する
	value = crm_element_value(msg, F_ATTRD_SET);
	if(value != NULL) {
		//attr_setが取得できればハッシュエントリにセットする
		crm_free(hash_entry->set);
		hash_entry->set = crm_strdup(value);
		crm_debug("\t%s->set: %s", attr, value);
	}
	//受信メッセージのattr_sectionを取得する	
	value = crm_element_value(msg, F_ATTRD_SECTION);
	if(value == NULL) {
		//取得出来ない場合は、statusをセットする
		value = XML_CIB_TAG_STATUS;
	}
	crm_free(hash_entry->section);
	//ハッシュエントリにsectionをセットする
	hash_entry->section = crm_strdup(value);
	crm_debug_2("\t%s->section: %s", attr, value);
	//受信メッセージのattr_dampeningを取得する
	value = crm_element_value(msg, F_ATTRD_DAMPEN);
	if(value != NULL) {
		//取得できた場合は、ハッシュエントリのdampenとtimeoutにセットする
		crm_free(hash_entry->dampen);
		hash_entry->dampen = crm_strdup(value);

		hash_entry->timeout = crm_get_msec(value);
		crm_debug_2("\t%s->timeout: %s", attr, value);
	}

	log_hash_entry(LOG_DEBUG_2, hash_entry, "Found (and updated) entry:");
	return hash_entry;
}

#if SUPPORT_HEARTBEAT
//Heartbeatの場合のha接続断処理
static void
attrd_ha_connection_destroy(gpointer user_data)
{
	crm_debug_3("Invoked");
	if(need_shutdown) {
		/* we signed out, so this is expected */
		crm_info("Heartbeat disconnection complete");
		return;
	}

	crm_crit("Lost connection to heartbeat service!");
	if (mainloop != NULL && g_main_is_running(mainloop)) {
		g_main_quit(mainloop);
		return;
	}
	exit(LSB_EXIT_OK);	
}
//Heartbeatの場合のクラスタ通信通信コールバック処理
// * flushメッセージ受信後の処理もこれになる
static void
attrd_ha_callback(HA_Message *msg, void* private_data)
{
	attr_hash_entry_t *hash_entry = NULL;
	//受信メッセージの展開
	xmlNode *xml	   = convert_ha_message(NULL, msg, __FUNCTION__);
	const char *from   = crm_element_value(xml, F_ORIG);
	const char *op     = crm_element_value(xml, F_ATTRD_TASK);
	const char *host   = crm_element_value(xml, F_ATTRD_HOST);
	const char *ignore = crm_element_value(xml, F_ATTRD_IGNORE_LOCALLY);
	//host指定があって、自ノードからの通信の場合
	if(host != NULL && safe_str_eq(host, attrd_uname)) {
	    //ローカルコールバック処理
	    crm_info("Update relayed from %s", from);
	    attrd_local_callback(xml);
	    
	} else if(ignore == NULL || safe_str_neq(from, attrd_uname)) {
		//即時処理済みのメッセージではないか、自ノード以外の通信の場合
		crm_info("%s message from %s", op, from);
		//通信メッセージの属性からハッシュテーブルを検索する
		hash_entry = find_hash_entry(xml);
		//dampenタイマーを停止する
		stop_attrd_timer(hash_entry);
		//cibの更新処理
		attrd_perform_update(hash_entry);
	}
	free_xml(xml);
}

#endif

#if SUPPORT_AIS
//corosyncの場合のクラスタ通信コールバック処理
static gboolean
attrd_ais_dispatch(AIS_Message *wrapper, char *data, int sender) 
{
    xmlNode *xml = NULL;

    if(wrapper->header.id == crm_class_cluster) {
	    xml = string2xml(data);
	    if(xml == NULL) {
		crm_err("Bad message received: %d:'%.120s'", wrapper->id, data);
	    }
    }

    if(xml != NULL) {
	attr_hash_entry_t *hash_entry = NULL;
	const char *op     = crm_element_value(xml, F_ATTRD_TASK);
	const char *host   = crm_element_value(xml, F_ATTRD_HOST);
	const char *ignore = crm_element_value(xml, F_ATTRD_IGNORE_LOCALLY);

	crm_xml_add_int(xml, F_SEQ, wrapper->id);
	crm_xml_add(xml, F_ORIG, wrapper->sender.uname);
	
	if(host != NULL && safe_str_eq(host, attrd_uname)) {
	    crm_info("Update relayed from %s", wrapper->sender.uname);
	    attrd_local_callback(xml);
	    
	} else 	if(ignore == NULL || safe_str_neq(wrapper->sender.uname, attrd_uname)) {
	    crm_debug_2("%s message from %s", op, wrapper->sender.uname);
	    hash_entry = find_hash_entry(xml);
	    stop_attrd_timer(hash_entry);
	    attrd_perform_update(hash_entry);
	}

	free_xml(xml);
    }
    
    return TRUE;
}
//corosyncの場合の接続断処理
static void
attrd_ais_destroy(gpointer unused)
{
    ais_fd_sync = -1;
    if(need_shutdown) {
	/* we signed out, so this is expected */
	crm_info("OpenAIS disconnection complete");
	return;
    }
    
    crm_crit("Lost connection to OpenAIS service!");
    if (mainloop != NULL && g_main_is_running(mainloop)) {
	g_main_quit(mainloop);
	return;
    }
    exit(LSB_EXIT_GENERIC);
}
#endif
//cib接続断処理
static void
attrd_cib_connection_destroy(gpointer user_data)
{
	cib_t *conn = user_data;
	conn->cmds->signoff(conn); /* Ensure IPC is cleaned up */

	if(need_shutdown) {
	    crm_info("Connection to the CIB terminated...");

	} else {
	    /* eventually this will trigger a reconnect, not a shutdown */ 
	    crm_err("Connection to the CIB terminated...");
	    exit(1);
	}
	
	return;
}

static void
update_for_hash_entry(gpointer key, gpointer value, gpointer user_data)
{
    attr_hash_entry_t *entry = value;
    if(entry->value != NULL) {
	attrd_timer_callback(value);
    }
}
//cibからのREPACE通知のコールバック処理
static void
do_cib_replaced(const char *event, xmlNode *msg)
{
    crm_info("Sending full refresh");
    //ハッシュテーブル全てのデータをトリガー更新処理
    g_hash_table_foreach(attr_hash, update_for_hash_entry, NULL);
}
//cib接続処理
static gboolean
cib_connect(void *user_data) 
{
    static int attempts = 1;
    static int max_retry = 20;
    gboolean was_err = FALSE;
    static cib_t *local_conn = NULL;
    
    if(local_conn == NULL) {
	local_conn = cib_new();
    }
    
    if(was_err == FALSE) {
	enum cib_errors rc = cib_not_connected;

	if(attempts < max_retry) {
	    crm_debug("CIB signon attempt %d", attempts);
	    rc = local_conn->cmds->signon(local_conn, T_ATTRD, cib_command);
	}

	if(rc != cib_ok && attempts > max_retry) {
	    crm_err("Signon to CIB failed: %s", cib_error2string(rc));
	    was_err = TRUE;

	} else if(rc != cib_ok) {
	    attempts++;
	    return TRUE;
	}
    }

    crm_info("Connected to the CIB after %d signon attempts", attempts);
	
    if(was_err == FALSE) {
	enum cib_errors rc = local_conn->cmds->set_connection_dnotify(
	    local_conn, attrd_cib_connection_destroy);
	if(rc != cib_ok) {
	    crm_err("Could not set dnotify callback");
	    was_err = TRUE;
	}
    }

    if(was_err == FALSE) {
		//cibからのREPACEの通知コールバックをセット
	if(cib_ok != local_conn->cmds->add_notify_callback(
	       local_conn, T_CIB_REPLACE_NOTIFY, do_cib_replaced)) {
	    crm_err("Could not set CIB notification callback");
	    was_err = TRUE;
	}
    }

    if(was_err) {
	crm_err("Aborting startup");
	exit(100);
    }

    cib_conn = local_conn;
    
    crm_info("Sending full refresh");
    //ハッシュテーブル全てのデータをトリガー更新処理(接続時に持っている属性値をセット)
    g_hash_table_foreach(attr_hash, update_for_hash_entry, NULL);
    
    return FALSE;
}


int
main(int argc, char ** argv)
{
	int flag = 0;
	int argerr = 0;
	gboolean was_err = FALSE;
	char *channel_name = crm_strdup(T_ATTRD);
	//ログ初期化
	crm_log_init(T_ATTRD, LOG_INFO, TRUE, FALSE, argc, argv);
	//SIGTERMハンドラをセット
	mainloop_add_signal(SIGTERM, attrd_shutdown);
	
	while ((flag = getopt(argc, argv, OPTARGS)) != EOF) {
		switch(flag) {
			case 'V':
				cl_log_enable_stderr(1);
				alter_debug(DEBUG_INC);
				break;
			case 'h':		/* Help message */
				usage(T_ATTRD, LSB_EXIT_OK);
				break;
			default:
				++argerr;
				break;
		}
	}

	if (optind > argc) {
		++argerr;
	}
    
	if (argerr) {
		usage(T_ATTRD, LSB_EXIT_GENERIC);
	}
	//グローバルハッシュテーブルの作成
	attr_hash = g_hash_table_new_full(
		g_str_hash, g_str_equal, NULL, free_hash_entry);

	crm_info("Starting up");

	if(was_err == FALSE) {
	    void *destroy = NULL;
	    void *dispatch = NULL;
	    void *data = NULL;
	    
#if SUPPORT_AIS
	    if(is_openais_cluster()) {
		destroy = attrd_ais_destroy;
		dispatch = attrd_ais_dispatch;	
	    }
#endif
	    
#if SUPPORT_HEARTBEAT
	    if(is_heartbeat_cluster()) {
		data = &attrd_cluster_conn;
		dispatch = attrd_ha_callback;
		destroy = attrd_ha_connection_destroy;
	    }
#endif
	    //Heartbeat,Corosyncへの接続とクラスタメッセージコールバック、切断コールバックセット
	    if(FALSE == crm_cluster_connect(
		   &attrd_uname, &attrd_uuid, dispatch, destroy, data)) {
		crm_err("HA Signon failed");
		was_err = TRUE;
	    }
	}

	crm_info("Cluster connection active");

	if(was_err == FALSE) {
		//クライアント接続と接続コールバック、切断コールバックセット
		int rc = init_server_ipc_comms(
			channel_name, attrd_connect,
			default_ipc_connection_destroy);
		
		if(rc != 0) {
			crm_err("Could not start IPC server");
			was_err = TRUE;
		}
	}
	
	crm_info("Accepting attribute updates");

	mainloop = g_main_new(FALSE);
	//cib接続処理
	if(0 == g_timeout_add_full(G_PRIORITY_LOW+1, 5000, cib_connect, NULL, NULL)) {
	    //接続失敗
	    crm_info("Adding timer failed");
	    was_err = TRUE;
	}
	
	if(was_err) {
	    crm_err("Aborting startup");
	    return 100;
	} 

	crm_info("Starting mainloop...");
	g_main_run(mainloop);
	crm_info("Exiting...");
	//後処理
#if SUPPORT_HEARTBEAT
	if(is_heartbeat_cluster()) {
		//heartbeatと切断
	    attrd_cluster_conn->llc_ops->signoff(attrd_cluster_conn, TRUE);
	    attrd_cluster_conn->llc_ops->delete(attrd_cluster_conn);
	}
#endif

	if(cib_conn) {
		//cibと切断
	    cib_conn->cmds->signoff(cib_conn);
	    cib_delete(cib_conn);
	}
	//ハッシュテーブルなど解放
	g_hash_table_destroy(attr_hash);
	crm_free(channel_name);
	crm_free(attrd_uuid);
	empty_uuid_cache();

	return 0;
}

struct attrd_callback_s 
{
	char *attr;
	char *value;
};
//cibからのコールバック処理
static void
attrd_cib_callback(xmlNode *msg, int call_id, int rc,
		   xmlNode *output, void *user_data)
{
	int err_level = LOG_ERR;
	attr_hash_entry_t *hash_entry = NULL;
	struct attrd_callback_s *data = user_data;
	if(data->value == NULL && rc == cib_NOTEXISTS) {
		rc = cib_ok;
	}

	switch(rc) {
	    case cib_ok://属性更新の完了
		crm_debug("Update %d for %s=%s passed", call_id, data->attr, data->value);
		//更新属性がハッシュテーブルに存在するか検索する
		hash_entry = g_hash_table_lookup(attr_hash, data->attr);

		if(hash_entry) {
			//ハッシュテーブルに存在する場合は、cibへの格納済み値をクリアする
		    crm_free(hash_entry->stored_value);
		    hash_entry->stored_value = NULL;
		    if(data->value != NULL) {
				//更新した値がNULLでない場合には、cibへの格納済み値を更新する
			hash_entry->stored_value = crm_strdup(data->value);
		    }
		}
		break;		
	    case cib_diff_failed:    /* When an attr changes while the CIB is syncing */
	    case cib_remote_timeout: /* When an attr changes while there is a DC election */
	    case cib_NOTEXISTS: /* When an attr changes while the CIB is syncing a
				 *   newer config from a node that just came up
				 */
		err_level = LOG_WARNING;
	    default:
		do_crm_log(err_level, "Update %d for %s=%s failed: %s",
			   call_id, data->attr, data->value, cib_error2string(rc));
	}
	//データの解放
	crm_free(data->value);
	crm_free(data->attr);
	crm_free(data);
}

//cibの更新処理
void
attrd_perform_update(attr_hash_entry_t *hash_entry)
{
	int rc = cib_ok;
	struct attrd_callback_s *data = NULL;
	
	if(hash_entry == NULL) {
		//該当ハッシュが存在しない場合には無視する
	    return;
	    
	} else if(cib_conn == NULL) {
	    crm_info("Delaying operation %s=%s: cib not connected", hash_entry->id, crm_str(hash_entry->value));
	    return;
	    
	} else if(hash_entry->value == NULL) {
		//属性削除処理
		/* delete the attr */
		rc = delete_attr(cib_conn, cib_none, hash_entry->section, attrd_uuid,
				 hash_entry->set, hash_entry->uuid, hash_entry->id, NULL, FALSE);

		if(hash_entry->stored_value) {
		    crm_info("Sent delete %d: node=%s, attr=%s, id=%s, set=%s, section=%s",
			     rc, attrd_uuid, hash_entry->id, hash_entry->uuid?hash_entry->uuid:"<n/a>",
			     hash_entry->set, hash_entry->section);

		} else if(rc < 0  && rc != cib_NOTEXISTS) {
		    crm_info("Delete operation failed: node=%s, attr=%s, id=%s, set=%s, section=%s: %s (%d)",
			     attrd_uuid, hash_entry->id, hash_entry->uuid?hash_entry->uuid:"<n/a>",
			     hash_entry->set, hash_entry->section, cib_error2string(rc), rc);

		} else {
		    crm_debug_2("Sent delete %d: node=%s, attr=%s, id=%s, set=%s, section=%s",
			     rc, attrd_uuid, hash_entry->id, hash_entry->uuid?hash_entry->uuid:"<n/a>",
			     hash_entry->set, hash_entry->section);
		}
		
	} else {
		//属性作成・更新処理
		/* send update */
		rc = update_attr(cib_conn, cib_none, hash_entry->section,
 				 attrd_uuid, hash_entry->set, hash_entry->uuid,
 				 hash_entry->id, hash_entry->value, FALSE);
		if(safe_str_neq(hash_entry->value, hash_entry->stored_value) || rc < 0) {
		    crm_info("Sent update %d: %s=%s", rc, hash_entry->id, hash_entry->value);
		} else {
		    crm_debug_2("Sent update %d: %s=%s", rc, hash_entry->id, hash_entry->value);
		}
	}

	crm_malloc0(data, sizeof(struct attrd_callback_s));
	data->attr = crm_strdup(hash_entry->id);
	if(hash_entry->value != NULL) {
	    data->value = crm_strdup(hash_entry->value);
	}
	//cibからのコールバックをセットする
	add_cib_op_callback(cib_conn, rc, FALSE, data, attrd_cib_callback);
	
	return;
}
//ローカルコールバック処理
void
attrd_local_callback(xmlNode * msg)
{
	static int plus_plus_len = 5;
	attr_hash_entry_t *hash_entry = NULL;
	//受信メッセージの展開
	const char *from  = crm_element_value(msg, F_ORIG);
	const char *op    = crm_element_value(msg, F_ATTRD_TASK);
	const char *attr  = crm_element_value(msg, F_ATTRD_ATTRIBUTE);
	const char *value = crm_element_value(msg, F_ATTRD_VALUE);
	const char *host  = crm_element_value(msg, F_ATTRD_HOST);

	if(safe_str_eq(op, "refresh")) {
		//refreshメッセージを受信した場合には、全てのattrd内のハッシュテーブル値を送信する
		crm_info("Sending full refresh (origin=%s)", from);
		g_hash_table_foreach(attr_hash, update_for_hash_entry, NULL);
		return;
	}

	if(host != NULL && safe_str_neq(host, attrd_uname)) {
		//受信メッセージのhostがNULLでなくて、自ノードでない場合は無視して、クラスタにメッセージを送信する
	    send_cluster_message(host, crm_msg_attrd, msg, FALSE);
	    return;
	}
	
	crm_debug("%s message from %s: %s=%s", op, from, attr, crm_str(value));
	//attrd内部ハッシュテーブルから受信した属性値を検索、新規設定する
	hash_entry = find_hash_entry(msg);
	if(hash_entry == NULL) {
	    return;
	}
	//uuid情報があればセットする
	if(hash_entry->uuid == NULL) {
	    const char *key  = crm_element_value(msg, F_ATTRD_KEY);
	    if(key) {
		hash_entry->uuid = crm_strdup(key);
	    }
	}
	
	crm_debug("Supplied: %s, Current: %s, Stored: %s",
		  value, hash_entry->value, hash_entry->stored_value);
	
	if(safe_str_eq(value, hash_entry->value)
	   && safe_str_eq(value, hash_entry->stored_value)) {
	    crm_debug_2("Ignoring non-change");
		//同じ値でかつ、cibに更新済みの値の場合は無視する
	    return;

	} else if(value) {
		//値処理
		//++,=及び属性値の処理
	    int offset = 1;
	    int int_value = 0;
	    int value_len = strlen(value);
	    if(value_len < (plus_plus_len + 2)
	       || value[plus_plus_len] != '+'
	       || (value[plus_plus_len+1] != '+' && value[plus_plus_len+1] != '=')) {
		goto set_unexpanded;	//値属性(++など以外）はインクリメントしないで処理(goto)
	    }
	    //以下は、++時の属性値の更新
	    int_value = char2score(hash_entry->value);
	    if(value[plus_plus_len+1] != '+') {
		const char *offset_s = value+(plus_plus_len+2);
		offset = char2score(offset_s);
	    }
	    int_value += offset;
	    
	    if(int_value > INFINITY) {
		int_value = INFINITY;
	    }
	    
	    crm_info("Expanded %s=%s to %d", attr, value, int_value);
	    crm_xml_add_int(msg, F_ATTRD_VALUE, int_value);
	    value = crm_element_value(msg, F_ATTRD_VALUE);
	}

  set_unexpanded:
	if(safe_str_eq(value, hash_entry->value) && hash_entry->timer_id) {
		//タイマーが既にセットされていて、同じ属性値の場合には処理しない
	    /* We're already waiting to set this value */
	    return;
	}
	//属性値をクリア
	crm_free(hash_entry->value);
	hash_entry->value = NULL;
	if(value != NULL) {
		//新しい属性値をセットする
		hash_entry->value = crm_strdup(value);
		crm_debug("New value of %s is %s", attr, value);
	}
	//damepnタイマーをキャンセルする
	//初期セットなどタイマー設定なしの場合は何もしない
	stop_attrd_timer(hash_entry);
	
	if(hash_entry->timeout > 0) {
		//damepn!=0時はdampenタイマー後に更新処理
		hash_entry->timer_id = g_timeout_add(
			hash_entry->timeout, attrd_timer_callback, hash_entry);
	} else {
		//dapmen=0時は即時に更新処理を実行する
		attrd_trigger_update(hash_entry);
	}
	
	return;
}
//dampenタイマー処理
gboolean
attrd_timer_callback(void *user_data)
{
	//タイマーを無効にする
    stop_attrd_timer(user_data);
    //トリガー更新処理の実行
    attrd_trigger_update(user_data);
    return TRUE; /* Always return true, removed cleanly by stop_attrd_timer() */
}
//トリガー更新処理(dampenタイマー起動、dampen=0時の即時更新処理)
//** flushメッセージ送信とdamepn=0時の自ノード即時更新 **
gboolean
attrd_trigger_update(attr_hash_entry_t *hash_entry)
{
	xmlNode *msg = NULL;

	/* send HA message to everyone */
	crm_info("Sending flush op to all hosts for: %s (%s)",
		 hash_entry->id, crm_str(hash_entry->value));
 	log_hash_entry(LOG_DEBUG_2, hash_entry, "Sending flush op to all hosts for:");
	//送信flushメッセージを作成する
	msg = create_xml_node(NULL, __FUNCTION__);
	crm_xml_add(msg, F_TYPE, T_ATTRD);
	crm_xml_add(msg, F_ORIG, attrd_uname);
	crm_xml_add(msg, F_ATTRD_TASK, "flush");
	crm_xml_add(msg, F_ATTRD_ATTRIBUTE, hash_entry->id);
	crm_xml_add(msg, F_ATTRD_SET, hash_entry->set);
	crm_xml_add(msg, F_ATTRD_SECTION, hash_entry->section);
	crm_xml_add(msg, F_ATTRD_DAMPEN, hash_entry->dampen);
	crm_xml_add(msg, F_ATTRD_VALUE, hash_entry->value);

	if(hash_entry->timeout <= 0) {
	//dampen=0の場合には、自ノードではすぐにcibに属性を送信する
	//自ノードへのflushメッセージの受信を無視させる為に、IGNOER_LOCALLYを送信メッセージにセットする
		crm_xml_add(msg, F_ATTRD_IGNORE_LOCALLY, hash_entry->value);
		attrd_perform_update(hash_entry);
	}
	//クラスターメッセージで全ノードにflushメッセージを送信する
	send_cluster_message(NULL, crm_msg_attrd, msg, FALSE);
	free_xml(msg);
	
	return TRUE;
}
