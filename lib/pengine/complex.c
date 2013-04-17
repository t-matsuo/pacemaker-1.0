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

#include <utils.h>
#include <crm/pengine/rules.h>
#include <crm/msg_xml.h>


extern xmlNode *get_object_root(const char *object_type,xmlNode *the_root);
void populate_hash(xmlNode *nvpair_list, GHashTable *hash,
		   const char **attrs, int attrs_length);

resource_object_functions_t resource_class_functions[] = {
	{
		native_unpack,
		native_find_rsc,
		native_parameter,
		native_print,
		native_active,
		native_resource_state,
		native_location,
		native_free
	},
	{
		group_unpack,
		native_find_rsc,
		native_parameter,
		group_print,
		group_active,
		group_resource_state,
		native_location,
		group_free
	},
	{
		clone_unpack,
		native_find_rsc,
		native_parameter,
		clone_print,
		clone_active,
		clone_resource_state,
		native_location,
		clone_free
	},
	{
		master_unpack,
		native_find_rsc,
		native_parameter,
		clone_print,
		clone_active,
		clone_resource_state,
		native_location,
		clone_free
	}
};

enum pe_obj_types get_resource_type(const char *name)
{
	if(safe_str_eq(name, XML_CIB_TAG_RESOURCE)) {
		return pe_native;

	} else if(safe_str_eq(name, XML_CIB_TAG_GROUP)) {
		return pe_group;

	} else if(safe_str_eq(name, XML_CIB_TAG_INCARNATION)) {
		return pe_clone;

	} else if(safe_str_eq(name, XML_CIB_TAG_MASTER)) {
		return pe_master;
	}
	
	return pe_unknown;
}

const char *get_resource_typename(enum pe_obj_types type)
{
    switch(type) {
	case pe_native:
	    return XML_CIB_TAG_RESOURCE;
	case pe_group:
	    return XML_CIB_TAG_GROUP;
	case pe_clone:
	    return XML_CIB_TAG_INCARNATION;
	case pe_master:
	    return XML_CIB_TAG_MASTER;
	case pe_unknown:
	    return "unknown";
    }
    return "<unknown>";
}

static void dup_attr(gpointer key, gpointer value, gpointer user_data)
{
	add_hash_param(user_data, key, value);
}
/* ハッシュテーブルにmeta_attributesノード、instance_attributesノードの内容をセットする */
void
get_meta_attributes(GHashTable *meta_hash, resource_t *rsc,
		    node_t *node, pe_working_set_t *data_set)
{
	GHashTable *node_hash = NULL;
	if(node) {
		/* ノードが指定されている場合は、node_hashハッシュテーブルをnode->details->attrsハッシュテーブルにセットする */
		node_hash = node->details->attrs;
	}
	/* 対象xmlの属性を全て処理して、ハッシュテーブルに追加する */
	xml_prop_iter(rsc->xml, prop_name, prop_value,
		      add_hash_param(meta_hash, prop_name, prop_value);
		);
	/* 対象xmlのmeta_attributesを展開して、ハッシュテーブルに追加する */
	unpack_instance_attributes(data_set->input, rsc->xml, XML_TAG_META_SETS, node_hash,
				   meta_hash, NULL, FALSE, data_set->now);

	/* populate from the regular attributes until the GUI can create
	 * meta attributes
	 */
	/* 対象xmlのinstance_attributesを展開して、ハッシュテーブルに追加する */
	unpack_instance_attributes(data_set->input, rsc->xml, XML_TAG_ATTR_SETS, node_hash,
				   meta_hash, NULL, FALSE, data_set->now);

	/* set anything else based on the parent */
	if(rsc->parent != NULL) {
		/* 親リソースが存在する場合は、その親リソースのmetaハッシュテーブルの内容もハッシュテーブルに追加する */
		g_hash_table_foreach(rsc->parent->meta, dup_attr, meta_hash);
	}	

	/* and finally check the defaults */
	/* 状態遷移作業エリアのrsc_defaultsタグのmeta_attributesを展開して、ハッシュテーブルに追加する */
	unpack_instance_attributes(data_set->input, data_set->rsc_defaults, XML_TAG_META_SETS, node_hash,
				   meta_hash, NULL, FALSE, data_set->now);
}
/* instance_attributesの内容を展開して、メタハッシュテーブルにセットする */
void
get_rsc_attributes(GHashTable *meta_hash, resource_t *rsc,
		   node_t *node, pe_working_set_t *data_set)
{
    GHashTable *node_hash = NULL;
    if(node) {
		/* ノードが指定されている場合は、そのノードのattrsハッシュテーブルをセットする */
	node_hash = node->details->attrs;
    }
	/* 対象xmlのinstance_attributesを展開して、ハッシュテーブルに追加する */
    unpack_instance_attributes(data_set->input, rsc->xml, XML_TAG_ATTR_SETS, node_hash,
			       meta_hash, NULL, FALSE, data_set->now);
    
    /* set anything else based on the parent */
    if(rsc->parent != NULL) {
		/* 親リソースが存在する場合は、親利リソースの内容も、metaハッシュテーブルの内容もハッシュテーブルに追加する */
	get_rsc_attributes(meta_hash, rsc->parent, node, data_set);

    } else {
	/* and finally check the defaults */
		/* 状態遷移作業エリアのrsc_defaultsタグのinstance_attributesを展開して、ハッシュテーブルに追加する */
	unpack_instance_attributes(data_set->input, data_set->rsc_defaults, XML_TAG_ATTR_SETS, node_hash,
				   meta_hash, NULL, FALSE, data_set->now);
    }
}
/* 共通リソース情報展開処理 */
gboolean	
common_unpack(xmlNode * xml_obj, resource_t **rsc,
	      resource_t *parent, pe_working_set_t *data_set)
{
	xmlNode *ops = NULL;
	resource_t *top = NULL;
	const char *value = NULL;
	const char *id    = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *class = crm_element_value(xml_obj, XML_AGENT_ATTR_CLASS);

	crm_log_xml_debug_3(xml_obj, "Processing resource input...");
	
	if(id == NULL) {
		pe_err("Must specify id tag in <resource>");
		return FALSE;
		
	} else if(rsc == NULL) {
		pe_err("Nowhere to unpack resource into");
		return FALSE;
		
	}
	crm_malloc0(*rsc, sizeof(resource_t));
	/* xml情報から"operationss"ノードのポインタを取得する */
	ops = find_xml_node(xml_obj, "operations", FALSE);
	/* xmlポインタに自身のxml情報へのポインタをセットする */
	(*rsc)->xml  = xml_obj;
	(*rsc)->parent  = parent;
	(*rsc)->ops_xml = expand_idref(ops, data_set->input);

	(*rsc)->variant = get_resource_type(crm_element_name(xml_obj));
	if((*rsc)->variant == pe_unknown) {
		/* unknownリソースの場合は、ERRORログを出力して、確保したエリアを解放して、FALSE */
		pe_err("Unknown resource type: %s", crm_element_name(xml_obj));
		crm_free(*rsc);
		return FALSE;
	}
	/* 展開用にparametersハッシュテーブル,metaハッシュテーブルを生成する */
	(*rsc)->parameters = g_hash_table_new_full(
		g_str_hash,g_str_equal, g_hash_destroy_str,g_hash_destroy_str);
	
	(*rsc)->meta = g_hash_table_new_full(
		g_str_hash,g_str_equal, g_hash_destroy_str,g_hash_destroy_str);
	/* xml情報から"clone"設定があれば取り出す:cloneリソースかどうかでid付けを切り替える */
	value = crm_element_value(xml_obj, XML_RSC_ATTR_INCARNATION);
	if(value) {
		/* cloneの場合は、idに"id:取り出した値をidにセット */
		(*rsc)->id = crm_concat(id, value, ':');
		/* metaハッシュテーブルに、cloneキーと取り出した値をセットする */
		add_hash_param((*rsc)->meta, XML_RSC_ATTR_INCARNATION, value);
		
	} else {
		/* cloneでない場合は、そのまま子タグのidをidにセット */
		(*rsc)->id = crm_strdup(id);
	}
	/* long_nameをセットする（親の場合はそのまま) */
	if(parent) {
		/* 親リソースがある場合は、親リソースのlong_name + ":" + idでlong_nameをセット */
		(*rsc)->long_name = crm_concat(parent->long_name, (*rsc)->id, ':');
	} else {
		/* 親リソースがない場合は、そのままidをセット */
		(*rsc)->long_name = crm_strdup((*rsc)->id);
	}
	
	/* fnsにリソースタイプに対応したクラス処理へのポインタをセット */
	(*rsc)->fns = &resource_class_functions[(*rsc)->variant];
	crm_debug_3("Unpacking resource...");
	/* 子タグ内のinstance_attributes,meta_attributesをmetaハッシュテーブルに展開する */
	/* 親リソースが存在する場合には、親リソースのmetaハッシュテーブルの内容もmetaハッシュテーブルに展開する */
	/* また、cib情報のrsc_defaultsの内容もmetaハッシュテーブルに展開する */
	get_meta_attributes((*rsc)->meta, *rsc, NULL, data_set);
	/* フラグを０初期化 *//* pe_rsc_runnableフラグをセット *//* pe_rsc_provisionalフラグをセット */
	(*rsc)->flags = 0;
	set_bit((*rsc)->flags, pe_rsc_runnable); 
	set_bit((*rsc)->flags, pe_rsc_provisional); 
	/* 状態遷移作業エリアのflagにpe_rsc_managedがセットされている場合は、 */
	/* pe_rsc_runnableフラグをセット 													*/
	if(is_set(data_set->flags, pe_flag_is_managed_default)) {
	    set_bit((*rsc)->flags, pe_rsc_managed); 
	}
	(*rsc)->rsc_cons	   = NULL; 				/* colocationリストをクリア */
	(*rsc)->actions            = NULL;			/* actionsリストをクリア */
	(*rsc)->role		   = RSC_ROLE_STOPPED;	/* 現在のroleをSTOPPEDで初期化 */
	(*rsc)->next_role	   = RSC_ROLE_UNKNOWN;	/* 次のroleをUNKNOWNで初期化 */

	(*rsc)->recovery_type      = recovery_stop_start;
	/* stickinessを状態遷移作業エリアのdefault_resource_stickiness */
	(*rsc)->stickiness         = data_set->default_resource_stickiness;
	(*rsc)->migration_threshold= INFINITY;		/* 故障閾値をINFINITY */
	(*rsc)->failure_timeout    = 0;				/* 故障回数を0で初期化 */
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、priorityを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_CIB_ATTR_PRIORITY);
	(*rsc)->priority	   = crm_parse_int(value, "0"); /* 取り出したprorityをpriortiyにセット。取り出せない場合は０ */
	(*rsc)->effective_priority = (*rsc)->priority;		/* effective_prioritypにもpriorityをセット */
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、notifyを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_NOTIFY);
	if(crm_is_true(value)) {
	    /* notifyがTRUEの場合は、リソースのフラグにpe_rsc_notifyをセット */
	    set_bit((*rsc)->flags, pe_rsc_notify); 
	}
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、is-managedを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_MANAGED);
	if(value != NULL && safe_str_neq("default", value)) {
	    /* 取り出せた場合で、defaultでない場合 */
	    gboolean bool_value = TRUE;
	    crm_str_to_boolean(value, &bool_value);
	    if(bool_value == FALSE) {
			/* FALSEの場合は、リソースのフラグのpe_rsc_managedをクリア */
		clear_bit((*rsc)->flags, pe_rsc_managed); 
	    } else {
			/* TRUEの場合は、リソースのフラグのpe_rsc_managedをセット */
		set_bit((*rsc)->flags, pe_rsc_managed); 
	    }
	}
	/* 状態遷移作業エリアのflagにpe_flag_maintenance_modeがセットされている場合は、 */
	/* リソースのフラグのpe_rsc_managedをクリア */
	/* ※メンテナンスモード中は、リソースは管理しない */
	if(is_set(data_set->flags, pe_flag_maintenance_mode)) {
	    clear_bit((*rsc)->flags, pe_rsc_managed);
	}
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、globally-uniqueを取り出す */
	crm_debug_2("Options for %s", (*rsc)->id);
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_UNIQUE);
	/* このリソース情報の最上位（トップの親リソース）のリソースを取得する */
	top = uber_parent(*rsc);
	if(crm_is_true(value) || top->variant < pe_clone) {
		/* globally-uniqueがTRUEか、そのトップリソースがpe_unknown、pe_native、pe_groupの場合(pe_clone,pe_master以外) */
	  	/* リソースのフラグのpe_rsc_uniqueをセット */
	    set_bit((*rsc)->flags, pe_rsc_unique); 
	}
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、restart-typeを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_RESTART);
	if(safe_str_eq(value, "restart")) {
		/* restart-typeがrestartの場合は、restart_typeにpe_restart_restart */
		(*rsc)->restart_type = pe_restart_restart;
		crm_debug_2("\tDependency restart handling: restart");

	} else {
		/* restart-typeがその他の場合は、restart_typeにpe_restart_ignore */
		(*rsc)->restart_type = pe_restart_ignore;
		crm_debug_2("\tDependency restart handling: ignore");
	}
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、multiple-activeを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_MULTIPLE);
	if(safe_str_eq(value, "stop_only")) {
		/* multiple-activがstop_onlyの場合は、recovery_typeにrecovery_stop_only */
		(*rsc)->recovery_type = recovery_stop_only;
		crm_debug_2("\tMultiple running resource recovery: stop only");

	} else if(safe_str_eq(value, "block")) {
		/* multiple-activがblockの場合は、recovery_typeにrecovery_block */
		(*rsc)->recovery_type = recovery_block;
		crm_debug_2("\tMultiple running resource recovery: block");

	} else {		
		/* multiple-activがその他の場合は、recovery_typeにrecovery_stop_start */
		(*rsc)->recovery_type = recovery_stop_start;
		crm_debug_2("\tMultiple running resource recovery: stop/start");
	}
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、resource-stickinessを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_STICKINESS);
	if(value != NULL && safe_str_neq("default", value)) {
		/* resource-stickinessが取れた場合で、default以外の場合は、取得した値をstickinessにセットする */
		/* ※ 先に全体の値（data_set->default_resource_stickiness)から設定しているが、リソース個別の */
		/*    設定がある場合にはここで上書きされる */
		(*rsc)->stickiness = char2score(value);
	}
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、migration-thresholdを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_FAIL_STICKINESS);
	if(value != NULL && safe_str_neq("default", value)) {
		/* migration-thresholdが取れた場合で、default以外の場合は、取得した値をmigration_thresholdにセット */
		/* ※ 先にINFINITY値を設定しているが、リソース個別の設定がある場合にはここで上書きされる */
		(*rsc)->migration_threshold = char2score(value);

	} else if(value == NULL) {
	    /* Make a best-effort guess at a migration threshold for people with 0.6 configs
	     * try with underscores and hyphens, from both the resource and global defaults section
	     */ 
		/* migration_thresholdが設定されていない場合 */
		/* 子タグ内から展開してあるmetaハッシュテーブル内から、*/
		/* resource-failure-stickines, resource_failure_stickiness, */
		/* default-resource-failure-stickiness, default_resource_failure_stickinessの順で */
		/* 取得を試みる */
	    value = g_hash_table_lookup((*rsc)->meta, "resource-failure-stickiness");
	    if(value == NULL) {
		value = g_hash_table_lookup((*rsc)->meta, "resource_failure_stickiness");
	    }
	    if(value == NULL) {
		value = g_hash_table_lookup(data_set->config_hash, "default-resource-failure-stickiness");
	    }
	    if(value == NULL) {
		value = g_hash_table_lookup(data_set->config_hash, "default_resource_failure_stickiness");
	    }

	    if(value) {
			/* 値が取れた場合は、値をスコア化する */
		int fail_sticky = char2score(value);
		if(fail_sticky == -INFINITY) {
			/* -INFINITYの場合は、migration_thresholdに１をセットする */
		    (*rsc)->migration_threshold = 1;
		    crm_info("Set a migration threshold of %d for %s based on a failure-stickiness of %s",
			     (*rsc)->migration_threshold, (*rsc)->id, value);

		} else if((*rsc)->stickiness != 0 && fail_sticky != 0) {
			/* このリソースのstickinessが０でなくて、スコア化した値が０でない場合 */
			/* リソースのstickiness÷スコア化した値で、migration_thresholdをセットする */
		    (*rsc)->migration_threshold = (*rsc)->stickiness / fail_sticky;
		    if((*rsc)->migration_threshold < 0) {
			/* Make sure it's positive */
					/* セットしたmigration_thresholdが０以下になった場合は、migration_thresholdの絶対値でセットする */
					/* Make sure it's positive */
			(*rsc)->migration_threshold = 0 - (*rsc)->migration_threshold;
		    }
	    	/* migration_thresholdをインクリメントする */
		    (*rsc)->migration_threshold += 1;
		    crm_info("Calculated a migration threshold for %s of %d based on a stickiness of %d/%s",
			     (*rsc)->id, (*rsc)->migration_threshold, (*rsc)->stickiness, value);
		}
	    }
	}

	
	/* 子タグ内から展開してあるmetaハッシュテーブル内から、failure-timeoutを取り出す */
	value = g_hash_table_lookup((*rsc)->meta, XML_RSC_ATTR_FAIL_TIMEOUT);
	if(value != NULL) {
	    /* call crm_get_msec() and convert back to seconds */
	    /* call crm_get_msec() and convert back to seconds */
	    /* 値が取れた場合は、取れた値÷1000をfailure_timeoutにセット */
	    /* ※秒化する */
	    (*rsc)->failure_timeout = (crm_get_msec(value) / 1000);
	}
	/* リソースの"target-role"属性をリソースのnext_roleに反映する */
	get_target_role(*rsc, &((*rsc)->next_role));
	crm_debug_2("\tDesired next state: %s",
		    (*rsc)->next_role!=RSC_ROLE_UNKNOWN?role2text((*rsc)->next_role):"default");
	/*------------ リソース個別のunpack処理を実行する-------------- */
	if((*rsc)->fns->unpack(*rsc, data_set) == FALSE) {
		return FALSE;
	}
	
	if(is_set(data_set->flags, pe_flag_symmetric_cluster)) {
		/* *** 通常のnativeリソースはここで、allowed_nodesが全ノード分スコア０で作成されることになる ***/
		/* pe_flag_symmetric_clusterフラグがセットされている場合(デフォルトでセット) */
		/* リソースの配置ノード情報(allowed_nodes)にノード情報を追加し、ノード情報の重み(weight)にスコア０を加算 */
		resource_location(*rsc, NULL, 0, "symmetric_default", data_set);
	}
	
	crm_debug_2("\tAction notification: %s",
		    is_set((*rsc)->flags, pe_rsc_notify)?"required":"not required");

	if(safe_str_eq(class, "stonith")) {
		/* リソースのclassが"stonith"だった場合は、pe_flag_have_stonith_resourceフラグをセットして、stonithリソースの保持を記録 */
	    set_bit_inplace(data_set->flags, pe_flag_have_stonith_resource);
	}
	
/* 	data_set->resources = g_list_append(data_set->resources, (*rsc)); */
	return TRUE;
}


void common_update_score(resource_t *rsc, const char *id, int score) 
{
    node_t *node = NULL;
    node = pe_find_node_id(rsc->allowed_nodes, id);
    if(node != NULL) {
	crm_debug_2("Updating score for %s on %s: %d + %d",
		    rsc->id, id, node->weight, score);
	node->weight = merge_weights(node->weight, score);
    }

    if(rsc->children) {
	slist_iter(
	    child_rsc, resource_t, rsc->children, lpc,
	    common_update_score(child_rsc, id, score);
	    );
    }
}

resource_t *uber_parent(resource_t *rsc) 
{
	resource_t *parent = rsc;
	if(parent == NULL) {
	    return NULL;
	}
	while(parent->parent != NULL) {
		parent = parent->parent;
	}
	return parent;
}

node_t *rsc_known_on(resource_t *rsc, GListPtr *list) 
{
    node_t *one = NULL;
    GListPtr result = NULL;

    if(rsc->children) {
	slist_iter(child, resource_t, rsc->children, lpc,
		   rsc_known_on(child, &result);
	    );
	
    } else if(rsc->known_on) {
	result = g_list_copy(rsc->known_on);
    }

    if(result && g_list_length(result) == 1) {
	one = g_list_nth_data(result, 0);
    }
    
    if(list) {
	slist_iter(node, node_t, result, lpc,
		   if(*list == NULL || pe_find_node_id(*list, node->details->id) == NULL) {
		       *list = g_list_append(*list, node);
		   }
	    );
    }

    g_list_free(result);	
    return one;
}

void common_free(resource_t *rsc)
{
	if(rsc == NULL) {
		return;
	}
	
	crm_debug_5("Freeing %s %d", rsc->id, rsc->variant);

	g_list_free(rsc->rsc_cons);
	g_list_free(rsc->rsc_cons_lhs);

	if(rsc->parameters != NULL) {
		g_hash_table_destroy(rsc->parameters);
	}
	if(rsc->meta != NULL) {
		g_hash_table_destroy(rsc->meta);
	}
	if(rsc->parent == NULL && is_set(rsc->flags, pe_rsc_orphan)) {
		free_xml(rsc->xml);
	}
	if(rsc->running_on) {
		g_list_free(rsc->running_on);
		rsc->running_on = NULL;
	}
	if(rsc->known_on) {
		g_list_free(rsc->known_on);
		rsc->known_on = NULL;
	}
	if(rsc->actions) {
		g_list_free(rsc->actions);
		rsc->actions = NULL;
	}
	pe_free_shallow_adv(rsc->rsc_location, FALSE);
	pe_free_shallow_adv(rsc->allowed_nodes, TRUE);
	crm_free(rsc->id);
	crm_free(rsc->long_name);	
	crm_free(rsc->clone_name);
	crm_free(rsc->allocated_to);
	crm_free(rsc->variant_opaque);
	crm_free(rsc);
	crm_debug_5("Resource freed");
}

