/* --------------------------------------------------------------------------
 *
 * License
 *
 * The contents of this file are subject to the Jabber Open Source License
 * Version 1.0 (the "JOSL").  You may not copy or use this file, in either
 * source code or executable form, except in compliance with the JOSL. You
 * may obtain a copy of the JOSL at http://www.jabber.org/ or at
 * http://www.opensource.org/.  
 *
 * Software distributed under the JOSL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied.  See the JOSL
 * for the specific language governing rights and limitations under the
 * JOSL.
 *
 * Copyrights
 * 
 * Portions created by or assigned to Jabber.com, Inc. are 
 * Copyright (c) 1999-2002 Jabber.com, Inc.  All Rights Reserved.  Contact
 * information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 * Portions Copyright (c) 1998-1999 Jeremie Miller.
 * 
 * Acknowledgements
 * 
 * Special thanks to the Jabber Open Source Contributors for their
 * suggestions and support of Jabber.
 * 
 * Alternatively, the contents of this file may be used under the terms of the
 * GNU General Public License Version 2 or later (the "GPL"), in which case
 * the provisions of the GPL are applicable instead of those above.  If you
 * wish to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the JOSL,
 * indicate your decision by deleting the provisions above and replace them
 * with the notice and other provisions required by the GPL.  If you do not
 * delete the provisions above, a recipient may use your version of this file
 * under either the JOSL or the GPL. 
 * 
 * --------------------------------------------------------------------------*/

#include "jadc2s.h"

/* IP connection rate info */
typedef struct connection_rate_st
{
    char* ip; /* need a copy of the ip */
    int count; /* How many have connected */
    time_t first_time; /* The time of the first conn */
} *connection_rate_t;

/* arg for walker */
struct connection_rate_arg_st
{
    c2s_t c2s;
    time_t now;
};

static void _walk_rate_cleanup(xht rates, const char* key, void* val, void* arg)
{
    /* XXX Threading issues */
    connection_rate_t rate = (connection_rate_t)val;
    struct connection_rate_arg_st* info = (struct connection_rate_arg_st*)arg;
    c2s_t c2s = (c2s_t)info->c2s;

    log_debug(ZONE, "key(%s) val: %x", key, val);
    if ((info->now - rate->first_time) > c2s->connection_rate_seconds)
    {
        log_debug(ZONE, "free and zap");
        xhash_zap(rates, key);
        free(rate->ip);
        free(rate);
    }
}
    
/***
* walk over the rate table and clear out the old entries
* @param c2s the c2s context
*/
void connection_rate_cleanup(c2s_t c2s)
{
    static time_t last;
    struct connection_rate_arg_st arg;
    time_t now;

    /* If there are no entries or it's not dirty, bail */
    if ((xhash_count(c2s->connection_rates) <= 0) && 
            (!xhash_dirty(c2s->connection_rates)))
        return;

    if ((time(&now) - last) > c2s->connection_rate_seconds)
    {
        /* Setup the argument to the walker */
        arg.c2s = c2s;
        arg.now = now;
        
        xhash_walk(c2s->connection_rates, _walk_rate_cleanup, (void*)&arg);
        time(&last);
    }
}

/***
* See if a connection is within the rate limit
*
* @param c2s the c2s context
* @param ip the ip to check
* @return 0 on valid 1 on invalid
*/
int connection_rate_check(c2s_t c2s, const char* ip)
{
    connection_rate_t cr;
    time_t now;
    
    /* See if this is disabled */
    if (c2s->connection_rate_times == 0 || c2s->connection_rate_seconds == 0)
        return 0;

    /* XXX TODO This will need to be locked if it is threaded */
    cr = (connection_rate_t)xhash_get(c2s->connection_rates, ip);

    /* If it is NULL they are the first of a possible series */
    if (cr == NULL)
    {
        cr = malloc(sizeof(struct connection_rate_st));
        cr->ip = strdup(ip);
        cr->count = 1;
        time(&cr->first_time);
        xhash_put(c2s->connection_rates, cr->ip, (void*)cr);
        return 0;
    }

    /* If they are outside the time limit just reset them */
    if ((time(&now) - cr->first_time) > c2s->connection_rate_seconds)
    {
        cr->first_time = now;
        cr->count = 1;
        return 0;
    }

    /* see if they have too many conns */
    cr->count++;
    if (cr->count > c2s->connection_rate_times)
        return 1;
    
    return 0;
}
