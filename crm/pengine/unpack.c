/* $Id: unpack.c,v 1.28 2004/09/17 13:03:10 andrew Exp $ */
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
#include <portability.h>
#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>

#include <lrm/lrm_api.h>

#include <glib.h>
#include <libxml/tree.h>

#include <heartbeat.h> /* for ONLINESTATUS */

#include <pengine.h>
#include <pe_utils.h>

int      max_valid_nodes = 0;
int      order_id        = 1;
GListPtr agent_defaults  = NULL;
gboolean stonith_enabled = FALSE;

GListPtr match_attrs(const char *attr, const char *op, const char *value,
		     const char *type, GListPtr node_list);

gboolean unpack_rsc_to_attr(xmlNodePtr xml_obj,
			    GListPtr rsc_list,
			    GListPtr node_list,
			    GListPtr *node_constraints);

gboolean unpack_rsc_to_node(xmlNodePtr xml_obj,
			    GListPtr rsc_list,
			    GListPtr node_list,
			    GListPtr *node_constraints);

gboolean unpack_rsc_order(
	xmlNodePtr xml_obj, GListPtr rsc_list, GListPtr *action_constraints);

gboolean unpack_rsc_dependancy(
	xmlNodePtr xml_obj, GListPtr rsc_list, GListPtr *action_constraints);

gboolean unpack_rsc_location(
	xmlNodePtr xml_obj, GListPtr rsc_list, GListPtr node_list,
	GListPtr *action_constraints);

gboolean unpack_lrm_rsc_state(
	node_t *node, xmlNodePtr lrm_state,
	GListPtr rsc_list, GListPtr nodes,
	GListPtr *actions, GListPtr *node_constraints);

gboolean add_node_attrs(xmlNodePtr attrs, node_t *node);

gboolean unpack_healthy_resource(GListPtr *node_constraints, GListPtr *actions,
	xmlNodePtr rsc_entry, resource_t *rsc_lh, node_t *node);

gboolean unpack_failed_resource(GListPtr *node_constraints, GListPtr *actions,
	xmlNodePtr rsc_entry, resource_t *rsc_lh, node_t *node);

gboolean determine_online_status(xmlNodePtr node_state, node_t *this_node);

gboolean unpack_lrm_agents(node_t *node, xmlNodePtr agent_list);

gboolean is_node_unclean(xmlNodePtr node_state);

gboolean rsc2rsc_new(const char *id, enum con_strength strength, enum rsc_con_type type,
		     resource_t *rsc_lh, resource_t *rsc_rh);

gboolean create_ordering(
	const char *id, enum con_strength strength,
	resource_t *rsc_lh, resource_t *rsc_rh, GListPtr *action_constraints);

rsc_to_node_t *rsc2node_new(
	const char *id, resource_t *rsc,
	double weight, gboolean can_run, node_t *node,
	GListPtr *node_constraints);

const char *get_agent_param(resource_t *rsc, const char *param);

const char *get_agent_param_rsc(resource_t *rsc, const char *param);

const void *get_agent_param_metadata(resource_t *rsc, const char *param);

const char *get_agent_param_global(resource_t *rsc, const char *param);

const char *param_value(xmlNodePtr parent, const char *name);

gboolean
unpack_config(xmlNodePtr config)
{
	const char *value = NULL;
	
	value = param_value(config, "failed_nodes");

	crm_debug("config %p", config);
	crm_debug("value %p", value);

	if(safe_str_eq(value, "stonith")) {
		crm_debug("Enabling STONITH of failed nodes");
		stonith_enabled = TRUE;
	} else {
		stonith_enabled = FALSE;
	}
	
	return TRUE;
}

const char *
param_value(xmlNodePtr parent, const char *name) 
{
	xmlNodePtr a_default = find_entity(
		parent, XML_CIB_TAG_NVPAIR, name, FALSE);

	return xmlGetProp(a_default, XML_NVPAIR_ATTR_VALUE);
}

const char *
get_agent_param(resource_t *rsc, const char *param)
{
	const char *value = NULL;

	if(param == NULL) {
		return NULL;
	}
	
	value = get_agent_param_rsc(rsc, param);
	if(value == NULL) {
		value = get_agent_param_metadata(rsc, param);
	}
	if(value == NULL) {
		value = get_agent_param_global(rsc, param);
	}
	
	return value;
}

const char *
get_agent_param_rsc(resource_t *rsc, const char *param)
{
	xmlNodePtr xml_rsc = rsc->xml;
	return xmlGetProp(xml_rsc, param);
}

const void *
get_agent_param_metadata(resource_t *rsc, const char *param)
{
	return NULL;
}

const char *
get_agent_param_global(resource_t *rsc, const char *param)
{
	const char * value = NULL;/*g_hashtable_lookup(agent_global_defaults, param); */
	if(value == NULL) {
		crm_err("No global value default for %s", param);
	}
	return value;
}

gboolean
unpack_global_defaults(xmlNodePtr defaults)
{
	return TRUE;
}


gboolean
unpack_nodes(xmlNodePtr xml_nodes, GListPtr *nodes)
{
	node_t *new_node   = NULL;
	xmlNodePtr attrs   = NULL;
	const char *id     = NULL;
	const char *uname  = NULL;
	const char *type   = NULL;

	crm_verbose("Begining unpack...");
	xml_child_iter(
		xml_nodes, xml_obj, XML_CIB_TAG_NODE,

		attrs   = xml_obj->children;
		id     = xmlGetProp(xml_obj, XML_ATTR_ID);
		uname  = xmlGetProp(xml_obj, XML_ATTR_UNAME);
		type   = xmlGetProp(xml_obj, XML_ATTR_TYPE);

		crm_verbose("Processing node %s/%s", uname, id);

		if(attrs != NULL) {
			attrs = attrs->children;
		}
		
		if(id == NULL) {
			crm_err("Must specify id tag in <node>");
			xml_iter_continue(xml_obj);
		}
		if(type == NULL) {
			crm_err("Must specify type tag in <node>");
			xml_iter_continue(xml_obj);
		}
		crm_malloc(new_node, sizeof(node_t));
		if(new_node == NULL) {
			return FALSE;
		}
		
		new_node->weight  = 1.0;
		new_node->fixed   = FALSE;
		crm_malloc(new_node->details,
			   sizeof(struct node_shared_s));

		if(new_node->details == NULL) {
			crm_free(new_node);
			return FALSE;
		}

		crm_verbose("Creaing node for entry %s/%s", uname, id);
		new_node->details->id		= id;
		new_node->details->uname	= uname;
		new_node->details->type		= node_ping;
		new_node->details->online	= FALSE;
		new_node->details->unclean	= FALSE;
		new_node->details->shutdown	= FALSE;
		new_node->details->running_rsc	= NULL;
		new_node->details->agents	= NULL;
		new_node->details->attrs        = g_hash_table_new(
			g_str_hash, g_str_equal);
		
		if(safe_str_eq(type, "member")) {
			new_node->details->type = node_member;
		}

		add_node_attrs(attrs, new_node);
		*nodes = g_list_append(*nodes, new_node);    
		crm_verbose("Done with node %s", xmlGetProp(xml_obj, "uname"));

		crm_debug_action(print_node("Added", new_node, FALSE));
		);
  
	*nodes = g_list_sort(*nodes, sort_node_weight);

	return TRUE;
}

gboolean 
unpack_resources(xmlNodePtr xml_resources,
		 GListPtr *resources,
		 GListPtr *actions,
		 GListPtr *action_cons,
		 GListPtr all_nodes)
{
	crm_verbose("Begining unpack...");
	xml_child_iter(
		xml_resources, xml_obj, XML_CIB_TAG_RESOURCE,
		action_t *action_stop  = NULL;
		action_t *action_start = NULL;
		const char *id         = xmlGetProp(xml_obj, XML_ATTR_ID);
		const char *stopfail   = xmlGetProp(xml_obj, "on_stopfail");
		const char *restart    = xmlGetProp(xml_obj, "restart_type");

		const char *max_instances      = xmlGetProp(
			xml_obj, "max_instances");
		const char *max_node_instances = xmlGetProp(
			xml_obj, "max_node_instances");
		const char *max_masters      = xmlGetProp(
			xml_obj, "max_masters");
		const char *max_node_masters = xmlGetProp(
			xml_obj, "max_node_masters");

		const char *version    = xmlGetProp(xml_obj, XML_ATTR_VERSION);
		resource_t *new_rsc = NULL;
		const char *priority   = xmlGetProp(
			xml_obj, XML_CIB_ATTR_PRIORITY);

		crm_verbose("Processing resource...");
		
		if(id == NULL) {
			crm_err("Must specify id tag in <resource>");
			xml_iter_continue(xml_obj);
		}
		crm_malloc(new_rsc, sizeof(resource_t));

		if(new_rsc == NULL) {
			return FALSE;
		}
		
		new_rsc->id		= id;
		new_rsc->xml		= xml_obj;
		crm_malloc(new_rsc->agent, sizeof(lrm_agent_t));
		new_rsc->agent->class	= xmlGetProp(xml_obj, "class");
		new_rsc->agent->type	= xmlGetProp(xml_obj, "type");
		new_rsc->agent->version	= version?version:"0.0";

		new_rsc->priority	    = atoi(priority?priority:"0"); 
		new_rsc->effective_priority = new_rsc->priority;

		new_rsc->max_instances	    = atoi(
			max_instances?max_instances:"1"); 
		new_rsc->max_node_instances = atoi(
			max_node_instances?max_node_instances:"1"); 
		new_rsc->max_masters	    = atoi(
			max_masters?max_masters:"0"); 
		new_rsc->max_node_masters   = atoi(
			max_node_masters?max_node_masters:"0"); 

		new_rsc->candidate_colors   = NULL;
		new_rsc->actions            = NULL;
		new_rsc->color		= NULL; 
		new_rsc->runnable	= TRUE; 
		new_rsc->provisional	= TRUE; 
		new_rsc->allowed_nodes	= NULL;
		new_rsc->rsc_cons	= NULL; 
		new_rsc->node_cons	= NULL; 
		new_rsc->cur_node	= NULL;
		
		if(safe_str_eq(stopfail, "ignore")) {
			new_rsc->stopfail_type = pesf_ignore;
		} else if(safe_str_eq(stopfail, "stonith")) {
			new_rsc->stopfail_type = pesf_stonith;
		} else {
			new_rsc->stopfail_type = pesf_block;
		}

		if(safe_str_eq(restart, "restart")) {
			new_rsc->restart_type = pe_restart_restart;
		} else if(safe_str_eq(restart, "recover")) {
			new_rsc->restart_type = pe_restart_recover;
		} else {
			new_rsc->restart_type = pe_restart_ignore;
		}

		action_stop    = action_new(new_rsc, stop_rsc);
		*actions       = g_list_append(*actions, action_stop);
		new_rsc->stop  = action_stop;
		new_rsc->actions =
			g_list_append(new_rsc->actions, action_stop);

		action_start   = action_new(new_rsc, start_rsc);
		*actions       = g_list_append(*actions, action_start);
		new_rsc->start = action_start;
		new_rsc->actions =
			g_list_append(new_rsc->actions, action_start);

		order_new(action_stop,action_start,pecs_startstop,action_cons);

		*resources = g_list_append(*resources, new_rsc);
	
		crm_debug_action(print_resource("Added", new_rsc, FALSE));
		);
	
	*resources = g_list_sort(*resources, sort_rsc_priority);

	return TRUE;
}



gboolean 
unpack_constraints(xmlNodePtr xml_constraints,
		   GListPtr nodes, GListPtr resources,
		   GListPtr *node_constraints,
		   GListPtr *action_constraints)
{
	crm_verbose("Begining unpack...");
	xml_child_iter(
		xml_constraints, xml_obj, NULL,

		const char *id = xmlGetProp(xml_obj, XML_ATTR_ID);
		if(id == NULL) {
			crm_err("Constraint <%s...> must have an id",
				xml_obj->name);
			xml_iter_continue(xml_obj);
		}

		crm_verbose("Processing constraint %s %s", xml_obj->name,id);
		if(safe_str_eq("rsc_order", xml_obj->name)) {
			unpack_rsc_order(
				xml_obj, resources, action_constraints);

		} else if(safe_str_eq("rsc_dependancy", xml_obj->name)) {
			unpack_rsc_dependancy(
				xml_obj, resources, action_constraints);

		} else if(safe_str_eq("rsc_location", xml_obj->name)) {
			unpack_rsc_location(
				xml_obj, resources, nodes, node_constraints);

		} else {
			crm_err("Unsupported constraint type: %s",
				xml_obj->name);
		}
		);

	return TRUE;
}

rsc_to_node_t *
rsc2node_new(const char *id, resource_t *rsc,
	     double weight, gboolean can, node_t *node,
	     GListPtr *node_constraints)
{
	rsc_to_node_t *new_con = NULL;

	if(rsc == NULL || id == NULL) {
		crm_err("Invalid constraint %s for rsc=%p", id, rsc);
		return NULL;
	}

	crm_malloc(new_con, sizeof(rsc_to_node_t));
	if(new_con != NULL) {
		new_con->id           = id;
		new_con->rsc_lh       = rsc;
		new_con->node_list_rh = NULL;
		new_con->can          = can;
		
		if(can) {
			new_con->weight = weight;
		} else {
			new_con->weight = -1;
		}
		
		if(node != NULL) {
			new_con->node_list_rh = g_list_append(NULL, node);
		}
		
		*node_constraints = g_list_append(*node_constraints, new_con);
	}
	
	return new_con;
}




/* remove nodes that are down, stopping */
/* create +ve rsc_to_node constraints between resources and the nodes they are running on */
/* anything else? */
gboolean
unpack_status(xmlNodePtr status,
	      GListPtr nodes, GListPtr rsc_list,
	      GListPtr *actions, GListPtr *node_constraints)
{
	const char *uname     = NULL;

	xmlNodePtr lrm_rsc    = NULL;
	xmlNodePtr lrm_agents = NULL;
	xmlNodePtr attrs      = NULL;
	node_t    *this_node  = NULL;
	
	crm_verbose("Begining unpack");
	xml_child_iter(
		status, node_state, XML_CIB_TAG_STATE,

/*		id         = xmlGetProp(node_state, XML_ATTR_ID); */
		uname      = xmlGetProp(node_state,    XML_ATTR_UNAME);
		attrs      = find_xml_node(node_state, "attributes");
		lrm_rsc    = find_xml_node(node_state, XML_CIB_TAG_LRM);
		lrm_agents = find_xml_node(lrm_rsc,    "lrm_agents");
		lrm_rsc    = find_xml_node(lrm_rsc,    XML_LRM_TAG_RESOURCES);
		lrm_rsc    = find_xml_node(lrm_rsc,    "lrm_resource");

		crm_verbose("Processing node %s", uname);
		this_node = pe_find_node(nodes, uname);

		if(uname == NULL) {
			/* error */
			xml_iter_continue(node_state);

		} else if(this_node == NULL) {
			crm_err("Node %s in status section no longer exists",
				uname);
			xml_iter_continue(node_state);
		}
		
		crm_verbose("Adding runtime node attrs");
		add_node_attrs(attrs, this_node);

		crm_verbose("determining node state");
		determine_online_status(node_state, this_node);

		crm_verbose("Processing lrm resource entries");
		unpack_lrm_rsc_state(this_node, lrm_rsc, rsc_list, nodes,
				     actions, node_constraints);

		crm_verbose("Processing lrm agents");
		unpack_lrm_agents(this_node, lrm_agents);

		);

	return TRUE;
	
}

gboolean
determine_online_status(xmlNodePtr node_state, node_t *this_node)
{
	const char *uname      = xmlGetProp(node_state,XML_ATTR_UNAME);
	const char *state      = xmlGetProp(node_state,XML_NODE_ATTR_STATE);
	const char *exp_state  = xmlGetProp(node_state,XML_CIB_ATTR_EXPSTATE);
	const char *join_state = xmlGetProp(node_state,XML_CIB_ATTR_JOINSTATE);
	const char *crm_state  = xmlGetProp(node_state,XML_CIB_ATTR_CRMDSTATE);
	const char *ccm_state  = xmlGetProp(node_state,XML_CIB_ATTR_INCCM);
	const char *shutdown   = xmlGetProp(node_state,XML_CIB_ATTR_SHUTDOWN);
	const char *unclean    = NULL;/*xmlGetProp(node_state,XML_CIB_ATTR_STONITH); */
	
	if(safe_str_eq(join_state, CRMD_JOINSTATE_MEMBER)
	   && safe_str_eq(ccm_state, XML_BOOLEAN_YES)
	   && safe_str_eq(crm_state, ONLINESTATUS)
	   && shutdown == NULL) {
		this_node->details->online = TRUE;

	} else {
		crm_verbose("remove");
		/* remove node from contention */
		this_node->weight = -1;
		this_node->fixed = TRUE;

		crm_verbose("state %s, expected %s, shutdown %s",
			    state, exp_state, shutdown);

		if(unclean != NULL) {
			this_node->details->unclean = TRUE;
				
		} else if(shutdown != NULL) {
			this_node->details->shutdown = TRUE;

		} else if(is_node_unclean(node_state)) {
			/* report and or take remedial action */
			this_node->details->unclean = TRUE;
		}

		if(this_node->details->unclean) {
			crm_verbose("Node %s is due for STONITH", uname);
		}

		if(this_node->details->shutdown) {
			crm_verbose("Node %s is due for shutdown", uname);
		}
	}
	return TRUE;
}

gboolean
is_node_unclean(xmlNodePtr node_state)
{
	const char *state      = xmlGetProp(node_state,XML_NODE_ATTR_STATE);
	const char *exp_state  = xmlGetProp(node_state,XML_CIB_ATTR_EXPSTATE);
	const char *join_state = xmlGetProp(node_state,XML_CIB_ATTR_JOINSTATE);
	const char *crm_state  = xmlGetProp(node_state,XML_CIB_ATTR_CRMDSTATE);
	const char *ccm_state  = xmlGetProp(node_state,XML_CIB_ATTR_INCCM);

	if(safe_str_eq(exp_state, CRMD_STATE_INACTIVE)) {
		return FALSE;

	/* do an actual calculation once STONITH is available */

	/* } else if(...) { */
	}

	/* for now... */
	if(0) {
		state = NULL;
		join_state = NULL;
		crm_state = NULL;
		ccm_state = NULL;
	}
	
	return FALSE;
}

gboolean
unpack_lrm_agents(node_t *node, xmlNodePtr agent_list)
{
	/* if the agent is not listed, remove the node from
	 * the resource's list of allowed_nodes
	 */
	lrm_agent_t *agent   = NULL;
	const char *version  = NULL;

	if(agent_list == NULL) {
		return FALSE;
	}

	xml_child_iter(
		agent_list, xml_agent, XML_LRM_TAG_AGENT,

		crm_malloc(agent, sizeof(lrm_agent_t));
		if(agent == NULL) {
			continue;
		}
		
		agent->class   = xmlGetProp(xml_agent, "class");
		agent->type    = xmlGetProp(xml_agent, "type");
		version        = xmlGetProp(xml_agent, "version");
		agent->version = version?version:"0.0";

		crm_trace("Adding agent %s/%s v%s to node %s",
			  agent->class,
			  agent->type,
			  agent->version,
			  node->details->uname);
			  
		node->details->agents = g_list_append(
			node->details->agents, agent);
		);
	
	return TRUE;
}


gboolean
unpack_lrm_rsc_state(node_t *node, xmlNodePtr lrm_rsc,
		     GListPtr rsc_list, GListPtr nodes,
		     GListPtr *actions, GListPtr *node_constraints)
{
	xmlNodePtr rsc_entry  = NULL;
	
	const char *rsc_id    = NULL;
	const char *node_id   = NULL;
	const char *rsc_state = NULL;
	const char *op_status = NULL;
	const char *last_rc   = NULL;
	const char *last_op   = NULL;
	resource_t *rsc_lh    = NULL;
	op_status_t  action_status_i = LRM_OP_ERROR;
	xmlNodePtr stonith_list = NULL;

	while(lrm_rsc != NULL) {
		rsc_entry = lrm_rsc;
		lrm_rsc   = lrm_rsc->next;
		
		rsc_id    = xmlGetProp(rsc_entry, XML_ATTR_ID);
		node_id   = xmlGetProp(rsc_entry, XML_LRM_ATTR_TARGET);
		rsc_state = xmlGetProp(rsc_entry, XML_LRM_ATTR_RSCSTATE);
		op_status = xmlGetProp(rsc_entry, XML_LRM_ATTR_OPSTATUS);
		last_rc   = xmlGetProp(rsc_entry, XML_LRM_ATTR_RC);
		last_op   = xmlGetProp(rsc_entry, XML_LRM_ATTR_LASTOP);
		
		rsc_lh    = pe_find_resource(rsc_list, rsc_id);

		crm_verbose("[%s] Processing %s on %s (%s)",
			    rsc_entry->name, rsc_id, node_id, rsc_state);

		if(rsc_lh == NULL) {
			crm_err("Could not find a match for resource"
				" %s in %s's status section",
				rsc_id, node_id);
			continue;
		} else if(op_status == NULL) {
			crm_err("Invalid resource status entry for %s in %s",
				rsc_id, node_id);
			continue;
		}

		xml_child_iter(
			stonith_list, rsc_entry, "can_fence",

			node_t *node = pe_find_node(
				nodes, xmlGetProp(stonith_list, "id"));

			rsc_lh->fencable_nodes = g_list_append(
				rsc_lh->fencable_nodes, node_copy(node));
			);
		
		action_status_i = atoi(op_status);

		if(action_status_i == -1) {
			/*
			 * TODO: this needs more thought
			 * Some cases:
			 * - PE reinvoked with pending action that will succeed
			 * - PE reinvoked with pending action that will fail
			 * - After DC election
			 * - After startup
			 *
			 * pending start - required start
			 * pending stop  - required stop
			 * pending <any> on unavailable node - stonith
			 *
			 * For now this should do
			 */
			if(safe_str_eq(last_op, "stop")) {
				/* map this to a timeout so it is re-issued */
				action_status_i = LRM_OP_TIMEOUT;
			} else {
				/* map this to a "done" so it is not marked
				 * as failed, then make sure it is re-issued
				 */
				action_status_i = LRM_OP_DONE;
				rsc_lh->start->optional = FALSE;
			}
		}

		switch(action_status_i) {
			case LRM_OP_DONE:
				unpack_healthy_resource(
					node_constraints, actions,
					rsc_entry, rsc_lh,node);
				break;
			case LRM_OP_ERROR:
			case LRM_OP_TIMEOUT:
			case LRM_OP_NOTSUPPORTED:
				unpack_failed_resource(
					node_constraints, actions, 
					rsc_entry, rsc_lh,node);
				break;
			case LRM_OP_CANCELLED:
				/* do nothing?? */
				crm_warn("Dont know what to do for cancelled ops yet");
				break;
		}
	}
	return TRUE;
}

gboolean
unpack_failed_resource(GListPtr *node_constraints, GListPtr *actions,
		       xmlNodePtr rsc_entry, resource_t *rsc_lh, node_t *node)
{
	const char *last_op  = xmlGetProp(rsc_entry, "last_op");

	crm_debug("Unpacking failed action %s on %s", last_op, rsc_lh->id);
	
	if(safe_str_neq(last_op, "stop")) {
		/* not running */
		/* do not run the resource here again */
		rsc2node_new("dont_run_generate",
			     rsc_lh, -1.0, FALSE, node, node_constraints);

		/* schedule a stop here just in case? */
		action_new(rsc_lh, stop_rsc);
		
		return TRUE;
		
	} 

	switch(rsc_lh->stopfail_type) {
		case pesf_stonith:
			/* remedial action:
			 *   shutdown (so all other resources are
			 *   stopped gracefully) and then STONITH node
			 */
			
			if(stonith_enabled == FALSE) {
				crm_err("STONITH is not enabled in this cluster but is required for resource %s after a failed stop", rsc_lh->id);
				rsc_lh->start->runnable = FALSE;
				break;
			}
			
			/* treat it as if it is still running */
			rsc_lh->cur_node = node;
			node->details->running_rsc = g_list_append(
				node->details->running_rsc, rsc_lh);
			
			if(node->details->online) {
				node->details->shutdown = TRUE;
			}
			node->details->unclean  = TRUE;
			break;
			
		case pesf_block:
			crm_warn("SHARED RESOURCE %s WILL REMAIN BLOCKED"
				 " UNTIL CLEANED UP MANUALLY ON NODE %s",
				 rsc_lh->id, node->details->uname);
			rsc_lh->start->runnable = FALSE;
			break;
			
		case pesf_ignore:
			crm_warn("SHARED RESOURCE %s IS NOT PROTECTED",
				 rsc_lh->id);
			/* do not run the resource here again */
			rsc2node_new(
				"dont_run_generate",
				rsc_lh, -1.0, FALSE, node, node_constraints);

			break;
	}
		
	return TRUE;
}

gboolean
unpack_healthy_resource(GListPtr *node_constraints, GListPtr *actions,
			xmlNodePtr rsc_entry, resource_t *rsc_lh, node_t *node)
{
	const char *last_op  = xmlGetProp(rsc_entry, "last_op");

	crm_debug("Unpacking healthy action %s on %s", last_op, rsc_lh->id);

	if(safe_str_neq(last_op, "stop")) {

		if(rsc_lh->cur_node != NULL) {
			crm_err("Resource %s running on multiple nodes %s, %s",
				rsc_lh->id,
				rsc_lh->cur_node->details->uname,
				node->details->uname);
			/* TODO: some recovery action!! */
			/* like force a stop on the second node? */
			return FALSE;
			
		} else {
			/* create the link between this node and the rsc */
			crm_verbose("Setting cur_node = %s for rsc = %s",
				    node->details->uname, rsc_lh->id);
			
			rsc_lh->cur_node = node;
			node->details->running_rsc = g_list_append(
				node->details->running_rsc, rsc_lh);
		}
	}

	return TRUE;
}

gboolean
rsc2rsc_new(const char *id, enum con_strength strength, enum rsc_con_type type,
	    resource_t *rsc_lh, resource_t *rsc_rh)
{
	rsc_to_rsc_t *new_con      = NULL;
	rsc_to_rsc_t *inverted_con = NULL;

	if(rsc_lh == NULL || rsc_rh == NULL){
		/* error */
		return FALSE;
	}

	crm_malloc(new_con, sizeof(rsc_to_rsc_t));
	if(new_con != NULL) {
		new_con->id       = id;
		new_con->rsc_lh   = rsc_lh;
		new_con->rsc_rh   = rsc_rh;
		new_con->strength = strength;
		new_con->variant  = type;
		
		inverted_con = invert_constraint(new_con);
		
		rsc_lh->rsc_cons = g_list_insert_sorted(
			rsc_lh->rsc_cons, new_con, sort_cons_strength);
		
		rsc_rh->rsc_cons = g_list_insert_sorted(
			rsc_rh->rsc_cons, inverted_con, sort_cons_strength);
	} else {
		return FALSE;
	}
	
	return TRUE;
}

gboolean
order_new(action_t *before, action_t *after, enum con_strength strength,
	  GListPtr *action_constraints)
{
	order_constraint_t *order = NULL;

	if(before == NULL || after == NULL || action_constraints == NULL){
		crm_err("Invalid inputs b=%p, a=%p l=%p",
			before, after, action_constraints);
		return FALSE;
	}

	crm_malloc(order, sizeof(order_constraint_t));

	if(order != NULL) {
		order->id        = order_id++;
		order->strength  = strength;
		order->lh_action = before;
		order->rh_action = after;
		
		*action_constraints = g_list_append(
			*action_constraints, order);
	}
	
	return TRUE;
}

gboolean
unpack_rsc_dependancy(xmlNodePtr xml_obj,
		  GListPtr rsc_list,
		  GListPtr *action_constraints)
{
	enum con_strength strength_e = pecs_ignore;

	const char *id_lh    = xmlGetProp(xml_obj, "from");
	const char *id       = xmlGetProp(xml_obj, XML_ATTR_ID);
	const char *id_rh    = xmlGetProp(xml_obj, "to");
	const char *type = xmlGetProp(xml_obj, XML_ATTR_TYPE);

	resource_t *rsc_lh   = pe_find_resource(rsc_list, id_lh);
	resource_t *rsc_rh   = pe_find_resource(rsc_list, id_rh);
 
#if 1
	/* relates to the ifdef below */
	action_t *before, *after;
#endif
	if(rsc_lh == NULL) {
		crm_err("No resource (con=%s, rsc=%s)", id, id_lh);
		return FALSE;
	} else if(rsc_rh == NULL) {
		crm_err("No resource (con=%s, rsc=%s)", id, id_rh);
		return FALSE;
	}
	
	if(safe_str_eq(type, XML_STRENGTH_VAL_MUST)) {
		strength_e = pecs_must;
		
	} else if(safe_str_eq(type, XML_STRENGTH_VAL_SHOULD)) {
		crm_err("Type %s is no longer supported", type);
		strength_e = pecs_must;
		
	} else if(safe_str_eq(type, XML_STRENGTH_VAL_SHOULDNOT)) {
		crm_err("Type %s is no longer supported", type);
		strength_e = pecs_must_not;
		
	} else if(safe_str_eq(type, XML_STRENGTH_VAL_MUSTNOT)) {
		strength_e = pecs_must_not;

	} else {
		crm_err("Unknown value for %s: %s", "type", type);
		return FALSE;
	}

	/* make sure the lower priority resource stops before
	 *  the higher is started, otherwise they may be both running
	 *  on the same node when the higher is replacing the lower
	 */
	if(rsc_lh->priority >= rsc_rh->priority) {
		before = rsc_rh->stop;
		after  = rsc_lh->start;
	} else {
		before = rsc_lh->stop;
		after  = rsc_rh->start;
	}
	
	order_new(before, after, strength_e, action_constraints);

	/* make sure the lower priority resource starts after
	 *  the higher is started
	 */
	if(rsc_lh->priority < rsc_rh->priority) {
		before = rsc_rh->start;
		after  = rsc_lh->start;
	} else {
		before = rsc_lh->start;
		after  = rsc_rh->start;
	}
	order_new(before, after, strength_e,action_constraints);
	
	return rsc2rsc_new(id, strength_e, same_node, rsc_lh, rsc_rh);
}

gboolean
unpack_rsc_order(xmlNodePtr xml_obj,
		  GListPtr rsc_list,
		  GListPtr *action_constraints)
{
	enum con_strength strength_e = pecs_ignore;

	const char *id       = xmlGetProp(xml_obj, XML_ATTR_ID);
	const char *id_lh    = xmlGetProp(xml_obj, "from");
	const char *id_rh    = xmlGetProp(xml_obj, "to");
	const char *type     = xmlGetProp(xml_obj, XML_ATTR_TYPE);

	resource_t *rsc_lh   = pe_find_resource(rsc_list, id_lh);
	resource_t *rsc_rh   = pe_find_resource(rsc_list, id_rh);

	if(xml_obj == NULL) {
		crm_err("No constraint object to process.");
		return FALSE;

	} else if(id == NULL) {
		crm_err("%s constraint must have an id", xml_obj->name);
		return FALSE;
		
	} else if(rsc_lh == NULL || rsc_rh == NULL) {
		crm_err("Constraint %s needs two sides lh: %p rh: %p"
			" (NULL indicates missing side)",
			id, rsc_lh, rsc_rh);
		return FALSE;
	
	} else if(safe_str_eq(type, "after")) {
		rsc2rsc_new(id, strength_e, start_after, rsc_lh, rsc_rh);
		order_new(rsc_rh->stop, rsc_lh->stop, pecs_must,
			  action_constraints);
		order_new(rsc_lh->start, rsc_rh->start, pecs_must,
			  action_constraints);
	} else {
		rsc2rsc_new(id, strength_e, start_before, rsc_lh, rsc_rh);
		order_new(rsc_lh->stop, rsc_rh->stop, pecs_must,
			  action_constraints);
		order_new(rsc_rh->start, rsc_lh->start, pecs_must,
			  action_constraints);
	}
	return TRUE;
}


/* do NOT free the nodes returned here */
GListPtr
match_attrs(const char *attr, const char *op, const char *value,
	    const char *type, GListPtr node_list)
{
	int lpc = 0, lpc2 = 0;
	GListPtr result = NULL;
	
	if(attr == NULL || op == NULL) {
		crm_err("Invlaid attribute or operation in expression"
			" (\'%s\' \'%s\' \'%s\')",
			crm_str(attr), crm_str(op), crm_str(value));
		return NULL;
	}
	

	slist_iter(
		node, node_t, node_list, lpc,
		gboolean accept = FALSE;
		
		int cmp = 0;
		const char *h_val = (const char*)g_hash_table_lookup(
			node->details->attrs, attr);

		if(value != NULL && h_val != NULL) {
			if(type == NULL || (safe_str_eq(type, "string"))) {
				cmp = strcmp(h_val, value);

			} else if(safe_str_eq(type, "number")) {
				float h_val_f = atof(h_val);
				float value_f = atof(value);

				if(h_val_f < value_f) {
					cmp = -1;
				} else if(h_val_f > value_f)  {
					cmp = 1;
				} else {
					cmp = 0;
				}
				
			} else if(safe_str_eq(type, "version")) {
				cmp = compare_version(h_val, value);

			}
			
		} else if(value == NULL && h_val == NULL) {
			cmp = 0;
		} else if(value == NULL) {
			cmp = 1;
		} else {
			cmp = -1;
		}
		
		if(safe_str_eq(op, "exists")) {
			if(h_val != NULL) accept = TRUE;	

		} else if(safe_str_eq(op, "notexists")) {
			if(h_val == NULL) accept = TRUE;

		} else if(safe_str_eq(op, "running")) {
			GListPtr rsc_list = node->details->running_rsc;
			slist_iter(
				rsc, resource_t, rsc_list, lpc2,
				if(safe_str_eq(rsc->id, attr)) {
					accept = TRUE;
				}
				);

		} else if(safe_str_eq(op, "not_running")) {
			GListPtr rsc_list = node->details->running_rsc;
			accept = TRUE;
			slist_iter(
				rsc, resource_t, rsc_list, lpc2,
				if(safe_str_eq(rsc->id, attr)) {
					accept = FALSE;
					break;
				}
				);

		} else if(safe_str_eq(op, "eq")) {
			if((h_val == value) || cmp == 0)
				accept = TRUE;

		} else if(safe_str_eq(op, "ne")) {
			if((h_val == NULL && value != NULL)
			   || (h_val != NULL && value == NULL)
			   || cmp != 0)
				accept = TRUE;

		} else if(value == NULL || h_val == NULL) {
			/* the comparision is meaningless from this point on */
			accept = FALSE;
			
		} else if(safe_str_eq(op, "lt")) {
			if(cmp < 0) accept = TRUE;
			
		} else if(safe_str_eq(op, "lte")) {
			if(cmp <= 0) accept = TRUE;
			
		} else if(safe_str_eq(op, "gt")) {
			if(cmp > 0) accept = TRUE;
			
		} else if(safe_str_eq(op, "gte")) {
			if(cmp >= 0) accept = TRUE;
			
		}
		
		if(accept) {
			crm_trace("node %s matched", node->details->uname);
			result = g_list_append(result, node);
		} else {
			crm_trace("node %s did not match", node->details->uname);
		}		   
		);
	
	return result;
}

gboolean
add_node_attrs(xmlNodePtr attrs, node_t *node)
{
	const char *name  = NULL;
	const char *value = NULL;
	
	while(attrs != NULL){
		name  = xmlGetProp(attrs, XML_NVPAIR_ATTR_NAME);
		value = xmlGetProp(attrs, XML_NVPAIR_ATTR_VALUE);
			
		if(name != NULL
		   && value != NULL
		   && safe_val(NULL, node, details) != NULL) {
			crm_verbose("Adding %s => %s", name, value);

			/* this is frustrating... no way to pass in const
			 *  keys or values yet docs say:
			 *   Note: If keys and/or values are dynamically
			 *   allocated, you should free them first.
			 */
			g_hash_table_insert(node->details->attrs,
					    crm_strdup(name),
					    crm_strdup(value));
		}
		attrs = attrs->next;
	}	
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("uname"),
			    crm_strdup(node->details->uname));
 	g_hash_table_insert(node->details->attrs,
			    crm_strdup("id"),
			    crm_strdup(node->details->id));
	return TRUE;
}



gboolean
unpack_rsc_location(
	xmlNodePtr xml_obj,
	GListPtr rsc_list, GListPtr node_list, GListPtr *node_constraints)
{
/*

  <constraints>
     <rsc_location rsc="Filesystem-whatever-1" timestamp="..." lifetime="...">
     	<rule score="+50.0" result="can">
<!ATTLIST node_expression
	  id         CDATA #REQUIRED
	  attribute  CDATA #REQUIRED
	  operation  (lt|gt|lte|gte|eq|ne|exists|notexists)
	  value      CDATA #IMPLIED
	  type	     (integer|string|version)    'string'>

	</rule>
     	<rule score="+500.0">
       		<node_expression match="cpu:50GHz" />
	</rule>
     	<rule result="cannot">
       		<node_expression not_match="san"/>
	</rule>
...

   Translation:

   Further translation:
       
*/
	gboolean were_rules = FALSE;
	const char *id_lh   = xmlGetProp(xml_obj, "rsc");
	const char *id      = xmlGetProp(xml_obj, XML_ATTR_ID);
	resource_t *rsc_lh = pe_find_resource(rsc_list, id_lh);

	if(rsc_lh == NULL) {
		crm_err("No resource (con=%s, rsc=%s)",
		       id, id_lh);
		return FALSE;
	}
			
	xml_child_iter(
		xml_obj, rule, "rule",

		gboolean first_expr = TRUE;
		gboolean can_run    = FALSE;
		gboolean do_and     = TRUE;

		const char *rule_id = xmlGetProp(rule, XML_ATTR_ID);
		const char *score   = xmlGetProp(rule, "score");
		const char *result  = xmlGetProp(rule, "result");
		const char *boolean = xmlGetProp(rule, "boolean_op");
		GListPtr match_L    = NULL;
		GListPtr old_list   = NULL;
		float score_f       = atof(score?score:"0.0");

		rsc_to_node_t *new_con = NULL;

		were_rules = TRUE;
		
		if(safe_str_eq(boolean, "or")) {
			do_and = FALSE;
		}

		if(result == NULL || (safe_str_eq(result, "can"))) {
			can_run = TRUE;
		}

		new_con = rsc2node_new(rule_id, rsc_lh, score_f,
				       can_run, NULL, node_constraints);

		if(new_con == NULL) {
			continue;
		}
		
		gboolean rule_has_expressions = FALSE;
		xml_child_iter(
			rule, expr, "expression",

			const char *attr  = xmlGetProp(expr, "attribute");
			const char *op    = xmlGetProp(expr, "operation");
			const char *value = xmlGetProp(expr, "value");
			const char *type  = xmlGetProp(expr, "type");
			
			rule_has_expressions = TRUE;
			crm_trace("processing expression: %s",
				  xmlGetProp(expr, "id"));

			match_L = match_attrs(
				attr, op, value, type, node_list);
			
			if(first_expr) {
				new_con->node_list_rh =	node_list_dup(
					match_L, FALSE);
				first_expr = FALSE;
				xml_iter_continue(expr);
			}

			old_list = new_con->node_list_rh;

			if(do_and) {
				crm_trace("do_and");
				
				new_con->node_list_rh = node_list_and(
					old_list, match_L, FALSE);
			} else {
				crm_trace("do_or");
				
				new_con->node_list_rh = node_list_or(
					old_list, match_L, FALSE);
			}
			pe_free_shallow_adv(match_L,  FALSE);
			pe_free_shallow_adv(old_list, TRUE);
			);

		if(rule_has_expressions == FALSE) {
			/* feels like a hack */
			crm_debug("Rule %s had no expressions,"
				  " adding all nodes", xmlGetProp(rule, "id"));
			
			new_con->node_list_rh = node_list_dup(node_list,FALSE);
		}
		
		if(new_con->node_list_rh == NULL) {
			crm_warn("No matching nodes for constraint/rule %s/%s",
				 id, xmlGetProp(rule, "id"));
		}
		
		crm_debug_action(print_rsc_to_node("Added", new_con, FALSE));
		);

	if(were_rules == FALSE) {
		crm_err("no rules for constraint %s", id);
	}

	
	return TRUE;
}

