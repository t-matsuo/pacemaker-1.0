/* $Id: crmd_fsa.h,v 1.4 2004/02/17 22:11:57 lars Exp $ */
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

/*======================================
 *	States the DC/CRMd can be in
 *======================================*/
enum crmd_fsa_state {
        S_IDLE = 0,	/* Nothing happening */

        S_ELECTION,	/* Take part in the election algorithm as 
			 * described below
			 */
	S_INTEGRATION,	/* integrate that status of new nodes (which is 
			 * all of them if we have just been elected DC)
			 * to form a complete and up-to-date picture of
			 * the CIB
			 */
        S_NOT_DC,	/* we are in crmd/slave mode */
        S_POLICY_ENGINE,/* Determin the next stable state of the cluster
			 */
        S_RECOVERY,	/* Something bad happened, check everything is ok
			 * before continuing and attempt to recover if
			 * required
			 */
        S_RECOVERY_DC,	/* Something bad happened to the DC, check
			 * everything is ok before continuing and attempt
			 * to recover if required
			 */
        S_RELEASE_DC,	/* we were the DC, but now we arent anymore,
			 * possibly by our own request, and we should 
			 * release all unnecessary sub-systems, finish
			 * any pending actions, do general cleanup and
			 * unset anything that makes us think we are
			 * special :)
			 */
        S_STARTING,	/* we are just starting out */
        S_STOPPING,	/* We are in the final stages of shutting down */
        S_TERMINATE,	/* We are going to shutdown, this is the equiv of
			 * "Sending TERM signal to all processes" in Linux
			 * and in worst case scenarios could be considered
			 * a self STONITH
			 */
        S_TRANSITION_ENGINE,/* Attempt to make the calculated next stable
			     * state of the cluster a reality
			     */

	/*  ----------- Last input found in table is above ---------- */
	S_ILLEGAL,	/* This is an illegal FSA state */
			/* (must be last) */
};
#define MAXSTATE	S_ILLEGAL
/*
	A state diagram can be constructed from the dc_fsa.dot with the
	following command:

	dot -Tpng crmd_fsa.dot > crmd_fsa.png

Description:

      Once we start and do some basic sanity checks, we go into the
      S_NOT_DC state and await instructions from the DC or input from
      the CCM which indicates the election algorithm needs to run.

      If the election algorithm is triggered we enter the S_ELECTION state
      from where we can either go back to the S_NOT_DC state or progress
      to the S_INTEGRATION state (or S_RELEASE_DC if we used to be the DC
      but arent anymore).

      The election algorithm has been adapted from
      http://www.cs.indiana.edu/cgi-bin/techreports/TRNNN.cgi?trnum=TR521

      Loosly known as the Bully Algorithm, its major points are:
      - Election is initiated by any node (N) notices that the coordinator
	is no longer responding
      - Concurrent multiple elections are possible
      - Algorithm
	  + N sends ELECTION messages to all nodes that occur earlier in
	  the CCM's membership list.
	  + If no one responds, N wins and becomes coordinator
	  + N sends out COORDINATOR messages to all other nodes in the
	  partition
	  + If one of higher-ups answers, it takes over. N is done.
      
      Once the election is complete, if we are the DC, we enter the
      S_INTEGRATION state which is a DC-in-waiting style state.  We are
      the DC, but we shouldnt do anything yet because we may not have an
      up-to-date picture of the cluster.  There may of course be times
      when this fails, so we should go back to the S_RECOVERY stage and
      check everything is ok.  We may also end up here if a new node came
      online, since each node is authorative on itself and we would want
      to incorporate its information into the CIB.

      Once we have the latest CIB, we then enter the S_POLICY_ENGINE state
      where invoke the Policy Engine. It is possible that between
      invoking the Policy Engine and recieving an answer, that we recieve
      more input. In this case we would discard the orginal result and
      invoke it again.

      Once we are satisfied with the output from the Policy Engine we
      enter S_TRANSITION_ENGINE and feed the Policy Engine's output to the
      Transition Engine who attempts to make the Policy Engine's
      calculation a reality.  If the transition completes successfully,
      we enter S_IDLE, otherwise we go back to S_POLICY_ENGINE with the
      current unstable state and try again.
      
      Of course we may be asked to shutdown at any time, however we must
      progress to S_NOT_DC before doing so.  Once we have handed over DC
      duties to another node, we can then shut down like everyone else,
      that is by asking the DC for permission and waiting it to take all
      our resources away.

      The case where we are the DC and the only node in the cluster is a
      special case and handled as an escalation which takes us to
      S_SHUTDOWN.  Similarly if any other point in the shutdown
      fails or stalls, this is escalated and we end up in S_TERMINATE.

      At any point, the CRMd/DC can relay messages for its sub-systems,
      but outbound messages (from sub-systems) should probably be blocked
      until S_INTEGRATION (for the DC case) or the join protocol has
      completed (for the CRMd case)
      
*/

/*======================================
 *
 * 	Inputs/Events/Stimuli to be given to the finite state machine
 *
 *	Some of these a true events, and others a synthesised based on
 *	the "register" (see below) and the contents or source of messages.
 *
 *	At this point, my plan is to have a loop of some sort that keeps
 *	going until recieving I_NULL
 *
 *======================================*/
enum crmd_fsa_input {
	I_NULL,		/* Nothing happened */
	
	I_CIB_UPDATE,	/* An update to the CIB occurred */
	I_DC_TIMEOUT,	/* We have lost communication with the DC */
	I_ELECTION_RELEASE_DC,	/* The election completed and we were not
				 * elected, but we were the DC beforehand
				 */
	I_ELECTION_DC,	/* The election completed and we were (re-)elected
			 * DC
			 */
	I_ERROR,	/* Something bad happened (more serious than
			 * I_FAIL) and may not have been due to the action
			 * being performed.  For example, we may have lost
			 * our connection to the CIB.
			 */
	I_FAIL,		/* The action failed to complete successfully */
	I_NODE_JOIN,	/* A node has entered the CCM membership list*/
	I_NODE_LEFT,	/* A node shutdown (possibly unexpectedly) */
	I_NODE_LEAVING,	/* A node has asked to be shutdown */
	I_NOT_DC,	/* We are not and were not the DC before or after
			 * the current operation or state
			 */
	I_RECOVERED,	/* The recovery process completed successfully */
	I_RELEASE_FAIL,	/* We could not give up DC status for some reason
			 */
	I_RELEASE_SUCCESS,	/* We are no longer the DC */
	I_RESTART,	/* The current set of actions needs to be
			 * restarted
			 */
	I_REQUEST,	/* Some non-resource, non-ccm action is required
			   of us, eg. ping */
	I_ROUTER,	/* Do our job as router and forward this to the
			   right place */
	I_SHUTDOWN,	/* We need to shutdown */
	I_SUCCESS,	/* The action completed successfully */


	
	/*  ------------ Last input found in table is above ----------- */
	I_ILLEGAL,	/* This is an illegal value for an FSA input */
			/* (must be last) */
};
#define MAXINPUT	I_ILLEGAL


/*======================================
 *
 * actions
 *
 * Some of the actions below will always occur together for now, but I can
 * forsee that this may not always be the case.  So I've spilt them up so
 * that if they ever do need to be called independantly in the future, it
 * wont be a problem. 
 *
 * For example, separating A_LRM_CONNECT from A_STARTUP might be useful 
 * if we ever try to recover from a faulty or disconnected LRM.
 *
 *======================================*/

	 /* Dont do anything */
#define  A_NOTHING	0x0000000000000000 

/* -- Operational actions -- */
	/* Hook to perform any actions (other than starting the CIB,
	 * connecting to HA or the CCM) that might be needed as part
	 * of the startup.
	 */
#define	 A_STARTUP	0x0000000000000001

	/* Shutdown ourselves and Heartbeat */
#define	 A_SHUTDOWN	0x0000000000000002

	/* Something bad happened, try to recover */
#define	 A_RECOVER	0x0000000000000004

	/* Shutdown ourselves, but dont take Heartbeat (or the LRM?)
	 * with us
	 */
#define  A_DISCONNECT	0x0000000000000008


	/* Connect to Heartbeat */
#define	 A_HA_CONNECT	0x0000000000000010

	/* Connect to the CCM */
#define	 A_CCM_CONNECT	0x0000000000000020

	/* Connect to the Local Resource Manager */
#define	 A_LRM_CONNECT	0x0000000000000040


/* -- CIB actions -- */
	/* Start the CIB */
#define	 A_CIB_START	0x0000000000000100

	/* Calculate the most up to date CIB.  This would be called
	 * mulitple times, once to initiate and every time an
	 * appropriate response comes from a slave node.
	 */
#define	 A_CIB_CALC	0x0000000000000200

	/* Ask the local CIB for information */
#define	 A_CIB_QUERY	0x0000000000000400

	/* Stop the CIB, it is no longer required */
#define	 A_CIB_STOP	0x0000000000000800


/* -- CIB & Join protocol actions -- */
	/* Distribute the CIB to the other nodes */
#define	 A_CIB_SEND	0x0000000000001000

	/* Update the CIB locally */
#define	 A_CIB_UPDATE	0x0000000000002000

	/* Send a welcome message to new
				    * node(s)
				    */
#define	 A_JOIN_WELCOME	0x0000000000004000

	/* Acknowledge the DC as our overlord*/
#define	 A_JOIN_ACK	0x0000000000008000


/* -- DC related actions -- */
	/* Process whatever it is the CCM is trying to tell us.
	 * This will generate inputs such as I_NODE_JOIN,
	 * I_NODE_LEAVE, I_SHUTDOWN, I_DC_RELEASE, I_DC_TAKEOVER
	 */
#define	 A_CCM_EVENT	0x0000000000010000

	/* Hook to perform any actions (apart from starting, the TE, PE
	 * and gathering the latest CIB) that might be necessary before
	 * taking over the responsibilities of being the DC.
	 */
#define	 A_DC_TAKEOVER	0x0000000000020000

	/* Hook to perform any actions (apart from starting, the TE, PE 
	 * and gathering the latest CIB) that might be necessary before 
	 * giving up the responsibilities of being the DC.
	 */
#define	 A_DC_RELEASE	0x0000000000040000

	/* Kill another node that is too sick to be the DC or is not
	 * configured correctly. This may involve just adding a constraint
	 * and invokeing the PE/TE
	 */
#define	 A_DC_ASSASINATE 0x0000000000080000


/* -- Message actions -- */
	/* Put the request into a queue for processing.  We do this every 
	 * time so that the processing is consistent.  The intent is to 
	 * allow the DC to keep doing important work while still not
	 * loosing requests.
	 * Messages are not considered recieved until processed.
	 */
#define	 A_MSG_STORE	0x0000000000100000

	/* Process the queue of requests */
#define	 A_MSG_PROCESS	0x0000000000200000

	/* Send the message to the correct recipient */
#define	 A_MSG_ROUTE	0x0000000000400000

	/* Required? Acknowledge or reply to a message */
#define	 A_MSG_ACK	0x0000000000800000


/* -- DC-Only clients -- */
	/* Start the Policy Engine */
#define	 A_PE_START	0x0000000001000000

	/* Calculate the next state for the cluster.  This is only
	 * invoked once per needed calculation.
	 */
#define	 A_PE_INVOKE	0x0000000002000000

	/* Stop the Policy Engine, it is no  longer required - Only the
	 * DC needs to run the PE.
	 */
#define	 A_PE_STOP	0x0000000004000000


#define	 A_TE_START	0x0000000010000000

	/* Start the Transition Engine */
#define	 A_TE_INVOKE	0x0000000020000000

	/* Attempt to reach the newly  calculated cluster state.  This is 
	 * only called once per transition (except if it is asked to
	 * stop the transition or start a new one).
	 * Once given a cluster state to reach, the TE will determin
	 * tasks that can be performed in parallel, execute them, wait
	 * for replies and then determin the next set until the new
	 * state is reached or no further tasks can be taken.
	 */
#define	 A_TE_STOP	0x0000000040000000

	/* Stop the Transition Engine, it is no longer required - Only
	 * the DC needs to run the TE.
	 */
/* #define  A_ 0x000000000 */


/* -- Misc -- */
	/* Add a system generate "block" so that resources arent moved
	 * to or are activly moved away from the affected node.  This
	 * way we can return quickly even if busy with other things.
	 */
#define	 A_NODE_BLOCK	0x0000000080000000

/*======================================
 *
 * "register" contents
 *
 * Things we may want to remember regardless of which state we are in.
 *
 * These also count as inputs for synthesizing I_*
 *
 *======================================*/
#define	R_THE_DC	0x00000001 /* Are we the DC? */
#define	R_STARTING	0x00000002 /* Are we starting up? */
#define	R_SHUTDOWN	0x00000004 /* Are we trying to shut down? */
#define	R_CIB_DONE	0x00000008 /* Have we calculated the CIB? */

#define R_JOIN_OK	0x00000010 /* Have we completed the join process */
#define R_HAVE_CIB	0x00000020 /* Do we have an up-to-date CIB */
#define	R_HAVE_RES	0x00000040 /* Do we have any resources running
				      locally */
#define	R_INVOKE_PE	0x00000080 /* Does the PE needed to be invoked at
				      the next appropriate point? */

#define	R_CIB_CONNECTED	0x00000100 /* Is the CIB connected? */
#define	R_PE_CONNECTED	0x00000200 /* Is the Policy Engine connected? */
#define	R_TE_CONNECTED	0x00000400 /* Is the Transition Engine connected? */
#define	R_LRM_CONNECTED	0x00000800 /* Is the Local Resource Manager
				      connected? */

#define	R_REQ_PEND	0x00001000 /* Are there Requests waiting for
				      processing? */
#define	R_PE_PEND	0x00002000 /* Has the PE been invoked and we're
				      awaiting a reply? */
#define	R_TE_PEND	0x00004000 /* Has the TE been invoked and we're
				      awaiting completion? */ 
#define	R_RESP_PEND	0x00008000 /* Do we have clients waiting on a
				      response? if so perhaps we shouldnt
				      stop yet */

/* #define	R_	0x00010000 /\* Unused *\/ */
/* #define	R_	0x00020000 /\* Unused *\/ */
/* #define	R_	0x00040000 /\* Unused *\/ */
/* #define	R_	0x00080000 /\* Unused *\/ */
