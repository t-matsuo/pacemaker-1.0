<transition_graph cluster-delay="60s" stonith-timeout="60s" failed-stop-offset="INFINITY" failed-start-offset="INFINITY" batch-limit="30" transition_id="0">
  <synapse id="0" priority="1000000">
    <action_set>
      <rsc_op id="3" operation="probe_complete" operation_key="probe_complete" on_node="node1" on_node_uuid="node1">
        <attributes CRM_meta_op_no_wait="true" crm_feature_set="3.0.1"/>
      </rsc_op>
    </action_set>
    <inputs/>
  </synapse>
  <synapse id="1" priority="1000000">
    <action_set>
      <rsc_op id="4" operation="probe_complete" operation_key="probe_complete" on_node="node2" on_node_uuid="node2">
        <attributes CRM_meta_op_no_wait="true" crm_feature_set="3.0.1"/>
      </rsc_op>
    </action_set>
    <inputs/>
  </synapse>
</transition_graph>

