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

#include <crm/msg_xml.h>
#include <allocate.h>
#include <utils.h>
#include <lib/pengine/utils.h>

#define VARIANT_CLONE 1
#include <lib/pengine/variant.h>

gint sort_clone_instance(gconstpointer a, gconstpointer b);

void child_stopping_constraints(
	clone_variant_data_t *clone_data, 
	resource_t *self, resource_t *child, resource_t *last,
	pe_working_set_t *data_set);

void child_starting_constraints(
	clone_variant_data_t *clone_data, 
	resource_t *self, resource_t *child, resource_t *last,
	pe_working_set_t *data_set);
/* 親cloneリソースの配置ノード情報から、指定ノードを検索する */
static node_t *
parent_node_instance(const resource_t *rsc, node_t *node)
{
	node_t *ret = NULL;
	if(node != NULL) {
		/* ノードがNULLでない場合、親cloneリソースの配置ノード情報から、指定ノード情報を検索する */
		ret = pe_find_node_id(
			rsc->parent->allowed_nodes, node->details->id);
	}
	/* 検索結果を返す */
	return ret;
}
/* エラーが発生しているかどうかのチェック */
static gboolean did_fail(const resource_t *rsc)
{
    if(is_set(rsc->flags, pe_rsc_failed)) {
		/* pe_rsc_failedフラグがセットされている場合は、TRUE */
	return TRUE;
    }
	/* 全ての子リソースを処理する */
    slist_iter(
	child_rsc, resource_t, rsc->children, lpc,
	if(did_fail(child_rsc)) {
			/* 子リソースのエラーをチェックしてエラーならTRUE */
	    return TRUE;
	}
	);
	/* 全子リソースにエラーがなければFALSE */
    return FALSE;
}

/* クローンの子リソースをソートする */
gint sort_clone_instance(gconstpointer a, gconstpointer b)
{
	int level = LOG_DEBUG_3;
	node_t *node1 = NULL;
	node_t *node2 = NULL;

	gboolean can1 = TRUE;
	gboolean can2 = TRUE;
	gboolean with_scores = TRUE;
	
	const resource_t *resource1 = (const resource_t*)a;
	const resource_t *resource2 = (const resource_t*)b;

	CRM_ASSERT(resource1 != NULL);
	CRM_ASSERT(resource2 != NULL);

	/* allocation order:
	 *  - active instances
	 *  - instances running on nodes with the least copies
	 *  - active instances on nodes that cant support them or are to be fenced
	 *  - failed instances
	 *  - inactive instances
	 */	

	if(resource1->running_on && resource2->running_on) {
		if(g_list_length(resource1->running_on) < g_list_length(resource2->running_on)) {
			do_crm_log_unlikely(level, "%s < %s: running_on", resource1->id, resource2->id);
			return -1;
			
		} else if(g_list_length(resource1->running_on) > g_list_length(resource2->running_on)) {
			do_crm_log_unlikely(level, "%s > %s: running_on", resource1->id, resource2->id);
			return 1;
		}
	}
	
	if(resource1->running_on) {
		node1 = resource1->running_on->data;
	}
	if(resource2->running_on) {
		node2 = resource2->running_on->data;
	}

	if(node1) {
	    node_t *match = pe_find_node_id(resource1->allowed_nodes, node1->details->id);
	    if(match == NULL || match->weight < 0) {
		do_crm_log_unlikely(level, "%s: current location is unavailable", resource1->id);
		node1 = NULL;
		can1 = FALSE;
	    }
	}

	if(node2) {
	    node_t *match = pe_find_node_id(resource2->allowed_nodes, node2->details->id);
	    if(match == NULL || match->weight < 0) {
		do_crm_log_unlikely(level, "%s: current location is unavailable", resource2->id);
		node2 = NULL;
		can2 = FALSE;
	    }
	}

	if(can1 != can2) {
		if(can1) {
			do_crm_log_unlikely(level, "%s < %s: availability of current location", resource1->id, resource2->id);
			return -1;
		}
		do_crm_log_unlikely(level, "%s > %s: availability of current location", resource1->id, resource2->id);
		return 1;
	}
	
	if(resource1->priority < resource2->priority) {
		do_crm_log_unlikely(level, "%s < %s: priority", resource1->id, resource2->id);
		return 1;

	} else if(resource1->priority > resource2->priority) {
		do_crm_log_unlikely(level, "%s > %s: priority", resource1->id, resource2->id);
		return -1;
	}
	
	if(node1 == NULL && node2 == NULL) {
			do_crm_log_unlikely(level, "%s == %s: not active",
					   resource1->id, resource2->id);
			return 0;
	}

	if(node1 != node2) {
		if(node1 == NULL) {
			do_crm_log_unlikely(level, "%s > %s: active", resource1->id, resource2->id);
			return 1;
		} else if(node2 == NULL) {
			do_crm_log_unlikely(level, "%s < %s: active", resource1->id, resource2->id);
			return -1;
		}
	}
	
	can1 = can_run_resources(node1);
	can2 = can_run_resources(node2);
	if(can1 != can2) {
		if(can1) {
			do_crm_log_unlikely(level, "%s < %s: can", resource1->id, resource2->id);
			return -1;
		}
		do_crm_log_unlikely(level, "%s > %s: can", resource1->id, resource2->id);
		return 1;
	}

	node1 = parent_node_instance(resource1, node1);
	node2 = parent_node_instance(resource2, node2);
	if(node1 != NULL && node2 == NULL) {
		do_crm_log_unlikely(level, "%s < %s: not allowed", resource1->id, resource2->id);
		return -1;
	} else if(node1 == NULL && node2 != NULL) {
		do_crm_log_unlikely(level, "%s > %s: not allowed", resource1->id, resource2->id);
		return 1;
	}
	
	if(node1 == NULL) {
		do_crm_log_unlikely(level, "%s == %s: not allowed", resource1->id, resource2->id);
		return 0;
	}

	if(node1->count < node2->count) {
		do_crm_log_unlikely(level, "%s < %s: count", resource1->id, resource2->id);
		return -1;

	} else if(node1->count > node2->count) {
		do_crm_log_unlikely(level, "%s > %s: count", resource1->id, resource2->id);
		return 1;
	}

	if(with_scores) {
	    int max = 0;
	    int lpc = 0;
	    GListPtr list1 = node_list_dup(resource1->allowed_nodes, FALSE, FALSE);
	    GListPtr list2 = node_list_dup(resource2->allowed_nodes, FALSE, FALSE);

	    /* Current score */
	    node1 = g_list_nth_data(resource1->running_on, 0);
	    node1 = pe_find_node_id(list1, node1->details->id);

	    node2 = g_list_nth_data(resource2->running_on, 0);
	    node2 = pe_find_node_id(list2, node2->details->id);

	    if(node1->weight < node2->weight) {
		do_crm_log_unlikely(level, "%s < %s: current score", resource1->id, resource2->id);
		return 1;

	    } else if(node1->weight > node2->weight) {
		do_crm_log_unlikely(level, "%s > %s: current score", resource1->id, resource2->id);
		return -1;
	    }

	    /* All scores */
	    list1 = g_list_sort(list1, sort_node_weight);
	    list2 = g_list_sort(list2, sort_node_weight);
	    max = g_list_length(list1);
	    if(max < g_list_length(list2)) {
		max = g_list_length(list2);
	    }
	    
	    for(;lpc < max; lpc++) {
		node1 = g_list_nth_data(list1, lpc);
		node2 = g_list_nth_data(list2, lpc);
		if(node1 == NULL) {
		    do_crm_log_unlikely(level, "%s < %s: node score NULL", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return 1;
		} else if(node2 == NULL) {
		    do_crm_log_unlikely(level, "%s > %s: node score NULL", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return -1;
		}
		
		if(node1->weight < node2->weight) {
		    do_crm_log_unlikely(level, "%s < %s: node score", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return 1;
		    
		} else if(node1->weight > node2->weight) {
		    do_crm_log_unlikely(level, "%s > %s: node score", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return -1;
		}
	    }

	    pe_free_shallow(list1); pe_free_shallow(list2);
	}

	can1 = did_fail(resource1);
	can2 = did_fail(resource2);
	if(can1 != can2) {
	    if(can1) {
		do_crm_log_unlikely(level, "%s > %s: failed", resource1->id, resource2->id);
		return 1;
	    }
	    do_crm_log_unlikely(level, "%s < %s: failed", resource1->id, resource2->id);
	    return -1;
	}

	if(node1 && node2) {
	    int max = 0;
	    int lpc = 0;
	    GListPtr list1 = g_list_append(NULL, node_copy(resource1->running_on->data));
	    GListPtr list2 = g_list_append(NULL, node_copy(resource2->running_on->data));

	    /* Possibly a replacement for the with_scores block above */
	    
	    slist_iter(
		constraint, rsc_colocation_t, resource1->parent->rsc_cons_lhs, lpc,
		do_crm_log_unlikely(level+1, "Applying %s to %s", constraint->id, resource1->id);
		
		list1 = rsc_merge_weights(
		    constraint->rsc_lh, resource1->id, list1,
		    constraint->node_attribute,
		    constraint->score/INFINITY, FALSE, TRUE);
		);    

	    slist_iter(
		constraint, rsc_colocation_t, resource2->parent->rsc_cons_lhs, lpc,
		do_crm_log_unlikely(level+1, "Applying %s to %s", constraint->id, resource2->id);
		
		list2 = rsc_merge_weights(
		    constraint->rsc_lh, resource2->id, list2,
		    constraint->node_attribute,
		    constraint->score/INFINITY, FALSE, TRUE);
		);    

	    list1 = g_list_sort(list1, sort_node_weight);
	    list2 = g_list_sort(list2, sort_node_weight);
	    max = g_list_length(list1);
	    if(max < g_list_length(list2)) {
		max = g_list_length(list2);
	    }
	    
	    for(;lpc < max; lpc++) {
		node1 = g_list_nth_data(list1, lpc);
		node2 = g_list_nth_data(list2, lpc);
		if(node1 == NULL) {
		    do_crm_log_unlikely(level, "%s < %s: colocated score NULL", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return 1;
		} else if(node2 == NULL) {
		    do_crm_log_unlikely(level, "%s > %s: colocated score NULL", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return -1;
		}
		
		if(node1->weight < node2->weight) {
		    do_crm_log_unlikely(level, "%s < %s: colocated score", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return 1;
		    
		} else if(node1->weight > node2->weight) {
		    do_crm_log_unlikely(level, "%s > %s: colocated score", resource1->id, resource2->id);
		    pe_free_shallow(list1); pe_free_shallow(list2);
		    return -1;
		}
	    }

	    pe_free_shallow(list1); pe_free_shallow(list2);
	}
	
	
	do_crm_log_unlikely(level, "%s == %s: default %d", resource1->id, resource2->id, node2->weight);
	return 0;
}
/* 対象ノードでの、cloneリソースのインスタンスの起動可否を決定する */
/* 起動可能な場合は、配置可能なノードを返すが、起動不可なノードの場合には、その対象ノードのスコアを-INFINITYにセットする */
static node_t *
can_run_instance(resource_t *rsc, node_t *node)
{
	node_t *local_node = NULL;
	clone_variant_data_t *clone_data = NULL;
	if(can_run_resources(node) == FALSE) {
		/* 対象ノードでリソースが起動できない場合 */
		/* ※配置不可 */
		goto bail;

	} else if(is_set(rsc->flags, pe_rsc_orphan)) {
		/* 対象リソースがpe_rsc_orphanな場合 */
		/* ※配置不可 */
		goto bail;
	}
	/* 対象ノードが親cloneリソースの配置ノードリスト（allowed_nodes）に含まれるか検索する */
	local_node = parent_node_instance(rsc, node);
	/* 親cloneリソースの固有データを取得する */
	get_clone_variant_data(clone_data, rsc->parent);

	if(local_node == NULL) {
		/* 対象ノードが親cloneリソースの配置ノードリストに含まれない場合 */
		/* ※配置不可 */
		crm_warn("%s cannot run on %s: node not allowed",
			rsc->id, node->details->uname);
		goto bail;

	} else if(local_node->count < clone_data->clone_node_max) {
		/* 対象ノードが親cloneリソースの配置ノードに含まれて、まだ、clone_node_maxに達していない場合 */
		/* 配置ノードを返す */
		return local_node;

	} else {
		/* 重要・すでに対象クローンがこのノードで起動していて、それがclone_node_maxを超えている場合は、 */
		/* 他のノードで起動する為に、bailでこのノードのスコアを-INFINTYにセットする */
		/* この仕組みを使って、各ノードに順にcloneを配置できる */
		crm_debug_2("%s cannot run on %s: node full",
			    rsc->id, node->details->uname);
	}

  bail:
	if(node) {
		/* 対象ノードが配置不可なので、-INFINITYをセットする */
		/* ※colorでも配置ノードとしては処理されなくなる */
		common_update_score(rsc, node->details->id, -INFINITY);
	}
	return NULL;
}

/* cloneリソースの子リソースのcolor処理 */
static node_t *
color_instance(resource_t *rsc, pe_working_set_t *data_set) 
{
	node_t *chosen = NULL;
	node_t *local_node = NULL;

	crm_debug_2("Processing %s", rsc->id);

	if(is_not_set(rsc->flags, pe_rsc_provisional)) {
		/* すでに、native_assign_node()処理されたリソースに関しては、処理しない */
		/* location(native_location)を実行して、決定している配置先のノード情報allocated_toを返す */
		return rsc->fns->location(rsc, NULL, FALSE);

	} else if(is_set(rsc->flags, pe_rsc_allocating)) {
		/* pe_rsc_allocatingがセットされているリソースも処理しない */
		crm_debug("Dependency loop detected involving %s", rsc->id);
		return NULL;
	}

	if(rsc->allowed_nodes) {
		/* 子リソースの配置ノード情報がすでに存在する場合 */
		/* 全ての配置ノード情報で、リソースが実行可能かどうかチェックする */
		/* ※can_run_instance内で、実行不可能なノードに関しては(既に起動しているノード)、-INFINITYがセットされる */
		slist_iter(try_node, node_t, rsc->allowed_nodes, lpc,
			   can_run_instance(rsc, try_node);
			);
	}
	/*******************************************************************************/
	/* 子のリソース毎のcolor処理を実行する(colocationなどの反映と実行ノードの決定) */
	/*******************************************************************************/
	chosen = rsc->cmds->color(rsc, data_set);
	if(chosen) {
		/* 子リソース毎のcolor処理の結果、子リソースの実行ノードが決定した場合 */
		
		/* 親リソースの配置ノードリストから、子リソースの実行ノードを検索する */
		local_node = pe_find_node_id(
			rsc->parent->allowed_nodes, chosen->details->id);

		if(local_node) {
			/* 親リソースの配置ノードに子リソースの実行ノードが含まれた場合 */
		    /* 起動リソース数をカウントアップする */
		    local_node->count++;
		} else if(is_set(rsc->flags, pe_rsc_managed)) {
		    /* what to do? we can't enforce per-node limits in this case */
			/* 親リソースの配置ノードに子リソースの実行ノードが含まれれない場合で、 pe_rsc_managedフラグがセットされている場合 */
		    crm_config_err("%s not found in %s (list=%d)",
				   chosen->details->id, rsc->parent->id,
				   g_list_length(rsc->parent->allowed_nodes));
		}
	}
	/* 実行ノードを返す */
	return chosen;
}

static void append_parent_colocation(resource_t *rsc, resource_t *child, gboolean all) 
{
	/************************* 親リソース(cloneリソース)に対するcolocationタグのrsc指定を処理する ***********************/
	/* 親リソース(cloneリソース)情報のrsc_consリスト(リソースに対するcolocation情報のrsc指定)をすべて処理する */
    slist_iter(cons, rsc_colocation_t, rsc->rsc_cons, lpc,
	       if(all || cons->score < 0 || cons->score == INFINITY) {
				/* allがTRUEか、処理対象のcolocationがマイナス値指定か、INFINTIYの場合は子リソースのrsc_consリストに親リソースの情報を */
				/* 追加する */
		   child->rsc_cons = g_list_append(child->rsc_cons, cons);
	       }
	       
	);
	/************************* 親リソース(cloneリソース)に対するcolocationタグのwith-rsc指定を処理する ***********************/
	/* 親リソース(cloneリソース)情報のrsc_cons_lnsリスト(リソースに対するcolocation情報のwith-rsc指定)をすべて処理する */
    slist_iter(cons, rsc_colocation_t, rsc->rsc_cons_lhs, lpc,
	       if(all || cons->score < 0) {
				/* allがTRUEか、処理対象のcolocationがマイナス値指定の場合は子リソースのrsc_consリストに親リソースの情報を */
				/* 追加する */
		   child->rsc_cons_lhs = g_list_append(child->rsc_cons_lhs, cons);
	       }
	);
}
/* cloneリソースのcolor処理 */
node_t *
clone_color(resource_t *rsc, pe_working_set_t *data_set)
{
	int allocated = 0;
	int available_nodes = 0;
	clone_variant_data_t *clone_data = NULL;
	get_clone_variant_data(clone_data, rsc);

	if(is_not_set(rsc->flags, pe_rsc_provisional)) {
		/* すでに、native_assign_node()処理されたリソースに関しては、処理しない */
		return NULL;

	} else if(is_set(rsc->flags, pe_rsc_allocating)) {
		/* pe_rsc_allocatingがセットされているリソースも処理しない */
		crm_debug("Dependency loop detected involving %s", rsc->id);
		return NULL;
	}

	set_bit(rsc->flags, pe_rsc_allocating);
	crm_debug_2("Processing %s", rsc->id);

	/* this information is used by sort_clone_instance() when deciding in which 
	 * order to allocate clone instances
	 */
	/************************* このリソースに対するcolocationタグのwith-rsc指定を処理する ***********************/
	/* このリソース情報のrsc_cons_lhsリスト(このリソースに対する他のリソースからcolocation情報でwith-rsc指定されている情報）をすべて処理する */
	/* ※このリソースが、他のリソースからcolocation情報でwith-rsc指定されていなければ、処理されない事になる */
	/* ※ rsc=X with-rsc=A																			*/
	/*    rsc=Y with-rsc=Aのような場合、このクローンリソースがAだった場合に、処理を実行することになる */
	slist_iter(
	    constraint, rsc_colocation_t, rsc->rsc_cons_lhs, lpc,
	    /* このリソースの配置可能なノードリストに、colocation指定のrsc指定リソースのmerge_weights処理を行う */
	    /*   with-rsc指定されている側のリソースの配置可能なノードリスト(allowed_nodes)のweightに、 */
	    /*   rsc指定されているリソースの置可能なノードリスト(allowed_nodes)のweightをマージ（反映する) */
	    rsc->allowed_nodes = constraint->rsc_lh->cmds->merge_weights(
		constraint->rsc_lh, rsc->id, rsc->allowed_nodes,
		constraint->node_attribute, constraint->score/INFINITY, TRUE, TRUE);
	    );
	/* スコアをダンプする */
	dump_node_scores(show_scores?0:scores_log_level, rsc, __FUNCTION__, rsc->allowed_nodes);
	
	/* count now tracks the number of clones currently allocated */
	/* クローンリソースの配置可能ノード情報リストの起動している（起動予定も含む）リソース数を０リセットする 	*/
	slist_iter(node, node_t, rsc->allowed_nodes, lpc,
		   node->count = 0;
		);
	/* クローンリソースの全ての子リソースを処理する */
	/*   ※実行中の子リソースを処理 */
	slist_iter(child, resource_t, rsc->children, lpc,
		   if(g_list_length(child->running_on) > 0) {
				/* 子リソースのrunning_onノード情報を取りだす */
			   node_t *child_node = child->running_on->data;
			   /* 子リソースの起動しているノード情報から親ノード情報を取り出す */
			   node_t *local_node = parent_node_instance(
				   child, child->running_on->data);
			   if(local_node) {
					/* 親ノードで起動中のリソース数をカウントアップする */
				   local_node->count++;
			   } else {
				   crm_err("%s is running on %s which isn't allowed",
					   child->id, child_node->details->uname);
			   }
		   }
		);
	/* クローンリソースの子リソースをソートする */
	rsc->children = g_list_sort(rsc->children, sort_clone_instance);

	/* count now tracks the number of clones we have allocated */
	/* クローンリソースの配置可能ノード情報リストの起動している（起動予定も含む）リソース数を０リセットする 	*/
	slist_iter(node, node_t, rsc->allowed_nodes, lpc,
		   node->count = 0;
		);
	/* クローンリソースのリソースが配置可能なノード情報のリストをweightでソートする */
	rsc->allowed_nodes = g_list_sort(
		rsc->allowed_nodes, sort_node_weight);
	/* クローンリソースのリソースが配置可能なノード情報のリストを全て処理する */
	slist_iter(node, node_t, rsc->allowed_nodes, lpc,
			/* 配置可能なノード情報でリソースが起動可能な場合は、カウントアップする */
		   if(can_run_resources(node)) {
		       available_nodes++;
		   }
	    );
	/* クローンリソースの全ての子リソース情報を処理する */
	slist_iter(child, resource_t, rsc->children, lpc,
		   if(allocated >= clone_data->clone_max) {
				/* allocatedがclone_maxに達した場合は、全ての子リソースの配置可能なノード情報のweightに-INFINITYをセットする */
				/* ※これが実行されるとclone_max以上のクローンの子リソースは配置不可 */
			   crm_debug("Child %s not allocated - limit reached", child->id);
			   resource_location(child, NULL, -INFINITY, "clone_color:limit_reached", data_set);

		   } else if (clone_data->clone_max < available_nodes) {
		       /* Only include positive colocation preferences of dependant resources
			* if not every node will get a copy of the clone
			*/
				/* 起動可能なクラスタ構成ノード数が、まだ、clone_maxに達していない場合 */
				/* 親リソースのcolocation指定を子リソースに反映する */
				/* 重要：この時はcloneリソースのcolocationでwith-src指定されている分はセットされるので注意 */
		       append_parent_colocation(rsc, child, TRUE);

		   } else {
				/* 起動可能なクラスタ構成ノード数がちょうど、clone_maxの場合 */
				/* 親リソースのcolocation指定を子リソースに反映する */
				/* 重要：この時はcloneリソースのcolocationでwith-src指定されている分はセットされないので注意 */
				/* よって、colocationをもったリソースの１次故障→cloneの２次故障でも、故障cloneが単体で処理される */
		       append_parent_colocation(rsc, child, FALSE);
		   }
		   /*******************************/
		   /* 子リソースのcolorを処理する */
		   /*******************************/
		   if(color_instance(child, data_set)) {
				/* 配置ノードが決定すれば配置カウンタをインクリメントする */
			   allocated++;
		   }
		);

	crm_debug("Allocated %d %s instances of a possible %d",
		  allocated, rsc->id, clone_data->clone_max);
	/* pe_rsc_provisionalフラグ、pe_rsc_allocatingフラグをクリアする */
	clear_bit(rsc->flags, pe_rsc_provisional);
	clear_bit(rsc->flags, pe_rsc_allocating);
	
	return NULL;
}
/* 生成された子リソースのアクション情報から */
/* cloneリソースの固有データのstopping,starting,activeフラグをセットする */
static void
clone_update_pseudo_status(
    resource_t *rsc, gboolean *stopping, gboolean *starting, gboolean *active) 
{
	if(rsc->children) {
	    slist_iter(child, resource_t, rsc->children, lpc,
		/* 子リソースがある場合、全ての子リソースのclone_update_pseudo_statusを処理する */
		       clone_update_pseudo_status(child, stopping, starting, active)
		);
	    return;
	}
    
	CRM_ASSERT(active != NULL);
	CRM_ASSERT(starting != NULL);
	CRM_ASSERT(stopping != NULL);

	if(rsc->running_on) {
		/* 対象リソースのrunning_onリスト(実行ノードリスト)がＮＵＬＬでない場合は、activeにTRUEをセットする */
	    *active = TRUE;
	}
	/* 対象リソースの全てのアクションを処理する */
	slist_iter(
		action, action_t, rsc->actions, lpc,

		if(*starting && *stopping) {
			/* すでに、フラグがTRUEなら処理しない */
			return;

		} else if(action->optional) {
			/* オプションのアクションは処理しない */
			crm_debug_3("Skipping optional: %s", action->uuid);
			continue;

		} else if(action->pseudo == FALSE && action->runnable == FALSE){
			/* PSEUDOでないアクションで、実行待ちのアクションも処理しない */
			crm_debug_3("Skipping unrunnable: %s", action->uuid);
			continue;

		} else if(safe_str_eq(RSC_STOP, action->task)) {
			/* stopアクションの場合は、stoppingフラグをセット */
			crm_debug_2("Stopping due to: %s", action->uuid);
			*stopping = TRUE;

		} else if(safe_str_eq(RSC_START, action->task)) {
			/* startアクションの場合 */
			if(action->runnable == FALSE) {
				crm_debug_3("Skipping pseudo-op: %s run=%d, pseudo=%d",
					    action->uuid, action->runnable, action->pseudo);
			} else {
				/* 実行可能なアクションであれば、startingフラグをセット */
				crm_debug_2("Starting due to: %s", action->uuid);
				crm_debug_3("%s run=%d, pseudo=%d",
					    action->uuid, action->runnable, action->pseudo);
				*starting = TRUE;
			}
		}
		);

}
/* アクションの検索処理 */
static action_t *
find_rsc_action(resource_t *rsc, const char *key, gboolean active_only, GListPtr *list)
{
    action_t *match = NULL;
    GListPtr possible = NULL;
    GListPtr active = NULL;
    /* リソースのアクション情報リストから該当キーのアクションリストを取得する */
    possible = find_actions(rsc->actions, key, NULL);

    if(active_only) {
		/* activeオンリーの検索の場合 */
	slist_iter(op, action_t, possible, lpc,
		   if(op->optional == FALSE) {
				/* 検索したアクションリストの中で */
				/* 必須のアクションのみを作業リストに積み上げる */
				active = g_list_append(active, op);
		   }
	    );
	
	if(active && g_list_length(active) == 1) {
		/* activeのみを検索した場合で、１件のアクションだけだった場合 */
		/* 結果に先頭のアクションをセットする */
	    match = g_list_nth_data(active, 0);
	}
	
	if(list) {
			/* 返却リストが存在する場合 */
			/* 返却リストに作業リストをセットして、ポインタにはNULLをセット */
	    *list = active; active = NULL;
	}
	
    } else if(possible && g_list_length(possible) == 1) {
		/* activeオンリーでない場合で、検索結果がある場合で、１件のアクションだけだった場合 */
		/* 結果に最初の検索結果の先頭アクションをセットする */
	match = g_list_nth_data(possible, 0);

    } if(list) {
		/* 返却リストが存在する場合 */
		/* １件以上の検索結果がある場合は、返却リストにセットして、ポインタにはNULLをセット */
	*list = possible; possible = NULL;
    }    
	/* 作業リストを解放する */
    if(possible) {
	g_list_free(possible);
    }
    if(active) {
	g_list_free(active);
    }
    /* １件のみの場合の検索結果を返す */
    return match;
}
/* 子リソースのlast_startとstarアクションとstopとlast_stopアクションのactions_after,actions_beforeを生成する */
/* ※子リソース間のstart->start,stop->stopのactions_after,actions_beforeを生成することになる */
static void
child_ordering_constraints(resource_t *rsc, pe_working_set_t *data_set)
{
    char *key = NULL;
    action_t *stop = NULL;
    action_t *start = NULL; 
    action_t *last_stop = NULL;
    action_t *last_start = NULL;
    gboolean active_only = TRUE; /* change to false to get the old behavior */
    clone_variant_data_t *clone_data = NULL;
    /* clone固有データを取得する */
    get_clone_variant_data(clone_data, rsc);

    if(clone_data->ordered == FALSE) {
		/* 固有データのorderedがFALSEの場合は処理しない */
	return;
    }
    /* 全ての子リソース情報を処理する */
    slist_iter(
	child, resource_t, rsc->children, lpc,
		/* 子リソースのstopキーを生成する */
	key = stop_key(child);
		/* stopキーのアクションを子リソースのアクション情報から検索する */
	stop = find_rsc_action(child, key, active_only, NULL);
	crm_free(key);
		/* 子リソースのstartキーを生成する */
	key = start_key(child);
		/* startキーのアクションを子リソースのアクション情報から検索する */
	start = find_rsc_action(child, key, active_only, NULL);
	crm_free(key);
	
	if(stop) {
			/* stopアクションが存在した場合 */
	    if(last_stop) {
		/* child/child relative stop */
				/* 先頭リソース以外の場合は、stopとlast_stopのactions_after,actions_beforeを生成する */
		order_actions(stop, last_stop, pe_order_implies_left);
	    }
	    	/* stopアクション情報ポインタを保存する */
	    last_stop = stop;
	}
	
	if(start) {
	    if(last_start) {
		/* child/child relative start */
			/* 先頭リソース以外の場合は、last_startとstarのactions_after,actions_beforeを生成する */
		order_actions(last_start, start, pe_order_implies_left);
	    }
	    	/* startアクション情報ポインタを保存する */
	    last_start = start;
	}
	);
}
/* cloneリソースのアクション生成処理 */
void clone_create_actions(resource_t *rsc, pe_working_set_t *data_set)
{
	gboolean child_active = FALSE;
	gboolean child_starting = FALSE;
	gboolean child_stopping = FALSE;

	action_t *stop = NULL;
	action_t *stopped = NULL;

	action_t *start = NULL;
	action_t *started = NULL;

	resource_t *last_start_rsc = NULL;
	resource_t *last_stop_rsc = NULL;
	clone_variant_data_t *clone_data = NULL;
	/* cloneの固有データを取得する */
	get_clone_variant_data(clone_data, rsc);

	crm_debug_2("Creating actions for %s", rsc->id);
	/* cloneリソースの全ての子リソースを処理する */
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		/* 子リソースのアクション生成処理を実行する */
		child_rsc->cmds->create_actions(child_rsc, data_set);
		/* 生成された子リソースのアクション情報から */
		/* cloneリソースの固有データのstopping,starting,activeフラグをセットする */
		clone_update_pseudo_status(
		    child_rsc, &child_stopping, &child_starting, &child_active);
		
		if(is_set(child_rsc->flags, pe_rsc_starting)) {
			/* 開始している最後の子リソースをセットする */
			last_start_rsc = child_rsc;
		}
		if(is_set(child_rsc->flags, pe_rsc_stopping)) {
			/* 停止している最後の子リソースをセットする */
			last_stop_rsc = child_rsc;
		}
		);

	/* start */
	/* cloneのstartアクションを生成する */
	start = start_action(rsc, NULL, !child_starting);
	started = custom_action(rsc, started_key(rsc),
				RSC_STARTED, NULL, !child_starting, TRUE, data_set);

	start->pseudo = TRUE;
	start->runnable = TRUE;
	/* cloneのstartedアクションを生成する */
	started->pseudo = TRUE;
	started->priority = INFINITY;

	if(child_active || child_starting) {
		/* 子リソースのどれかのstartアクションが実行可能か、すでに起動している場合(runnning_onリストに存在)は */
		/* startedも実行可能にする */
	    started->runnable = TRUE;
	}
	/* 子リソースのstartアクションとstopアクションのactions_after,actions_beforeを生成する */
    /* ※子リソース間のstart->start,stop->stopのactions_after,actions_beforeを生成することになる */
	child_ordering_constraints(rsc, data_set);
	child_starting_constraints(clone_data, rsc, NULL, last_start_rsc, data_set);
	if(clone_data->start_notify == NULL) {
		/* notifyのフラグがセットされている場合は */
	    clone_data->start_notify = create_notification_boundaries(rsc, RSC_START, start, started, data_set);
	}
	
	/* stop */
	/* cloneのstopアクションを生成する */
	stop = stop_action(rsc, NULL, !child_stopping);
	stopped = custom_action(rsc, stopped_key(rsc),
				RSC_STOPPED, NULL, !child_stopping, TRUE, data_set);

	stop->pseudo = TRUE;
	stop->runnable = TRUE;
	/* cloneのstoppedアクションを生成する */
	stopped->pseudo = TRUE;
	stopped->runnable = TRUE;
	stopped->priority = INFINITY;
	/* cloneリソースとその子リソースのstopの内部ORDER情報を処理する */
	child_stopping_constraints(clone_data, rsc, NULL, last_stop_rsc, data_set);
	if(clone_data->stop_notify == NULL) {
		/* notifyのフラグがセットされている場合は */
	    clone_data->stop_notify = create_notification_boundaries(rsc, RSC_STOP, stop, stopped, data_set);

	    if(clone_data->stop_notify && clone_data->start_notify) {
		order_actions(clone_data->stop_notify->post_done, clone_data->start_notify->pre, pe_order_optional);	
	    }
	}
}
/* cloneリソースとその子リソースのstartの内部制約を処理する */
void
child_starting_constraints(
	clone_variant_data_t *clone_data,
	resource_t *rsc, resource_t *child, resource_t *last,
	pe_working_set_t *data_set)
{
	if(child == NULL && last == NULL) {
		/* 子リソースがなくて、最後の子リソースもない時は処理しない */
	    crm_debug("%s has no active children", rsc->id);
	    return;
	}
    
	if(child != NULL) {
		/* 子リソースの場合 */
		/* clone親リソースのstartと、子リソースのstartのORDER情報をセットする */
		order_start_start(
		    rsc, child, pe_order_runnable_left|pe_order_implies_left_printed);
		/* 子リソースのstartとclone親リソースのSTARTEDのORDER情報をセットする */
		new_rsc_order(child, RSC_START, rsc, RSC_STARTED, 
			      pe_order_implies_right_printed, data_set);
	}
	/* 以下は未実行(FALASE &&..) */
	if(FALSE && clone_data->ordered) {
		/* clone親リソースの固有データのORDEREDがTRUEの場合 */
		if(child == NULL) {
		    /* last child start before global started */
		    /* 最後の子リソースの場合は、最後の子リソースのstartと、clone親リソースのSTARTEDのORDER情報をセットする */
		    new_rsc_order(last, RSC_START, rsc, RSC_STARTED, 
				  pe_order_runnable_left, data_set);

		} else if(last == NULL) {
			/* global start before first child start */
		    /* 最初の子リソースの場合は、clone親リソースのstartと最初の子リソースのstartのORDER情報をセットする */
			order_start_start(
				rsc, child, pe_order_implies_left);

		} else {
			/* child/child relative start */
			/* 子リソースの間のstartのORDER情報をセットする */
			order_start_start(last, child, pe_order_implies_left);
		}
	}
}
/* cloneリソースとその子リソースのstopの内部制約を処理する */
void
child_stopping_constraints(
	clone_variant_data_t *clone_data,
	resource_t *rsc, resource_t *child, resource_t *last,
	pe_working_set_t *data_set)
{
	if(child == NULL && last == NULL) {
		/* 子リソースがなくて、最後の子リソースもない時は処理しない */
	    crm_debug("%s has no active children", rsc->id);
	    return;
	}

	if(child != NULL) {
		/* 子リソースの場合 */
		/* clone親リソースのstopと、子リソースのstopのODER情報をセットする */
		order_stop_stop(rsc, child, pe_order_shutdown|pe_order_implies_left_printed);
		/* 子リソースのstp@とclone親リソースのSTOPPEDのORDER情報をセットする */
		new_rsc_order(child, RSC_STOP, rsc, RSC_STOPPED,
			      pe_order_implies_right_printed, data_set);
	}
	
	/* 以下は未実行(FALASE &&..) */
	if(FALSE && clone_data->ordered) {
		/* clone親リソースの固有データのORDEREDがTRUEの場合 */
		if(last == NULL) {
		    /* first child stop before global stopped */
		    /* 最後の子リソースの場合は、最後の子リソースのstopと、clone親リソースのSTOPPEDのORDER情報をセットする */
		    new_rsc_order(child, RSC_STOP, rsc, RSC_STOPPED,
				  pe_order_runnable_left, data_set);
			
		} else if(child == NULL) {
			/* global stop before last child stop */
		    /* 最初の子リソースの場合は、clone親リソースのstopと最初の子リソースのstopのORDER情報をセットする */
			order_stop_stop(
				rsc, last, pe_order_implies_left);
		} else {
			/* child/child relative stop */
			/* 子リソースの間のstopORDER情報をセットする */
			order_stop_stop(child, last, pe_order_implies_left);
		}
	}
}

/* cloneリソースの内部制約を処理する */
void
clone_internal_constraints(resource_t *rsc, pe_working_set_t *data_set)
{
	resource_t *last_rsc = NULL;	
	clone_variant_data_t *clone_data = NULL;
	/* 固有データを取得する */
	get_clone_variant_data(clone_data, rsc);
	/* native_internal_constraintsを実行する */
	native_internal_constraints(rsc, data_set);
	
	/* global stop before stopped */
	/* cloneリソースのSTOPと、STOPPEDのORDER情報を生成する */
	new_rsc_order(rsc, RSC_STOP, rsc, RSC_STOPPED, pe_order_runnable_left, data_set);

	/* global start before started */
	/* cloneリソースのSTARTと、STARTEDのORDER情報を生成する */
	new_rsc_order(rsc, RSC_START, rsc, RSC_STARTED, pe_order_runnable_left, data_set);
	
	/* global stopped before start */
	/* cloneリソースのSTOPPEDと、STARTのORDER情報を生成する */
	new_rsc_order(rsc, RSC_STOPPED, rsc, RSC_START, pe_order_optional, data_set);
	/* 全ての子リソース情報を処理する */
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		/* 子リソースの内部制約を処理する */
		child_rsc->cmds->internal_constraints(child_rsc, data_set);
		/* cloneリソースと子リソースのstartの内部制約を処理する */
		child_starting_constraints(
			clone_data, rsc, child_rsc, last_rsc, data_set);
		/* cloneリソースと子リソースのstopの内部制約を処理する */
		child_stopping_constraints(
			clone_data, rsc, child_rsc, last_rsc, data_set);
		/* 最後の子リソース情報をセットする */		
		last_rsc = child_rsc;
		);
}

static void
assign_node(resource_t *rsc, node_t *node, gboolean force)
{
    if(rsc->children) {
		/* 子リソースがある場合は、全ての子リソースの配置ノードをセットする */
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		/* 指定ノードを配置ノード(rsc->allocated_to)にアサインする */
		native_assign_node(child_rsc, NULL, node, force);
	    );
	return;
    }
	/* 指定ノードを配置ノード(rsc->allocated_to)にアサインする */
    native_assign_node(rsc, NULL, node, force);
}

static resource_t*
find_compatible_child_by_node(
    resource_t *local_child, node_t *local_node, resource_t *rsc, enum rsc_role_e filter, gboolean current)
{

	node_t *node = NULL;
	clone_variant_data_t *clone_data = NULL;
	/* clone固有データを取得する */
	get_clone_variant_data(clone_data, rsc);
	
	if(local_node == NULL) {
		crm_err("Can't colocate unrunnable child %s with %s",
			 local_child->id, rsc->id);
		return NULL;
	}
	
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,

		/* enum rsc_role_e next_role = minimum_resource_state(child_rsc, current); */
		enum rsc_role_e next_role = child_rsc->fns->state(child_rsc, current);
		node = child_rsc->fns->location(child_rsc, NULL, current);

		if(filter != RSC_ROLE_UNKNOWN && next_role != filter) {
		    crm_debug_3("Filtered %s", child_rsc->id);
		    continue;
		}
		
		if(node && local_node && node->details == local_node->details) {
			crm_debug_2("Pairing %s with %s on %s",
				    local_child->id, child_rsc->id, node->details->uname);
			return child_rsc;
		}
		);

	crm_debug_3("Can't pair %s with %s", local_child->id, rsc->id);
	return NULL;
}

resource_t*
find_compatible_child(
    resource_t *local_child, resource_t *rsc, enum rsc_role_e filter, gboolean current)
{
	/* 指定したclone子リソースがlocalノードで起動しているかどうか検索する */
	/* 起動している場合は、その子リソース情報 */
	/* 起動していない場合は、NULL */	resource_t *pair = NULL;
	GListPtr scratch = NULL;
	node_t *local_node = NULL;
	clone_variant_data_t *clone_data = NULL;
	/* clone固有データを取得する */
	get_clone_variant_data(clone_data, rsc);
	
	/* 子リソースのノード情報を取得する(リスト取得なし、単一なノード情報のみ取得) */
	local_node = local_child->fns->location(local_child, NULL, current);
	if(local_node) {
		/* ノード情報が取得できない場合は処理しない */
	    return find_compatible_child_by_node(local_child, local_node, rsc, filter, current);
	}

	scratch = node_list_dup(local_child->allowed_nodes, FALSE, TRUE);
	scratch = g_list_sort(scratch, sort_node_weight);

	slist_iter(
		node, node_t, scratch, lpc,

		pair = find_compatible_child_by_node(
		    local_child, node, rsc, filter, current);
		if(pair) {
		    goto done;
		}
		);
	
	crm_debug("Can't pair %s with %s", local_child->id, rsc->id);
  done:
	slist_destroy(node_t, node, scratch, crm_free(node));
	return pair;
}

/* colocationのrsc指定リソース側の処理 */
void clone_rsc_colocation_lh(
	resource_t *rsc_lh, resource_t *rsc_rh, rsc_colocation_t *constraint)
{
	/* -- Never called --
	 *
	 * Instead we add the colocation constraints to the child and call from there
	 */
	
	CRM_CHECK(FALSE, crm_err("This functionality is not thought to be used. Please report a bug."));
	CRM_CHECK(rsc_lh, return);
	CRM_CHECK(rsc_rh, return);
	
	slist_iter(
		child_rsc, resource_t, rsc_lh->children, lpc,
		
		child_rsc->cmds->rsc_colocation_lh(child_rsc, rsc_rh, constraint);
		);

	return;
}
/* colocationのwith-rsc指定リソース側の処理 */
void clone_rsc_colocation_rh(
	resource_t *rsc_lh, resource_t *rsc_rh, rsc_colocation_t *constraint)
{
	gboolean do_interleave = FALSE;
	clone_variant_data_t *clone_data = NULL;
	clone_variant_data_t *clone_data_lh = NULL;

	CRM_CHECK(constraint != NULL, return);
	CRM_CHECK(rsc_lh != NULL, pe_err("rsc_lh was NULL for %s", constraint->id); return);
	CRM_CHECK(rsc_rh != NULL, pe_err("rsc_rh was NULL for %s", constraint->id); return);
	CRM_CHECK(rsc_lh->variant == pe_native, return);
	
	get_clone_variant_data(clone_data, constraint->rsc_rh);
	crm_debug_3("Processing constraint %s: %s -> %s %d",
		    constraint->id, rsc_lh->id, rsc_rh->id, constraint->score);

	if(constraint->rsc_lh->variant >= pe_clone) {

	    get_clone_variant_data(clone_data_lh, constraint->rsc_lh);
	    if (clone_data_lh->interleave && clone_data->clone_node_max != clone_data_lh->clone_node_max) {
		crm_config_err("Cannot interleave "XML_CIB_TAG_INCARNATION
			       " %s and %s because"
			       " they do not support the same number of"
			       " resources per node",
			       constraint->rsc_lh->id, constraint->rsc_rh->id);
			
		/* only the LHS side needs to be labeled as interleave */
	    } else if(clone_data_lh->interleave) {
		do_interleave = TRUE;
	    }
	}

	if(is_set(rsc_rh->flags, pe_rsc_provisional)) {
		crm_debug_3("%s is still provisional", rsc_rh->id);
		return;

	} else if(do_interleave) {
	    resource_t *rh_child = NULL;

	    rh_child = find_compatible_child(rsc_lh, rsc_rh, RSC_ROLE_UNKNOWN, FALSE);
	    
	    if(rh_child) {
		crm_debug("Pairing %s with %s", rsc_lh->id, rh_child->id);
		rsc_lh->cmds->rsc_colocation_lh(rsc_lh, rh_child, constraint);

	    } else if(constraint->score >= INFINITY) {
		crm_notice("Cannot pair %s with instance of %s", rsc_lh->id, rsc_rh->id);
		assign_node(rsc_lh, NULL, TRUE);

	    } else {
		crm_debug("Cannot pair %s with instance of %s", rsc_lh->id, rsc_rh->id);
	    }
	    
	    return;
	    
	} else if(constraint->score >= INFINITY) {
		/* 通常（rsc指定リソースのinterleaveがFALSEの場合）で、colocationのスコアがINFINITYの場合 */
		GListPtr rhs = NULL;
		/* with-rscリソースの全ての子リソースを処理する */
		slist_iter(
			child_rsc, resource_t, rsc_rh->children, lpc,
			/* 子リソースのlocationを検索する */
			node_t *chosen = child_rsc->fns->location(child_rsc, NULL, FALSE);
			if(chosen != NULL) {
				/* 配置候補がある場合は、リストに追加する */
				rhs = g_list_append(rhs, chosen);
			}
			/* この処理で、with-rsc指定されたリソースが配置候補として動けるのかどうかのリストが出来る */
			);
		/* with-rscリソースの配置候補をrsc指定の配置候補へ反映する */
		/* これによって、withリソースの配置候補で、with-rsc指定されたリソースが配置候補にない場合 */
		/* (with-rscリソースが起動出来ないノード)には、rscリソース自体も配置できなく(-INFININITYがセット)される */
		rsc_lh->allowed_nodes = node_list_exclude(rsc_lh->allowed_nodes, rhs, FALSE);
		g_list_free(rhs);
		return;
	}

	slist_iter(
		child_rsc, resource_t, rsc_rh->children, lpc,
		
		child_rsc->cmds->rsc_colocation_rh(rsc_lh, child_rsc, constraint);
		);
}

/*

  Clone <-> Clone ordering
  
  S  : Start(ed)
  S' : Stop(ped)
  P  : Promote(d)
  D  : Demote(d)
  
  Started == Demoted

       First A then B
    A:0		    B:0
 Old	New	Old	New

 S'	S'	S	S'
 S'	S'	S'	-
 S'	S	S	S+
 S'	S	S'	S
 S	S'	S	S'
 S	S'	S'	-
 S	S	S	-
 S	S	S'	S

 S'	S'	P	S'
 S'	S'	S'	-
 S'	P	P	P+
 S'	P	S'	P
 P	S'	P	S'
 P	S'	S'	-
 P	P	P	-
 P	P	S'	P

 D	D	P	D
 D	D	D	-
 D	P	P	P+
 D	P	D	P
 P	D	P	D
 P	D	D	-
 P	P	P	-
 P	P	D	P

  Clone <-> Primitive ordering
  
  S  : Start(ed)
  S' : Stop(ped)
  P  : Promote(d)
  D  : Demote(d)
  F  : False
  T  : True
  F' : A good idea?
  
  Started == Demoted

       First A then B
    A:0		    B
 Old	New	Old	Create Constraint

 S'	S'	S	F
 S'	S'	S'	F'
 S	S'	S	T
 S	S'	S'	F
 S'	S	S	T
 S'	S	S'	T
 S	S	S	F'
 S	S	S'	T

 S'	S'	S	F
 S'	S'	S'	F'
 P	S'	S	T
 P	S'	S'	F
 S'	P	S	T
 S'	P	S'	T
 P	P	S	F'
 P	P	S'	F

 S'	S'	S	F
 S'	S'	S'	F'
 D	S'	S	T
 D	S'	S'	F
 S'	D	S	T
 S'	D	S'	T
 D	D	S	F'
 D	D	S'	T
 
*/
static gboolean detect_restart(resource_t *rsc) 
{
    gboolean restart = FALSE;
    
    /* Look for restarts */
    action_t *start = NULL;
    /* 検索キーにstartをセットする */
    char *key = start_key(rsc);
    /* 対象リソースのアクション情報リストからstartアクションを検索する */
    GListPtr possible_matches = find_actions(rsc->actions, key, NULL);
    crm_free(key);
		
    if(possible_matches) {
		/* 対象リソースのアクション情報リストにstartアクションが設定されている場合 */
		/* 検索したstartアクションを取得する */
	start = possible_matches->data;
		/* 検索リストを解放する */
	g_list_free(possible_matches);
    }
		
    if(start != NULL && start->optional == FALSE) {
		/* startアクションが検索できた場合で、アクションのoptionalがFALSEの場合 */
		/* restartフラグをセットする */
	restart = TRUE;
	crm_debug_2("Detected a restart for %s", rsc->id);
    }

    /* Otherwise, look for moves */
    if(restart == FALSE) {
	GListPtr old_hosts = NULL;
	GListPtr new_hosts = NULL;
	GListPtr intersection = NULL;
		/* 対象リソースのrunning_onリストをold_hostsに取得する */
	rsc->fns->location(rsc, &old_hosts, TRUE);
		/* 対象リソースのallocated_toリストをnew_hostsに取得する */
	rsc->fns->location(rsc, &new_hosts, FALSE);
	intersection = node_list_and(old_hosts, new_hosts, FALSE);

	if(intersection == NULL) {
	    	/* マージした結果のノードリストが存在しない場合は、restartフラグをセットする */
	    restart = TRUE; /* Actually a move but the result is the same */
	    crm_debug_2("Detected a move for %s", rsc->id);
	}

	slist_destroy(node_t, node, intersection, crm_free(node));
	g_list_free(old_hosts);
	g_list_free(new_hosts);
    }

    return restart;
}
/* cloneリソースとprimitive,groupリソースのorder処理 */
static void clone_rsc_order_lh_non_clone(resource_t *rsc, order_constraint_t *order, pe_working_set_t *data_set)
{
    GListPtr hosts = NULL;
    GListPtr rh_hosts = NULL;
    GListPtr intersection = NULL;

    const char *reason = "unknown";
    enum action_tasks task = start_rsc;
    enum rsc_role_e lh_role = RSC_ROLE_STARTED;

    int any_ordered = 0;
    gboolean down_stack = TRUE;
		    
    crm_debug_2("Clone-to-* ordering: %s -> %s 0x%.6x",
		order->lh_action_task, order->rh_action_task, order->type);
		    
    if(strstr(order->rh_action_task, "_"RSC_STOP"_0")
       || strstr(order->rh_action_task, "_"RSC_STOPPED"_0")) {
		/* then指定のアクションがSTOPかSTOPPEDの場合 */
	task = stop_rsc;
	reason = "down activity";
	lh_role = RSC_ROLE_STOPPED;
		/* then指定のリソースのrunning_on情報のリストをrh_hostsに取得する */
	order->rh_rsc->fns->location(order->rh_rsc, &rh_hosts, down_stack);
			
    } else if(strstr(order->rh_action_task, "_"RSC_DEMOTE"_0")
	      || strstr(order->rh_action_task, "_"RSC_DEMOTED"_0")) {
	task = action_demote;
	reason = "demotion activity";
	lh_role = RSC_ROLE_SLAVE;
	order->rh_rsc->fns->location(order->rh_rsc, &rh_hosts, down_stack);
			
    } else if(strstr(order->lh_action_task, "_"RSC_PROMOTE"_0")
	      || strstr(order->lh_action_task, "_"RSC_PROMOTED"_0")) {
	task = action_promote;
	down_stack = FALSE;
	reason = "promote activity";
	order->rh_rsc->fns->location(order->rh_rsc, &rh_hosts, down_stack);
	lh_role = RSC_ROLE_MASTER;
			
    } else if(strstr(order->rh_action_task, "_"RSC_START"_0")
	      || strstr(order->rh_action_task, "_"RSC_STARTED"_0")) {
		/* then指定のアクションがSTARTかSTARTEDの場合 */
	task = start_rsc;
	down_stack = FALSE;
	reason = "up activity";
		/* then指定のリソースのallocated_to情報のリストをrh_hostsに取得する */
	order->rh_rsc->fns->location(order->rh_rsc, &rh_hosts, down_stack);
	/* if(order->rh_rsc->variant > pe_clone) { */
	/*     lh_role = RSC_ROLE_SLAVE; */
	/* } */

    } else {
	crm_err("Unknown task: %s", order->rh_action_task);
	return;
    }
		    
    /* slist_iter(h, node_t, rh_hosts, llpc, crm_info("RHH: %s", h->details->uname)); */
	/* first指定cloneリソースの全ての子リソースを処理する */
    slist_iter(
	child_rsc, resource_t, rsc->children, lpc,

	gboolean create = FALSE;
	gboolean restart = FALSE;
		/* 子リソースのnext_roleを取り出す */
	enum rsc_role_e lh_role_new = child_rsc->fns->state(child_rsc, FALSE);
		/* 子リソースのroleを取り出す */
	enum rsc_role_e lh_role_old = child_rsc->fns->state(child_rsc, TRUE);
		/* 子リソースのdown_stackフラグに対応したrole(next_role)を取り出す */
	enum rsc_role_e child_role = child_rsc->fns->state(child_rsc, down_stack);

	crm_debug_4("Testing %s->%s for %s: %s vs. %s %s",
		    order->lh_action_task, order->rh_action_task, child_rsc->id,
		    role2text(lh_role), role2text(child_role), order->lh_action_task);
	
	if(rh_hosts == NULL) {
	    	/* thenリソースの配置（もしくは実行）ノード情報が取れない場合は処理をブレーク */
	    crm_debug_3("Terminating search: %s.%d list is empty: no possible %s",
			order->rh_rsc->id, down_stack, reason);
	    break;
	}

	if(lh_role_new == lh_role_old) {
			/* 子リソースのroleとnext_roleが同じ場合 */
			/* restartのアクションがセットされているか確認する */
	    restart = detect_restart(child_rsc);
	    if(restart == FALSE) {
		crm_debug_3("Ignoring %s->%s for %s: no relevant %s (no role change)",
			 order->lh_action_task, order->rh_action_task, child_rsc->id, reason);
				/* restartではない場合は、次の子リソースを処理する */
		continue;
	    }
	}

	hosts = NULL;
		/* 子リソースのdown_stackフラグに対応したノード情報を取得する */
	child_rsc->fns->location(child_rsc, &hosts, down_stack);
		/* 子リソースのノード情報とthen指定リソースのノード情報のandノード情報を生成する */
	intersection = node_list_and(hosts, rh_hosts, FALSE);
	/* slist_iter(h, node_t, hosts, llpc, crm_info("H: %s %s", child_rsc->id, h->details->uname)); */
	if(intersection == NULL) {
			/* andノード情報が取れない場合は、次の子リソースを処理する */
	    crm_debug_3("Ignoring %s->%s for %s: no relevant %s",
		     order->lh_action_task, order->rh_action_task, child_rsc->id, reason);
	    g_list_free(hosts);
	    continue;  
	}
			
	if(restart) {
			/* restartのアクションがセットされている場合 */
	    reason = "restart";
	    create = TRUE;
			    
	} else if(down_stack) {
	    if(lh_role_old > lh_role) {
		create = TRUE;
	    }
			    
	} else if(down_stack == FALSE) {
	    if(lh_role_old < lh_role) {
		create = TRUE;
	    }

	} else {
	    any_ordered++;
	    reason = "role";
	    crm_debug_4("Role: %s->%s for %s: %s vs. %s %s",
		       order->lh_action_task, order->rh_action_task, child_rsc->id,
		       role2text(lh_role_old), role2text(lh_role), order->lh_action_task);
			    
	}

	if(create) {
			/* createフラグがTRUEになった場合 */
	    char *task = order->lh_action_task;

	    any_ordered++;
	    crm_debug("Enforcing %s->%s for %s on %s: found %s",
		      order->lh_action_task, order->rh_action_task, child_rsc->id,
		      ((node_t*)intersection->data)->details->uname, reason);

	    order->lh_action_task = convert_non_atomic_task(task, child_rsc, TRUE, FALSE);
	    child_rsc->cmds->rsc_order_lh(child_rsc, order, data_set);
	    crm_free(order->lh_action_task);
	    order->lh_action_task = task;
	}
			
	crm_debug_3("Processed %s->%s for %s on %s: %s",
		    order->lh_action_task, order->rh_action_task, child_rsc->id,
		    ((node_t*)intersection->data)->details->uname, reason);
	
	/* slist_iter(h, node_t, hosts, llpc, */
	/* 	   crm_info("H: %s %s", child_rsc->id, h->details->uname)); */
			
	slist_destroy(node_t, node, intersection, crm_free(node));
	g_list_free(hosts);
			
	);
	/* then指定リソースからのノード情報リストを解放する */
    g_list_free(rh_hosts);

    if(any_ordered == 0 && down_stack == FALSE) {
	GListPtr lh_hosts = NULL;
	if(order->type & pe_order_runnable_left) {
	    	/* lh_hostsリストに対象リソースのallocated_toリストを取得する */
	    rsc->fns->location(rsc, &lh_hosts, FALSE);
	}
	if(lh_hosts == NULL) {
	    order->lh_action_task = convert_non_atomic_task(order->lh_action_task, rsc, TRUE, TRUE);
	    native_rsc_order_lh(rsc, order, data_set);			
	}
	g_list_free(lh_hosts);
    }
    order->type = pe_order_optional;
}
/* cloneリソースのorderのFirst指定処理 */
void clone_rsc_order_lh(resource_t *rsc, order_constraint_t *order, pe_working_set_t *data_set)
{
	resource_t *r1 = NULL;
	resource_t *r2 = NULL;	
	gboolean do_interleave = FALSE;
	clone_variant_data_t *clone_data = NULL;
	/* clone固有データの取得 */
	get_clone_variant_data(clone_data, rsc);

	crm_debug_4("%s->%s", order->lh_action_task, order->rh_action_task);
	if(order->rh_rsc == NULL) {
	    /* then指定のリソースがない場合 */
	    order->lh_action_task = convert_non_atomic_task(order->lh_action_task, rsc, FALSE, TRUE);
	    native_rsc_order_lh(rsc, order, data_set);
	    return;
	}
	/* ※ここからは、then指定がある場合の処理になる */
	/* 対象リソースの親トップリソースを取りだす */
	r1 = uber_parent(rsc);
	/* then指定リソースの親トップリソースを取り出す */
	r2 = uber_parent(order->rh_rsc);
	
	if(r1 == r2) {
		/* 親トップリソースが同じリソース間のorderの場合 */
		native_rsc_order_lh(rsc, order, data_set);
		return;
	}
	
	if(order->rh_rsc->variant > pe_group && clone_data->interleave) {
		/* thenリソースがclone,masterの場合 */
	    clone_variant_data_t *clone_data_rh = NULL;
	    /* thenリソースのclone固有データを取得する */
	    get_clone_variant_data(clone_data_rh, order->rh_rsc);


	    if(clone_data->clone_node_max == clone_data_rh->clone_node_max) {
		/* only the LHS side needs to be labeled as interleave */
			/* 一致する場合は、do_interleaveフラグをTRUEにセット */
		do_interleave = TRUE;

	    } else {
			/* first指定のclone_node_maxと、then指定のcloneリソースのclone_node_maxが一致しない場合は、エラーログ */
		crm_config_err("Cannot interleave "XML_CIB_TAG_INCARNATION
			       " %s and %s because they do not support the same"
			       " number of resources per node",
			       rsc->id, order->rh_rsc->id);
	    }
	}
	

	if(order->rh_rsc == NULL) {
		/* then指定のリソースがＮＵＬＬの場合 */
		/* ※無駄なきがする？先に判定して処理を抜けているので.... */
	    do_interleave = FALSE;
	}
	
	if(do_interleave) {
		/* do_interleaveフラグがTRUEの場合(cloneリソース間のorderの場合が該当する) */
	    resource_t *lh_child = NULL;
	    resource_t *rh_saved = order->rh_rsc;
	    gboolean current = FALSE;
	    
	    if(strstr(order->lh_action_task, "_stop_0") || strstr(order->lh_action_task, "_demote_0")) {
		current = TRUE;
	    }

	    slist_iter(
		rh_child, resource_t, rh_saved->children, lpc,
		
		CRM_ASSERT(rh_child != NULL);
		lh_child = find_compatible_child(rh_child, rsc, RSC_ROLE_UNKNOWN, current);
		if(lh_child == NULL && current) {
		    continue;
		    
		} else if(lh_child == NULL) {
		    crm_debug("No match found for %s (%d)", rh_child->id, current);

		    /* Me no like this hack - but what else can we do?
		     *
		     * If there is no-one active or about to be active
		     *   on the same node as rh_child, then they must
		     *   not be allowed to start
		     */
		    if(order->type & (pe_order_runnable_left|pe_order_implies_right) /* Mandatory */) {
			crm_info("Inhibiting %s from being active", rh_child->id);
			assign_node(rh_child, NULL, TRUE);
		    }
		    continue;
		}
		crm_debug("Pairing %s with %s", lh_child->id, rh_child->id);
		order->rh_rsc = rh_child;
		lh_child->cmds->rsc_order_lh(lh_child, order, data_set);
		order->rh_rsc = rh_saved;
		);
	    
	} else {
		/* do_interleaveフラグがFALSEの場合(cloneリソースと通常のリソース間のorderの場合が該当する) */
#if 0
	    if(order->type != pe_order_optional) {
		crm_debug("Upgraded ordering constraint %d - 0x%.6x", order->id, order->type);
		native_rsc_order_lh(rsc, order, data_set);
	    }
#endif

	    if(order->rh_rsc->variant < pe_clone) {
			/* then指定のリソースがgroup,prmitiveリソースの場合 */
		clone_rsc_order_lh_non_clone(rsc, order, data_set);		
		    
	    } else if(order->type & pe_order_implies_left) {
		if(rsc->variant == order->rh_rsc->variant) {
		    crm_debug_2("Clone-to-clone ordering: %s -> %s 0x%.6x",
				order->lh_action_task, order->rh_action_task, order->type);
		    /* stop instances on the same nodes as stopping RHS instances */
		    slist_iter(
			child_rsc, resource_t, rsc->children, lpc,
			native_rsc_order_lh(child_rsc, order, data_set);
			);
		} else {
		    /* stop everything */
		    slist_iter(
			child_rsc, resource_t, rsc->children, lpc,
			native_rsc_order_lh(child_rsc, order, data_set);
			);
		}
	    }
	}	
	
	if(do_interleave == FALSE || clone_data->ordered) {
	    order->lh_action_task = convert_non_atomic_task(order->lh_action_task, rsc, FALSE, TRUE);
	    native_rsc_order_lh(rsc, order, data_set);
	}	    
	
	if(is_set(rsc->flags, pe_rsc_notify)) {
	    order->type = pe_order_optional;
	    order->lh_action_task = convert_non_atomic_task(order->lh_action_task, rsc, TRUE, TRUE);
	    native_rsc_order_lh(rsc, order, data_set);
	}
}

static void clone_rsc_order_rh_non_clone(
    resource_t *lh_p, action_t *lh_action, resource_t *rsc, order_constraint_t *order)
{
    GListPtr lh_hosts = NULL;
    GListPtr intersection = NULL;
    const char *reason = "unknown";

    gboolean restart = FALSE;
    gboolean down_stack = TRUE;

    enum rsc_role_e rh_role = RSC_ROLE_STARTED;
    enum action_tasks task = start_rsc;

    enum rsc_role_e lh_role_new = lh_p->fns->state(lh_p, FALSE);
    enum rsc_role_e lh_role_old = lh_p->fns->state(lh_p, TRUE);

    /* Make sure the pre-req will be active */
    if(order->type & pe_order_runnable_left) {
	lh_p->fns->location(lh_p, &lh_hosts, FALSE);
	if(lh_hosts == NULL) {
	    crm_debug("Terminating search: Pre-requisite %s of %s is unrunnable", lh_p->id, rsc->id);
	    native_rsc_order_rh(lh_action, rsc, order);
	    return;
	}
	g_list_free(lh_hosts); lh_hosts = NULL;
    }

    if(strstr(order->lh_action_task, "_"RSC_STOP"_0")
       || strstr(order->lh_action_task, "_"RSC_STOPPED"_0")) {
	task = stop_rsc;
	reason = "down activity";
	rh_role = RSC_ROLE_STOPPED;
	lh_p->fns->location(lh_p, &lh_hosts, down_stack);

/* These actions are not possible for non-clones
   } else if(strstr(order->lh_action_task, "_"RSC_DEMOTE"_0")
   || strstr(order->lh_action_task, "_"RSC_DEMOTED"_0")) {
   task = action_demote;
   rh_role = RSC_ROLE_SLAVE;
   reason = "demotion activity";
   lh_p->fns->location(lh_p, &lh_hosts, down_stack);
		
   } else if(strstr(order->lh_action_task, "_"RSC_PROMOTE"_0")
   || strstr(order->lh_action_task, "_"RSC_PROMOTED"_0")) {
   task = action_promote;
   down_stack = FALSE;
   reason = "promote activity";
   lh_p->fns->location(lh_p, &lh_hosts, down_stack);
   rh_role = RSC_ROLE_MASTER;
*/
    } else if(strstr(order->lh_action_task, "_"RSC_START"_0")
	      || strstr(order->lh_action_task, "_"RSC_STARTED"_0")) {
	task = start_rsc;
	down_stack = FALSE;
	reason = "up activity";
	lh_p->fns->location(lh_p, &lh_hosts, down_stack);

    } else {
	crm_err("Unknown action: %s", order->lh_action_task);
	return;
    }
	    
    if(lh_role_new == lh_role_old) {
	restart = detect_restart(lh_action->rsc);
		
	if(FALSE && restart == FALSE) {
	    crm_debug_3("Ignoring %s->%s for %s: no relevant %s (no role change)",
			lh_action->task, order->lh_action_task, lh_p->id, reason);
	    goto cleanup;
	}
    }

    /* slist_iter(h, node_t, lh_hosts, llpc, crm_info("LHH: %s", h->details->uname)); */
    slist_iter(
	child_rsc, resource_t, rsc->children, lpc,

	gboolean create = FALSE;
	enum rsc_role_e child_role = child_rsc->fns->state(child_rsc, down_stack);

	crm_debug_4("Testing %s->%s for %s: %s vs. %s %s",
		    lh_action->task, order->lh_action_task, child_rsc->id,
		    role2text(rh_role), role2text(child_role), order->lh_action_task);
	
	if(lh_hosts == NULL) {
	    crm_debug_3("Terminating search: %s.%d list is empty: no possible %s",
			order->rh_rsc->id, down_stack, reason);
	    break;
	}

	intersection = NULL;
	if(task == stop_rsc) {
	    /* Only relevant for stopping stacks */

	    GListPtr hosts = NULL;
	    child_rsc->fns->location(child_rsc, &hosts, down_stack);
	    intersection = node_list_and(hosts, lh_hosts, FALSE);
	    g_list_free(hosts);
	    if(intersection == NULL) {
		crm_debug_3("Ignoring %s->%s for %s: no relevant %s",
			    lh_action->task, order->lh_action_task, child_rsc->id, reason);
		continue;
	    }
	}
	
	/* slist_iter(h, node_t, hosts, llpc, crm_info("H: %s %s", child_rsc->id, h->details->uname)); */
	if(restart) {
	    reason = "restart";
	    create = TRUE;
			    
	} else if(down_stack && lh_role_old >= rh_role) {
	    create = TRUE;
		    
	} else if(down_stack == FALSE && lh_role_old <= rh_role) {
	    create = TRUE;
		    
	} else {
	    reason = "role";
	}

	if(create) {
	    enum pe_ordering type = order->type;
	    child_rsc->cmds->rsc_order_rh(lh_action, child_rsc, order);
	    order->type = pe_order_optional;
	    native_rsc_order_rh(lh_action, rsc, order);
	    order->type = type;
	}
	
	crm_debug_3("Processed %s->%s for %s: found %s%s",
		    lh_action->task, order->lh_action_task, child_rsc->id, reason, create?" - enforced":"");
	
	/* slist_iter(h, node_t, hosts, llpc, */
	/* 	   crm_info("H: %s %s", child_rsc->id, h->details->uname)); */
		
	slist_destroy(node_t, node, intersection, crm_free(node));
	);
  cleanup:	    
    g_list_free(lh_hosts);
}
/* cloneリソースのorderのThen指定処理 */
void clone_rsc_order_rh(
	action_t *lh_action, resource_t *rsc, order_constraint_t *order)
{
	enum pe_ordering type = order->type;
	clone_variant_data_t *clone_data = NULL;
	/* first指定リソースの親トップリソースを検索する */
	resource_t *lh_p = uber_parent(lh_action->rsc);
	/* clone固有データを取得する */
	get_clone_variant_data(clone_data, rsc);
	crm_debug_2("%s->%s", order->lh_action_task, order->rh_action_task);

	if(safe_str_eq(CRM_OP_PROBED, lh_action->uuid)) {
		/* first指定が"probe_complete"の場合 */
	    /* 対象cloneリソースの全ての子リソースを処理する */
	    slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
			/* 子リソースのThen指定処理を実行する */
		child_rsc->cmds->rsc_order_rh(lh_action, child_rsc, order);
		);

	    if(rsc->fns->state(rsc, TRUE) < RSC_ROLE_STARTED
		&& rsc->fns->state(rsc, FALSE) > RSC_ROLE_STOPPED) {
			/* 対象cloneリソースのroleがRSC_ROLE_STARTEDでもなくて */
			/* next_roleがRSC_ROLE_STOPPED以上の場合 */
			/* typeにpe_order_implies_rightのＯＲ値をセットする */
		order->type |= pe_order_implies_right;
	    }

	} else if(lh_p && lh_p != rsc && lh_p->variant < pe_clone) {
	    /* first指定が"probe_complete"以外の場合で、親トップリソースが存在して、primitive,groupリソースの場合 */
	    /* clone_rsc_order_rh_non_clone処理を実行する */
	    clone_rsc_order_rh_non_clone(lh_p, lh_action, rsc, order);
	    return;
	}
	/* 上記のifのreturnで抜けない場合は、native_rsc_order_rh処理を実行する */
 	native_rsc_order_rh(lh_action, rsc, order);
	order->type = type;
}
/* クローンリソース、Masterリソースの共通で、locationルールを対象リソースに反映する */
void clone_rsc_location(resource_t *rsc, rsc_to_node_t *constraint)
{
	clone_variant_data_t *clone_data = NULL;
	get_clone_variant_data(clone_data, rsc);

	crm_debug_3("Processing location constraint %s for %s",
		    constraint->id, rsc->id);
	/* クローンリソースのallowed_nodesリストにlocationルールの適用可能なノードリストのスコアを反映する */
	/* ※これにより、クローンリソースのallowed_nodesリストは、locationルールのノード毎のスコアが反映されたallowed_nodesリストになっている */
	native_rsc_location(rsc, constraint);
	/* クローンリソースの全ての子リソースを処理する */
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,
		/* 子リソースのallowed_nodesリストにlocationルールのノード毎のスコアを反映する */
		child_rsc->cmds->rsc_location(child_rsc, constraint);
		);
}

/* cloneリソースをgraph展開する */
void clone_expand(resource_t *rsc, pe_working_set_t *data_set)
{
	clone_variant_data_t *clone_data = NULL;
	get_clone_variant_data(clone_data, rsc);

	crm_debug_2("Processing actions from %s", rsc->id);
	
	if(clone_data->start_notify) {
	    collect_notification_data(rsc, TRUE, TRUE, clone_data->start_notify);
	    expand_notification_data(clone_data->start_notify);
	    create_notifications(rsc, clone_data->start_notify, data_set);
	}

	if(clone_data->stop_notify) {
	    collect_notification_data(rsc, TRUE, TRUE, clone_data->stop_notify);
	    expand_notification_data(clone_data->stop_notify);
	    create_notifications(rsc, clone_data->stop_notify, data_set);
	}
	
	if(clone_data->promote_notify) {
	    collect_notification_data(rsc, TRUE, TRUE, clone_data->promote_notify);
	    expand_notification_data(clone_data->promote_notify);
	    create_notifications(rsc, clone_data->promote_notify, data_set);
	}
	
	if(clone_data->demote_notify) {
	    collect_notification_data(rsc, TRUE, TRUE, clone_data->demote_notify);
	    expand_notification_data(clone_data->demote_notify);
	    create_notifications(rsc, clone_data->demote_notify, data_set);
	}
	
	/* Now that the notifcations have been created we can expand the children */
	/* cloneリソースの全ての子リソースを処理する */
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,		
		/* 子リソースのgraph展開処理を実行する */		
		child_rsc->cmds->expand(child_rsc, data_set));
	/* ※すでに前の子リソース処理で処理されている子リソースのアクションの場合は、dumpがTRUEになっているので */
	/*   処理済みの為、native_expandでは再処理されない */
	/* ※なぜ、こういう作りになっているのかは不明... */
	native_expand(rsc, data_set);

	/* The notifications are in the graph now, we can destroy the notify_data */
	free_notification_data(clone_data->demote_notify);  clone_data->demote_notify = NULL;
	free_notification_data(clone_data->stop_notify);    clone_data->stop_notify = NULL;
	free_notification_data(clone_data->start_notify);   clone_data->start_notify = NULL;
	free_notification_data(clone_data->promote_notify); clone_data->promote_notify = NULL;
}


static gint sort_rsc_id(gconstpointer a, gconstpointer b)
{
	const resource_t *resource1 = (const resource_t*)a;
	const resource_t *resource2 = (const resource_t*)b;

	CRM_ASSERT(resource1 != NULL);
	CRM_ASSERT(resource2 != NULL);

	return strcmp(resource1->id, resource2->id);
}

static resource_t *find_instance_on(resource_t *rsc, node_t *node)
{
 	/* 全ての子リソースを処理する */
   slist_iter(child, resource_t, rsc->children, lpc,
	       GListPtr known_list = NULL;
	       /* 子リソースのリソース情報のknown_onリストを取得する(statusがUNKNOWNでないノードリスト) */
	       rsc_known_on(child, &known_list); 
	       /* 取得したknown_onリストを全て処理する */
	       slist_iter(known, node_t, known_list, lpc2,
			  if(node->details == known->details) {
				  /* known_onリストのノード情報と対象ノード情報が一致した場合は、子リソース情報を返す */
			      g_list_free(known_list);
			      return child;
			  }
		   );
		   /* 作業リストを解放する */
	       g_list_free(known_list);	       
	);
	/* 一致しない場合は、NULLを返す */
    return NULL;
}
/* クローンリソースのProbe処理 */
gboolean
clone_create_probe(resource_t *rsc, node_t *node, action_t *complete,
		    gboolean force, pe_working_set_t *data_set) 
{
	gboolean any_created = FALSE;
	clone_variant_data_t *clone_data = NULL;
	/* クローン固有データを取得する */
	get_clone_variant_data(clone_data, rsc);
	/* クローンの子リソースをrsc_idでソートする */
	rsc->children = g_list_sort(rsc->children, sort_rsc_id);
	if(rsc->children == NULL) {
	    pe_warn("Clone %s has no children", rsc->id);
	    return FALSE;
	}
	
	if(is_not_set(rsc->flags, pe_rsc_unique)
	   && clone_data->clone_node_max == 1) {
		/* only look for one copy */	 
		resource_t *child = NULL;

		/* Try whoever we probed last time */
		child = find_instance_on(rsc, node);
		if(child) {
		    return child->cmds->create_probe(
			child, node, complete, force, data_set);
		}

		/* Try whoever we plan on starting there */
		slist_iter(	 
			child_rsc, resource_t, rsc->children, lpc,	 

			node_t *local_node = child_rsc->fns->location(child_rsc, NULL, FALSE);
			if(local_node == NULL) {
			    continue;
			}
			
			if(local_node->details == node->details) {
			    return child_rsc->cmds->create_probe(
				child_rsc, node, complete, force, data_set);
			}
		    );

		/* Fall back to the first clone instance */
		child = rsc->children->data;
		return child->cmds->create_probe(child, node, complete, force, data_set);
	}
	slist_iter(
		child_rsc, resource_t, rsc->children, lpc,

		if(child_rsc->cmds->create_probe(
			   child_rsc, node, complete, force, data_set)) {
			any_created = TRUE;
		}
		
		if(any_created
		   && is_not_set(rsc->flags, pe_rsc_unique)
		   && clone_data->clone_node_max == 1) {
			/* only look for one copy (clone :0) */	 
			break;
		}
		);

	return any_created;
}
/* metaハッシュテーブルから"値を取り出して、xmlデータに追加する */
void clone_append_meta(resource_t *rsc, xmlNode *xml)
{
    char *name = NULL;
    clone_variant_data_t *clone_data = NULL;
    /* clone固有データを取得する */
    get_clone_variant_data(clone_data, rsc);

    /* "globally-unique"値をリソース情報のpe_rsc_unique値からtrue,falseでxmlデータにセットする */
    name = crm_meta_name(XML_RSC_ATTR_UNIQUE);
    crm_xml_add(xml, name, is_set(rsc->flags, pe_rsc_unique)?"true":"false");
    crm_free(name);
    /* "notify"値をリソース情報のpe_rsc_notify値からtrue,falseでxmlデータにセットする */
    name = crm_meta_name(XML_RSC_ATTR_NOTIFY);
    crm_xml_add(xml, name, is_set(rsc->flags, pe_rsc_notify)?"true":"false");
    crm_free(name);
    /* リソース情報のmetaハッシュテーブルから"clone-max"値を取り出す */
    name = crm_meta_name(XML_RSC_ATTR_INCARNATION_MAX);
    crm_xml_add_int(xml, name, clone_data->clone_max);
    crm_free(name);
    /* リソース情報のmetaハッシュテーブルから"clone-node-max"値を取り出す */
    name = crm_meta_name(XML_RSC_ATTR_INCARNATION_NODEMAX);
    /* "clone-node-max"値を取得した値からxmlデータにセットする */
    crm_xml_add_int(xml, name, clone_data->clone_node_max);
    crm_free(name);
}
