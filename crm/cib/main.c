/* $Id: main.c,v 1.3 2004/12/05 16:14:07 andrew Exp $ */
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

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <hb_api.h>
#include <clplumbing/uids.h>
#include <clplumbing/coredumps.h>

/* #include <ocf/oc_event.h> */

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/ctrl.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>

#include <cibio.h>
#include <callbacks.h>

#include <crm/dmalloc_wrapper.h>

/* #define REALTIME_SUPPORT 0 */
#define PID_FILE     WORKING_DIR"/"CRM_SYSTEM_CIB".pid"
#define DAEMON_LOG   LOG_DIR"/"CRM_SYSTEM_CIB".log"
#define DAEMON_DEBUG LOG_DIR"/"CRM_SYSTEM_CIB".debug"

GMainLoop*  mainloop = NULL;
const char* crm_system_name = CRM_SYSTEM_CIB;
const char *cib_our_uname = NULL;

void usage(const char* cmd, int exit_status);
int init_start(void);
gboolean cib_register_ha(ll_cluster_t *hb_cluster, const char *client_name);
void cib_shutdown(int nsig);
void cib_ha_connection_destroy(gpointer user_data);
gboolean startCib(const char *filename);

ll_cluster_t *hb_conn = NULL;

#define OPTARGS	"skrhV"

int
main(int argc, char ** argv)
{

	int req_comms_restart = FALSE;
	int req_restart = FALSE;
	int req_status = FALSE;
	int req_stop = FALSE;
	int argerr = 0;
	int flag;
    
#ifdef DEVEL_DIR
	mkdir(DEVEL_DIR, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
#endif
	
	/* Redirect messages from glib functions to our handler */
	g_log_set_handler(NULL,
			  G_LOG_LEVEL_ERROR      | G_LOG_LEVEL_CRITICAL
			  | G_LOG_LEVEL_WARNING  | G_LOG_LEVEL_MESSAGE
			  | G_LOG_LEVEL_INFO     | G_LOG_LEVEL_DEBUG
			  | G_LOG_FLAG_RECURSION | G_LOG_FLAG_FATAL,
			  cl_glib_msg_handler, NULL);
	/* and for good measure... */
	g_log_set_always_fatal((GLogLevelFlags)0);    
	
	cl_log_set_entity   (crm_system_name);
	cl_log_set_facility (LOG_USER);
	cl_log_set_logfile  (DAEMON_LOG);
	cl_log_set_debugfile(DAEMON_DEBUG);

	CL_SIGNAL(DEBUG_INC, alter_debug);
	CL_SIGNAL(DEBUG_DEC, alter_debug);
	CL_SIGNAL(SIGTERM,   cib_shutdown);

	client_list = g_hash_table_new(&g_str_hash, &g_str_equal);
	
	while ((flag = getopt(argc, argv, OPTARGS)) != EOF) {
		switch(flag) {
			case 'V':
				alter_debug(DEBUG_INC);
				break;
			case 's':		/* Status */
				req_status = TRUE;
				break;
			case 'r':		/* Restart */
				req_restart = TRUE;
				break;
			case 'c':		/* Restart */
				req_comms_restart = TRUE;
				break;
			case 'h':		/* Help message */
				usage(crm_system_name, LSB_EXIT_OK);
				break;
			default:
				++argerr;
				break;
		}
	}

	set_crm_log_level(LOG_TRACE);

	cl_set_corerootdir(DEVEL_DIR);	    
	cl_enable_coredumps(1);
	cl_cdtocoredir();
	
	if (optind > argc) {
		++argerr;
	}
    
	if (argerr) {
		usage(crm_system_name,LSB_EXIT_GENERIC);
	}
    
	/* read local config file */
    
	if (req_status){
		return init_status(PID_FILE, crm_system_name);
	}
  
	if (req_stop){
		return init_stop(PID_FILE);
	}
  
	if (req_restart) { 
		init_stop(PID_FILE);
	}

	return init_start();
}


int
init_start(void)
{
	gboolean was_error = FALSE;
#ifdef REALTIME_SUPPORT
	static int  crm_realtime = 1;
#endif

	hb_conn = ll_cluster_new("heartbeat");
	cib_register_ha(hb_conn, CRM_SYSTEM_CIB);

	if(startCib(CIB_FILENAME) == FALSE){
		crm_crit("Cannot start CIB... terminating");
		exit(1);
	}
	
	was_error = init_server_ipc_comms(
		crm_strdup("cib_callback"), cib_client_connect,
		default_ipc_connection_destroy);

	was_error = was_error || init_server_ipc_comms(
		crm_strdup("cib_ro"), cib_client_connect,
		default_ipc_connection_destroy);

	was_error = was_error || init_server_ipc_comms(
		crm_strdup("cib_rw"), cib_client_connect,
		default_ipc_connection_destroy);

	if(was_error == FALSE) {
		/* Create the mainloop and run it... */
		mainloop = g_main_new(FALSE);
		crm_info("Starting %s mainloop", crm_system_name);
		
#ifdef REALTIME_SUPPORT
		if (crm_realtime == 1) {
			cl_enable_realtime();

		} else if (crm_realtime == 0) {
			cl_disable_realtime();
		}
		cl_make_realtime(SCHED_RR, 5, 64, 64);
#endif
		g_main_run(mainloop);
		return_to_orig_privs();

	} else {
		crm_err("Couldnt start all communication channels, exiting.");
	}
	
	return 0;
}

void
usage(const char* cmd, int exit_status)
{
	FILE* stream;

	stream = exit_status ? stderr : stdout;

	fprintf(stream, "usage: %s [-srkh]"
		"[-c configure file]\n", cmd);
/* 	fprintf(stream, "\t-d\tsets debug level\n"); */
/* 	fprintf(stream, "\t-s\tgets daemon status\n"); */
/* 	fprintf(stream, "\t-r\trestarts daemon\n"); */
/* 	fprintf(stream, "\t-k\tstops daemon\n"); */
/* 	fprintf(stream, "\t-h\thelp message\n"); */
	fflush(stream);

	exit(exit_status);
}


gboolean
cib_register_ha(ll_cluster_t *hb_cluster, const char *client_name)
{
	int facility;
	
	if(safe_val3(NULL, hb_cluster, llc_ops, errmsg) == NULL) {
		crm_crit("cluster errmsg function unavailable");
	}
	
	crm_info("Signing in with Heartbeat");
	if (hb_cluster->llc_ops->signon(hb_cluster, client_name)!= HA_OK) {

		crm_err("Cannot sign on with heartbeat: %s",
			hb_cluster->llc_ops->errmsg(hb_cluster));
		return FALSE;
	}

	/* change the logging facility to the one used by heartbeat daemon */
	crm_info("Switching to Heartbeat logger");
	if (( facility =
	      hb_cluster->llc_ops->get_logfacility(hb_cluster)) > 0) {
		cl_log_set_facility(facility);
 	}	
	crm_verbose("Facility: %d", facility);	
  
	crm_debug("Be informed of CIB messages");
	if (HA_OK != hb_cluster->llc_ops->set_msg_callback(
		    hb_cluster, T_CIB, cib_peer_callback, hb_cluster)){
		
		crm_err("Cannot set msg callback: %s",
			hb_cluster->llc_ops->errmsg(hb_cluster));
		return FALSE;
	}

	crm_debug("Finding our node name");
	if ((cib_our_uname =
	     hb_cluster->llc_ops->get_mynodeid(hb_cluster)) == NULL) {
		crm_err("get_mynodeid() failed");
		return FALSE;
	}
	crm_info("FSA Hostname: %s", cib_our_uname);

	crm_debug("Adding channel to mainloop");
	G_main_add_IPC_Channel(
		G_PRIORITY_HIGH, hb_cluster->llc_ops->ipcchan(hb_cluster),
		FALSE, cib_ha_dispatch, hb_cluster /* userdata  */,  
		cib_ha_connection_destroy);

	return TRUE;
    
}

void
cib_ha_connection_destroy(gpointer user_data)
{
}

void
cib_shutdown(int nsig)
{
	static int shuttingdown = 0;
	CL_SIGNAL(nsig, cib_shutdown);
  
	if (!shuttingdown) {
		shuttingdown = 1;
	}
	if (mainloop != NULL && g_main_is_running(mainloop)) {
		g_main_quit(mainloop);
	} else {
		exit(LSB_EXIT_OK);
	}
}

gboolean
startCib(const char *filename)
{
	xmlNodePtr cib = readCibXmlFile(filename);
	if (initializeCib(cib)) {
		crm_info("CIB Initialization completed successfully");
	} else { 
		/* free_xml(cib); */
		crm_warn("CIB Initialization failed, "
			 "starting with an empty default.");
		activateCibXml(createEmptyCib(), filename);
	}
	return TRUE;
}
