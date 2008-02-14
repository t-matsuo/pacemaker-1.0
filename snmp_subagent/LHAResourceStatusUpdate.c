/*
 * Note: this file originally auto-generated by mib2c using
 *        : mib2c.notify.conf,v 5.2.2.1 2004/04/15 12:29:06 dts12 Exp $
 */
#include <crm_internal.h>

#include "hbagent.h"
#include "hbagentv2.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
#include "LHAResourceStatusUpdate.h"



static oid      snmptrap_oid[] = { 1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0 };

int
send_LHAResourceStatusUpdate_trap(struct hb_rsinfov2 *resource)
{
    netsnmp_variable_list *var_list = NULL;
    oid             LHAResourceStatusUpdate_oid[] =
        { 1, 3, 6, 1, 4, 1, 4682, 900, 11 };
    oid             LHAResourceName_oid[] =
        { 1, 3, 6, 1, 4, 1, 4682, 8, 1, 2, /* insert index here */  };
    oid             LHAResourceNode_oid[] =
        { 1, 3, 6, 1, 4, 1, 4682, 8, 1, 4, /* insert index here */  };
    oid             LHAResourceStatus_oid[] =
        { 1, 3, 6, 1, 4, 1, 4682, 8, 1, 5, /* insert index here */  };


    /*
     * Set the snmpTrapOid.0 value
     */
    snmp_varlist_add_variable(&var_list,
                              snmptrap_oid, OID_LENGTH(snmptrap_oid),
                              ASN_OBJECT_ID,
                              (u_char *)LHAResourceStatusUpdate_oid,
                               sizeof(LHAResourceStatusUpdate_oid));

    /*
     * Add any objects from the trap definition
     */
    snmp_varlist_add_variable(&var_list,
                              LHAResourceName_oid,
                              OID_LENGTH(LHAResourceName_oid),
                              ASN_OCTET_STR,
                              /*
                               * Set an appropriate value for LHAResourceName 
                               */
                                (u_char *)resource->resourceid,
                                strlen(resource->resourceid));
    snmp_varlist_add_variable(&var_list,
                              LHAResourceNode_oid,
                              OID_LENGTH(LHAResourceNode_oid),
                              ASN_OCTET_STR,
                              /*
                               * Set an appropriate value for LHAResourceNode 
                               */
                                (u_char *)resource->node,
                                strlen(resource->node));
    snmp_varlist_add_variable(&var_list,
                              LHAResourceStatus_oid,
                              OID_LENGTH(LHAResourceStatus_oid),
                              ASN_INTEGER,
                              /*
                               * Set an appropriate value for LHAResourceStatus 
                               */
                                (u_char *)&(resource->status),
                                sizeof(resource->status));

    /*
     * Add any extra (optional) objects here
     */

    /*
     * Send the trap to the list of configured destinations
     *  and clean up
     */
    send_v2trap(var_list);
    snmp_free_varbind(var_list);

    return SNMP_ERR_NOERROR;
}
