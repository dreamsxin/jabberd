/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  Jabber
 *  Copyright (C) 1998-1999 The Jabber Team http://jabber.org/
 */

/*

theory:
    we can sit in the middle and intercept all packets
    we watch all xdb set's, and store them in a hash based on owner jid
    we watch all gets, and check to see if we have something stored
    if we do, we check the lsat time it was set and compare against timeout

*/

#include "jabberd.h"

result base_cache_config(instance id, xmlnode x, void *arg)
{
    int timeout = -1;
    char *tstr;

    /* get the timeout value */
    tstr = xmlnode_get_data(x);
    if(tstr != NULL)
        timeout = atoi(tstr);

    if(id == NULL)
    {
        log_debug(ZONE,"base_cache_config validating configuration\n");
        if(j_strcmp(xmlnode_get_name(xmlnode_get_parent(x)),"xdb") != 0)
        {
            xmlnode_put_attrib(x,"error","'cache' is only valid in an xdb section");
            return r_ERR;
        }
        if(tstr != NULL && timeout <= 0)
        {
            xmlnode_put_attrib(x,"error","'cache' must contain an integer greater than zero for the number of seconds to cache xdb data");
            return r_ERR;
        }
        return r_PASS;
    }

    log_debug(ZONE,"base_cache_config performing configuration %s\n",xmlnode2str(x));

    return r_PASS;
}

void base_cache(void)
{
    log_debug(ZONE,"base_cache loading...\n");

    register_config("cache",base_cache_config,NULL);
}
