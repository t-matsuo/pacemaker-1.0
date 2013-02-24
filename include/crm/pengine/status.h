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
#ifndef PENGINE_STATUS__H
#define PENGINE_STATUS__H

#include <glib.h>
#include <crm/common/iso8601.h>
#include <crm/pengine/common.h>

typedef struct node_s node_t;
typedef struct action_s action_t;
typedef struct resource_s resource_t;

typedef enum no_quorum_policy_e {
	no_quorum_freeze,
	no_quorum_stop,
	no_quorum_ignore,
	no_quorum_suicide
} no_quorum_policy_t;

enum node_type {
	node_ping,
	node_member
};

enum pe_restart {
	pe_restart_restart,
	pe_restart_ignore
};

enum pe_find {
    pe_find_renamed  = 0x001,
    pe_find_partial  = 0x002,
    pe_find_clone    = 0x004,
    pe_find_current  = 0x008,
    pe_find_inactive = 0x010,
};

#define pe_flag_have_quorum		0x00000001ULL
#define pe_flag_symmetric_cluster	0x00000002ULL
#define pe_flag_is_managed_default	0x00000004ULL
#define pe_flag_maintenance_mode	0x00000008ULL

#define pe_flag_stonith_enabled		0x00000010ULL
#define pe_flag_have_stonith_resource	0x00000020ULL

#define pe_flag_stop_rsc_orphans	0x00000100ULL
#define pe_flag_stop_action_orphans	0x00000200ULL 	/* フラグがセットされている(stop-orphan-actionsがTRUE:デフォルトはTRUE)場合、*/
													/* statusタグ内に存在するlrm_resource情報だがリソース情報には存在しない操作情報の場合 */
													/* CancelXmlOp()で操作をキャンセルする */
													/* フラグがセットされていない場合は、処理されない */
#define pe_flag_stop_everything		0x00000400ULL

#define pe_flag_start_failure_fatal	0x00001000ULL
#define pe_flag_remove_after_stop	0x00002000ULL

/* data_set情報 */
typedef struct pe_working_set_s 
{
		xmlNode *input;
		ha_time_t *now;

		/* options extracted from the input */
		char *dc_uuid;
		node_t *dc_node;
		const char *stonith_action;

		unsigned long long flags;

		int stonith_timeout;
		int default_resource_stickiness;
		no_quorum_policy_t no_quorum_policy;

		GHashTable *config_hash;
		
		GListPtr nodes;							/* ノード情報リスト */
		GListPtr resources;						/* リソース情報リスト */
		GListPtr placement_constraints;
		GListPtr ordering_constraints;
		GListPtr colocation_constraints;
		
		GListPtr actions;
		xmlNode *failed;
		xmlNode *op_defaults;
		xmlNode *rsc_defaults;

		/* stats */
		int num_synapse;
		int max_valid_nodes;
		int order_id;
		int action_id;

		/* final output */
		xmlNode *graph;

} pe_working_set_t;
/* ノード詳細情報 */
struct node_shared_s { 
		const char *id; 
		const char *uname; 
		gboolean online;
		gboolean standby;
		gboolean standby_onfail;
		gboolean pending;
		gboolean unclean;
		gboolean shutdown;
		gboolean expected_up;
		gboolean is_dc;
		int	 num_resources;
		GListPtr running_rsc;	/* resource_t* */
		GListPtr allocated_rsc;	/* resource_t* */
		
		GHashTable *attrs;	/* char* => char* */
		enum node_type type;
}; 
/* ノード情報 */
struct node_s { 
		int	weight; 							/* weight(重み) */
		gboolean fixed;
		int      count;
		struct node_shared_s *details;			/* ノード詳細情報 */
};

#include <crm/pengine/complex.h>

#define pe_rsc_orphan		0x00000001ULL
#define pe_rsc_managed		0x00000002ULL

#define pe_rsc_notify		0x00000010ULL
#define pe_rsc_unique		0x00000020ULL		/* globally-unique=TRUEか、リソースの親リソースがpe_unknown、pe_native、pe_groupの場合(pe_clone,pe_master以外) にセット */

#define pe_rsc_provisional	0x00000100ULL
#define pe_rsc_allocating	0x00000200ULL		/* color処理実行中かどうかのフラグ */
#define pe_rsc_merging		0x00000400ULL

#define pe_rsc_try_reload	0x00001000ULL
#define pe_rsc_reload		0x00002000ULL

#define pe_rsc_failed		0x00010000ULL
#define pe_rsc_shutdown		0x00020000ULL
#define pe_rsc_runnable		0x00040000ULL
#define pe_rsc_start_pending	0x00080000ULL

#define pe_rsc_starting		0x00100000ULL
#define pe_rsc_stopping		0x00200000ULL

#define pe_rsc_failure_ignored  0x01000000ULL
/* リソース情報 */
struct resource_s { 
		char *id; 									/* リソースid *//* クローンの場合はid:clone名 */
		char *clone_name; 
		char *long_name; 							/* 親リソースがない場合は、idをセット */
													/* 親リソースがある場合は、親リソースlong_name:id */
		xmlNode *xml; 								/* 受信したcib.xml内のリソースのxmlへのポインタ		*/
		xmlNode *ops_xml; 							/* リソースタグ内のoperationタグへのポインタ			*/

		resource_t *parent;							/* 親リソースへのポインタ　親リソースがないリソースの場合はNULL */
		void *variant_opaque;						/* group,cloneなどのリソース毎の固有データ */
		enum pe_obj_types variant;					/* リソースのタイプ */
		resource_object_functions_t *fns;			/* fns処理配列ポインタ */
 		resource_alloc_functions_t  *cmds;			/* cmds処理配列ポインタ */

		enum rsc_recovery_type recovery_type;		/* リカバリタイプ */
		enum pe_restart        restart_type;		/* リスタートタイプ */
													/* restart-typeが指定されている場合は、そのタイプ値。指定なしの場合は、pe_restart_ignore */

		int	 priority; 
		int	 stickiness; 							/* stickiness値 	*/
		int	 sort_index; 
		int	 failure_timeout;
		int	 effective_priority; 
		int	 migration_threshold;	/* リソースのmigration-threshold値 */

		unsigned long long flags;
	
		GListPtr rsc_cons_lhs;     /* rsc_colocation_t* *//* colocationで、自リソースがwith-rsc指定されている場合のcolocation情報 */
		GListPtr rsc_cons;         /* rsc_colocation_t* *//* colocationで、自リソースがrsc指定されている場合のcolocation情報 */
		GListPtr rsc_location;     /* rsc_to_node_t*    *//* location指定の情報	*/
		GListPtr actions;	   /* action_t*         */
		/* リソース配置を決定したノード情報 */
		node_t *allocated_to;
		/* runnning_onリスト : リソースの現在状態からSTOPPED,UNKNOWN以外のrole(起動済み)と判断した場合に設定されるノードのリスト */
		/* 起動しているノード情報が入る */
		GListPtr running_on;       /* node_t*   */
		/* known_onリスト : リソースの現在状態からUNKNOWN以外のroleと判断した場合に設定されるノードのリスト */
		/* UNKNOWN以外のノード情報が入る */
		GListPtr known_on;	   /* node_t* */
		/* リソースが配置可能なノード情報のリスト */
		/* 		初期の展開時(native_unpack)にクリア */
		GListPtr allowed_nodes;    /* node_t*   */

		enum rsc_role_e role;		/* 現在のrole */
		enum rsc_role_e next_role;	/* 次の遷移role */

		GHashTable *meta;			/* リソースのmeta情報のハッシュテーブル */	   
		GHashTable *parameters;		/* リソースのparameter情報のハッシュテーブル	*/

		GListPtr children;	  /* resource_t* */	/* 子リソース情報リスト */
};
/* アクション情報 */
struct action_s 
{
		int         id;
		int         priority;
		resource_t *rsc;
		void       *rsc_opaque;
		node_t     *node;
		char *task;

		char *uuid;
		xmlNode *op_entry;
		
		gboolean pseudo;
		gboolean runnable;
		gboolean optional;
		gboolean print_always;
		gboolean failure_is_fatal;
		gboolean implied_by_stonith;
		gboolean allow_reload_conversion; /* no longer used */

		enum rsc_start_requirement needs;
		enum action_fail_response  on_fail;
		enum rsc_role_e fail_role;
		
		gboolean dumped;
		gboolean processed;

		action_t *pre_notify;
		action_t *pre_notified;
		action_t *post_notify;
		action_t *post_notified;
		
		int seen_count;

		GHashTable *meta;
		GHashTable *extra;
		
		GListPtr actions_before; /* action_warpper_t* */
		GListPtr actions_after;  /* action_warpper_t* */
};
/* notify情報 */
typedef struct notify_data_s {
	GHashTable *keys;

	const char *action;
	
	action_t *pre;
	action_t *post;
	action_t *pre_done;
	action_t *post_done;

	GListPtr active;   /* notify_entry_t*  */
	GListPtr inactive; /* notify_entry_t*  */
	GListPtr start;    /* notify_entry_t*  */
	GListPtr stop;     /* notify_entry_t*  */
	GListPtr demote;   /* notify_entry_t*  */
	GListPtr promote;  /* notify_entry_t*  */
	GListPtr master;   /* notify_entry_t*  */
	GListPtr slave;    /* notify_entry_t*  */
		
} notify_data_t;

gboolean cluster_status(pe_working_set_t *data_set);
extern void set_working_set_defaults(pe_working_set_t *data_set);
extern void cleanup_calculations(pe_working_set_t *data_set);
extern resource_t *pe_find_resource(GListPtr rsc_list, const char *id_rh);
extern node_t *pe_find_node(GListPtr node_list, const char *uname);
extern node_t *pe_find_node_id(GListPtr node_list, const char *id);
extern GListPtr find_operations(
    const char *rsc, const char *node, gboolean active_filter, pe_working_set_t *data_set);

#endif
