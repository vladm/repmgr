/*
 * repmgr-action-witness.c
 *
 * Implements witness actions for the repmgr command line utility
 *
 * Copyright (c) 2ndQuadrant, 2010-2018
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <sys/stat.h>

#include "repmgr.h"
#include "dirutil.h"
#include "compat.h"
#include "controldata.h"

#include "repmgr-client-global.h"
#include "repmgr-action-witness.h"

static char		repmgr_user[MAXLEN];
static char		repmgr_db[MAXLEN];

void
do_witness_register(void)
{
	PGconn	   *witness_conn = NULL;
	PGconn	   *primary_conn = NULL;
	RecoveryType recovery_type = RECTYPE_UNKNOWN;
	NodeInfoList nodes = T_NODE_INFO_LIST_INITIALIZER;
	t_node_info node_record = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status = RECORD_NOT_FOUND;
	bool		record_created = false;

	log_info(_("connecting to witness node \"%s\" (ID: %i)"),
			 config_file_options.node_name,
			 config_file_options.node_id);

	witness_conn = establish_db_connection_quiet(config_file_options.conninfo);

	if (PQstatus(witness_conn) != CONNECTION_OK)
	{
		log_error(_("unable to connect to witness node \"%s\" (ID: %i)"),
				  config_file_options.node_name,
				  config_file_options.node_id);
		log_detail("%s",
				   PQerrorMessage(witness_conn));
		log_hint(_("the witness node must be running before it can be registered"));
		exit(ERR_BAD_CONFIG);
	}

	/* check witness node's recovery type */
	recovery_type = get_recovery_type(witness_conn);

	if (recovery_type == RECTYPE_STANDBY)
	{
		log_error(_("provided node is a standby"));
		log_error(_("a witness node must run on an independent primary server"));

		PQfinish(witness_conn);

		exit(ERR_BAD_CONFIG);
	}

	/* check that witness node is not a BDR node */
	if (is_bdr_db_quiet(witness_conn) == true)
	{
		log_error(_("witness node is a BDR node"));
		log_hint(_("a witness node cannot be configured for a BDR cluster"));

		PQfinish(witness_conn);

		exit(ERR_BAD_CONFIG);
	}


	/* connect to primary with provided parameters */
	log_info(_("connecting to primary node"));
	/*
	 * Extract the repmgr user and database names from the conninfo string
	 * provided in repmgr.conf
	 */
	get_conninfo_value(config_file_options.conninfo, "user", repmgr_user);
	get_conninfo_value(config_file_options.conninfo, "dbname", repmgr_db);

	param_set_ine(&source_conninfo, "user", repmgr_user);
	param_set_ine(&source_conninfo, "dbname", repmgr_db);

	/* We need to connect to check configuration and copy it */
	primary_conn = establish_db_connection_by_params(&source_conninfo, false);

	if (PQstatus(primary_conn) != CONNECTION_OK)
	{
		log_error(_("unable to connect to the primary node"));
		log_hint(_("a primary node must be configured before registering a witness node"));

		PQfinish(witness_conn);

		exit(ERR_BAD_CONFIG);
	}

	/* check primary node's recovery type */
	recovery_type = get_recovery_type(witness_conn);

	if (recovery_type == RECTYPE_STANDBY)
	{
		log_error(_("provided primary node is a standby"));
		log_error(_("provide the connection details of the cluster's primary server"));

		PQfinish(witness_conn);
		PQfinish(primary_conn);

		exit(ERR_BAD_CONFIG);
	}

	/* check that primary node is not a BDR node */
	if (is_bdr_db_quiet(primary_conn) == true)
	{
		log_error(_("primary node is a BDR node"));
		log_hint(_("a witness node cannot be configured for a BDR cluster"));

		PQfinish(witness_conn);
		PQfinish(primary_conn);

		exit(ERR_BAD_CONFIG);
	}

	/* XXX sanity check witness node is not part of main cluster */


	/* create repmgr extension, if does not exist */
	if (runtime_options.dry_run == false &&  !create_repmgr_extension(witness_conn))
	{
		PQfinish(witness_conn);
		PQfinish(primary_conn);

		exit(ERR_BAD_CONFIG);
	}

	/*
	 * check if node record exists on primary, overwrite if -F/--force provided,
	 * otherwise exit with error
	 */

	record_status = get_node_record(primary_conn,
									config_file_options.node_id,
									&node_record);

	if (record_status == RECORD_FOUND)
	{
		/*
		 * If node is not a witness, cowardly refuse to do anything, let the
		 * user work out what's the correct thing to do.
		 */
		if (node_record.type != WITNESS)
		{
			log_error(_("node \"%s\" (ID: %i) is already registered as a %s node"),
					  config_file_options.node_name,
					  config_file_options.node_id,
					  get_node_type_string(node_record.type));
			log_hint(_("use \"repmgr %s unregister\" to remove a non-witness node record"),
					 get_node_type_string(node_record.type));

			PQfinish(witness_conn);
			PQfinish(primary_conn);

			exit(ERR_BAD_CONFIG);
		}

		if (!runtime_options.force)
		{
			log_error(_("witness node is already registered"));
			log_hint(_("use option -F/--force to reregister the node"));


			PQfinish(witness_conn);
			PQfinish(primary_conn);

			exit(ERR_BAD_CONFIG);
		}
	}


	// XXX check other node with same name does not exist

	/*
	 * if repmgr.nodes contains entries, delete if -F/--force provided,
	 * otherwise exit with error
	 */
	get_all_node_records(witness_conn, &nodes);

	log_verbose(LOG_DEBUG, "%i node records found", nodes.node_count);

	if (nodes.node_count > 0)
	{
		if (!runtime_options.force)
		{
			log_error(_("witness node is already initialised and contains node records"));
			log_hint(_("use option -F/--force to reinitialise the node"));
			PQfinish(primary_conn);
			PQfinish(witness_conn);
			exit(ERR_BAD_CONFIG);
		}
	}

	clear_node_info_list(&nodes);

	if (runtime_options.dry_run == true)
	{
		log_info(_("prerequisites for registering the witness node are met"));
		PQfinish(primary_conn);
		PQfinish(witness_conn);
		exit(SUCCESS);
	}
	/* create record on primary */

	/*
	 * node record exists - update it (at this point we have already
	 * established that -F/--force is in use)
	 */

	init_node_record(&node_record);

	/* these values are mandatory, setting them to anything else has no point */
	node_record.type = WITNESS;
	node_record.priority = 0;
	node_record.upstream_node_id = get_primary_node_id(primary_conn);

	if (record_status == RECORD_FOUND)
	{
		record_created = update_node_record(primary_conn,
											"witness register",
											&node_record);
	}
	else
	{
		record_created = create_node_record(primary_conn,
											"witness register",
											&node_record);
	}

	if (record_created == false)
	{
		log_error(_("unable to create or update node record on primary"));
		PQfinish(primary_conn);
		PQfinish(witness_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* sync records from primary */
	if (witness_copy_node_records(primary_conn, witness_conn) == false)
	{
		log_error(_("unable to copy repmgr node records from primary"));
		PQfinish(primary_conn);
		PQfinish(witness_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* create event */
	create_event_record(primary_conn,
						&config_file_options,
						config_file_options.node_id,
						"witness_register",
						true,
						NULL);

	PQfinish(primary_conn);
	PQfinish(witness_conn);

	log_info(_("witness registration complete"));
	log_notice(_("witness node \"%s\" (ID: %i) successfully registered"),
			   config_file_options.node_name, config_file_options.node_id);

	return;
}


void
do_witness_unregister(void)
{
	PGconn	   *witness_conn = NULL;
	PGconn	   *primary_conn = NULL;
	t_node_info node_record = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status = RECORD_NOT_FOUND;
	bool		node_record_deleted = false;
	bool		witness_available = true;

	log_info(_("connecting to witness node \"%s\" (ID: %i)"),
			 config_file_options.node_name,
			 config_file_options.node_id);

	witness_conn = establish_db_connection_quiet(config_file_options.conninfo);

	if (PQstatus(witness_conn) != CONNECTION_OK)
	{
		if (!runtime_options.force)
		{
			log_error(_("unable to connect to witness node \"%s\" (ID: %i)"),
					  config_file_options.node_name,
					  config_file_options.node_id);
			log_detail("%s", PQerrorMessage(witness_conn));
			log_hint(_("provide -F/--force to remove the witness record if the server is not running"));
			exit(ERR_BAD_CONFIG);
		}

		log_notice(_("unable to connect to witness node \"%s\" (ID: %i), removing node record on cluster primary only"),
				   config_file_options.node_name,
				   config_file_options.node_id);
		witness_available = false;
	}

	if (witness_available == true)
	{
		primary_conn = get_primary_connection_quiet(witness_conn, NULL, NULL);
	}
	else
	{
		/*
		 * Extract the repmgr user and database names from the conninfo string
		 * provided in repmgr.conf
		 */
		get_conninfo_value(config_file_options.conninfo, "user", repmgr_user);
		get_conninfo_value(config_file_options.conninfo, "dbname", repmgr_db);

		param_set_ine(&source_conninfo, "user", repmgr_user);
		param_set_ine(&source_conninfo, "dbname", repmgr_db);

		primary_conn = establish_db_connection_by_params(&source_conninfo, false);

	}

	if (PQstatus(primary_conn) != CONNECTION_OK)
	{
		log_error(_("unable to connect to primary"));
		log_detail("%s", PQerrorMessage(primary_conn));

		if (witness_available == true)
		{
			PQfinish(witness_conn);
		}
		else
		{
			log_hint(_("provide connection details to primary server"));
		}
		exit(ERR_BAD_CONFIG);
	}

	/* Check node exists and is really a witness */
	record_status = get_node_record(primary_conn, config_file_options.node_id, &node_record);

	if (record_status != RECORD_FOUND)
	{
		log_error(_("no record found for node %i"), config_file_options.node_id);

		if (witness_available == true)
			PQfinish(witness_conn);
		PQfinish(primary_conn);

		exit(ERR_BAD_CONFIG);
	}

	if (node_record.type != WITNESS)
	{
		log_error(_("node %i is not a witness node"), config_file_options.node_id);
		log_detail(_("node %i is a %s node"), config_file_options.node_id, get_node_type_string(node_record.type));

		if (witness_available == true)
			PQfinish(witness_conn);
		PQfinish(primary_conn);

		exit(ERR_BAD_CONFIG);
	}

	if (runtime_options.dry_run == true)
	{
		log_info(_("prerequisites for unregistering the witness node are met"));
		if (witness_available == true)
			PQfinish(witness_conn);
		PQfinish(primary_conn);

		exit(SUCCESS);
	}

	log_info(_("unregistering witness node %i"), config_file_options.node_id);
	node_record_deleted = delete_node_record(primary_conn,
										     config_file_options.node_id);

	if (node_record_deleted == false)
	{
		PQfinish(primary_conn);
		PQfinish(witness_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* sync records from primary */
	if (witness_available == true && witness_copy_node_records(primary_conn, witness_conn) == false)
	{
		log_error(_("unable to copy repmgr node records from primary"));
		PQfinish(primary_conn);
		PQfinish(witness_conn);
		exit(ERR_BAD_CONFIG);
	}

	/* Log the event */
	create_event_record(primary_conn,
						&config_file_options,
						config_file_options.node_id,
						"witness_unregister",
						true,
						NULL);

	PQfinish(primary_conn);

	if (witness_available == true)
		PQfinish(witness_conn);

	log_info(_("witness unregistration complete"));
	log_detail(_("witness node with id %i (conninfo: %s) successfully unregistered"),
			    config_file_options.node_id, config_file_options.conninfo);

	return;
}


void do_witness_help(void)
{
	print_help_header();

	printf(_("Usage:\n"));
	printf(_("    %s [OPTIONS] witness register\n"), progname());
	printf(_("    %s [OPTIONS] witness unregister\n"), progname());

	printf(_("WITNESS REGISTER\n"));
	puts("");
	printf(_("  \"witness register\" registers a witness node.\n"));
	puts("");
	printf(_("  Requires provision of connection information for the primary\n"));
	puts("");
	printf(_("  --dry-run                           check prerequisites but don't make any changes\n"));
	printf(_("  -F, --force                         overwrite an existing node record\n"));
	puts("");

	printf(_("WITNESS UNREGISTER\n"));
	puts("");
	printf(_("  \"witness register\" unregisters a witness node.\n"));
	puts("");
	printf(_("  --dry-run                           check prerequisites but don't make any changes\n"));
	printf(_("  -F, --force                         unregister when witness node not running\n"));
	puts("");

	return;
}
