/* --------------------------------------------------------------------------
 *
 * License
 *
 * The contents of this file are subject to the Jabber Open Source License
 * Version 1.0 (the "License").  You may not copy or use this file, in either
 * source code or executable form, except in compliance with the License.  You
 * may obtain a copy of the License at http://www.jabber.com/license/ or at
 * http://www.opensource.org/.  
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied.  See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * Copyrights
 * 
 * Portions created by or assigned to Jabber.com, Inc. are 
 * Copyright (c) 1999-2000 Jabber.com, Inc.  All Rights Reserved.  Contact
 * information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 * Portions Copyright (c) 1998-1999 Jeremie Miller.
 * 
 * Acknowledgements
 * 
 * Special thanks to the Jabber Open Source Contributors for their
 * suggestions and support of Jabber.
 * 
 * --------------------------------------------------------------------------*/

#include "jabberd.h"

result base_file_deliver(instance id, dpacket p, void* arg)
{
    FILE* f = (FILE*)arg;
    char* message = NULL;

    message = xmlnode_get_data(p->x);
    if (message == NULL)
    {
       log_debug(ZONE,"base_file_deliver error: no message available to print.\n");
       return r_ERR;
    }

    if (fprintf(f,"%s\n", message) == EOF)
    {
        log_debug(ZONE,"base_file_deliver error: error writing to file(%d).\n", errno);
        return r_ERR;
    }
    fflush(f);

    /* Release the packet */
    pool_free(p->p);
    return r_DONE;    
}

void _base_file_shutdown(void *arg)
{
    FILE *filehandle=(FILE*)arg;
    fclose(filehandle);
}

result base_file_config(instance id, xmlnode x, void *arg)
{
    FILE* filehandle = NULL;
        
    if(id == NULL)
    {
        log_debug(ZONE,"base_file_config validating configuration");

        if (xmlnode_get_data(x) == NULL)
        {
            log_debug(ZONE,"base_file_config error: no filename provided.");
            xmlnode_put_attrib(x,"error","'file' tag must contain a filename to write to");
            return r_ERR;
        }
        return r_PASS;
    }

    log_debug(ZONE,"base_file configuring instance %s",id->id);

    if(id->type != p_LOG)
    {
        log_alert(NULL,"ERROR in instance %s: <file>..</file> element only allowed in log sections", id->id);
        return r_ERR;
    }

    /* Attempt to open/create the file */
    filehandle = fopen(xmlnode_get_data(x), "a");
    if (filehandle == NULL)
    {
        log_alert(NULL,"base_file_config error: error opening file (%d): %s", errno, strerror(errno));
        return r_ERR;
    }

    /* Register a handler for this instance... */
    register_phandler(id, o_DELIVER, base_file_deliver, (void*)filehandle); 

    pool_cleanup(id->p, _base_file_shutdown, (void*)filehandle); 
    
    return r_DONE;
}

void base_file(void)
{
    log_debug(ZONE,"base_file loading...");
    register_config("file",base_file_config,NULL);
}