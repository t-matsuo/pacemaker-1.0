<transition_graph cluster-delay="60s" stonith-timeout="60s" failed-stop-offset="INFINITY" failed-start-offset="INFINITY" batch-limit="30" transition_id="0">
  <synapse id="0">
    <action_set>
      <rsc_op id="5" operation="monitor" operation_key="rsc3_monitor_0" on_node="node2" on_node_uuid="node2">
        <primitive id="rsc3" long-id="rsc3" class="heartbeat" type="apache"/>
        <attributes CRM_meta_op_target_rc="7" CRM_meta_timeout="20000" allow_migrate="on" crm_feature_set="3.0.1"/>
      </rsc_op>
    </action_set>
    <inputs/>
  </synapse>
  <synapse id="1">
    <action_set>
      <rsc_op id="6" operation="migrate_to" operation_key="rsc3_migrate_to_0" on_node="node1" on_node_uuid="node1">
        <primitive id="rsc3" long-id="rsc3" class="heartbeat" type="apache"/>
        <attributes CRM_meta_migrate_source="node1" CRM_meta_migrate_target="node2" CRM_meta_timeout="20000" allow_migrate="on" crm_feature_set="3.0.1"/>
      </rsc_op>
    </action_set>
    <inputs>
      <trigger>
        <pseudo_event id="2" operation="probe_complete" operation_key="probe_complete"/>
      </trigger>
    </inputs>
  </synapse>
  <synapse id="2">
    <action_set>
      <rsc_op id="7" operation="migrate_from" operation_key="rsc3_migrate_from_0" on_node="node2" on_node_uuid="node2">
        <primitive id="rsc3" long-id="rsc3" class="heartbeat" type="apache"/>
        <attributes CRM_meta_migrate_source="node1" CRM_meta_migrate_source_uuid="node1" CRM_meta_migrate_target="node2" CRM_meta_timeout="20000" allow_migrate="on" crm_feature_set="3.0.1"/>
      </rsc_op>
    </action_set>
    <inputs>
      <trigger>
        <pseudo_event id="2" operation="probe_complete" operation_key="probe_complete"/>
      </trigger>
      <trigger>
        <rsc_op id="6" operation="migrate_to" operation_key="rsc3_migrate_to_0" on_node="node1" on_node_uuid="node1"/>
      </trigger>
    </inputs>
  </synapse>
  <synapse id="3">
    <action_set>
      <pseudo_event id="1" operation="all_stopped" operation_key="all_stopped">
        <attributes crm_feature_set="3.0.1"/>
      </pseudo_event>
    </action_set>
    <inputs>
      <trigger>
        <rsc_op id="6" operation="migrate_to" operation_key="rsc3_migrate_to_0" on_node="node1" on_node_uuid="node1"/>
      </trigger>
      <trigger>
        <rsc_op id="7" operation="migrate_from" operation_key="rsc3_migrate_from_0" on_node="node2" on_node_uuid="node2"/>
      </trigger>
    </inputs>
  </synapse>
  <synapse id="4">
    <action_set>
      <pseudo_event id="2" operation="probe_complete" operation_key="probe_complete">
        <attributes crm_feature_set="3.0.1"/>
      </pseudo_event>
    </action_set>
    <inputs>
      <trigger>
        <rsc_op id="3" operation="probe_complete" operation_key="probe_complete" on_node="node1" on_node_uuid="node1"/>
      </trigger>
      <trigger>
        <rsc_op id="4" operation="probe_complete" operation_key="probe_complete" on_node="node2" on_node_uuid="node2"/>
      </trigger>
    </inputs>
  </synapse>
  <synapse id="5" priority="1000000">
    <action_set>
      <rsc_op id="3" operation="probe_complete" operation_key="probe_complete" on_node="node1" on_node_uuid="node1">
        <attributes CRM_meta_op_no_wait="true" crm_feature_set="3.0.1"/>
      </rsc_op>
    </action_set>
    <inputs/>
  </synapse>
  <synapse id="6" priority="1000000">
    <action_set>
      <rsc_op id="4" operation="probe_complete" operation_key="probe_complete" on_node="node2" on_node_uuid="node2">
        <attributes CRM_meta_op_no_wait="true" crm_feature_set="3.0.1"/>
      </rsc_op>
    </action_set>
    <inputs>
      <trigger>
        <rsc_op id="5" operation="monitor" operation_key="rsc3_monitor_0" on_node="node2" on_node_uuid="node2"/>
      </trigger>
    </inputs>
  </synapse>
</transition_graph>

