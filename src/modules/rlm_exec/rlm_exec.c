/*
 * rlm_exe.c
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2002  The FreeRADIUS server project
 * Copyright 2002  Alan DeKok <aland@ox.org>
 */

#include "autoconf.h"
#include "libradius.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radiusd.h"
#include "modules.h"
#include "conffile.h"

static const char rcsid[] = "$Id$";

/*
 *	Define a structure for our module configuration.
 */
typedef struct rlm_exec_t {
	char	*xlat_name;
	int	wait;
	char	*program;
	char	*input;
	char	*output;
} rlm_exec_t;

/*
 *	A mapping of configuration file names to internal variables.
 *
 *	Note that the string is dynamically allocated, so it MUST
 *	be freed.  When the configuration file parse re-reads the string,
 *	it free's the old one, and strdup's the new one, placing the pointer
 *	to the strdup'd string into 'config.string'.  This gets around
 *	buffer over-flows.
 */
static CONF_PARSER module_config[] = {
	{ "wait", PW_TYPE_BOOLEAN,  offsetof(rlm_exec_t,wait), NULL, "yes" },
	{ "program",  PW_TYPE_STRING_PTR,
	  offsetof(rlm_exec_t,program), NULL, NULL },
	{ "input_pairs", PW_TYPE_STRING_PTR,
	  offsetof(rlm_exec_t,input), NULL, NULL },
	{ "output_pairs",  PW_TYPE_STRING_PTR,
	  offsetof(rlm_exec_t,output), NULL, NULL },
	{ NULL, -1, 0, NULL, NULL }		/* end the list */
};


/*
 *	Decode the configuration file string to a pointer to
 *	a value-pair list in the REQUEST data structure.
 */
static VALUE_PAIR **decode_string(REQUEST *request, const char *string)
{
	if (!string) return NULL;

	/*
	 *	Yuck.  We need a 'switch' over character strings
	 *	in C.
	 */
	if (strcmp(string, "request") == 0) {
		return &request->packet->vps;
	}

	if (strcmp(string, "reply") == 0) {
		if (!request->reply) return NULL;

		return &request->reply->vps;
	}

	if (strcmp(string, "proxy-request") == 0) {
		if (!request->proxy) return NULL;

		return &request->proxy->vps;
	}

	if (strcmp(string, "proxy-reply") == 0) {
		if (!request->proxy_reply) return NULL;

		return &request->proxy_reply->vps;
	}

	if (strcmp(string, "config") == 0) {
		return &request->config_items;
	}

	return NULL;
}


/*
 *	Do xlat of strings.
 */ 
static int exec_xlat(void *instance, REQUEST *request,
		     char *fmt, char *out, int outlen,
		     RADIUS_ESCAPE_STRING func)
{
	int		result;
	rlm_exec_t	*inst = instance;
	VALUE_PAIR	**input_pairs;

	input_pairs = decode_string(request, inst->input);
	if (!input_pairs) {
		radlog(L_ERR, "rlm_exec (%s): Failed to find input pairs for xlat",
		       inst->xlat_name);
		out[0] = '\0';
		return 0;
	}

	/*
	 *	FIXME: Do xlat of program name?
	 */
	DEBUG2("rlm_exec (%s): Executing %s", inst->xlat_name, fmt);
	result = radius_exec_program(fmt, request, inst->wait,
				     out, outlen, *input_pairs, NULL);
	DEBUG2("rlm_exec (%s): result %d", inst->xlat_name, result);
	if (result != 0) {
		out[0] = '\0';
		return 0;
	}

	return strlen(out);
}


/*
 *	Detach an instance and free it's data.
 */
static int exec_detach(void *instance)
{
	rlm_exec_t	*inst = instance;

	if (inst->xlat_name) {
		xlat_unregister(inst->xlat_name, exec_xlat);
		free(inst->xlat_name);
	}

	/*
	 *  Free the strings.
	 */
	if (inst->program) free(inst->program);
	if (inst->input) free(inst->input);
	if (inst->output) free(inst->output);

	free(inst);
	return 0;
}


/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int exec_instantiate(CONF_SECTION *conf, void **instance)
{
	rlm_exec_t	*inst;
	char		*xlat_name;
	
	/*
	 *	Set up a storage area for instance data
	 */
	
	inst = rad_malloc(sizeof(rlm_exec_t));
	memset(inst, 0, sizeof(rlm_exec_t));
		
	/*
	 *	If the configuration parameters can't be parsed, then
	 *	fail.
	 */
	if (cf_section_parse(conf, inst, module_config) < 0) {
		radlog(L_ERR, "rlm_exec: Failed parsing the configuration");
		exec_detach(inst);
		return -1;
	}
	
	/*
	 *	No input pairs defined.  Why are we executing a program?
	 */
	if (!inst->input) {
		radlog(L_ERR, "rlm_exec: Must define input pairs for external program.");
		exec_detach(inst);
		return -1;
	}

	/*
	 *	Sanity check the config.  If we're told to NOT wait,
	 *	then the output pairs must not be defined.
	 */
	if (!inst->wait &&
	    (inst->output != NULL)) {
		radlog(L_ERR, "rlm_exec: Cannot read output pairs if wait=no");
		exec_detach(inst);
		return -1;
	}

	xlat_name = cf_section_name2(conf);
	if (xlat_name == NULL) 
		xlat_name = cf_section_name1(conf);
	if (xlat_name){ 
		inst->xlat_name = strdup(xlat_name);
		xlat_register(xlat_name, exec_xlat, inst); 
	} 

	*instance = inst;
	
	return 0;
}


/*
 *  Dispatch an exec method
 */
static int exec_dispatch(void *instance, REQUEST *request)
{
	int result;
	VALUE_PAIR **input_pairs, **output_pairs;
	VALUE_PAIR *answer;
	rlm_exec_t *inst = (rlm_exec_t *) instance;

	/*
	 *	We need a program to execute.
	 */
	if (!inst->program) {
		radlog(L_ERR, "rlm_exec (%s): We require a program to execute",
		       inst->xlat_name);
		return RLM_MODULE_FAIL;
	}

	/*
	 *	Decide what input/output the program takes.
	 */
	input_pairs = decode_string(request, inst->input);
	output_pairs = decode_string(request, inst->output);

	/*
	 *	We need a place to store the returned VP's
	 */
	if (!output_pairs) {
		radlog(L_ERR, "rlm_exec (%s): Nowhere to place output",
		       inst->xlat_name);
		return RLM_MODULE_FAIL;
	}
	
	/*
	 *	This function does it's own xlat of the input program
	 *	to execute.
	 *
	 *	FIXME: if inst->program starts with %{, then
	 *	do an xlat ourselves.  This will allow us to do
	 *	program = %{Exec-Program}, which this module
	 *	xlat's into it's string value, and then the
	 *	exec program function xlat's it's string value
	 *	into something else.
	 */
	result = radius_exec_program(inst->program, request,
				     inst->wait, NULL, 0,
				     *input_pairs, &answer);
	if (result != 0) {
		radlog(L_ERR, "rlm_exec (%s): External script failed",
		       inst->xlat_name);
		return RLM_MODULE_FAIL;
	}

	/*
	 *	Move the answer over to the output pairs.
	 */
	pairmove(output_pairs, &answer);

	pairfree(&answer);

	return RLM_MODULE_OK;
}


/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
module_t rlm_exec = {
	"exec",				/* Name */
	RLM_TYPE_THREAD_SAFE,		/* type */
	NULL,				/* initialization */
	exec_instantiate,		/* instantiation */
	{
		exec_dispatch,		/* authentication */
		exec_dispatch,	        /* authorization */
		exec_dispatch,		/* pre-accounting */
		exec_dispatch,		/* accounting */
		NULL,			/* check simul */
		exec_dispatch,		/* pre-proxy */
		exec_dispatch,		/* post-proxy */
		exec_dispatch		/* post-auth */
	},
	exec_detach,			/* detach */
	NULL,				/* destroy */
};
