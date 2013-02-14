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

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>


#include <glib.h>

#include <crm/pengine/status.h>
#include <pengine.h>
#include <allocate.h>
#include <utils.h>
#include <crm/pengine/rules.h>
#include <lib/pengine/utils.h>

#define EXPAND_CONSTRAINT_IDREF(__set, __rsc, __name) do {				\
	__rsc = pe_find_resource(data_set->resources, __name);		\
	if(__rsc == NULL) {						\
	    crm_config_err("%s: No resource found for %s", __set, __name); \
	    return FALSE;						\
	}								\
    } while(0)
/* 受信したxmlのconstraintsノードを展開する */
gboolean 
unpack_constraints(xmlNode * xml_constraints, pe_working_set_t *data_set)
{
	xmlNode *lifetime = NULL;
	/* constraintsノード内の全てのノードを処理する */
	xml_child_iter(
		xml_constraints, xml_obj, 
		/* 子タグのidエレメントを取り出す */
		const char *id = crm_element_value(xml_obj, XML_ATTR_ID);
		if(id == NULL) {
			/* idが設定されていない場合は、エラーログを出力して次の子タグを処理する */
			crm_config_err("Constraint <%s...> must have an id",
				crm_element_name(xml_obj));
			continue;
		}

		crm_debug_3("Processing constraint %s %s",
			    crm_element_name(xml_obj),id);
		/* 子タグ内にlifetimeの子タグを含んでいる場合は取り出す */
		lifetime = first_named_child(xml_obj, "lifetime");
		if(lifetime) {
			/* lifetimeが入っている場合は、WARNログを出力する */
			/* ※確か古いバージョンにはあったパラメータだったと思う */
		    crm_config_warn("Support for the lifetime tag, used by %s, is deprecated."
				    " The rules it contains should instead be direct decendants of the constraint object", id);
		}
		
		/* lifetimeが適用可能かチェックする(現在はTRUE) */
		if(test_ruleset(lifetime, NULL, data_set->now) == FALSE) {
			crm_info("Constraint %s %s is not active",
				 crm_element_name(xml_obj), id);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_ORDER,
				      crm_element_name(xml_obj))) {
			/* "rsc_order"ノードを展開する */
			unpack_rsc_order(xml_obj, data_set);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_DEPEND,
				      crm_element_name(xml_obj))) {
			/* "rsc_colocation"ノードを展開する */
			unpack_rsc_colocation(xml_obj, data_set);

		} else if(safe_str_eq(XML_CONS_TAG_RSC_LOCATION,
				      crm_element_name(xml_obj))) {
			/* "rsc_location"ノードを展開する */
			unpack_rsc_location(xml_obj, data_set);

		} else {
			pe_err("Unsupported constraint type: %s",
				crm_element_name(xml_obj));
		}
		);

	return TRUE;
}
/* actionの逆になるアクションを返す処理 */
static const char *
invert_action(const char *action) 
{
	if(safe_str_eq(action, RSC_START)) {
		return RSC_STOP;

	} else if(safe_str_eq(action, RSC_STOP)) {
		return RSC_START;
		
	} else if(safe_str_eq(action, RSC_PROMOTE)) {
		return RSC_DEMOTE;
		
 	} else if(safe_str_eq(action, RSC_DEMOTE)) {
		return RSC_PROMOTE;

	} else if(safe_str_eq(action, RSC_PROMOTED)) {
		return RSC_DEMOTED;
		
	} else if(safe_str_eq(action, RSC_DEMOTED)) {
		return RSC_PROMOTED;

	} else if(safe_str_eq(action, RSC_STARTED)) {
		return RSC_STOPPED;
		
	} else if(safe_str_eq(action, RSC_STOPPED)) {
		return RSC_STARTED;
	}
	/* その他の場合は、NULL */
	crm_config_warn("Unknown action: %s", action);
	return NULL;
}

static gboolean
contains_stonith(resource_t *rsc)
{
    GListPtr gIter = rsc->children;

    if(gIter == FALSE) {
	const char *class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);
	if(safe_str_eq(class, "stonith")) {
	    return TRUE;
	}
    }
    
    for(; gIter != NULL; gIter = gIter->next) {
	resource_t *child = (resource_t*)gIter->data;
	if(contains_stonith(child)) {
	    return TRUE;
	}
    }
    return FALSE;
}
/* 複雑でない1つのrsc_orderを展開する(現状はこれ） */
static gboolean
unpack_simple_rsc_order(xmlNode * xml_obj, pe_working_set_t *data_set)
{
	int score_i = 0;
	int order_id = 0;
	resource_t *rsc_then = NULL;
	resource_t *rsc_first = NULL;
	gboolean invert_bool = TRUE;
	enum pe_ordering cons_weight = pe_order_optional;

	const char *id_first  = NULL;
	const char *id_then  = NULL;
	const char *action_then = NULL;
	const char *action_first = NULL;
	/* rsc_orderタグからid, score, symmetrical属性の値を取り出す */
	const char *id     = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *score  = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
	const char *invert = crm_element_value(xml_obj, XML_CONS_ATTR_SYMMETRICAL);
	/* symmetricalをbool化する */
	crm_str_to_boolean(invert, &invert_bool);
	
	if(xml_obj == NULL) {
		/* rsc_orderタグが空の場合は、処理しない */
		crm_config_err("No constraint object to process.");
		return FALSE;

	} else if(id == NULL) {
		/* id属性が取れない場合も処理しない */
		crm_config_err("%s constraint must have an id",
			crm_element_name(xml_obj));
		return FALSE;		
	}
	/* rsc_orderタグから then, first属性の値を取り出す */
	id_then  = crm_element_value(xml_obj, XML_ORDER_ATTR_THEN);
	id_first = crm_element_value(xml_obj, XML_ORDER_ATTR_FIRST);
	/* rsc_orderタグから then-action, first-action属性の値を取り出す */
	action_then  = crm_element_value(xml_obj, XML_ORDER_ATTR_THEN_ACTION);
	action_first = crm_element_value(xml_obj, XML_ORDER_ATTR_FIRST_ACTION);

	if(action_first == NULL) {
		/* first-action属性が取れない場合は、action_firstにRSC_STARTをセット */
	    action_first = RSC_START;
	}
	if(action_then == NULL) {
		/* then-action属性が取れない場合は、action_thenにaction_firstをセット */
	    action_then = action_first;
	}

	if(id_then == NULL || id_first == NULL) {
		/* then, first属性のどちらかの値が取れない場合は処理しない */
		crm_config_err("Constraint %s needs two sides lh: %s rh: %s",
			      id, crm_str(id_then), crm_str(id_first));
		return FALSE;
	}	
	/* 状態遷移作業エリアのリソース情報リストから、then属性、first属性に指定された */
	/* リソース情報を取りだす */
	rsc_then = pe_find_resource(data_set->resources, id_then);
	rsc_first = pe_find_resource(data_set->resources, id_first);

	if(rsc_then == NULL) {
		/* then属性のリソース情報が存在しない場合は処理しない */
		crm_config_err("Constraint %s: no resource found for name '%s'", id, id_then);
		return FALSE;
	
	} else if(rsc_first == NULL) {
		/* first属性のリソース情報が存在しない場合は処理しない */
		crm_config_err("Constraint %s: no resource found for name '%s'", id, id_first);
		return FALSE;
	}

	if(score == NULL && rsc_then->variant == pe_native && rsc_first->variant > pe_group) {
	    /* score属性が指定なしで、then属性のリソースがnativeで、 */
	    /* first属性のリソースがpe_cloneか、pe_masterの場合は、scoreを０で処理する */
	    /* ※firstリソースがclone,masterの後にnativeがthen指定されている場合 */
	    score = "0";

	} else if(score == NULL) {
		/* その他の場合で、score指定がない場合は、socreをINFINITYで処理する */
	    score = "INFINITY";
	}

	if(safe_str_eq(action_first, RSC_STOP) && contains_stonith(rsc_then)) {
		if(contains_stonith(rsc_first) == FALSE) {
			crm_config_err("Constraint %s: Ordering STONITH resource (%s) to stop before %s is illegal",
				   id, rsc_first->id, rsc_then->id);
		}
		return FALSE;
	}
	/* scoreを数値化する */
	/* ※orderタグのscoreについては数値指定できるが、実際は */
	/*		0, マイナス値、プラス値によって、cons_weight値を設定しているだけである */
	/*   内部でのorder生成では、全くスコアではなく、cons_weightの設定enumを利用して処理している */
	/*   よって、colocationの用に配置に対して、反映されるスコア値ではない */
	score_i = char2score(score);
	/* cons_weightをpe_order_optionalで初期化 */
	cons_weight = pe_order_optional;
	if(score_i == 0 && rsc_then->restart_type == pe_restart_restart) {
		/* スコアが０で、then属性のリソースのrestart_typeがpe_restart_restartの場合 */
		crm_debug_2("Upgrade : recovery - implies right");
 		cons_weight |= pe_order_implies_right;
	}
	
	if(score_i < 0) {
		/* スコアが０以下の場合 */
		crm_debug_2("Upgrade : implies left");
 		cons_weight |= pe_order_implies_left;

	} else if(score_i > 0) {
		/* スコアが０以上の場合 */
		crm_debug_2("Upgrade : implies right");
 		cons_weight |= pe_order_implies_right;
		if(safe_str_eq(action_then, RSC_START)
		   || safe_str_eq(action_then, RSC_PROMOTE)) {
			/* action_thenがRSC_STARTか、RSC_PROMOTEの場合 */
			crm_debug_2("Upgrade : runnable");
			cons_weight |= pe_order_runnable_left;
		}
	}
	/* order情報を生成する(状態遷移作業エリアのordering_constraintsリストに追加) */
	order_id = new_rsc_order(rsc_first, action_first, rsc_then, action_then, cons_weight, data_set);

	crm_debug_2("order-%d (%s): %s_%s before %s_%s flags=0x%.6x",
		    order_id, id, rsc_first->id, action_first, rsc_then->id, action_then,
		    cons_weight);
	
	
	if(invert_bool == FALSE) {
		/* symmetrical属性がFALSE指定の場合はここでTRUEリターン */
		return TRUE;
	}
	/* symmetricalがTRUEの場合は、*/
	/* then-action, first-actionエレメントの逆のアクションを取得する */
	action_then = invert_action(action_then);
	action_first = invert_action(action_first);
	/* 以降で、逆のorder情報を生成しているはず... */
	/* ※後で確認予定 */
	if(safe_str_eq(action_first, RSC_STOP) && contains_stonith(rsc_then)) {
	    if(contains_stonith(rsc_first) == FALSE) {
		crm_config_err("Constraint %s: Ordering STONITH resource (%s) to stop before %s is illegal",
		       id, rsc_first->id, rsc_then->id);
	    }
	    return FALSE;
	}

	cons_weight = pe_order_optional;
	if(score_i == 0 && rsc_then->restart_type == pe_restart_restart) {
		crm_debug_2("Upgrade : recovery - implies left");
 		cons_weight |= pe_order_implies_left;
	}
	
	score_i *= -1;
	if(score_i < 0) {
		crm_debug_2("Upgrade : implies left");
 		cons_weight |= pe_order_implies_left;
		if(safe_str_eq(action_then, RSC_DEMOTE)) {
			crm_debug_2("Upgrade : demote");
			cons_weight |= pe_order_demote;
		}
		
	} else if(score_i > 0) {
		crm_debug_2("Upgrade : implies right");
 		cons_weight |= pe_order_implies_right;
		if(safe_str_eq(action_then, RSC_START)
		   || safe_str_eq(action_then, RSC_PROMOTE)) {
			crm_debug_2("Upgrade : runnable");
			cons_weight |= pe_order_runnable_left;
		}
	}

	if(action_then == NULL || action_first == NULL) {
		crm_config_err("Cannot invert rsc_order constraint %s."
			       " Please specify the inverse manually.", id);
		return TRUE;
	}
	/* order情報を生成する(状態遷移作業エリアのordering_constraintsリストに追加) */
	order_id = new_rsc_order(
	    rsc_then, action_then, rsc_first, action_first, cons_weight, data_set);
	crm_debug_2("order-%d (%s): %s_%s before %s_%s flags=0x%.6x",
		    order_id, id, rsc_then->id, action_then, rsc_first->id, action_first,
		    cons_weight);
	
	return TRUE;
}
/* "rsc_location"ノードを展開する */
gboolean
unpack_rsc_location(xmlNode * xml_obj, pe_working_set_t *data_set)
{
	gboolean empty = TRUE;
	/* rsc_locationノードのrsc属性,id属性,score属性,node属性を取り出す */
	/* 取り出したrsc属性からdata_setに展開されたresourcesリストを検索ひ、resource情報のポインタを取得する */
	const char *id_lh   = crm_element_value(xml_obj, "rsc");
	const char *id      = crm_element_value(xml_obj, XML_ATTR_ID);
	resource_t *rsc_lh  = pe_find_resource(data_set->resources, id_lh);
	const char *node    = crm_element_value(xml_obj, "node");
	const char *score   = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
	
	if(rsc_lh == NULL) {
		/* only a warn as BSC adds the constraint then the resource */
		/* location指定されたリソースが存在しない場合は警告ログを出力して、FALSE */
		crm_config_warn("No resource (con=%s, rsc=%s)", id, id_lh);
		return FALSE;
	}

	if(node != NULL && score != NULL) {
		/* rsc_locationにnode,score指定されている場合は、スコアを数値化する */
	    int score_i = char2score(score);
	    /* 状態遷移作業エリアのノード情報から該当するノード情報を取り出す */
	    node_t *match = pe_find_node(data_set->nodes, node);

	    if(match) {
			/* rsc_location情報を
					状態遷移作業エリアのplacement_constraintsリスト
					対象リソースのrsc_locationリスト
				にセットする
			*/
		rsc2node_new(id, rsc_lh, score_i, match, data_set);
		return TRUE;
	    } else {
			/* ノード情報が存在しない場合は、このrsc_locationは処理しない */
		return FALSE;
	    }
	}
	/* rsc_locationタグ内の全てのrule子タグを処理する */
	xml_child_iter_filter(
		xml_obj, rule_xml, XML_TAG_RULE,
		empty = FALSE;
		crm_debug_2("Unpacking %s/%s", id, ID(rule_xml));
		/*----------------------------------------*/
		/* rule子タグから、location情報を生成する */
		/*----------------------------------------*/
		generate_location_rule(rsc_lh, rule_xml, data_set);
		);

	if(empty) {
		/* nodeとsocreがなくてrule子タグがないrsc_locationがあった場合は、エラーログを出力する */
		crm_config_err("Invalid location constraint %s:"
			      " rsc_location must contain at least one rule",
			      ID(xml_obj));
	}
	return TRUE;
}

static int
get_node_score(const char *rule, const char *score, gboolean raw, node_t *node)
{
	int score_f = 0;
	if(score == NULL) {
		/* score指定がない場合は、エラーログを出力して０でリターン */
		pe_err("Rule %s: no score specified.  Assuming 0.", rule);
	
	} else if(raw) {
		/* raw指定がTRUEの場合は、sore値を数値に変換して、リターン */
		/* ※通常のruleではこちらが採用 */
		/* score-attributeなど特殊な設定の場合は、raw指定はFALSE */
		score_f = char2score(score);
	
	} else {
		/* raw指定がFALSE....すなわち */
		/* score-attributeかcore-attribute-mangledがる場合には */
		/* ノード情報のdetailsのattrsハッシュテーブルから、score値を取り出す */
		const char *attr_score = g_hash_table_lookup(
			node->details->attrs, score);
		if(attr_score == NULL) {
			/* 取り出せない場合は、-INFINITYでリターン */
			crm_debug("Rule %s: node %s did not have a value for %s",
				  rule, node->details->uname, score);
			score_f = -INFINITY;
			
		} else {
			/* 取り出した場合は、attrsハッシュテーブルの値を数値化してリターン */
			crm_debug("Rule %s: node %s had value %s for %s",
				  rule, node->details->uname, attr_score, score);
			score_f = char2score(attr_score);
		}
	}
	return score_f;
}

/* １つのrsc_locationタグ内のruleタグを処理して、location情報を生成する処理 */
rsc_to_node_t *
generate_location_rule(
	resource_t *rsc, xmlNode *rule_xml, pe_working_set_t *data_set)
{	
	const char *rule_id = NULL;
	const char *score   = NULL;
	const char *boolean = NULL;
	const char *role    = NULL;

	GListPtr match_L  = NULL;
	
	int score_f   = 0;
	gboolean do_and = TRUE;
	gboolean accept = TRUE;
	gboolean raw_score = TRUE;
	
	rsc_to_node_t *location_rule = NULL;
	
	/* IDRER設定も含めて、ruleのタグを取得する */
	/* ※IDREFの場合は、参照先からruleタグが取られることになる */
	rule_xml = expand_idref(rule_xml, data_set->input);
	/* ruleタグのid, boolean-op, role属性の値を取り出す */
	rule_id = crm_element_value(rule_xml, XML_ATTR_ID);
	boolean = crm_element_value(rule_xml, XML_RULE_ATTR_BOOLEAN_OP);
	role = crm_element_value(rule_xml, XML_RULE_ATTR_ROLE);

	crm_debug_2("Processing rule: %s", rule_id);

	if(role != NULL && text2role(role) == RSC_ROLE_UNKNOWN) {
		/* roleが設定されていて、RSC_ROLE_UNKNOWNの場合は、処理しない */
		pe_err("Bad role specified for %s: %s", rule_id, role);
		return NULL;
	}
	/* ruleタグのscoreエレメントの値を取り出す */
	score = crm_element_value(rule_xml, XML_RULE_ATTR_SCORE);
	if(score != NULL) {
		/* scoreが取れた場合は、数値化する */
		score_f = char2score(score);
		
	} else {
		/* scoreが取れない場合は、ruleタグ内のscore-attributeエレメントを取り出す */
		score = crm_element_value(
			rule_xml, XML_RULE_ATTR_SCORE_ATTRIBUTE);
		if(score == NULL) {
			/* score-attributeが取り出せない場合は、score-attribute-mangledエレメントを取り出す */
			score = crm_element_value(
				rule_xml, XML_RULE_ATTR_SCORE_MANGLED);
		}
		if(score != NULL) {
			/* 取り出せた場合は、raw_scoreフラグをFALSEにセットする */
			/* score-attributeかcore-attribute-mangledを採用した場合には、raw_coreはFALSEになる */
			raw_score = FALSE;
		}
	}
	if(safe_str_eq(boolean, "or")) {
		/* 取り出した ruleタグのboolean-opエレメントが"or"の場合は、do_endフラグをFALSEにセットする */
		/* ※orによる連結のrule処理 */
		do_and = FALSE;
	}
	/* rsc_location情報を
		状態遷移作業エリアのplacement_constraintsリスト
		対象リソースのrsc_locationリスト
		にセットする
	*/
	location_rule = rsc2node_new(rule_id, rsc, 0, NULL, data_set);
	
	if(location_rule == NULL) {
		/* location情報エリアが確保出来ない場合は、NULL */
		return NULL;
	}
	if(role != NULL) {
		crm_debug_2("Setting role filter: %s", role);
		location_rule->role_filter = text2role(role);
		if(location_rule->role_filter == RSC_ROLE_SLAVE) {
		    /* Fold slave back into Started for simplicity
		     * At the point Slave location constraints are evaluated,
		     * all resources are still either stopped or started
		     */  
		    location_rule->role_filter = RSC_ROLE_STARTED;
		}
	}
	if(do_and) {
		/* "or"がなくなって、最後のruleの場合 */
		/* -INFINTYのweightとなっているノードも含めて、状態遷移作業エリアのノード情報から */
		/* 全てのノード情報リストをweightを０にリセットした状態で作業用ノードリストに取得する */
		match_L = node_list_dup(data_set->nodes, TRUE, FALSE);
		/* 取得したリストの全てのノード情報を処理する */
		slist_iter(
			node, node_t, match_L, lpc,
			/* ノード情報のweightに処理対象のruleのスコアをセット */
			/* これによって、orが終わった時点で */
			node->weight = get_node_score(rule_id, score, raw_score, node);
			);
	}
	/* １つのruleを処理する都度、全ての状態遷移作業エリアのノード情報分処理する */
	slist_iter(
	    node, node_t, data_set->nodes, lpc,
			/* 処理対象のruleが適用可能なルールか判定する */
			accept = test_rule(
			    rule_xml, node->details->attrs, RSC_ROLE_UNKNOWN, data_set->now);

			crm_debug_2("Rule %s %s on %s", ID(rule_xml), accept?"passed":"failed", node->details->uname);
			/* 処理対象のruleからスコア値を取り出す */
			score_f = get_node_score(rule_id, score, raw_score, node);
/* 			if(accept && score_f == -INFINITY) { */
/* 				accept = FALSE; */
/* 			} */
			
			if(accept) {
				/* 処理対象のruleが対象ノードに適用可能なruleの場合 */
				/* 作業用ノードリストから、対象ノードを検索する */
				node_t *local = pe_find_node_id(
					match_L, node->details->id);
				if(local == NULL && do_and) {
					/* ノードが存在しない場合で、最後の場合は処理しない */
					/* 適用可能なruleだが、最後までruleには、作業用ノードリストは存在していないということ */
					continue;
					
				} else if(local == NULL) {
					/* ノードが存在しない場合で最後でない場合は、作業用ノードリストにそのノードを追加する */
					/* 適用可能なruleだが、作業用ノードリストは存在していないということ */
					local = node_copy(node);
					match_L = g_list_append(match_L, local);
				}

				if(do_and == FALSE) {
					/* 最後でない場合は、ノードのweightに処理対象のruleのスコアを加算する */
					local->weight = merge_weights(
						local->weight, score_f);
				}
				/* ノードの重みをログ出力する */
				crm_debug_2("node %s now has weight %d",
					    node->details->uname, local->weight);
				
			} else if(do_and && !accept) {
				/* 最後の場合で、対象ノードに適用できないruleの場合 */
				/* remove it */
				/* 作業用ノードリストから、対象ノードを検索する */
				node_t *delete = pe_find_node_id(
					match_L, node->details->id);
				if(delete != NULL) {
					/* 作業用ノードリストに存在した場合は、作業用ノードリストから削除 */
					match_L = g_list_remove(match_L,delete);
					crm_debug_5("node %s did not match",
						    node->details->uname);
				}
				crm_free(delete);
			}
		);
	/* 確保したエリアのnode_list_rhリストに作業用ノード情報をセットする */
	/* or処理中は、node_list_rhは変化 */
	/* 最後のrule処理後は、適用可能なrule情報のみnode_list_rhリストにセットされている */
	location_rule->node_list_rh = match_L;
	if(location_rule->node_list_rh == NULL) {
		crm_debug_2("No matching nodes for rule %s", rule_id);
		return NULL;
	} 

	crm_debug_3("%s: %d nodes matched",
		    rule_id, g_list_length(location_rule->node_list_rh));
	/* location情報エリアをリターン */
	/* ※1つのruleごとに1つのlocation情報が生成されているので注意 */
	return location_rule;
}
/* with-rscエレメントから生成された２つのcolocation情報の対象となっているリソース情報のpriorityを比較して、priorityで入れ替える */
/* 同じpriorityの場合は、ソース情報のid情報のstrcmpで入れ替ええる */
static gint sort_cons_priority_lh(gconstpointer a, gconstpointer b)
{
	const rsc_colocation_t *rsc_constraint1 = (const rsc_colocation_t*)a;
	const rsc_colocation_t *rsc_constraint2 = (const rsc_colocation_t*)b;

	if(a == NULL) { return 1; }
	if(b == NULL) { return -1; }

	CRM_ASSERT(rsc_constraint1->rsc_lh != NULL);
	CRM_ASSERT(rsc_constraint1->rsc_rh != NULL);
	
	if(rsc_constraint1->rsc_lh->priority > rsc_constraint2->rsc_lh->priority) {
	    return -1;
	}
	
	if(rsc_constraint1->rsc_lh->priority < rsc_constraint2->rsc_lh->priority) {
	    return 1;
	}

	/* Process clones before primitives and groups */
	if (rsc_constraint1->rsc_lh->variant > rsc_constraint2->rsc_lh->variant) {
	    return -1;
	} else if (rsc_constraint1->rsc_lh->variant < rsc_constraint2->rsc_lh->variant) {
	    return 1;
	}

	return strcmp(rsc_constraint1->rsc_lh->id, rsc_constraint2->rsc_lh->id);
}
/* rscエレメントから生成された２つのcolocation情報の対象となっているリソース情報のpriorityを比較して、priorityで入れ替える */
/* 同じpriorityの場合は、ソース情報のid情報のstrcmpで入れ替ええる */
static gint sort_cons_priority_rh(gconstpointer a, gconstpointer b)
{
	const rsc_colocation_t *rsc_constraint1 = (const rsc_colocation_t*)a;
	const rsc_colocation_t *rsc_constraint2 = (const rsc_colocation_t*)b;

	if(a == NULL) { return 1; }
	if(b == NULL) { return -1; }

	CRM_ASSERT(rsc_constraint1->rsc_lh != NULL);
	CRM_ASSERT(rsc_constraint1->rsc_rh != NULL);
	
	if(rsc_constraint1->rsc_rh->priority > rsc_constraint2->rsc_rh->priority) {
	    return -1;
	}
	
	if(rsc_constraint1->rsc_rh->priority < rsc_constraint2->rsc_rh->priority) {
	    return 1;
	}

	/* Process clones before primitives and groups */
	if (rsc_constraint1->rsc_rh->variant > rsc_constraint2->rsc_rh->variant) {
	    return -1;
	} else if (rsc_constraint1->rsc_rh->variant < rsc_constraint2->rsc_rh->variant) {
	    return 1;
	}

	return strcmp(rsc_constraint1->rsc_rh->id, rsc_constraint2->rsc_rh->id);
}
/* １つのcolocationタグ情報からcolocation情報を作成する */
/* 生成されたcolocation情報は、
    	rsc属性に対応したリソース情報のrsc_consリスト
    	with-rsc属性に対応したリソース情報のrsc_cons_lhsリスト
    	状態遷移作業エリアのcolocation_constraints情報リスト
       に追加される
*/
gboolean
rsc_colocation_new(const char *id, const char *node_attr, int score,
		   resource_t *rsc_lh, resource_t *rsc_rh,
		   const char *state_lh, const char *state_rh,
		   pe_working_set_t *data_set)
{
	rsc_colocation_t *new_con      = NULL;
	if(rsc_lh == NULL){
		/* rsc属性に対応したリソース情報のポインタがＮＵＬＬの場合は、FALSE */
		crm_config_err("No resource found for LHS %s", id);
		return FALSE;

	} else if(rsc_rh == NULL){
		/* with-rsc属性に対応したリソース情報のポインタがＮＵＬＬの場合は、FALSE */
		crm_config_err("No resource found for RHS of %s", id);
		return FALSE;
	}
	/* colocation情報エリアを生成する */
	crm_malloc0(new_con, sizeof(rsc_colocation_t));
	if(new_con == NULL) {
		return FALSE;
	}

	if(state_lh == NULL
	   || safe_str_eq(state_lh, RSC_ROLE_STARTED_S)) {
		/* rsc-role属性引数がないか、Startedの場合は、state_lhをRSC_ROLE_UNKNOWN_Sにセットする */
		state_lh = RSC_ROLE_UNKNOWN_S;
	}

	if(state_rh == NULL
	   || safe_str_eq(state_rh, RSC_ROLE_STARTED_S)) {
		/* with-rsc-role属性引数がないか、Startedの場合は、state_rhをRSC_ROLE_UNKNOWN_Sにセットする */
		state_rh = RSC_ROLE_UNKNOWN_S;
	} 
	/* id, rsc, with-rscなどの情報を生成したcolocation情報エリアにセットする */
	new_con->id       = id;
	new_con->rsc_lh   = rsc_lh;
	new_con->rsc_rh   = rsc_rh;
	new_con->score   = score;
	new_con->role_lh = text2role(state_lh);
	new_con->role_rh = text2role(state_rh);
	new_con->node_attribute = node_attr;

	if(node_attr == NULL) {
		/* node_attrが指定なしの場合は、#unameをセットする */
	    node_attr = "#"XML_ATTR_UNAME;
	}
	
	crm_debug_3("%s ==> %s (%s %d)", rsc_lh->id, rsc_rh->id, node_attr, score);
	/* rsc属性に対応したリソース情報のrsc_consリストに生成したcolocation情報を追加、ソートする */
	rsc_lh->rsc_cons = g_list_insert_sorted(
		rsc_lh->rsc_cons, new_con, sort_cons_priority_rh);
	/* with-rsc属性に対応したリソース情報のrsc_cons_lhsリストに生成したcolocation情報を追加、ソートする */
	rsc_rh->rsc_cons_lhs = g_list_insert_sorted(
		rsc_rh->rsc_cons_lhs, new_con, sort_cons_priority_lh);
	/* 状態遷移作業エリアのcolocation_constraints情報リストに生成したcolocation情報を追加する */
	data_set->colocation_constraints = g_list_append(
		data_set->colocation_constraints, new_con);
	
	return TRUE;
}

/* LHS before RHS */
/* order情報を生成する */
int
new_rsc_order(resource_t *lh_rsc, const char *lh_task, 
	      resource_t *rh_rsc, const char *rh_task, 
	      enum pe_ordering type, pe_working_set_t *data_set)
{
    char *lh_key = NULL;
    char *rh_key = NULL;

    CRM_CHECK(lh_rsc != NULL,  return -1);
    CRM_CHECK(lh_task != NULL, return -1);
    CRM_CHECK(rh_rsc != NULL,  return -1);
    CRM_CHECK(rh_task != NULL, return -1);
  	/*-----------------------------------------------*/
  	/* rsc_orderからキー情報を作成する 				*/
  	/*-----------------------------------------------*/
  	/* first指定のリソース情報と、thenリソースのactionとinterval:0でキーを作成する */  
    lh_key = generate_op_key(lh_rsc->id, lh_task, 0);
    rh_key = generate_op_key(rh_rsc->id, rh_task, 0);
    /* 作成したキーでrsc_orderのアクション情報を生成する */
	/* 態遷移作業エリアのordering_constraintsリストに生成したエリアを追加される */
    return custom_action_order(lh_rsc, lh_key, NULL,
			       rh_rsc, rh_key, NULL, type, data_set);
}
/* ２つのリソース間のORDER情報を生成する */
/* LHS before RHS */
int
custom_action_order(
	resource_t *lh_rsc, char *lh_action_task, action_t *lh_action,
	resource_t *rh_rsc, char *rh_action_task, action_t *rh_action,
	enum pe_ordering type, pe_working_set_t *data_set)
{
	order_constraint_t *order = NULL;
	if(lh_rsc == NULL && lh_action) {
		/* first指定のリソースがない場合で、first指定のアクションがある場合は */
		/* first指定のアクションのリソース情報をfirst指定のリソース情報セットする */
		lh_rsc = lh_action->rsc;
	}
	if(rh_rsc == NULL && rh_action) {
		/* then指定のリソースがない場合で、then指定のアクションがある場合は */
		/* then指定のアクションのリソース情報をthen指定のリソース情報セットする */
		rh_rsc = rh_action->rsc;
	}

	if((lh_action == NULL && lh_rsc == NULL)
	   || (rh_action == NULL && rh_rsc == NULL)){
		/* first,thenの指定リソースなし、指定アクションなしの場合は処理しない */
		crm_config_err("Invalid inputs %p.%p %p.%p",
			      lh_rsc, lh_action, rh_rsc, rh_action);
		crm_free(lh_action_task);
		crm_free(rh_action_task);
		return -1;
	}
	/* order_constraint_tエリアを確保 */
	crm_malloc0(order, sizeof(order_constraint_t));

	crm_debug_3("Creating ordering constraint %d",
		    data_set->order_id);
	/* 確保エリアのidに状態遷移作業エリアのorder_idをインクリメントセット */
	order->id             = data_set->order_id++;
	/* type....first,thenリソースの情報をセット */
	order->type           = type;
	order->lh_rsc         = lh_rsc;
	order->rh_rsc         = rh_rsc;
	order->lh_action      = lh_action;
	order->rh_action      = rh_action;
	order->lh_action_task = lh_action_task;
	order->rh_action_task = rh_action_task;
	/* 状態遷移作業エリアのordering_constraintsリストに生成したエリアを追加する */
	data_set->ordering_constraints = g_list_append(
		data_set->ordering_constraints, order);
	/* 以下でログを出力 */
	if(lh_rsc != NULL && rh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " %s/%s before %s/%s",
			 order->id, ordering_type2text(order->type),
			 lh_rsc->id, lh_action_task,
			 rh_rsc->id, rh_action_task);
		
	} else if(lh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " %s/%s before action %d (%s)",
			 order->id, ordering_type2text(order->type),
			 lh_rsc->id, lh_action_task,
			 rh_action->id, rh_action_task);
		
	} else if(rh_rsc != NULL) {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " action %d (%s) before %s/%s",
			 order->id, ordering_type2text(order->type),
			 lh_action->id, lh_action_task,
			 rh_rsc->id, rh_action_task);
		
	} else {
		crm_debug_4("Created ordering constraint %d (%s):"
			 " action %d (%s) before action %d (%s)",
			 order->id, ordering_type2text(order->type),
			 lh_action->id, lh_action_task,
			 rh_action->id, rh_action_task);
	}
	
	return order->id;
}

static enum pe_ordering get_flags(
    const char *id, int score, const char *action_1, const char *action_2) {
    enum pe_ordering flags = pe_order_optional;

    if(score < 0) {
	crm_debug_2("Upgrade %s: implies left", id);
	flags |= pe_order_implies_left;
	if(safe_str_eq(action_2, RSC_DEMOTE)) {
	    crm_debug_2("Upgrade %s: demote", id);
	    flags |= pe_order_demote;
	}
	
    } else if(score > 0) {
	crm_debug_2("Upgrade %s: implies right", id);
	flags |= pe_order_implies_right;
	if(safe_str_eq(action_1, RSC_START)
	   || safe_str_eq(action_1, RSC_PROMOTE)) {
	    crm_debug_2("Upgrade %s: runnable", id);
	    flags |= pe_order_runnable_left;
	}
    }
    return flags;
}


static gboolean
unpack_order_set(xmlNode *set, int score,
		 action_t **begin, action_t **end,
		 action_t **inv_begin, action_t **inv_end, const char *symmetrical, pe_working_set_t *data_set) 
{
    resource_t *last = NULL;
    resource_t *resource = NULL;

    int local_score = score;
    gboolean sequential = FALSE;
    enum pe_ordering flags = pe_order_optional;
    
    char *key = NULL;
    const char *id = ID(set);
    const char *action = crm_element_value(set, "action");
    const char *sequential_s = crm_element_value(set, "sequential");
    const char *score_s = crm_element_value(set, XML_RULE_ATTR_SCORE);

    char *pseudo_id = NULL;
    char *end_id    = NULL;
    char *begin_id  = NULL;

    if(action == NULL) {
	action = RSC_START;
    }
    
    pseudo_id = crm_concat(id, action, '-');
    end_id    = crm_concat(pseudo_id, "end", '-');
    begin_id  = crm_concat(pseudo_id, "begin", '-');

    *end = get_pseudo_op(end_id, data_set);
    *begin = get_pseudo_op(begin_id, data_set);
    
    crm_free(pseudo_id);
    crm_free(begin_id);
    crm_free(end_id);

    if(score_s) {
	local_score = char2score(score_s);
    }
    if(sequential_s == NULL) {
	sequential_s = "1";
    }
    
    sequential = crm_is_true(sequential_s);
    flags = get_flags(id, local_score, action, action);
    
    xml_child_iter_filter(
	set, xml_rsc, XML_TAG_RESOURCE_REF,

	EXPAND_CONSTRAINT_IDREF(id, resource, ID(xml_rsc));

	key = generate_op_key(resource->id, action, 0);
	custom_action_order(NULL, NULL, *begin, resource, key, NULL,
			    flags|pe_order_implies_left_printed, data_set);

	key = generate_op_key(resource->id, action, 0);
	custom_action_order(resource, key, NULL, NULL, NULL, *end,
			    flags|pe_order_implies_right_printed, data_set);
	
	if(sequential) {
	    if(last != NULL) {
		new_rsc_order(last, action, resource, action, flags, data_set);
	    }
	    last = resource;
	}

	);

    if(crm_is_true(symmetrical) == FALSE) {
	goto done;
    }

    last = NULL;
    local_score *= -1;
    action = invert_action(action);
    
    pseudo_id = crm_concat(id, action, '-');
    end_id    = crm_concat(pseudo_id, "end", '-');
    begin_id  = crm_concat(pseudo_id, "begin", '-');

    *inv_end = get_pseudo_op(end_id, data_set);
    *inv_begin = get_pseudo_op(begin_id, data_set);

    crm_free(pseudo_id);
    crm_free(begin_id);
    crm_free(end_id);

    flags = get_flags(id, local_score, action, action);
    
    xml_child_iter_filter(
	set, xml_rsc, XML_TAG_RESOURCE_REF,

	EXPAND_CONSTRAINT_IDREF(id, resource, ID(xml_rsc));

	key = generate_op_key(resource->id, action, 0);
	custom_action_order(NULL, NULL, *inv_begin, resource, key, NULL,
			    flags|pe_order_implies_left_printed, data_set);

	key = generate_op_key(resource->id, action, 0);
	custom_action_order(resource, key, NULL, NULL, NULL, *inv_end,
			    flags|pe_order_implies_right_printed, data_set);
	
	if(sequential) {
	    if(last != NULL) {
		new_rsc_order(resource, action, last, action, flags, data_set);
	    }
	    last = resource;
	}

	);

  done:
    return TRUE;
}

static gboolean order_rsc_sets(
    const char *id, xmlNode *set1, xmlNode *set2, int score, pe_working_set_t *data_set) {
		
    resource_t *rsc_1 = NULL;
    resource_t *rsc_2 = NULL;
	    
    const char *action_1 = crm_element_value(set1, "action");
    const char *action_2 = crm_element_value(set2, "action");

    const char *sequential_1 = crm_element_value(set1, "sequential");
    const char *sequential_2 = crm_element_value(set2, "sequential");

    enum pe_ordering flags = get_flags(id, score, action_1, action_2);
	    
    if(crm_is_true(sequential_1)) {
	/* get the first one */
	xml_child_iter_filter(
	    set1, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
	    break;
	    );
    }

    if(crm_is_true(sequential_2)) {
	/* get the last one */
	const char *rid = NULL;
	xml_child_iter_filter(
	    set2, xml_rsc, XML_TAG_RESOURCE_REF,
	    rid = ID(xml_rsc);
	    );
	EXPAND_CONSTRAINT_IDREF(id, rsc_2, rid);
    }

    if(rsc_1 != NULL && rsc_2 != NULL) {
	new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);

    } else if(rsc_1 != NULL) {
	xml_child_iter_filter(
	    set2, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc));
	    new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);
	    );

    } else if(rsc_2 != NULL) {
	xml_child_iter_filter(
	    set1, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
	    new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);
	    );

    } else {
	xml_child_iter_filter(
	    set1, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));

	    xml_child_iter_filter(
		set2, xml_rsc_2, XML_TAG_RESOURCE_REF,
		EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc_2));
		new_rsc_order(rsc_1, action_1, rsc_2, action_2, flags, data_set);
		);
	    );
    }
    return TRUE;
}
/* costraintsタグ内の１つのrsc_order子タグを処理する */
/* 状態遷移作業エリアのordering_constraintsリストにセットする */
gboolean
unpack_rsc_order(xmlNode *xml_obj, pe_working_set_t *data_set)
{
	int score_i = 0;
	gboolean any_sets = FALSE;

	action_t *set_end = NULL;
	action_t *set_begin = NULL;

	action_t *set_inv_end = NULL;
	action_t *set_inv_begin = NULL;
	
	xmlNode *last = NULL;
	action_t *last_end = NULL;
	action_t *last_begin = NULL;
	action_t *last_inv_end = NULL;
	action_t *last_inv_begin = NULL;
	/* rsc_orderタグからid, score, symmetrical属性の値を取り出す */
	const char *id    = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
	const char *invert = crm_element_value(xml_obj, XML_CONS_ATTR_SYMMETRICAL);

	if(invert == NULL) {
		/* symmetrical属性が設定されていない場合は、invert=TRUE */
	    invert = "true";
	}

	if(score == NULL) {
		/* score属性が設定されていない場合は、score=INFINITY */
	    score = "INFINITY";
	}
	
	score_i = char2score(score);
	/* rsc_orderタグ内の全てのresource_set子タグを処理する */
	xml_child_iter_filter(
	    xml_obj, set, "resource_set",

	    any_sets = TRUE;
	    if(unpack_order_set(set, score_i, &set_begin, &set_end,
				&set_inv_begin, &set_inv_end, invert, data_set) == FALSE) {
		return FALSE;

	    } else if(last) {
		const char *set_action = crm_element_value(set, "action");
		const char *last_action = crm_element_value(last, "action");
		enum pe_ordering flags = get_flags(id, score_i, last_action, set_action);
		order_actions(last_end, set_begin, flags);

		if(crm_is_true(invert)) {
		    set_action = invert_action(set_action?set_action:RSC_START);
		    last_action = invert_action(last_action?last_action:RSC_START);
		    score_i *= -1;
		    
		    flags = get_flags(id, score_i, last_action, set_action);
		    order_actions(last_inv_begin, set_inv_end, flags);
		}
		
	    } else if(/* never called */last && order_rsc_sets(id, last, set, score_i, data_set) == FALSE) {
		return FALSE;

	    }
	    last = set;
	    last_end = set_end;
	    last_begin = set_begin;
	    last_inv_end = set_inv_end;
	    last_inv_begin = set_inv_begin;
	    );

	if(any_sets == FALSE) {
		/* resource_set子タグを処理しない場合 */
		/* ※サンプルのような形のrsc_orderはこちらで処理 */
	    return unpack_simple_rsc_order(xml_obj, data_set);
	}

	return TRUE;
}
/* 単純でないresource_setを持ったcolocationタグの展開処理 */
static gboolean
unpack_colocation_set(xmlNode *set, int score, pe_working_set_t *data_set) 
{
    resource_t *with = NULL;
    resource_t *resource = NULL;
    const char *set_id = ID(set);
    const char *role = crm_element_value(set, "role");
    const char *sequential = crm_element_value(set, "sequential");
    int local_score = score;

    const char *score_s = crm_element_value(set, XML_RULE_ATTR_SCORE);
    if(score_s) {
	local_score = char2score(score_s);
    }
    
    if(sequential != NULL && crm_is_true(sequential) == FALSE) {
	return TRUE;

    } else if(local_score >= 0) {
	xml_child_iter_filter(
	    set, xml_rsc, XML_TAG_RESOURCE_REF,
	    
	    EXPAND_CONSTRAINT_IDREF(set_id, resource, ID(xml_rsc));
	    if(with != NULL) {
		crm_debug_2("Colocating %s with %s", resource->id, with->id);
		rsc_colocation_new(set_id, NULL, local_score, resource, with, role, role, data_set);
	    }

	    with = resource;
	    );
	
    } else {
	/* Anti-colocating with every prior resource is
	 * the only way to ensure the intuitive result
	 * (ie. that no-one in the set can run with anyone
	 * else in the set)
	 */
	
	xml_child_iter_filter(
	    set, xml_rsc, XML_TAG_RESOURCE_REF,
	    
	    EXPAND_CONSTRAINT_IDREF(set_id, resource, ID(xml_rsc));

	    xml_child_iter_filter(
		set, xml_rsc_with, XML_TAG_RESOURCE_REF,
		if(safe_str_eq(resource->id, ID(xml_rsc_with))) {
		    break;
		}
		EXPAND_CONSTRAINT_IDREF(set_id, with, ID(xml_rsc_with));
		crm_debug_2("Anti-Colocating %s with %s", resource->id, with->id);
		    /* colocation情報を作成する */
	        /* 生成されたcolocation情報は、
		    	rscエレメントに対応したリソース情報のrsc_consリスト
    			with-rscエレメントに対応したリソース情報のrsc_cons_lhsリスト
    			状態遷移作業エリアのcolocation_constraints情報リスト
       			に追加される
    		*/
		rsc_colocation_new(set_id, NULL, local_score, resource, with, role, role, data_set);
		);
	    );
    }
    
    return TRUE;
}

static gboolean colocate_rsc_sets(
    const char *id, xmlNode *set1, xmlNode *set2, int score, pe_working_set_t *data_set)
{
    resource_t *rsc_1 = NULL;
    resource_t *rsc_2 = NULL;
	    
    const char *role_1 = crm_element_value(set1, "role");
    const char *role_2 = crm_element_value(set2, "role");

    const char *sequential_1 = crm_element_value(set1, "sequential");
    const char *sequential_2 = crm_element_value(set2, "sequential");

    if(crm_is_true(sequential_1)) {
	/* get the first one */
	xml_child_iter_filter(
	    set1, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
	    break;
	    );
    }

    if(crm_is_true(sequential_2)) {
	/* get the last one */
	const char *rid = NULL;
	xml_child_iter_filter(
	    set2, xml_rsc, XML_TAG_RESOURCE_REF,
	    rid = ID(xml_rsc);
	    );
	EXPAND_CONSTRAINT_IDREF(id, rsc_2, rid);
    }

    if(rsc_1 != NULL && rsc_2 != NULL) {
		    /* colocation情報を作成する */
	        /* 生成されたcolocation情報は、
		    	rscエレメントに対応したリソース情報のrsc_consリスト
    			with-rscエレメントに対応したリソース情報のrsc_cons_lhsリスト
    			状態遷移作業エリアのcolocation_constraints情報リスト
       			に追加される
    		*/
	rsc_colocation_new(id, NULL, score, rsc_1, rsc_2, role_1, role_2, data_set);

    } else if(rsc_1 != NULL) {
	xml_child_iter_filter(
	    set2, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc));
		    /* colocation情報を作成する */
	        /* 生成されたcolocation情報は、
		    	rscエレメントに対応したリソース情報のrsc_consリスト
    			with-rscエレメントに対応したリソース情報のrsc_cons_lhsリスト
    			状態遷移作業エリアのcolocation_constraints情報リスト
       			に追加される
    		*/
	    rsc_colocation_new(id, NULL, score, rsc_1, rsc_2, role_1, role_2, data_set);
	    );

    } else if(rsc_2 != NULL) {
	xml_child_iter_filter(
	    set1, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));
		    /* colocation情報を作成する */
	        /* 生成されたcolocation情報は、
		    	rscエレメントに対応したリソース情報のrsc_consリスト
    			with-rscエレメントに対応したリソース情報のrsc_cons_lhsリスト
    			状態遷移作業エリアのcolocation_constraints情報リスト
       			に追加される
    		*/
	    rsc_colocation_new(id, NULL, score, rsc_1, rsc_2, role_1, role_2, data_set);
	    );

    } else {
	xml_child_iter_filter(
	    set1, xml_rsc, XML_TAG_RESOURCE_REF,
	    EXPAND_CONSTRAINT_IDREF(id, rsc_1, ID(xml_rsc));

	    xml_child_iter_filter(
		set2, xml_rsc_2, XML_TAG_RESOURCE_REF,
		EXPAND_CONSTRAINT_IDREF(id, rsc_2, ID(xml_rsc_2));
		    /* colocation情報を作成する */
	        /* 生成されたcolocation情報は、
		    	rscエレメントに対応したリソース情報のrsc_consリスト
    			with-rscエレメントに対応したリソース情報のrsc_cons_lhsリスト
    			状態遷移作業エリアのcolocation_constraints情報リスト
       			に追加される
    		*/		rsc_colocation_new(id, NULL, score, rsc_1, rsc_2, role_1, role_2, data_set);
		);
	    );
    }

    return TRUE;
}
/* constraintsタグ内の１つのresource_setを持たないrsc_colocationタグを展開する */
static gboolean unpack_simple_colocation(xmlNode *xml_obj, pe_working_set_t *data_set)
{
    int score_i = 0;
	/* rsc_colocationタグ内の id, socre, rsc, with-rsc, */
	/* rsc-role, with-rsc-role, node-attribute, symmetrical属性の値を取り出す*/
    const char *id       = crm_element_value(xml_obj, XML_ATTR_ID);
    const char *score    = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
    
    const char *id_lh    = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE);
    const char *id_rh    = crm_element_value(xml_obj, XML_COLOC_ATTR_TARGET);
    const char *state_lh = crm_element_value(xml_obj, XML_COLOC_ATTR_SOURCE_ROLE);
    const char *state_rh = crm_element_value(xml_obj, XML_COLOC_ATTR_TARGET_ROLE);
    const char *attr     = crm_element_value(xml_obj, XML_COLOC_ATTR_NODE_ATTR);    

    const char *symmetrical = crm_element_value(xml_obj, XML_CONS_ATTR_SYMMETRICAL);
  	/* rsc属性指定されたリソース情報を状態遷移作業エリアのリソース情報から取り出す */
    resource_t *rsc_lh = pe_find_resource(data_set->resources, id_lh);
    /* with-rsc属性指定されたリソース情報を状態遷移作業エリアのリソース情報から取り出す */
    resource_t *rsc_rh = pe_find_resource(data_set->resources, id_rh);
    
    if(rsc_lh == NULL) {
		/* rsc属性指定のリソースが存在しない場合は、FALSE */
	crm_config_err("No resource (con=%s, rsc=%s)", id, id_lh);
	return FALSE;
	
    } else if(rsc_rh == NULL) {
		/* with-rsc属性指定のリソースが存在しない場合は、FALSE */
	crm_config_err("No resource (con=%s, rsc=%s)", id, id_rh);
	return FALSE;
    }

    if(crm_is_true(symmetrical)) {
		/* symmetrical属性がTRUEの場合は、WARNログを出力する */
	crm_config_warn("The %s colocation constraint attribute has been removed."
			"  It didn't do what you think it did anyway.",
			XML_CONS_ATTR_SYMMETRICAL);
    }

    if(score) {
		/* score属性がある場合は数値化する */
	score_i = char2score(score);
    }
    /* colocation情報を作成する */
    /* 生成されたcolocation情報は、
    	rsc属性に対応したリソース情報のrsc_consリスト
    	with-rsc属性に対応したリソース情報のrsc_cons_lhsリスト
    	状態遷移作業エリアのcolocation_constraints情報リスト
       に追加される
    */
    /* id属性、node-attribute属性、score属性 */
    /* rsc属性に対応したリソース情報へのポインタ、with-rscエレメントに対応したリソース情報へのポインタ */
    /* rsc-role属性、with-rsc-role属性、状態遷移作業エリアを引数 */
    rsc_colocation_new(id, attr, score_i, rsc_lh, rsc_rh, state_lh, state_rh, data_set);
    return TRUE;
}
/* constraintsタグ内の１つのrsc_colocationタグを展開する */
gboolean
unpack_rsc_colocation(xmlNode *xml_obj, pe_working_set_t *data_set)
{
	int score_i = 0;
	xmlNode *last = NULL;
	gboolean any_sets = FALSE;
	/* rsc_colocationタグ内の id, scoreを取り出す */
	const char *id    = crm_element_value(xml_obj, XML_ATTR_ID);
	const char *score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);

	if(score) {
		/* scoreが取れた場合は、数値化する */
	    score_i = char2score(score);
	}
	/* rsc_colocationタグ内にresource_set子タグを全て処理する */
	xml_child_iter_filter(
	    xml_obj, set, "resource_set",

	    any_sets = TRUE;
	    /* unpack_colocation_setでresource_set子タグを処理する */
	    if(unpack_colocation_set(set, score_i, data_set) == FALSE) {
			/* 処理が失敗した場合は、FALSEを返す */
		return FALSE;

	    } else if(last && colocate_rsc_sets(id, last, set, score_i, data_set) == FALSE) {
			/* unpack_colocation_set処理に成功して、最後のresource_set処理の場合は、*/
			/* colocate_rsc_sets処理を実行して、処理に失敗した場合は、FALSEを返す */
		return FALSE;
	    }
	    last = set;
	    );

	
	if(any_sets == FALSE) {
		/* resource_setを含まないrsc_colocationタグの場合は、こちらでunpack_simple_colocation処理 */
		/* ※サンプルの場合は、こちらで処理される */
	    return unpack_simple_colocation(xml_obj, data_set);
	}

	return TRUE;
}

gboolean is_active(rsc_to_node_t *cons)
{
	return TRUE;
}
