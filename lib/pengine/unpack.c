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

#include <lrm/lrm_api.h>
#include <glib.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>
#include <crm/common/util.h>
#include <crm/pengine/status.h>
#include <crm/pengine/rules.h>
#include <utils.h>
#include <unpack.h>

#define set_config_flag(data_set, option, flag) do {			\
	const char *tmp = pe_pref(data_set->config_hash, option);	\
	if(tmp) {							\
	    if(crm_is_true(tmp)) {					\
		set_bit_inplace(data_set->flags, flag);			\
	    } else {							\
		clear_bit_inplace(data_set->flags, flag);		\
	    }								\
	}								\
    } while(0)

gboolean unpack_rsc_op(
    resource_t *rsc, node_t *node, xmlNode *xml_op,
    enum action_fail_response *failed, pe_working_set_t *data_set);
/* 指定ノードをunclean状態に設定する処理 */
static void pe_fence_node(pe_working_set_t *data_set, node_t *node, const char *reason)
{
    CRM_CHECK(node, return);
    if(node->details->unclean == FALSE) {
	if(is_set(data_set->flags, pe_flag_stonith_enabled)) {
		/* ノードのuncleanがFALSEの場合は、状態遷移作業エリアのpe_flag_stonith_enabledを判定してログを出力する */
	    crm_warn("Node %s will be fenced %s", node->details->uname, reason);
	} else {
	    crm_warn("Node %s is unclean %s", node->details->uname, reason);
	}
    }
    /* ノードをアンクリーン(unclean=TRUE)に設定する */
    node->details->unclean = TRUE;
}

/* crm_configノードの展開処理 */
gboolean
unpack_config(xmlNode *config, pe_working_set_t *data_set)
{
	const char *value = NULL;
	/* config_hashハッシュテーブルを生成して、data_setにセットする */
	GHashTable *config_hash = g_hash_table_new_full(
		g_str_hash,g_str_equal, g_hash_destroy_str,g_hash_destroy_str);

	data_set->config_hash = config_hash;
	/* crm_configノード内のcluster_property_setノードの値を、configu_hashにセットする */
	unpack_instance_attributes(
		data_set->input, config, XML_CIB_TAG_PROPSET, NULL, config_hash,
		CIB_OPTIONS_FIRST, FALSE, data_set->now);
	/* 展開したconfig_hashをverifyする */
	verify_pe_options(data_set->config_hash);
	
	value = pe_pref(data_set->config_hash, "stonith-timeout");
	data_set->stonith_timeout = crm_get_msec(value);
	crm_debug("STONITH timeout: %d", data_set->stonith_timeout);

	set_config_flag(data_set, "stonith-enabled", pe_flag_stonith_enabled);
	crm_debug("STONITH of failed nodes is %s",
		  is_set(data_set->flags, pe_flag_stonith_enabled)?"enabled":"disabled");	

	data_set->stonith_action = pe_pref(data_set->config_hash, "stonith-action");
	crm_debug_2("STONITH will %s nodes", data_set->stonith_action);	
	
	set_config_flag(data_set, "stop-all-resources", pe_flag_stop_everything);
	crm_debug("Stop all active resources: %s",
		  is_set(data_set->flags, pe_flag_stop_everything)?"true":"false");
	
	set_config_flag(data_set, "symmetric-cluster", pe_flag_symmetric_cluster);
	if(is_set(data_set->flags, pe_flag_symmetric_cluster)) {
		crm_debug("Cluster is symmetric"
			 " - resources can run anywhere by default");
	}

	value = pe_pref(data_set->config_hash, "default-resource-stickiness");
	data_set->default_resource_stickiness = char2score(value);
	crm_debug("Default stickiness: %d",
		 data_set->default_resource_stickiness);

	value = pe_pref(data_set->config_hash, "no-quorum-policy");

	if(safe_str_eq(value, "ignore")) {
		data_set->no_quorum_policy = no_quorum_ignore;
		
	} else if(safe_str_eq(value, "freeze")) {
		data_set->no_quorum_policy = no_quorum_freeze;

	} else if(safe_str_eq(value, "suicide")) {
	    gboolean do_panic = FALSE;
	    crm_element_value_int(data_set->input, XML_ATTR_QUORUM_PANIC, &do_panic);

	    if(is_set(data_set->flags, pe_flag_stonith_enabled) == FALSE){
		crm_config_err("Setting no-quorum-policy=suicide makes no sense if stonith-enabled=false");
	    }

	    if(do_panic && is_set(data_set->flags, pe_flag_stonith_enabled)) {
		data_set->no_quorum_policy = no_quorum_suicide;

	    } else if(is_set(data_set->flags, pe_flag_have_quorum) == FALSE && do_panic == FALSE) {
		crm_notice("Resetting no-quorum-policy to 'stop': The cluster has never had quorum");
		data_set->no_quorum_policy = no_quorum_stop;
	    }
	    
	} else {
		data_set->no_quorum_policy = no_quorum_stop;
	}
	
	switch (data_set->no_quorum_policy) {
		case no_quorum_freeze:
			crm_debug("On loss of CCM Quorum: Freeze resources");
			break;
		case no_quorum_stop:
			crm_debug("On loss of CCM Quorum: Stop ALL resources");
			break;
		case no_quorum_suicide:
			crm_notice("On loss of CCM Quorum: Fence all remaining nodes");
			break;
		case no_quorum_ignore:
			crm_notice("On loss of CCM Quorum: Ignore");
			break;
	}

	set_config_flag(data_set, "stop-orphan-resources", pe_flag_stop_rsc_orphans);
	crm_debug_2("Orphan resources are %s",
		    is_set(data_set->flags, pe_flag_stop_rsc_orphans)?"stopped":"ignored");	
	
	set_config_flag(data_set, "stop-orphan-actions", pe_flag_stop_action_orphans);
	crm_debug_2("Orphan resource actions are %s",
		    is_set(data_set->flags, pe_flag_stop_action_orphans)?"stopped":"ignored");	

	set_config_flag(data_set, "remove-after-stop", pe_flag_remove_after_stop);
	crm_debug_2("Stopped resources are removed from the status section: %s",
		    is_set(data_set->flags, pe_flag_remove_after_stop)?"true":"false");	
	
	set_config_flag(data_set, "maintenance-mode", pe_flag_maintenance_mode);
	crm_debug_2("Maintenance mode: %s",
		    is_set(data_set->flags, pe_flag_maintenance_mode)?"true":"false");	

	if(is_set(data_set->flags, pe_flag_maintenance_mode)) {
	    clear_bit(data_set->flags, pe_flag_is_managed_default);
	} else {
	    set_config_flag(data_set, "is-managed-default", pe_flag_is_managed_default);
	}
	crm_debug_2("By default resources are %smanaged",
		    is_set(data_set->flags, pe_flag_is_managed_default)?"":"not ");

	set_config_flag(data_set, "start-failure-is-fatal", pe_flag_start_failure_fatal);
	crm_debug_2("Start failures are %s",
		    is_set(data_set->flags, pe_flag_start_failure_fatal)?"always fatal":"handled by failcount");

	node_score_red    = char2score(pe_pref(data_set->config_hash, "node-health-red"));
	node_score_green  = char2score(pe_pref(data_set->config_hash, "node-health-green"));
	node_score_yellow = char2score(pe_pref(data_set->config_hash, "node-health-yellow"));

	crm_info("Node scores: 'red' = %s, 'yellow' = %s, 'green' = %s",
		 pe_pref(data_set->config_hash, "node-health-red"),
		 pe_pref(data_set->config_hash, "node-health-yellow"),
		 pe_pref(data_set->config_hash, "node-health-green"));
	
	return TRUE;
}
/* nodesノードの展開処理 */
gboolean
unpack_nodes(xmlNode * xml_nodes, pe_working_set_t *data_set)
{
	node_t *new_node   = NULL;
	const char *id     = NULL;
	const char *uname  = NULL;
	const char *type   = NULL;
	gboolean unseen_are_unclean = TRUE;
	/* crm_configノード展開セットされたconfig_hashハッシュテーブルから、"startup-fencing"値を取り出す */
	const char *blind_faith = pe_pref(
		data_set->config_hash, "startup-fencing");
	
	if(crm_is_true(blind_faith) == FALSE) {
		/* startup-fencing値がFALSEの場合は、unseen_are_uncleanをFALSEにセットし、警告ログを出力する */
		unseen_are_unclean = FALSE;
		crm_warn("Blind faith: not fencing unseen nodes");
	}
	/* 全てのnodesノード内のnodeノードを処理する */
	xml_child_iter_filter(
		xml_nodes, xml_obj, XML_CIB_TAG_NODE,

		new_node = NULL;
		/* nodeノードのid属性,uname属性,type属性を取得する */
		id     = crm_element_value(xml_obj, XML_ATTR_ID);
		uname  = crm_element_value(xml_obj, XML_ATTR_UNAME);
		type   = crm_element_value(xml_obj, XML_ATTR_TYPE);
		crm_debug_3("Processing node %s/%s", uname, id);

		if(id == NULL) {
			/* idがNULLの場合は、エラーログを出力し該当ノードは処理しない */
			crm_config_err("Must specify id tag in <node>");
			continue;
		}
		if(type == NULL) {
			/* typeがNULLの場合は、エラーログを出力し該当ノードは処理しない */
			crm_config_err("Must specify type tag in <node>");
			continue;
		}
		if(pe_find_node(data_set->nodes, uname) != NULL) {
			/* data_set->nodesに既に存在するノード(uname)の場合は、警告メッセージを出力する */
		    crm_config_warn("Detected multiple node entries with uname=%s"
				    " - this is rarely intended", uname);
		}
		/* ノード情報エリアを確保する */
		crm_malloc0(new_node, sizeof(node_t));
		if(new_node == NULL) {
			return FALSE;	/* 確保出来ない場合は処理を終了する */
		}
		/* ノード情報エリアのweight,fixedを初期化し、detailsエリアを確保する */
		new_node->weight = 0;
		new_node->fixed  = FALSE;
		crm_malloc0(new_node->details,
			   sizeof(struct node_shared_s));

		if(new_node->details == NULL) {
			crm_free(new_node);
			return FALSE;	/* 確保出来ない場合は処理を終了する */
		}
		/* ノード情報エリアに展開したid,unameとtype=node_ping,online=FALSE,shutdown=FALSE,稼動中のリソース(runnning_rsc)=NULL */
		/* をセットし、attrsハッシュテーブルを新規生成してセットする */
		crm_debug_3("Creaing node for entry %s/%s", uname, id);
		new_node->details->id		= id;
		new_node->details->uname	= uname;
		new_node->details->type		= node_ping;
		new_node->details->online	= FALSE;
		new_node->details->shutdown	= FALSE;
		new_node->details->running_rsc	= NULL;
		new_node->details->attrs        = g_hash_table_new_full(
			g_str_hash, g_str_equal,
			g_hash_destroy_str, g_hash_destroy_str);
		
/* 		if(data_set->have_quorum == FALSE */
/* 		   && data_set->no_quorum_policy == no_quorum_stop) { */
/* 			/\* start shutting resources down *\/ */
/* 			new_node->weight = -INFINITY; */
/* 		} */
		
		if(is_set(data_set->flags, pe_flag_stonith_enabled) == FALSE || unseen_are_unclean == FALSE) {
			/* blind faith... */
			/* stonith-enabledがFALSEか、startup-fencingがFALSE(unseen_are_unclean)の場合は、uncleanをFALSEにセットする */
			new_node->details->unclean = FALSE; 

		} else {
			/* all nodes are unclean until we've seen their
			 * status entry
			 */
			/* その他の場合は、ノード情報をunclean(TRUE)でセットする */
			new_node->details->unclean = TRUE;
		}
		
		if(type == NULL
		   || safe_str_eq(type, "member")
		   || safe_str_eq(type, NORMALNODE)) {
			/* 取得したtypeがNULLか、memberか、normalの場合は、typeにnode_memberをセットする */
			new_node->details->type = node_member;
		}
		/* ノード情報のattrハッシュテーブルに属性を展開する */
		add_node_attrs(xml_obj, new_node, FALSE, data_set);
		/* 処理したnodeノードのノード情報をdata_set->nodesリストに追加する */
		data_set->nodes = g_list_append(data_set->nodes, new_node);    
		crm_debug_3("Done with node %s",
			    crm_element_value(xml_obj, XML_ATTR_UNAME));
		);
  
	return TRUE;
}
/* resourcesノードの展開処理 */
gboolean 
unpack_resources(xmlNode * xml_resources, pe_working_set_t *data_set)
{
	/* resourcesノードの全ての内容を処理する */
	xml_child_iter(
		xml_resources, xml_obj, 

		resource_t *new_rsc = NULL;
		crm_debug_3("Beginning unpack... <%s id=%s... >",
			    crm_element_name(xml_obj), ID(xml_obj));
		/* 共通展開処理を実行する */
		if(common_unpack(xml_obj, &new_rsc, NULL, data_set)) {
			/* 展開したリソース情報をdata_set->resourcesリストに追加する */
			data_set->resources = g_list_append(
				data_set->resources, new_rsc);
			/* リソース展開結果をログに出力 */
			print_resource(LOG_DEBUG_3, "Added", new_rsc, FALSE);

		} else {
			/* 展開処理に失敗した場合はエラーをログを出力し、展開したリソースを解放する */
			crm_config_err("Failed unpacking %s %s",
				      crm_element_name(xml_obj),
				      crm_element_value(xml_obj, XML_ATTR_ID));
			if(new_rsc != NULL && new_rsc->fns != NULL) {
				new_rsc->fns->free(new_rsc);
			}
		}
		);
	/* 全てのresourcesノードが処理が終わったら、priorityに応じて、data_set->resourceリストをソートする */
	data_set->resources = g_list_sort(
		data_set->resources, sort_rsc_priority);

	if(is_set(data_set->flags, pe_flag_stonith_enabled) && is_set(data_set->flags, pe_flag_have_stonith_resource) == FALSE) {
		/* stonith-enabled=trueであるにも関わらず、stonithリソースが存在しない場合は、エラーログを出力する */
	    crm_config_err("Resource start-up disabled since no STONITH resources have been defined");
	    crm_config_err("Either configure some or disable STONITH with the stonith-enabled option");
	    crm_config_err("NOTE: Clusters with shared data need STONITH to ensure data integrity");
	}
	
	return TRUE;
}


/* remove nodes that are down, stopping */
/* create +ve rsc_to_node constraints between resources and the nodes they are running on */
/* anything else? */
/* statusノードの展開処理 */
gboolean
unpack_status(xmlNode * status, pe_working_set_t *data_set)
{
	const char *id    = NULL;
	const char *uname = NULL;

	xmlNode * lrm_rsc    = NULL;
	xmlNode * attrs      = NULL;
	node_t    *this_node  = NULL;
	/* statusノードの全てのnode_stateノードを処理する */
	crm_debug_3("Beginning unpack");
	xml_child_iter_filter(
		status, node_state, XML_CIB_TAG_STATE,
		/* node_stateノードのid属性、uname属性を取得する */
		id    = crm_element_value(node_state, XML_ATTR_ID);
		uname = crm_element_value(node_state,    XML_ATTR_UNAME);
		/* node_stateノード内の"transient_attributes"ノードのポインタを取得する */
		attrs = find_xml_node(
			node_state, XML_TAG_TRANSIENT_NODEATTRS, FALSE);
		/* node_stateノード内の"lrm"ノードのポインタを取得する */
		lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
		/* 取得した"lrm"ノードから、"lrm_resources"ノードのポインタを取得する */
		lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);

		crm_debug_3("Processing node %s", uname);
		/* data_set->nodesリストからidで検索し、ノード情報のポインタを取得する */
		this_node = pe_find_node_id(data_set->nodes, id);

		if(uname == NULL) {
			/* error */
			continue;

		} else if(this_node == NULL) {
			/* nodes情報から展開されていないノードの情報の場合は警告ログを出力し処理しない */
			crm_config_warn("Node %s in status section no longer exists",
				       uname);
			continue;
		}

		/* Mark the node as provisionally clean
		 * - at least we have seen it in the current cluster's lifetime
		 */
		/* ノード情報のuncleanをFALSEに設定する */
		this_node->details->unclean = FALSE;
		/* ノード属性情報のハッシュテーブル(node->details->attrs)への追加処理 */
		add_node_attrs(attrs, this_node, TRUE, data_set);

		if(crm_is_true(g_hash_table_lookup(this_node->details->attrs, "standby"))) {
			/* ノード属性情報の"standby"値がtrueの場合は、standbyをTRUEに設定する */
			crm_info("Node %s is in standby-mode",
				 this_node->details->uname);
			this_node->details->standby = TRUE;
		}
		/* ノードの状態を決定する */
		crm_debug_3("determining node state");
		determine_online_status(node_state, this_node, data_set);

		if(this_node->details->online
		   && data_set->no_quorum_policy == no_quorum_suicide) {
		    /* Everything else should flow from this automatically
		     * At least until the PE becomes able to migrate off healthy resources 
		     */
		    /* 対象ノードをunclean状態に設定する */
		    pe_fence_node(data_set, this_node, "because the cluster does not have quorum");
		}
		);

	/* Now that we know all node states, we can safely handle migration ops
	 * But, for now, only process healthy nodes
	 *  - this is necessary for the logic in bug lf#2508 to function correctly 
	 */	
	/* onlineノードの"lrm_resources"ノードを処理する為に、statusノードの全てのnode_stateノードを処理する */
	xml_child_iter_filter(
		status, node_state, XML_CIB_TAG_STATE,
		/* node_stateノードのid属性を取得する */
		id = crm_element_value(node_state, XML_ATTR_ID);
		/* data_set->nodesリストからidで検索し、ノード情報のポインタを取得する */
		this_node = pe_find_node_id(data_set->nodes, id);
		
		if(this_node == NULL) {
			/* ノード情報が存在しない場合は処理しない */
			crm_info("Node %s is unknown", id);
			continue;
			
		} else if(this_node->details->online) {
			crm_debug_3("Processing lrm resource entries on healthy node: %s", this_node->details->uname);
			/* node_stateノード内の"lrm"ノードのポインタを取得する */
			lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
			/* 取得した"lrm"ノードから、"lrm_resources"ノードのポインタを取得する */
			lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);
			/* "lrm_resources"ノードの情報をノード情報に展開する */
			unpack_lrm_resources(this_node, lrm_rsc, data_set);
		}
		);

	/* Now handle failed nodes - but only if stonith is enabled
	 *
	 * By definition, offline nodes run no resources so there is nothing to do.
	 * Only when stonith is enabled do we need to know what is on the node to
	 * ensure rsc start events happen after the stonith
	 */
	/* 非onlineノードの"lrm_resources"ノードを処理する為に、statusノードの全てのnode_stateノードを処理する */
	xml_child_iter_filter(
		status, node_state, XML_CIB_TAG_STATE,

		if(!is_set(data_set->flags, pe_flag_stonith_enabled)) {
			/* stonith無しの場合は処理しない */
			break;
		}
		/* node_stateノードのid属性を取得する */
		id = crm_element_value(node_state, XML_ATTR_ID);
		/* data_set->nodesリストからidで検索し、ノード情報のポインタを取得する */
		this_node = pe_find_node_id(data_set->nodes, id);
		
		if(this_node == NULL || this_node->details->online) {
			/* ノード情報が存在しないか、online状態でない場合は処理しない */
			continue;
			
		} else {
			crm_debug_3("Processing lrm resource entries on unhealthy node: %s", this_node->details->uname);
			/* node_stateノード内の"lrm"ノードのポインタを取得する */
			lrm_rsc = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
			/* 取得した"lrm"ノードから、"lrm_resources"ノードのポインタを取得する */
			lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);
			/* "lrm_resources"ノードの情報をノード情報に展開する */
			unpack_lrm_resources(this_node, lrm_rsc, data_set);
		}
		);

	return TRUE;
	
}
/* stonith無しの場合の処理 */
static gboolean
determine_online_status_no_fencing(pe_working_set_t *data_set, xmlNode * node_state, node_t *this_node)
{
	gboolean online = FALSE;
	const char *join_state = crm_element_value(node_state, XML_CIB_ATTR_JOINSTATE);
	const char *crm_state  = crm_element_value(node_state, XML_CIB_ATTR_CRMDSTATE);
	const char *ccm_state  = crm_element_value(node_state, XML_CIB_ATTR_INCCM);
	const char *ha_state   = crm_element_value(node_state, XML_CIB_ATTR_HASTATE);
	const char *exp_state  = crm_element_value(node_state, XML_CIB_ATTR_EXPSTATE);

	if(ha_state == NULL) {
		ha_state = DEADSTATUS;
	}
	
	if(!crm_is_true(ccm_state) || safe_str_eq(ha_state, DEADSTATUS)){
		crm_debug_2("Node is down: ha_state=%s, ccm_state=%s",
			    crm_str(ha_state), crm_str(ccm_state));
		
	} else if(safe_str_eq(crm_state, ONLINESTATUS)) {
		if(safe_str_eq(join_state, CRMD_JOINSTATE_MEMBER)) {
			online = TRUE;
		} else {
			crm_debug("Node is not ready to run resources: %s", join_state);
		}
		
	} else if(this_node->details->expected_up == FALSE) {
		crm_debug_2("CRMd is down: ha_state=%s, ccm_state=%s",
			    crm_str(ha_state), crm_str(ccm_state));
		crm_debug_2("\tcrm_state=%s, join_state=%s, expected=%s",
			    crm_str(crm_state), crm_str(join_state),
			    crm_str(exp_state));
		
	} else {
		/* mark it unclean */
		/* 対象ノードをunclean状態に設定する */
		pe_fence_node(data_set, this_node, "because it is partially and/or un-expectedly down");
		crm_info("\tha_state=%s, ccm_state=%s,"
			 " crm_state=%s, join_state=%s, expected=%s",
			 crm_str(ha_state), crm_str(ccm_state),
			 crm_str(crm_state), crm_str(join_state),
			 crm_str(exp_state));
	}
	return online;
}
/* stonithありの場合の処理 */
static gboolean
determine_online_status_fencing(pe_working_set_t *data_set, xmlNode * node_state, node_t *this_node)
{
	gboolean online = FALSE;
	gboolean do_terminate = FALSE;
	const char *join_state = crm_element_value(node_state, XML_CIB_ATTR_JOINSTATE);
	const char *crm_state  = crm_element_value(node_state, XML_CIB_ATTR_CRMDSTATE);
	const char *ccm_state  = crm_element_value(node_state, XML_CIB_ATTR_INCCM);
	const char *ha_state   = crm_element_value(node_state, XML_CIB_ATTR_HASTATE);
	const char *exp_state  = crm_element_value(node_state, XML_CIB_ATTR_EXPSTATE);
	const char *terminate = g_hash_table_lookup(this_node->details->attrs, "terminate");

	if(ha_state == NULL) {
		ha_state = DEADSTATUS;
	}
	
	if(crm_is_true(terminate)) {
	    do_terminate = TRUE;

	} else if(terminate != NULL && strlen(terminate) > 0) {
	    /* could be a time() value */
	    char t = terminate[0];
	    if(t != '0' && isdigit(t)) {
		do_terminate = TRUE;
	    }
	}
	
	if(crm_is_true(ccm_state)
	   && safe_str_eq(ha_state, ACTIVESTATUS)
	   && safe_str_eq(crm_state, ONLINESTATUS)) {

	    if(safe_str_eq(join_state, CRMD_JOINSTATE_MEMBER)) {
		online = TRUE;
		if(do_terminate) {
		    /* 対象ノードをunclean状態に設定する */
		    pe_fence_node(data_set, this_node, "because termination was requested");
		}

	    } else if(join_state == exp_state /* == NULL */) {
		crm_info("Node %s is coming up", this_node->details->uname);
		crm_debug("\tha_state=%s, ccm_state=%s,"
			  " crm_state=%s, join_state=%s, expected=%s",
			  crm_str(ha_state), crm_str(ccm_state),
			  crm_str(crm_state), crm_str(join_state),
			  crm_str(exp_state));

	    } else if(safe_str_eq(join_state, CRMD_JOINSTATE_PENDING)) {
		crm_info("Node %s is not ready to run resources",
			 this_node->details->uname);
		this_node->details->standby = TRUE;
		this_node->details->pending = TRUE;
		online = TRUE;
		
	    } else if(safe_str_eq(join_state, CRMD_JOINSTATE_NACK)) {
		crm_warn("Node %s is not part of the cluster",
			 this_node->details->uname);
		this_node->details->standby = TRUE;
		this_node->details->pending = TRUE;
		online = TRUE;
		
	    } else if(safe_str_eq(join_state, exp_state)) {
		crm_info("Node %s is still coming up: %s",
			 this_node->details->uname, join_state);
		crm_info("\tha_state=%s, ccm_state=%s, crm_state=%s",
			 crm_str(ha_state), crm_str(ccm_state), crm_str(crm_state));
		this_node->details->standby = TRUE;
		this_node->details->pending = TRUE;
		online = TRUE;
		
	    } else {
		crm_warn("Node %s (%s) is un-expectedly down",
			 this_node->details->uname, this_node->details->id);
		crm_info("\tha_state=%s, ccm_state=%s,"
			 " crm_state=%s, join_state=%s, expected=%s",
			 crm_str(ha_state), crm_str(ccm_state),
			 crm_str(crm_state), crm_str(join_state),
			 crm_str(exp_state));
		/* 対象ノードをunclean状態に設定する */
		pe_fence_node(data_set, this_node, "because it is un-expectedly down");
	    }
		
	} else if(crm_is_true(ccm_state) == FALSE
 		  && safe_str_eq(ha_state, DEADSTATUS)
		  && safe_str_eq(crm_state, OFFLINESTATUS)
		  && this_node->details->expected_up == FALSE) {
		crm_debug("Node %s is down: join_state=%s, expected=%s",
			  this_node->details->uname,
			  crm_str(join_state), crm_str(exp_state));

#if 0
		/* While a nice optimization, it causes the cluster to block until the node
		 *  comes back online.  Which is a serious problem if the cluster software
		 *  is not configured to start at boot or stonith is configured to merely
		 *  stop the node instead of restart it.
		 * Easily triggered by setting terminate=true for the DC
		 */
	} else if(do_terminate) {
	    crm_info("Node %s is %s after forced termination",
		     this_node->details->uname, crm_is_true(ccm_state)?"coming up":"going down");
	    crm_debug("\tha_state=%s, ccm_state=%s,"
		      " crm_state=%s, join_state=%s, expected=%s",
		      crm_str(ha_state), crm_str(ccm_state),
		      crm_str(crm_state), crm_str(join_state),
		      crm_str(exp_state));
	    
	    if(crm_is_true(ccm_state) == FALSE) {
		this_node->details->standby = TRUE;
		this_node->details->pending = TRUE;
		online = TRUE;
	    }
#endif
	    
	} else if(this_node->details->expected_up) {
		/* mark it unclean */
		/* 対象ノードをunclean状態に設定する */
		pe_fence_node(data_set, this_node, "because it is un-expectedly down");
		crm_info("\tha_state=%s, ccm_state=%s,"
			 " crm_state=%s, join_state=%s, expected=%s",
			 crm_str(ha_state), crm_str(ccm_state),
			 crm_str(crm_state), crm_str(join_state),
			 crm_str(exp_state));

	} else {
		crm_info("Node %s is down", this_node->details->uname);
		crm_debug("\tha_state=%s, ccm_state=%s,"
			  " crm_state=%s, join_state=%s, expected=%s",
			  crm_str(ha_state), crm_str(ccm_state),
			  crm_str(crm_state), crm_str(join_state),
			  crm_str(exp_state));
	}
	return online;
}
/* ノードの状態を決定する */
gboolean
determine_online_status(
	xmlNode * node_state, node_t *this_node, pe_working_set_t *data_set)
{
	gboolean online = FALSE;
	const char *shutdown = NULL;
	const char *exp_state = crm_element_value(node_state, XML_CIB_ATTR_EXPSTATE);
	
	if(this_node == NULL) {
		crm_config_err("No node to check");
		return online;
	}

	this_node->details->shutdown = FALSE;
	this_node->details->expected_up = FALSE;
	shutdown = g_hash_table_lookup(this_node->details->attrs, XML_CIB_ATTR_SHUTDOWN);

	if(shutdown != NULL && safe_str_neq("0", shutdown)) {
	    this_node->details->shutdown = TRUE;

	} else if(safe_str_eq(exp_state, CRMD_JOINSTATE_MEMBER)) {
	    this_node->details->expected_up = TRUE;
	}
	
	if(is_set(data_set->flags, pe_flag_stonith_enabled) == FALSE) {
		/* stonith有りの場合 */
		online = determine_online_status_no_fencing(
		    data_set, node_state, this_node);
		
	} else {
		/* stonith無しの場合 */
		online = determine_online_status_fencing(
		    data_set, node_state, this_node);
	}
	
	if(online) {
		this_node->details->online = TRUE;

	} else {
		/* remove node from contention */
		this_node->fixed = TRUE;
		this_node->weight = -INFINITY;
	}

	if(online && this_node->details->shutdown) {
		/* dont run resources here */
		this_node->fixed = TRUE;
		this_node->weight = -INFINITY;
	}	

	if(this_node->details->unclean) {
		pe_proc_warn("Node %s is unclean", this_node->details->uname);

	} else if(this_node->details->online) {
	    crm_info("Node %s is %s", this_node->details->uname,
		     this_node->details->shutdown?"shutting down":
		     this_node->details->pending?"pending":
		     this_node->details->standby?"standby":"online");
	    
	} else {
		crm_debug_2("Node %s is offline", this_node->details->uname);
	}

	return online;
}

#define set_char(x) last_rsc_id[lpc] = x; complete = TRUE;

char *
clone_zero(const char *last_rsc_id)
{
    int lpc = 0;
    char *zero = NULL;

    CRM_CHECK(last_rsc_id != NULL, return NULL);
    if(last_rsc_id != NULL) {
	lpc = strlen(last_rsc_id);
    }
    
    while(--lpc > 0) {
	switch(last_rsc_id[lpc]) {
	    case 0:
		return NULL;
		break;
	    case '0':
	    case '1':
	    case '2':
	    case '3':
	    case '4':
	    case '5':
	    case '6':
	    case '7':
	    case '8':
	    case '9':
		break;
	    case ':':
		crm_malloc0(zero, lpc + 3);
		memcpy(zero, last_rsc_id, lpc);		
		zero[lpc] = ':';
		zero[lpc+1] = '0';
		zero[lpc+2] = 0;
		return zero;
	}
    }
    return NULL;
}

char *
increment_clone(char *last_rsc_id)
{
	int lpc = 0;
	int len = 0;
	char *tmp = NULL;
	gboolean complete = FALSE;

	CRM_CHECK(last_rsc_id != NULL, return NULL);
	if(last_rsc_id != NULL) {
		len = strlen(last_rsc_id);
	}
	
	lpc = len-1;
	while(complete == FALSE && lpc > 0) {
		switch (last_rsc_id[lpc]) {
			case 0:
				lpc--;
				break;
			case '0':
				set_char('1');
				break;
			case '1':
				set_char('2');
				break;
			case '2':
				set_char('3');
				break;
			case '3':
				set_char('4');
				break;
			case '4':
				set_char('5');
				break;
			case '5':
				set_char('6');
				break;
			case '6':
				set_char('7');
				break;
			case '7':
				set_char('8');
				break;
			case '8':
				set_char('9');
				break;
			case '9':
				last_rsc_id[lpc] = '0';
				lpc--;
				break;
			case ':':
				tmp = last_rsc_id;
				crm_malloc0(last_rsc_id, len + 2);
				memcpy(last_rsc_id, tmp, len);
				last_rsc_id[++lpc] = '1';
				last_rsc_id[len] = '0';
				last_rsc_id[len+1] = 0;
				complete = TRUE;
				crm_free(tmp);
				break;
			default:
				crm_err("Unexpected char: %c (%d)",
					last_rsc_id[lpc], lpc);
				return NULL;
				break;
		}
	}
	return last_rsc_id;
}


static int
get_clone(char *last_rsc_id)
{
    int clone = 0;
    int lpc = 0;
    int len = 0;

    CRM_CHECK(last_rsc_id != NULL, return -1);
    if(last_rsc_id != NULL) {
	len = strlen(last_rsc_id);
    }
	
    lpc = len-1;
    while(lpc > 0) {
	switch (last_rsc_id[lpc]) {
	    case '0':
	    case '1':
	    case '2':
	    case '3':
	    case '4':
	    case '5':
	    case '6':
	    case '7':
	    case '8':
	    case '9':
		clone += (int)(last_rsc_id[lpc] - '0') * (len - lpc);
		lpc--;
		break;
	    case ':':
		return clone;
		break;
	    default:
		crm_err("Unexpected char: %d (%c)",
			lpc, last_rsc_id[lpc]);
		return clone;
		break;
	}
    }
    return -1;
}
/* 擬似リソースの作成処理 */
static resource_t *
create_fake_resource(const char *rsc_id, xmlNode *rsc_entry, pe_working_set_t *data_set) 
{
	resource_t *rsc = NULL;
	/* 擬似リソース用に"primitive"ノードを作成する */
	xmlNode *xml_rsc  = create_xml_node(NULL, XML_CIB_TAG_RESOURCE);
	copy_in_properties(xml_rsc, rsc_entry);
	crm_xml_add(xml_rsc, XML_ATTR_ID, rsc_id);
	crm_log_xml_debug(xml_rsc, "Orphan resource");
	/* 共通展開処理を実行する */
	common_unpack(xml_rsc, &rsc, NULL, data_set);
	/* 孤立リソースフラグをセットする */
	set_bit(rsc->flags, pe_rsc_orphan);
	/* 作成した孤立リソースをdata_setのリソース情報リストに追加する */
	data_set->resources = g_list_append(data_set->resources, rsc);
	return rsc;
}

extern resource_t *create_child_clone(resource_t *rsc, int sub_id, pe_working_set_t *data_set);
/* クローンリソースの検索処理 */
static resource_t *find_clone(pe_working_set_t *data_set, node_t *node, resource_t *parent, const char *rsc_id) 
{
    int len = 0;
    resource_t *rsc = NULL;
    char *base = clone_zero(rsc_id);
    char *alt_rsc_id = NULL;

    CRM_ASSERT(parent != NULL);
    CRM_ASSERT(parent->variant == pe_clone || parent->variant == pe_master);

    if(base) {
	len = strlen(base);
    }
    if(len > 0) {
	base[len-1] = 0;
    }
    
    crm_debug_3("Looking for %s on %s in %s %d",
		rsc_id, node->details->uname, parent->id, is_set(parent->flags, pe_rsc_unique));
    
    if(is_set(parent->flags, pe_rsc_unique)) {
	crm_debug_3("Looking for %s", rsc_id);
	rsc = parent->fns->find_rsc(parent, rsc_id, NULL, pe_find_current);

    } else {
	crm_debug_3("Looking for %s on %s", base, node->details->uname);
	rsc = parent->fns->find_rsc(parent, base, node, pe_find_partial|pe_find_current);
	if(rsc != NULL && rsc->running_on) {
	    rsc = NULL;
	    crm_debug_3("Looking for an existing orphan for %s: %s on %s", parent->id, rsc_id, node->details->uname);

	    /* There is already an instance of this _anonymous_ clone active on "node".
	     *
	     * If there is a partially active orphan (only applies to clone groups) on
	     * the same node, use that.
	     * Otherwise create a new (orphaned) instance at "orphan_check:".
	    */
	    slist_iter(child, resource_t, parent->children, lpc,
			node_t *loc = child->fns->location(child, NULL, TRUE);
			if(loc && loc->details == node->details) {
				resource_t *tmp = child->fns->find_rsc(child, base, NULL, pe_find_partial|pe_find_current);
				if(tmp && tmp->running_on == NULL) {
					rsc = tmp;
					break;
				}
			}
			);
	    
	    goto orphan_check;

	} else if(((resource_t*)parent->children->data)->variant == pe_group) {
	    /* If we're grouped, we need to look for a peer thats active on $node
	     * and use their clone instance number
	     */
	    resource_t *peer = parent->fns->find_rsc(parent, NULL, node, pe_find_clone|pe_find_current);
	    if(peer && peer->running_on) {
		char buffer[256];
		int clone_num = get_clone(peer->id);

		snprintf(buffer, 256, "%s%d", base, clone_num);
		rsc = parent->fns->find_rsc(parent, buffer, node, pe_find_current|pe_find_inactive);
		if(rsc) {
		    crm_debug_3("Found someone active: %s on %s, becoming %s", peer->id, ((node_t*)peer->running_on->data)->details->uname, buffer);
		}
	    }	    
	}
	
        if(parent->fns->find_rsc(parent, rsc_id, NULL, pe_find_current)) {
            alt_rsc_id = crm_strdup(rsc_id);
        } else {
            alt_rsc_id = clone_zero(rsc_id);
        }

	while(rsc == NULL) {
	    rsc = parent->fns->find_rsc(parent, alt_rsc_id, NULL, pe_find_current);
	    if(rsc == NULL) {
		crm_debug_3("Unknown resource: %s", alt_rsc_id);
		break;
	    }
		
	    if(rsc->running_on == NULL) {
		crm_debug_3("Resource %s: just right", alt_rsc_id);
		break;
	    }

	    crm_debug_3("Resource %s: already active", alt_rsc_id);
	    alt_rsc_id = increment_clone(alt_rsc_id);
	    rsc = NULL;
	}
    }

  orphan_check:
    if(rsc == NULL) {
	/* Create an extra orphan */
	resource_t *top = create_child_clone(parent, -1, data_set);
	crm_debug("Created orphan for %s: %s on %s", parent->id, rsc_id, node->details->uname);
	rsc = top->fns->find_rsc(top, base, NULL, pe_find_current|pe_find_partial);
	CRM_ASSERT(rsc != NULL);
    }

    crm_free(rsc->clone_name); rsc->clone_name = NULL;
    if(safe_str_neq(rsc_id, rsc->id)) {
	crm_info("Internally renamed %s on %s to %s%s",
		 rsc_id, node->details->uname, rsc->id,
		 is_set(rsc->flags, pe_rsc_orphan)?" (ORPHAN)":"");
	rsc->clone_name = crm_strdup(rsc_id);
    }
    
    crm_free(alt_rsc_id);
    crm_free(base);
    return rsc;
}
/* 対象リソースがresourcesノードに存在し、data_setに展開されているかどうかの検索 */
static resource_t *
unpack_find_resource(
	pe_working_set_t *data_set, node_t *node, const char *rsc_id, xmlNode *rsc_entry)
{
	resource_t *rsc = NULL;
	resource_t *clone_parent = NULL;
	char *alt_rsc_id = crm_strdup(rsc_id);
	
	crm_debug_2("looking for %s", rsc_id);
	/* data_set->resourcesリストから対象リソースを検索する */
	rsc = pe_find_resource(data_set->resources, alt_rsc_id);
	/* no match */
	if(rsc == NULL) {
	    /* リソースがdata_setのリソース情報利リストに存在しない場合 */
	    /* Even when clone-max=0, we still create a single :0 orphan to match against */
	    char *tmp = clone_zero(alt_rsc_id);
	    resource_t *clone0 = pe_find_resource(data_set->resources, tmp);
	    clone_parent = uber_parent(clone0);
	    crm_free(tmp);
	    
	    crm_debug_2("%s not found: %s", alt_rsc_id, clone_parent?clone_parent->id:"orphan");

	} else {
		/* リソースが存在する場合は、リソースの親リソースを検索 */
	    clone_parent = uber_parent(rsc);
	}

	if(clone_parent && clone_parent->variant > pe_group) {
		/* 親リソースが存在して、親リソースがmasterかcloneの場合は、クローンリソースを検索する */
	    rsc = find_clone(data_set, node, clone_parent, rsc_id);
	    CRM_ASSERT(rsc != NULL);
	}
	
	crm_free(alt_rsc_id);
	return rsc;	/* 検索結果を返す */
}
/* 孤立(orphan)リソースの処理 */
static resource_t *
process_orphan_resource(xmlNode *rsc_entry, node_t *node, pe_working_set_t *data_set) 
{
	resource_t *rsc = NULL;
	const char *rsc_id   = crm_element_value(rsc_entry, XML_ATTR_ID);
	/* 孤立リソース検知のデバックログを出力する */
	crm_debug("Detected orphan resource %s on %s", rsc_id, node->details->uname);	
	/* 擬似リソースを作成する */
	rsc = create_fake_resource(rsc_id, rsc_entry, data_set);
	
	if(is_set(data_set->flags, pe_flag_stop_rsc_orphans) == FALSE) {
		/* stop-orphan-resourcesオプション(pe_flag_stop_rsc_orphansフラグ)がFALSEの場合は */
	    /* 作成した擬似リソースのpe_rsc_managedフラグをクリアする */
	    clear_bit(rsc->flags, pe_rsc_managed);
		
	} else {
		print_resource(LOG_DEBUG_3, "Added orphan", rsc, FALSE);
			
		CRM_CHECK(rsc != NULL, return NULL);
		/* 作成した擬似リソースをクラスタ内では配置不可としてlocation情報を追加する */
		resource_location(rsc, NULL, -INFINITY, "__orphan_dont_run__", data_set);
	}
	return rsc;
}
/* 操作状態からリソース状態をセットする */
static void
process_rsc_state(resource_t *rsc, node_t *node,
		  enum action_fail_response on_fail,
		  xmlNode *migrate_op,
		  pe_working_set_t *data_set) 
{
	if(on_fail == action_migrate_failure) {
	    /* 最終故障がaction_migrate_failureだった場合 */
	    node_t *from = NULL;
	    const char *uuid = crm_element_value(migrate_op, CRMD_ACTION_MIGRATED);
	    
	    on_fail = action_fail_recover;
	    
	    from = pe_find_node_id(data_set->nodes, uuid);
	    if(from != NULL) {
		process_rsc_state(rsc, from, on_fail, NULL, data_set);
	    } else {
		crm_log_xml_err(migrate_op, "Bad Op");
	    }
	}
	
	crm_debug_2("Resource %s is %s on %s: on_fail=%s",
		    rsc->id, role2text(rsc->role),
		node->details->uname, fail2text(on_fail));

	/* process current state */
	if(rsc->role != RSC_ROLE_UNKNOWN) { 
		/* roleがUNKNOWNでもない場合には、known_onリストにノード情報を追加する */
		rsc->known_on = g_list_append(rsc->known_on, node);
	}

	if(node->details->unclean) {
	    /* No extra processing needed
	     * Also allows resources to be started again after a node is shot
	     */
	    on_fail = action_fail_ignore;
	}
	/* 最終故障を判定する(故障していない場合は、処理されない) */
	switch(on_fail) {
	    case action_fail_ignore:
		/* nothing to do */
		break;
		
	    case action_fail_fence:
		/* treat it as if it is still running
		 * but also mark the node as unclean
		 */
		/* action_fail_fenceの場合は、対象ノードをunclean状態に設定する処理 */
		pe_fence_node(data_set, node, "to recover from resource failure(s)");
		break;
		
	    case action_fail_standby:
		/* action_fail_standbyの場合は、ノード情報のstandby,standby_onfailをTRUEにセットする */
		node->details->standby = TRUE;
		node->details->standby_onfail = TRUE;
		break;
		    
	    case action_fail_block:
		/* is_managed == FALSE will prevent any
		 * actions being sent for the resource
		 */
		/* action_fail_blockの場合は、pe_rsc_managedフラグをクリアしてunmanaged状態にセットする */
		clear_bit(rsc->flags, pe_rsc_managed);
		break;
		
	    case action_fail_migrate:
		/* make sure it comes up somewhere else
		 * or not at all
		 */
		/* action_fail_migrateの場合は、リソースの移動を行う為に対象ノードに配置不可(-INFINITY)のlocation情報をセットする */
		resource_location(rsc, node, -INFINITY,
				  "__action_migration_auto__",data_set);
		break;
		
	    case action_fail_stop:
	    /* 	action_fail_stopの場合は、対象リソースのnext_roleをSTOPPEDにセットする */
		rsc->next_role = RSC_ROLE_STOPPED;
		break;
		
	    case action_fail_recover:
		/* action_fail_recoverの場合 */
		if(rsc->role != RSC_ROLE_STOPPED && rsc->role != RSC_ROLE_UNKNOWN) { 
		    /* リソースの現在のroleがSTOPPED,UNKNOWN以外(起動している)は、pe_rsc_failedフラグをセットして、stopアクションをセット」する */
		    set_bit(rsc->flags, pe_rsc_failed);
		    stop_action(rsc, node, FALSE);
		}
		break;
		
	    case action_migrate_failure:
		/* anything extra? */
		break;
	}
	
	if(rsc->role != RSC_ROLE_STOPPED && rsc->role != RSC_ROLE_UNKNOWN) {
		/* --- 起動中のリソースの処理 --- */
		/* roleがSTOPPPEDでもなくて、UNKNOWNでもない場合は、起動している */
		if(is_set(rsc->flags, pe_rsc_orphan)) {
		    if(is_set(rsc->flags, pe_rsc_managed)) {
			crm_config_warn("Detected active orphan %s running on %s",
					rsc->id, node->details->uname);
		    } else {
			crm_config_warn("Cluster configured not to stop active orphans."
					" %s must be stopped manually on %s",
					rsc->id, node->details->uname);
		    }
		}
		/* --- 起動中のnativeリソース情報をセットする --- */
	    /* 起動しているので、リソースのrunnning_onリストにノード情報を追加する */
		/* ノードのrunning_rscリストにもリソース情報を追加する */
		/* unmanagedリソースの場合は、ノードを固定(INFINITY)する */
		native_add_running(rsc, node, data_set);
		if(on_fail != action_fail_ignore) {
			/* pe_rsc_failedフラグをセット */
		    set_bit(rsc->flags, pe_rsc_failed);
		}
			
	} else if(rsc->clone_name) {
		/* 起動していないリソースの場合で、clone_nameがセットされている場合は解放する */
		crm_debug_2("Resetting clone_name %s for %s (stopped)",
			    rsc->clone_name, rsc->id);
		crm_free(rsc->clone_name);
		rsc->clone_name = NULL;

	} else {
		char *key = stop_key(rsc);
		GListPtr possible_matches = find_actions(rsc->actions, key, node);
		slist_iter(stop, action_t, possible_matches, lpc,
			   stop->optional = TRUE;
			);
		crm_free(key);
	}
}

/* create active recurring operations as optional */ 
static void
process_recurring(node_t *node, resource_t *rsc,
		  int start_index, int stop_index,
		  GListPtr sorted_op_list, pe_working_set_t *data_set)
{
	const char *task = NULL;
	const char *status = NULL;
	
	crm_debug_3("%s: Start index %d, stop index = %d",
		    rsc->id, start_index, stop_index);
	/* ソート済みのlrm_rsc_opノードのリストを全て処理する */
	slist_iter(rsc_op, xmlNode, sorted_op_list, lpc,
		   int interval = 0;
		   char *key = NULL;
		   const char *id = ID(rsc_op);
		   const char *interval_s = NULL;
		   if(node->details->online == FALSE) {
			   crm_debug_4("Skipping %s/%s: node is offline",
				       rsc->id, node->details->uname);
				/* rm_rsc_opノード情報のノードが既にOFFLINEノードの場合も処理せずに抜ける */
			   break;
			   
		   } else if(start_index < stop_index) {
				/* 計算したstart操作とstop操作の関係で、stop操作が完了している場合は、処理せずに抜ける */
				/* -- monitor処理はstop完了後なので実行されていないと判断する -- */
			   crm_debug_4("Skipping %s/%s: not active",
				       rsc->id, node->details->uname);
			   break;
			   
		   } else if(lpc < start_index) {
				/* 計算したstart操作よりも前の操作は処理しないで抜ける */
			   crm_debug_4("Skipping %s/%s: old %d",
				       id, node->details->uname, lpc);
			   continue;
		   }
		   /* "interval"属性を取り出し、数値化する */	
		   interval_s = crm_element_value(rsc_op,XML_LRM_ATTR_INTERVAL);
		   interval = crm_parse_int(interval_s, "0");
		   if(interval == 0) {
			   /* "inteval"属性値が0の場合は、probeなので処理しない */
			   crm_debug_4("Skipping %s/%s: non-recurring",
				       id, node->details->uname);
			   continue;
		   }
		   /* "op-status"属性を取り出す */
		   status = crm_element_value(rsc_op, XML_LRM_ATTR_OPSTATUS);
		   if(safe_str_eq(status, "-1")) {
				/* "op-status"属性が-1の場合は処理しない */
			   crm_debug_4("Skipping %s/%s: status",
				       id, node->details->uname);
			   continue;
		   }
		   /* "operation"属性を取り出して、キーを生成する */
		   task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
		   /* create the action */
		   key = generate_op_key(rsc->id, task, interval);
		   crm_debug_3("Creating %s/%s", key, node->details->uname);
		   /* monitorなどでinterval実行されているアクションを生成する */
		   custom_action(rsc, key, task, node, TRUE, TRUE, data_set);
		);
}

void
calculate_active_ops(GListPtr sorted_op_list, int *start_index, int *stop_index) 
{
	int implied_monitor_start = -1;
	int implied_master_start = -1;
	const char *task = NULL;
	const char *status = NULL;

	*stop_index = -1;
	*start_index = -1;
	
	slist_iter(
		rsc_op, xmlNode, sorted_op_list, lpc,

		task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
		status = crm_element_value(rsc_op, XML_LRM_ATTR_OPSTATUS);

		if(safe_str_eq(task, CRMD_ACTION_STOP)
		   && safe_str_eq(status, "0")) {
			*stop_index = lpc;
			
		} else if(safe_str_eq(task, CRMD_ACTION_START)) {
			*start_index = lpc;
			
		} else if((implied_monitor_start <= *stop_index)
			  && safe_str_eq(task, CRMD_ACTION_STATUS)) {
			const char *rc = crm_element_value(rsc_op, XML_LRM_ATTR_RC);
			if(safe_str_eq(rc, "0") || safe_str_eq(rc, "8")) {
				implied_monitor_start = lpc;
			}
		} else if (safe_str_eq(task, CRMD_ACTION_PROMOTE)
			   || safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
			implied_master_start = lpc;
		}
		);

	if (*start_index == -1) {
		if (implied_master_start != -1) {
			*start_index = implied_master_start;
		} else if (implied_monitor_start != -1) {
			*start_index = implied_monitor_start;
		}
	}
}

/* lrm_resourceノード展開 */
static void
unpack_lrm_rsc_state(
	node_t *node, xmlNode * rsc_entry, pe_working_set_t *data_set)
{	
	int stop_index = -1;
	int start_index = -1;
	enum rsc_role_e req_role = RSC_ROLE_UNKNOWN;
	/* lrm_resourceノードのid属性から対象リソースidをセットする */
	const char *task = NULL;
	const char *rsc_id  = crm_element_value(rsc_entry, XML_ATTR_ID);

	resource_t *rsc = NULL;
	GListPtr op_list = NULL;
	GListPtr sorted_op_list = NULL;

	xmlNode *migrate_op = NULL;
	
	enum action_fail_response on_fail = FALSE;
	enum rsc_role_e saved_role = RSC_ROLE_UNKNOWN;
	
	crm_debug_3("[%s] Processing %s on %s",
		    crm_element_name(rsc_entry), rsc_id, node->details->uname);

	/* extract operations */
	op_list = NULL;
	sorted_op_list = NULL;
	/* 対象のlrm_resourceノード内の全てのlrm_rsc_opノードを処理する */
	xml_child_iter_filter(
		rsc_entry, rsc_op, XML_LRM_TAG_RSC_OP,
		/* op_listに１つのlrm_rsc_opノードを追加する */
		op_list = g_list_append(op_list, rsc_op);
		);

	if(op_list == NULL) {
		/* op_listが存在しない場合は処理を抜ける */
		/* if there are no operations, there is nothing to do */
		return;
	}

	/* find the resource */
	/* 対象リソースがresourcesノードに存在し、data_setに展開されているかどうかの検索する */
	rsc = unpack_find_resource(data_set, node, rsc_id, rsc_entry);
	if(rsc == NULL) {
		/* data_setのresourcesノードに存在しない場合は、孤立リソースとして処理する */
		/* -- 擬似リソースを作成して、resourcesリストに追加 -- */
		rsc = process_orphan_resource(rsc_entry, node, data_set);
	} 
	CRM_ASSERT(rsc != NULL);
	
	/* process operations */
	/* 現在のリソース情報のroleをセーブする */
	saved_role = rsc->role;
	/* on_failをaction_fail_ignoreで初期化する */
	on_fail = action_fail_ignore;
	/* 現在のリソース情報のroleをRSC_ROLE_UNKNOWNにセットする */
	rsc->role = RSC_ROLE_UNKNOWN;
	/* op_list(lrm_rsc_opノードの)リストを時系列でソートする */
	sorted_op_list = g_list_sort(op_list, sort_op_by_callid);
	/* ソートしたlrm_rsc_opノードのリストを全て処理する */
	/* -- 全ての時系列ソート済みのlrm_rsc_opノードを処理してリソース操作を展開する -- */
	slist_iter(
		rsc_op, xmlNode, sorted_op_list, lpc,
		/* lrm_rsc_opノードのoperation属性を取り出す */
		task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
		if(safe_str_eq(task, CRMD_ACTION_MIGRATED)) {
			migrate_op = rsc_op;
		}
		/* 1つのlrm_rsc_opを展開する(結果として全てのlrm_rsc_opが処理される) */
		/* ※リソースの起動状態などもこの展開で設定される */
		/* ※on_failには最後のlrm_rsc_opの値が設定されている */
		/* ※現在のロール(rsc->role)も設定されている */
		unpack_rsc_op(rsc, node, rsc_op, &on_fail, data_set);
		);

	/* create active recurring operations as optional */ 
	/* lrm_rsc_opタグのソート済みのリストから、start,stop操作のインデックスを取得する */
	calculate_active_ops(sorted_op_list, &start_index, &stop_index);
	/* 実行されているmonitor処理をアクション情報に追加する */
	process_recurring(node, rsc, start_index, stop_index,
			  sorted_op_list, data_set);
	
	/* no need to free the contents */
	g_list_free(sorted_op_list);
	/* lrm_resource内の最後のlrm_rsc_opの実行操作のon-failから、そのリソースの状態 */
	/*（次のロールや、エラーによってのリソースの配置不可のスコア(-INIFINITY)など)をセットする */
	/* 起動済みと判定されたリソースはリソースのrunnning_onリストにノード情報を追加する */
	process_rsc_state(rsc, node, on_fail, migrate_op, data_set);

	if(get_target_role(rsc, &req_role)) {
		/* 対象リソースのmetaハッシュテーブルから取得したtarget-roleの値がRSC_ROLE_SLAVEか、RSC_ROLE_MASTERで */
		/* かつ、pe_masterか、statefulがTRUEの場合 */
		if(rsc->next_role == RSC_ROLE_UNKNOWN || req_role < rsc->next_role) {
		crm_debug("%s: Overwriting calculated next role %s"
			  " with requested next role %s",
			  rsc->id, role2text(rsc->next_role),
			  role2text(req_role));
			/* next_roleがRSC_ROLE_UNKNOWNか、req_roleがnext_roleより小さい場合 */
			/* next_roleにreq_roleをセットする */
		rsc->next_role = req_role;

	    } else if(req_role > rsc->next_role) {
		crm_info("%s: Not overwriting calculated next role %s"
			 " with requested next role %s",
			 rsc->id, role2text(rsc->next_role),
			 role2text(req_role));
	    }
	}
		
	if(saved_role > rsc->role) {
		/* saved_roleがrsc->roleより大きい場合は、saved_roleを戻す */
		/* ※roleは次の重みとなっている */
		/*	enum rsc_role_e {
				RSC_ROLE_UNKNOWN, 	0
				RSC_ROLE_STOPPED, 	1
				RSC_ROLE_STARTED, 	2
				RSC_ROLE_SLAVE, 	3
				RSC_ROLE_MASTER, 	4
			};
		*/
		rsc->role = saved_role;
	}
}
/* "lrm_resources"ノードの情報展開 */
gboolean
unpack_lrm_resources(node_t *node, xmlNode * lrm_rsc_list, pe_working_set_t *data_set)
{
	CRM_CHECK(node != NULL, return FALSE);

	crm_debug_3("Unpacking resources on %s", node->details->uname);
	/* "lrm_resources"ノード内の全てのlrm_resourceノードを処理する */
	xml_child_iter_filter(
		lrm_rsc_list, rsc_entry, XML_LRM_TAG_RESOURCE,
		/* １つのlrm_resourceノードを展開する */
		unpack_lrm_rsc_state(node, rsc_entry, data_set);
		);
	
	return TRUE;
}

static void set_active(resource_t *rsc) 
{
    resource_t *top = uber_parent(rsc);
    if(top && top->variant == pe_master) {
		/* Master/Slaveリソースの場合は、対象リソースのroleをSLAVEにセット */
	rsc->role = RSC_ROLE_SLAVE;
    } else {
		/* その他のリソースの場合は、対象リソースのroleをSTARTEDにセット */
	rsc->role = RSC_ROLE_STARTED;
    }
}
/* lrm_rsc_opノードを展開 */
gboolean
unpack_rsc_op(resource_t *rsc, node_t *node, xmlNode *xml_op,
	      enum action_fail_response *on_fail, pe_working_set_t *data_set) 
{    
	const char *id          = NULL;
	const char *key        = NULL;
	const char *task        = NULL;
	const char *magic       = NULL;
 	const char *task_id     = NULL;
 	const char *actual_rc   = NULL;
/* 	const char *target_rc   = NULL;	 */
	const char *task_status = NULL;
	const char *interval_s  = NULL;
	const char *op_digest   = NULL;
	const char *op_version  = NULL;

	int interval = 0;
	int task_status_i = -2;
	int actual_rc_i = 0;
	int target_rc = -1;
	
	action_t *action = NULL;
	node_t *effective_node = NULL;
	resource_t *failed = NULL;

	gboolean expired = FALSE;
	gboolean is_probe = FALSE;
	gboolean clear_past_failure = FALSE;
	
	CRM_CHECK(rsc    != NULL, return FALSE);
	CRM_CHECK(node   != NULL, return FALSE);
	CRM_CHECK(xml_op != NULL, return FALSE);

	id	    = ID(xml_op);
	task        = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
 	task_id     = crm_element_value(xml_op, XML_LRM_ATTR_CALLID);
	task_status = crm_element_value(xml_op, XML_LRM_ATTR_OPSTATUS);
	op_digest   = crm_element_value(xml_op, XML_LRM_ATTR_OP_DIGEST);
	op_version  = crm_element_value(xml_op, XML_ATTR_CRM_VERSION);
	magic	    = crm_element_value(xml_op, XML_ATTR_TRANSITION_MAGIC);
	key	    = crm_element_value(xml_op, XML_ATTR_TRANSITION_KEY);

	CRM_CHECK(id != NULL, return FALSE);
	CRM_CHECK(task != NULL, return FALSE);
	CRM_CHECK(task_status != NULL, return FALSE);

	task_status_i = crm_parse_int(task_status, NULL);

	CRM_CHECK(task_status_i <= LRM_OP_ERROR, return FALSE);
	CRM_CHECK(task_status_i >= LRM_OP_PENDING, return FALSE);

	if(safe_str_eq(task, CRMD_ACTION_NOTIFY)) {
		/* safe to ignore these */
		return TRUE;
	}

	if(rsc->failure_timeout > 0) {
	    int last_run = 0;

	if(crm_element_value_int(xml_op, "last-rc-change", &last_run) == 0) {
		time_t now = get_timet_now(data_set);
		if(now > (last_run + rsc->failure_timeout)) {
		    expired = TRUE;
		}
	    }
	}
	
	crm_debug_2("Unpacking task %s/%s (call_id=%s, status=%s) on %s (role=%s)",
		    id, task, task_id, task_status, node->details->uname,
		    role2text(rsc->role));

	interval_s = crm_element_value(xml_op, XML_LRM_ATTR_INTERVAL);
	interval = crm_parse_int(interval_s, "0");
	
	if(interval == 0 && safe_str_eq(task, CRMD_ACTION_STATUS)) {
		/* intervalが0で、taskが"monitor"の場合は、Probe済みのフラグをセットする */
		is_probe = TRUE;
	}
	
	if(node->details->unclean) {
		crm_debug_2("Node %s (where %s is running) is unclean."
			  " Further action depends on the value of the stop's on-fail attribue",
			  node->details->uname, rsc->id);
	}

	actual_rc = crm_element_value(xml_op, XML_LRM_ATTR_RC);
	CRM_CHECK(actual_rc != NULL, return FALSE);	
	actual_rc_i = crm_parse_int(actual_rc, NULL);

	if(key) {
	    int dummy = 0;
	    char *dummy_string = NULL;
	    decode_transition_key(key, &dummy_string, &dummy, &dummy, &target_rc);
	    crm_free(dummy_string);
	}
	
	if(task_status_i == LRM_OP_DONE && target_rc >= 0) {
	    if(target_rc == actual_rc_i) {
		task_status_i = LRM_OP_DONE;
		
	    } else {
		task_status_i = LRM_OP_ERROR;
		crm_debug("%s on %s returned %d (%s) instead of the expected value: %d (%s)",
			 id, node->details->uname,
			 actual_rc_i, execra_code2string(actual_rc_i),
			 target_rc, execra_code2string(target_rc));
	    }

	} else if(task_status_i == LRM_OP_ERROR) {
	    /* let us decide that */
 	    task_status_i = LRM_OP_DONE;
	}
	
	if(task_status_i == LRM_OP_NOTSUPPORTED) {
	    actual_rc_i = EXECRA_UNIMPLEMENT_FEATURE;
	}

	if(expired
	   && actual_rc_i != EXECRA_NOT_RUNNING
	   && actual_rc_i != EXECRA_RUNNING_MASTER
	   && actual_rc_i != EXECRA_OK) {
	    crm_notice("Ignoring expired failure %s (rc=%d, magic=%s) on %s",
		       id, actual_rc_i, magic, node->details->uname);
	    goto done;
	}
	

	/* we could clean this up significantly except for old LRMs and CRMs that
	 * didnt include target_rc and liked to remap status
	 */
	switch(actual_rc_i) {
	    case EXECRA_NOT_RUNNING:
		if(is_probe || target_rc == actual_rc_i) {
		    task_status_i = LRM_OP_DONE;
		    rsc->role = RSC_ROLE_STOPPED;
		    
		    /* clear any previous failure actions */
		    *on_fail = action_fail_ignore;
		    rsc->next_role = RSC_ROLE_UNKNOWN;
		    
		} else if(safe_str_neq(task, CRMD_ACTION_STOP)) {
		    task_status_i = LRM_OP_ERROR;
		}
		break;
		
	    case EXECRA_RUNNING_MASTER:
		if(is_probe) {
		    task_status_i = LRM_OP_DONE;
		    crm_notice("Operation %s found resource %s active in master mode on %s",
			     id, rsc->id, node->details->uname);

		} else if(target_rc == actual_rc_i) {
		    /* nothing to do */

		} else if(target_rc >= 0) {
		    task_status_i = LRM_OP_ERROR;

		    /* legacy code for pre-0.6.5 operations */
		} else if(safe_str_neq(task, CRMD_ACTION_STATUS)
			  || rsc->role != RSC_ROLE_MASTER) {
		    task_status_i = LRM_OP_ERROR;
		    if(rsc->role != RSC_ROLE_MASTER) {
			crm_err("%s reported %s in master mode on %s",
				id, rsc->id,
				node->details->uname);
		    }
		}
		rsc->role = RSC_ROLE_MASTER;
		break;
		
	    case EXECRA_FAILED_MASTER:
		rsc->role = RSC_ROLE_MASTER;
		task_status_i = LRM_OP_ERROR;
		break;

	    case EXECRA_UNIMPLEMENT_FEATURE:
		if(interval > 0) {
		    task_status_i = LRM_OP_NOTSUPPORTED;
		    break;
		}
		/* else: fall through */
	    case EXECRA_INSUFFICIENT_PRIV:
	    case EXECRA_NOT_INSTALLED:
	    case EXECRA_INVALID_PARAM:
		effective_node = node;
		/* fall through */
	    case EXECRA_NOT_CONFIGURED:
		failed = rsc;
		if(is_not_set(rsc->flags, pe_rsc_unique)) {
		    failed = uber_parent(failed);
		}
		
		do_crm_log(actual_rc_i==EXECRA_NOT_INSTALLED?LOG_NOTICE:LOG_ERR,
                       "Preventing %s from re-starting %s %s: operation %s failed '%s' (rc=%d)",
                       failed->id,
                       effective_node ? "on" : "anywhere in the cluster",
                       effective_node ? effective_node->details->uname : "",
                       task,
                       execra_code2string(actual_rc_i), actual_rc_i);

		resource_location(failed, effective_node, -INFINITY, "hard-error", data_set);
		if(is_probe) {
			/* treat these like stops */
			task = CRMD_ACTION_STOP;
			task_status_i = LRM_OP_DONE;
			crm_xml_add(xml_op, XML_ATTR_UNAME, node->details->uname);
			if(actual_rc_i != EXECRA_NOT_INSTALLED
			   || is_set(data_set->flags, pe_flag_symmetric_cluster)) {
			    if ((node->details->shutdown == FALSE) || (node->details->online == TRUE)) {
			        add_node_copy(data_set->failed, xml_op);
			    }
			}
		}
		break;

	    case EXECRA_OK:
		if(is_probe && target_rc == 7) {
		    task_status_i = LRM_OP_DONE;
		    crm_notice("Operation %s found resource %s active on %s",
			     id, rsc->id, node->details->uname);

		    /* legacy code for pre-0.6.5 operations */
		} else if(target_rc < 0
		   && interval > 0
		   && rsc->role == RSC_ROLE_MASTER) {
		    /* catch status ops that return 0 instead of 8 while they
		     *   are supposed to be in master mode
		     */
		    task_status_i = LRM_OP_ERROR;
		}
		
		break;
		
	    default:
		if(task_status_i == LRM_OP_DONE) {
		    crm_info("Remapping %s (rc=%d) on %s to an ERROR",
			     id, actual_rc_i, node->details->uname);
		    task_status_i = LRM_OP_ERROR;
		}
	}

	if(task_status_i == LRM_OP_ERROR
	   || task_status_i == LRM_OP_TIMEOUT
	   || task_status_i == LRM_OP_NOTSUPPORTED) {
	    /* エラー、タイムアウト、NOTSUPPPORTED 終了オペレーションの場合 */
	    action = custom_action(rsc, crm_strdup(id), task, NULL, TRUE, FALSE, data_set);
	    if(expired) {
		crm_notice("Ignoring expired failure (calculated) %s (rc=%d, magic=%s) on %s",
			   id, actual_rc_i, magic, node->details->uname);
		goto done;

	    } else if(action->on_fail == action_fail_ignore) {
			/* on-fail="ignore"野場合は、終了オペレーションをOP_DONEに変更してエラーを無視 */
		crm_warn("Remapping %s (rc=%d) on %s to DONE: ignore",
			 id, actual_rc_i, node->details->uname);
		task_status_i = LRM_OP_DONE;
	    set_bit(rsc->flags, pe_rsc_failure_ignored);
	    crm_xml_add(xml_op, XML_ATTR_UNAME, node->details->uname);
	    if ((node->details->shutdown == FALSE) || (node->details->online == TRUE)) {
			/* 無視するが、故障情報には故障操作を追加セット(crm_monなどではこの故障情報を表示) */
		add_node_copy(data_set->failed, xml_op);
	    }
	    } 
	}
	
	switch(task_status_i) {
		case LRM_OP_PENDING: /* ペンディングオペレーションの場合 */
			if(safe_str_eq(task, CRMD_ACTION_START)) {
				set_bit(rsc->flags, pe_rsc_start_pending);
				set_active(rsc);
				
			} else if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				rsc->role = RSC_ROLE_MASTER;
			}
			break;
		
		case LRM_OP_DONE:	/* 正常終了オペレーションの場合 */
			crm_debug_3("%s/%s completed on %s",
				    rsc->id, task, node->details->uname);

			if(actual_rc_i == EXECRA_NOT_RUNNING) {
				clear_past_failure = TRUE;

			} else if(safe_str_eq(task, CRMD_ACTION_START)) {
				/* startがDONEしている場合は現在のroleをSTARTEDにセット */
				rsc->role = RSC_ROLE_STARTED;
				clear_past_failure = TRUE;
				
			} else if(safe_str_eq(task, CRMD_ACTION_STOP)) {
				/* stopがDONEしている場合は現在のroleをSTOPPPEDDにセット */
				rsc->role = RSC_ROLE_STOPPED;				
				clear_past_failure = TRUE;

			} else if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
				/* promoteがDONEしている場合は現在のroleをMASTERにセット */
				rsc->role = RSC_ROLE_MASTER;
				clear_past_failure = TRUE;

			} else if(safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
				/* demoteがDONEしている場合は現在のroleをSLAVEにセット */
				/* Demote from Master does not clear an error */
				rsc->role = RSC_ROLE_SLAVE;
				
			} else if(rsc->role < RSC_ROLE_STARTED) {
				/* monitorがDONEしている場合が該当 */
				/* で、roleが	RSC_ROLE_UNKNOWN, RSC_ROLE_STOPPEDの場合(展開直後のリソースのroleはSTOPPPED) */
				/* は、リソースはACTIVE状態(SLAVEもしくはSTARTED)でセット */
				crm_debug_3("%s active on %s",
					    rsc->id, node->details->uname);
				set_active(rsc);
			}

			/* clear any previous failure actions */
			if(clear_past_failure) {
				switch(*on_fail) {
				case action_fail_block:
				case action_fail_stop:
				case action_fail_fence:
				case action_fail_migrate:
				case action_fail_standby:
					crm_debug_2("%s.%s is not cleared by a completed stop",
								rsc->id, fail2text(*on_fail));
					break;

				case action_fail_ignore:
				case action_fail_recover:
				case action_migrate_failure:
					*on_fail = action_fail_ignore;
					rsc->next_role = RSC_ROLE_UNKNOWN;
				}
			}
			break;

		case LRM_OP_ERROR:			/* エラー、タイムアウト、NOTSUPPPORTED 終了オペレーションの場合 */
		case LRM_OP_TIMEOUT:
		case LRM_OP_NOTSUPPORTED:
			crm_warn("Processing failed op %s on %s: %s (%d)",
				 id, node->details->uname,
				 execra_code2string(actual_rc_i), actual_rc_i);
			crm_xml_add(xml_op, XML_ATTR_UNAME, node->details->uname);
			if ((node->details->shutdown == FALSE) || (node->details->online == TRUE)) {
				/* 故障情報に故障操作を追加セット(crm_monなどではこの故障情報を表示) */
			    add_node_copy(data_set->failed, xml_op);
			}

			if(*on_fail < action->on_fail) {
				*on_fail = action->on_fail;
			}

			if(safe_str_eq(task, CRMD_ACTION_STOP)) {
				/* 故障リソースの配置不可(-INFINITY)内部locatio情報を追加 */
			    resource_location(
				rsc, node, -INFINITY, "__stop_fail__", data_set);
			    
			} else if(safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
			    rsc->role = RSC_ROLE_MASTER;

			} else if(safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
			    /*
			     * staying in role=master ends up putting the PE/TE into a loop
			     * setting role=slave is not dangerous because no master will be
			     * promoted until the failed resource has been fully stopped
			     */
			    crm_warn("Forcing %s to stop after a failed demote action", rsc->id);
			    rsc->next_role = RSC_ROLE_STOPPED;
			    rsc->role = RSC_ROLE_SLAVE;
				
			} else if(compare_version("2.0", op_version) > 0
				  && safe_str_eq(task, CRMD_ACTION_START)) {
				/* "crm_feature_set"が2.0までのstar故障の場合は、 */
				/* 故障リソースの配置不可(-INFINITY)内部locatio情報を追加 */
			    crm_warn("Compatibility handling for failed op %s on %s",
				     id, node->details->uname);
			    resource_location(
				rsc, node, -INFINITY, "__legacy_start__", data_set);
			}

			if(rsc->role < RSC_ROLE_STARTED) {
			/* roleがRSC_ROLE_UNKNOWN, RSC_ROLE_STOPPEDの場合(展開直後のリソースのroleはSTOPPPED) */
			/* は、リソースはACTIVE状態(SLAVEもしくはSTARTED)でセット */
			    set_active(rsc);
			}

			crm_debug_2("Resource %s: role=%s, unclean=%s, on_fail=%s, fail_role=%s",
				    rsc->id, role2text(rsc->role),
				    node->details->unclean?"true":"false",
				    fail2text(action->on_fail),
				    role2text(action->fail_role));

			if(action->fail_role != RSC_ROLE_STARTED
			   && rsc->next_role < action->fail_role) {
				rsc->next_role = action->fail_role;
			}

			if(action->fail_role == RSC_ROLE_STOPPED) {
				/* on-fail="fence"でstonithが設定されていない場合 */
				/* on-fail="stop"の場合は、リソースが全ノードでもう起動出来ないように、*/
				/* このリソースの配置可能ノード情報の全てのノード情報のweightに-INFINITYをセットする */
				crm_err("Making sure %s doesn't come up again", rsc->id);
				/* make sure it doesnt come up again */
				pe_free_shallow_adv(rsc->allowed_nodes, TRUE);
				rsc->allowed_nodes = node_list_dup(
					data_set->nodes, FALSE, FALSE);
				slist_iter(
					node, node_t, rsc->allowed_nodes, lpc,
					node->weight = -INFINITY;
					);
			}
			
			pe_free_action(action);
			action = NULL;
			break;
		case LRM_OP_CANCELLED:
			/* do nothing?? */
			pe_err("Dont know what to do for cancelled ops yet");
			break;
	}

  done:
	crm_debug_3("Resource %s after %s: role=%s",
		    rsc->id, task, role2text(rsc->role));

	pe_free_action(action);
	
	return TRUE;
}
/* ノード属性情報のハッシュテーブル(node->details->attrs)への追加処理 */
gboolean
add_node_attrs(xmlNode *xml_obj, node_t *node, gboolean overwrite, pe_working_set_t *data_set)
{
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("#"XML_ATTR_UNAME),
			    crm_strdup(node->details->uname));
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("#"XML_ATTR_ID),
			    crm_strdup(node->details->id));
	if(safe_str_eq(node->details->id, data_set->dc_uuid)) {
		data_set->dc_node = node;
		node->details->is_dc = TRUE;
		g_hash_table_insert(node->details->attrs,
				    crm_strdup("#"XML_ATTR_DC),
				    crm_strdup(XML_BOOLEAN_TRUE));
	} else {
		g_hash_table_insert(node->details->attrs,
				    crm_strdup("#"XML_ATTR_DC),
				    crm_strdup(XML_BOOLEAN_FALSE));
	}
	
	unpack_instance_attributes(
		data_set->input, xml_obj, XML_TAG_ATTR_SETS, NULL,
		node->details->attrs, NULL, overwrite, data_set->now);

	return TRUE;
}

static GListPtr
extract_operations(const char *node, const char *rsc, xmlNode *rsc_entry, gboolean active_filter)
{	
    int stop_index = -1;
    int start_index = -1;
    
    GListPtr op_list = NULL;
    GListPtr sorted_op_list = NULL;

    /* extract operations */
    op_list = NULL;
    sorted_op_list = NULL;
    
    xml_child_iter_filter(
	rsc_entry, rsc_op, XML_LRM_TAG_RSC_OP,
	crm_xml_add(rsc_op, "resource", rsc);
	crm_xml_add(rsc_op, XML_ATTR_UNAME, node);
	op_list = g_list_append(op_list, rsc_op);
	);
    
    if(op_list == NULL) {
	/* if there are no operations, there is nothing to do */
	return NULL;
    }
    
    sorted_op_list = g_list_sort(op_list, sort_op_by_callid);
    
    /* create active recurring operations as optional */ 
    if(active_filter == FALSE) {
	return sorted_op_list;
    }
    
    op_list = NULL;
    
    calculate_active_ops(sorted_op_list, &start_index, &stop_index);	
    slist_iter(rsc_op, xmlNode, sorted_op_list, lpc,
	       if(start_index < stop_index) {
		   crm_debug_4("Skipping %s: not active", ID(rsc_entry));
		   break;
		   
	       } else if(lpc < start_index) {
		   crm_debug_4("Skipping %s: old", ID(rsc_op));
		   continue;
	       }
	       op_list = g_list_append(op_list, rsc_op);
	);
    
    g_list_free(sorted_op_list);
    return op_list;
}

GListPtr find_operations(
    const char *rsc, const char *node, gboolean active_filter, pe_working_set_t *data_set) 
{
    GListPtr output = NULL;
    GListPtr intermediate = NULL;

    xmlNode *tmp = NULL;
    xmlNode *status = find_xml_node(data_set->input, XML_CIB_TAG_STATUS, TRUE);

    const char *uname = NULL;
    node_t *this_node = NULL;
    
    xml_child_iter_filter(
	status, node_state, XML_CIB_TAG_STATE,
	
	uname = crm_element_value(node_state, XML_ATTR_UNAME);
	if(node != NULL && safe_str_neq(uname, node)) {
	    continue;
	}

	this_node = pe_find_node(data_set->nodes, uname);
	CRM_CHECK(this_node != NULL, continue);
	
	determine_online_status(node_state, this_node, data_set);
	
	if(this_node->details->online || is_set(data_set->flags, pe_flag_stonith_enabled)) {
	    /* offline nodes run no resources...
	     * unless stonith is enabled in which case we need to
	     *   make sure rsc start events happen after the stonith
	     */
	    tmp = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
	    tmp = find_xml_node(tmp, XML_LRM_TAG_RESOURCES, FALSE);

	    xml_child_iter_filter(
		tmp, lrm_rsc, XML_LRM_TAG_RESOURCE,
		const char *rsc_id  = crm_element_value(lrm_rsc, XML_ATTR_ID);
		if(rsc != NULL && safe_str_neq(rsc_id, rsc)) {
		    continue;
		}

		intermediate = extract_operations(uname, rsc_id, lrm_rsc, active_filter);
		output = g_list_concat(output, intermediate);
		);
	}
	);

    return output;
}
