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

#include <crm/pengine/rules.h>
#include <crm/pengine/status.h>
#include <unpack.h>
#include <utils.h>
#include <crm/msg_xml.h>

#define VARIANT_CLONE 1
#include "./variant.h"

void clone_create_notifications(
	resource_t *rsc, action_t *action, action_t *action_complete,
	pe_working_set_t *data_set);
void force_non_unique_clone(resource_t *rsc, const char *rid, pe_working_set_t *data_set);
resource_t *create_child_clone(resource_t *rsc, int sub_id, pe_working_set_t *data_set);
/* リソース情報のpe_rsc_orphanフラグをセットする */
static void mark_as_orphan(resource_t *rsc) 
{
    /* pe_rsc_orphanフラグをセット */
    set_bit(rsc->flags, pe_rsc_orphan);
    /* 対象リソース情報の全ての子リソース情報を処理する */
    slist_iter(
	child, resource_t, rsc->children, lpc,
		/* 子リソースに対して、本処理を実行する */
	mark_as_orphan(child);
	);
}
/* リソース情報のフラグクリア処理 */
static void clear_bit_recursive(resource_t *rsc, unsigned long long flag) 
{
	/* 対象フラグをクリアする */
    clear_bit_inplace(rsc->flags, flag);
    if(rsc->children) {
	    /* 対象リソース情報の全ての子リソース情報を処理する */
	slist_iter(
	    child_rsc, resource_t, rsc->children, lpc,
			/* 子リソースに対して、本処理を実行する */
	    clear_bit_recursive(child_rsc, flag);
	    );
    }
}
/* lsbリソースがclone,masterリソースの子リソースになっている場合に、 */
/* clone_node_max, clone_max値のセット,pe_rsc_uniqueフラグのクリアを行う */
void force_non_unique_clone(resource_t *rsc, const char *rid, pe_working_set_t *data_set) 
{
    if(rsc->variant == pe_clone || rsc->variant == pe_master) {
		/* pe_cloneか、pe_masterの場合 */
	clone_variant_data_t *clone_data = NULL;
		/* クローンリソースデータを取得する */
	get_clone_variant_data(clone_data, rsc);
		/* OCF以外のリソースが指定されている場合で、globally-unique=TRUEでは警告のメッセージを出力する */
	crm_config_warn("Clones %s contains non-OCF resource %s and so "
			"can only be used as an anonymous clone. "
			"Set the "XML_RSC_ATTR_UNIQUE" meta attribute to false",
			rsc->id, rid);
		/* clone_node_maxは１で、clone_maxはノード数で強制セットする */
	clone_data->clone_node_max = 1;
	clone_data->clone_max = g_list_length(data_set->nodes);
		/* pe_rsc_uniqueフラグをクリアする */
	clear_bit_recursive(rsc, pe_rsc_unique);
    }
}
/* cloneの子リソース生成処理 */
resource_t *
create_child_clone(resource_t *rsc, int sub_id, pe_working_set_t *data_set) 
{
	gboolean as_orphan = FALSE;
	char *inc_num = NULL;
	char *inc_max = NULL;
	resource_t *child_rsc = NULL;
	xmlNode * child_copy = NULL;
	clone_variant_data_t *clone_data = NULL;
	/* clone固有データを取得する */
	get_clone_variant_data(clone_data, rsc);

	CRM_CHECK(clone_data->xml_obj_child != NULL, return FALSE);

	if(sub_id < 0) {
		/* sub_idが０未満による呼び出しの場合は、 */
		/* as_orphanフラグをTRUEにセットして、sub_idをこのクローンの生成リソース数でセットする */
	    as_orphan = TRUE;
	    sub_id = clone_data->total_clones;
	}
	inc_num = crm_itoa(sub_id);
	inc_max = crm_itoa(clone_data->clone_max);	
	/* cloneリソースの生成情報に"clone"値としてsub_idを数値化したものを追加しておく */
	child_copy = copy_xml(clone_data->xml_obj_child);

	crm_xml_add(child_copy, XML_RSC_ATTR_INCARNATION, inc_num);
	/********************************************************/
	/* common_unpack処理を実行して、cloneリソースを生成する */
	/********************************************************/
	if(common_unpack(child_copy, &child_rsc,
			 rsc, data_set) == FALSE) {
		pe_err("Failed unpacking resource %s",
		       crm_element_value(child_copy, XML_ATTR_ID));
		child_rsc = NULL;
		goto bail;
	}
/* 	child_rsc->globally_unique = rsc->globally_unique; */
	/* 生成したクローンリソースをカウントアップ */
	clone_data->total_clones += 1;
	crm_debug_2("Setting clone attributes for: %s", child_rsc->id);
	/* 親リソースの子リソース情報リストに生成したクローン情報を追加する */
	rsc->children = g_list_append(rsc->children, child_rsc);
	if(as_orphan) {
		/* orphan(孤立)リソースのフラグをセットする */
	    mark_as_orphan(child_rsc);
	}
	/* 子リソースのmetaハッシュテーブルに、"clone-max"キーとして、親リソースのclone_maxを追加する */
	add_hash_param(child_rsc->meta, XML_RSC_ATTR_INCARNATION_MAX, inc_max);
	
	print_resource(LOG_DEBUG_3, "Added", child_rsc, FALSE);

  bail:
	crm_free(inc_num);
	crm_free(inc_max);
	
	return child_rsc;
}
/* masterリソース固有の展開処理 */
gboolean master_unpack(resource_t *rsc, pe_working_set_t *data_set)
{
	/* リソース情報のmetaハッシュテーブルから"master-max"の値を取得する */
	const char *master_max = g_hash_table_lookup(
		rsc->meta, XML_RSC_ATTR_MASTER_MAX);
	/* リソース情報のmetaハッシュテーブルから"master-node-max"の値を取得する */
	const char *master_node_max = g_hash_table_lookup(
		rsc->meta, XML_RSC_ATTR_MASTER_NODEMAX);

	g_hash_table_replace(rsc->meta, crm_strdup("stateful"), crm_strdup(XML_BOOLEAN_TRUE));
	/* cloneのリソース固有展開処理を実行する */
	if(clone_unpack(rsc, data_set)) {
		clone_variant_data_t *clone_data = NULL;
		/* clone固有データを取得する */
		get_clone_variant_data(clone_data, rsc);
		/* clone固有データのmaster_max,master_node_maxにmetaハッシュテーブルからの取得値をセットする */
		/* 変換できない場合は、デフォルトの１をセットする */
		clone_data->master_max = crm_parse_int(master_max, "1");
		clone_data->master_node_max = crm_parse_int(master_node_max, "1");
		return TRUE;
	}
	return FALSE;
}
/* cloneリソース固有の展開処理 */
gboolean clone_unpack(resource_t *rsc, pe_working_set_t *data_set)
{
	int lpc = 0;
	const char *type = NULL;
	resource_t *self = NULL;
	int num_xml_children = 0;	
	xmlNode *xml_tmp = NULL;
	xmlNode *xml_self = NULL;
	xmlNode *xml_obj = rsc->xml;
	clone_variant_data_t *clone_data = NULL;
	/* リソース情報のmetaハッシュテーブルから"ordered"の値を取得する */
	const char *ordered = g_hash_table_lookup(
		rsc->meta, XML_RSC_ATTR_ORDERED);
	/* リソース情報のmetaハッシュテーブルから"interleave"の値を取得する */
	const char *interleave = g_hash_table_lookup(
		rsc->meta, XML_RSC_ATTR_INTERLEAVE);
	/* リソース情報のmetaハッシュテーブルから"clone-max"の値を取得する */
	const char *max_clones = g_hash_table_lookup(
		rsc->meta, XML_RSC_ATTR_INCARNATION_MAX);
	/* リソース情報のmetaハッシュテーブルから"clone-node-max"の値を取得する */
	const char *max_clones_node = g_hash_table_lookup(
		rsc->meta, XML_RSC_ATTR_INCARNATION_NODEMAX);

	crm_debug_3("Processing resource %s...", rsc->id);
	/* cloneの固有データを確保する */
	crm_malloc0(clone_data, sizeof(clone_variant_data_t));
	/* 固有データのデフォルト値をセットする */
	rsc->variant_opaque = clone_data;
	clone_data->interleave  = FALSE;
	clone_data->ordered     = FALSE;
	
	clone_data->active_clones  = 0;
	clone_data->xml_obj_child  = NULL;
	/* metaハッシュテーブルから"clone-max"の値をセットする、変換で出来ない場合はデフォルト１をセットする */
	clone_data->clone_node_max = crm_parse_int(max_clones_node, "1");

	if(max_clones) {
		/* metaハッシュテーブルから"clone-max"の値が取得できていた場合セットする。 */
		/* 変換できない場合は、１をセットする */
	    clone_data->clone_max = crm_parse_int(max_clones, "1");

	} else if(g_list_length(data_set->nodes) > 0) {
	    /* metaハッシュテーブルから"clone-node-max"の値が取得できていない場合で、 */
	    /* 状態遷移作業エリアのノード情報リストの長さが０以上の場合は、ノード情報のリスト長からclone_maxをセットする */
	    clone_data->clone_max = g_list_length(data_set->nodes);

	} else {
		/* 上記以外の場合は、1をセットする */
	    clone_data->clone_max = 1; /* Handy during crm_verify */
	}
	
	if(crm_is_true(interleave)) {
		/* metaハッシュテーブルから"interleave"の値をセットする、変換で出来ない場合はデフォルトFALSEをセットする */
		clone_data->interleave = TRUE;
	}
	if(crm_is_true(ordered)) {
		/* metaハッシュテーブルから"ordered"の値をセットする、変換で出来ない場合はデフォルトFALSEをセットする */
		clone_data->ordered = TRUE;
	}
	if((rsc->flags & pe_rsc_unique) == 0 && clone_data->clone_node_max > 1) {
		/* リソース情報にpe_rsc_uniqueがセットされていないで、clone_node_maxが１より大きい場合 */
		/* 重要 : 通常qlobally-unique=FALSEで利用している場合には、clone_node_maxは1でしか指定出来ないということになる */
	    crm_config_err("Anonymous clones (%s) may only support one copy"
			   " per node", rsc->id);
	    clone_data->clone_node_max = 1;
	}

	crm_debug_2("Options for %s", rsc->id);
	crm_debug_2("\tClone max: %d", clone_data->clone_max);
	crm_debug_2("\tClone node max: %d", clone_data->clone_node_max);
	crm_debug_2("\tClone is unique: %s", is_set(rsc->flags, pe_rsc_unique)?"true":"false");
	/* 子リソースのxml情報にcloneのxml情報から"group"タグを取り出してセットする */
	clone_data->xml_obj_child = find_xml_node(
		xml_obj, XML_CIB_TAG_GROUP, FALSE);

	if(clone_data->xml_obj_child == NULL) {
		/* "group"タグが取得できない場合は、"primitive"タグを取り出してセットする */
	    clone_data->xml_obj_child = find_xml_node(
		xml_obj, XML_CIB_TAG_RESOURCE, TRUE);
	} else {
		/* "group"タグが取得できた場合は、"primitive"をカウントして、primitiveリソース数を計算する */
	    xml_child_iter_filter(xml_obj, a_child, XML_CIB_TAG_RESOURCE, num_xml_children++);
	}

	if(clone_data->xml_obj_child == NULL) {
		/* 子リソースのxml情報が取得できなかった場合は、エラーで処理しない */
		crm_config_err("%s has nothing to clone", rsc->id);
		return FALSE;
	}

	type = crm_element_name(clone_data->xml_obj_child);
	xml_child_iter_filter(xml_obj, a_child, type, num_xml_children++);

	if(num_xml_children > 1) {
	    crm_config_err("%s has too many children.  Only the first (%s) will be cloned.",
			   rsc->id, ID(clone_data->xml_obj_child));
	}
	
	/* 自身のcloneリソースxmlをセットする */
	xml_self = copy_xml(rsc->xml);
	/* this is a bit of a hack - but simplifies everything else */
	xmlNodeSetName(xml_self, ((const xmlChar*)XML_CIB_TAG_RESOURCE));
/* 	set_id(xml_self, "self", -1); */
	/* 自身の"operations"ノード情報を取得する */
	xml_tmp = find_xml_node(xml_obj, "operations", FALSE);
	if(xml_tmp != NULL) {
		/* 自身の"operations"ノード情報があれば、セットする */
		add_node_copy(xml_self, xml_tmp);
	}

	/* Make clones ever so slightly sticky by default
	 * This is the only way to ensure clone instances are not
	 *  shuffled around the cluster for no benefit
	 */
  	add_hash_param(rsc->meta, XML_RSC_ATTR_STICKINESS, "1");
	/* self用に自身のcloneリソースを展開する */
	if(common_unpack(xml_self, &self, rsc, data_set)) {
		clone_data->self = self;

	} else {
		crm_log_xml_err(xml_self, "Couldnt unpack dummy child");
		clone_data->self = self;
		return FALSE;
	}
	
	crm_debug_2("\tClone is unique (fixed): %s", is_set(rsc->flags, pe_rsc_unique)?"true":"false");
	/* リソース情報のpe_rsc_notifyフラグから、clone固有データのnotify_confirmをセットする */
	clone_data->notify_confirm = is_set(rsc->flags, pe_rsc_notify);
	/* リソース情報のmetaハッシュテーブルに、"globally-unique"キーとして、リソース情報のpe_rsc_uniqueフラグのbool値をセットする */
	add_hash_param(rsc->meta, XML_RSC_ATTR_UNIQUE,
		       is_set(rsc->flags, pe_rsc_unique)?XML_BOOLEAN_TRUE:XML_BOOLEAN_FALSE);
	/* clone_max分処理する(子リソースはclone_max個生成される) */
	for(lpc = 0; lpc < clone_data->clone_max; lpc++) {
		/* cloneの子リソースを生成する */
		create_child_clone(rsc, lpc, data_set);
	}

	if(clone_data->clone_max == 0) {
	    /* create one so that unpack_find_resource() will hook up
	     * any orphans up to the parent correctly
	     */
	     /* clone_maxが0指定の場合...あまり使わないが? */
	    create_child_clone(rsc, -1, data_set);
	}
	
	crm_debug_3("Added %d children to resource %s...",
		    clone_data->clone_max, rsc->id);
	return TRUE;
}
/* cloneリソースのアクティブ状態を確認する処理 */
gboolean clone_active(resource_t *rsc, gboolean all)
{
	clone_variant_data_t *clone_data = NULL;
	/* clone固有データを取得する */
	get_clone_variant_data(clone_data, rsc);
	/* 全ての子リソースを処理する */
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		/* 子リソースのアクティブ状態を確認する */
		gboolean child_active = child_rsc->fns->active(child_rsc, all);
		if(all == FALSE && child_active) {
			/* 全確認がFALSEで子リソースがアクティブ状態ならTRUEを返す */
			return TRUE;
		} else if(all && child_active == FALSE) {
			/* 全確認がTRUEで子リソースが非アクティブ状態ならFALSEを返す */
			return FALSE;
		}
		);
	if(all) {
		/* 全処理後、全確認がTRUEならTRUEを返す */
		return TRUE;
	} else {
		/* 全確認がFALSEならFALSEを返す(再帰用に必要) */
		return FALSE;
	}
}
/* リスト追加処理 */
static char *
add_list_element(char *list, const char *value) 
{
    int len = 0;
    int last = 0;

    if(value == NULL) {
	return list;
    }
    if(list) {
	last = strlen(list);
    }
    len = last + 2;  /* +1 space, +1 EOS */
    len += strlen(value);
    crm_realloc(list, len);
    sprintf(list + last, " %s", value);
    return list;
}

static char *
convert_host_list(GListPtr glist)
{
    char *result = NULL;
    GListPtr gIter = NULL;
    for (gIter = glist; gIter != NULL; gIter = gIter->next) {
	const char *host = (const char *)gIter->data;
	result = add_list_element(result, host);
    }
    return result;
}

static void
short_print(char *list, const char *prefix, const char *type, long options, void *print_data) 
{
    if(list) {
	if(options & pe_print_html) {
	    status_print("<li>");
	}
	status_print("%s%s: [%s ]", prefix, type, list);

	if(options & pe_print_html) {
	    status_print("</li>\n");

	} else if(options & pe_print_suppres_nl) {
	    /* nothing */
	} else if((options & pe_print_printf) || (options & pe_print_ncurses)) {
		status_print("\n");
	}

    }
}

void clone_print(
	resource_t *rsc, const char *pre_text, long options, void *print_data)
{
    char *child_text = NULL;
    char *master_list = NULL;
    char *started_list = NULL;
    char *stopped_list = NULL;
    GListPtr master_sort_glist = NULL;
    GListPtr started_sort_glist = NULL;
    const char *type = "Clone";
    clone_variant_data_t *clone_data = NULL;
    get_clone_variant_data(clone_data, rsc);

    if(pre_text == NULL) { pre_text = " "; }
    child_text = crm_concat(pre_text, "   ", ' ');

    if(rsc->variant == pe_master) {
	type = "Master/Slave";
    }

    status_print("%s%s Set: %s%s%s",
		 pre_text?pre_text:"", type, rsc->id,
		 is_set(rsc->flags, pe_rsc_unique)?" (unique)":"",
		 is_set(rsc->flags, pe_rsc_managed)?"":" (unmanaged)");
	
    if(options & pe_print_html) {
	status_print("\n<ul>\n");

    } else if((options & pe_print_log) == 0) {
	status_print("\n");
    }

    slist_iter(
	child_rsc, resource_t, rsc->children, lpc,

	gboolean print_full = FALSE;
	if(child_rsc->fns->active(child_rsc, FALSE) == FALSE) {
	    /* Inactive clone */
	    if(is_set(child_rsc->flags, pe_rsc_orphan)) {
		continue;

	    } else if(is_set(rsc->flags, pe_rsc_unique)) {
		print_full = TRUE;
	    } else {
		stopped_list = add_list_element(stopped_list, child_rsc->id);
	    }
		
	} else if(is_set(child_rsc->flags, pe_rsc_unique)
		  || is_set(child_rsc->flags, pe_rsc_orphan)
		  || is_set(child_rsc->flags, pe_rsc_managed) == FALSE
		  || is_set(child_rsc->flags, pe_rsc_failed)) {

	    /* Unique, unmanaged or failed clone */
	    print_full = TRUE;
		
	} else if(child_rsc->fns->active(child_rsc, TRUE)) {
	    /* Fully active anonymous clone */
	    node_t *location = child_rsc->fns->location(child_rsc, NULL, TRUE);

	    if(location) {
		const char *host = location->details->uname;
		enum rsc_role_e a_role = child_rsc->fns->state(child_rsc, TRUE);

		if(a_role > RSC_ROLE_SLAVE) {
		    /* And active on a single node as master */
		    if (options & pe_print_sort)  {
			master_sort_glist = g_list_insert_sorted(master_sort_glist, (gpointer)host, (GCompareFunc)strcmp);
		    } else {
			master_list = add_list_element(master_list, host);
		    }
		} else {
		    /* And active on a single node as started/slave */
		    if (options & pe_print_sort)  {
			started_sort_glist = g_list_insert_sorted(started_sort_glist, (gpointer)host, (GCompareFunc)strcmp);
		    } else {
			started_list = add_list_element(started_list, host);
		    }
		}
	    
	    } else {
		/* uncolocated group - bleh */
		print_full = TRUE;
	    }
		
	} else {
	    /* Partially active anonymous clone */
	    print_full = TRUE;
	}
	    
	if(print_full) {
	    if(options & pe_print_html) {
		status_print("<li>\n");
	    }
	    child_rsc->fns->print(
		child_rsc, child_text, options, print_data);
	    if(options & pe_print_html) {
		status_print("</li>\n");
	    }
	}
	    
	);
	
    if (options & pe_print_sort)  {
	master_list = convert_host_list(master_sort_glist);
	started_list = convert_host_list(started_sort_glist);
    }

    short_print(master_list, child_text, "Masters", options, print_data);
    short_print(started_list, child_text, rsc->variant==pe_master?"Slaves":"Started", options, print_data);
    short_print(stopped_list, child_text, "Stopped", options, print_data);

    crm_free(master_list);
    crm_free(started_list);
    crm_free(stopped_list);    
    if (master_sort_glist) {
	g_list_free(master_sort_glist);
    }
    if (started_sort_glist) {
	g_list_free(started_sort_glist);
    }
    
    if(options & pe_print_html) {
	status_print("</ul>\n");
    }

    crm_free(child_text);
}

void clone_free(resource_t *rsc)
{
	clone_variant_data_t *clone_data = NULL;
	get_clone_variant_data(clone_data, rsc);

	crm_debug_3("Freeing %s", rsc->id);

	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,

		crm_debug_3("Freeing child %s", child_rsc->id);
		free_xml(child_rsc->xml);
		child_rsc->fns->free(child_rsc);
		);

	crm_debug_3("Freeing child list");
	pe_free_shallow_adv(rsc->children, FALSE);

	if(clone_data->self) {
		free_xml(clone_data->self->xml);
		clone_data->self->fns->free(clone_data->self);
	}

	CRM_ASSERT(clone_data->demote_notify == NULL);
	CRM_ASSERT(clone_data->stop_notify == NULL);
	CRM_ASSERT(clone_data->start_notify == NULL);
	CRM_ASSERT(clone_data->promote_notify == NULL);
	
	common_free(rsc);
}
/* クローンリソースのrole取得処理(もっとも大きいroleを取得する) */
enum rsc_role_e
clone_resource_state(const resource_t *rsc, gboolean current)
{
	enum rsc_role_e clone_role = RSC_ROLE_UNKNOWN;

	clone_variant_data_t *clone_data = NULL;
	/* clone固有データを取得 */
	get_clone_variant_data(clone_data, rsc);
	/* 全ての子リソースを処理する */
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		/* 子リソースのroleを取得する */
		enum rsc_role_e a_role = child_rsc->fns->state(child_rsc, current);
		if(a_role > clone_role) {
			/* 取得roleが保存roleより大きいroleの場合は、保存roleにセット */
			clone_role = a_role;
		}
		);

	crm_debug_3("%s role: %s", rsc->id, role2text(clone_role));
	/* 保存roleを返す */
	return clone_role;
}
