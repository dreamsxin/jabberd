#include "jabberd.h"

/* how many seconds until packets begin to "bounce" that should be delivered */
#define ACCEPT_TIMEOUT 600

/* each instance can share ports */

/*
how does this mess work... hmmm...

the accept section lives in an instance
it can share an ip/port w/ another section, but the secret must be unique,
so it either finds an existing listen thread and adds it's secret and instance pair to it,
or starts a new listen thread
and each section registers a "default" sink packet handler
when new connections come in, a read and write thread are started to deal with it
if a valid handshake is negotiated, the write thread starts to pull packets from the default sink handler
if the default sink is already in use by another write thread, another sink packet handler is registered
when the write threads exit, only the default sink would persist, others all r_UNREG

the handshake is:
<handshake host="1234.foo.jabber.org">SHAHASH of id+secret</handshake>
the host attrib is optional, and when used causes a new sink/handler to be registerd in the o_FILTER stage to "hijack" packets to that host
note: the use of the host attrib and <host> element in the config are not automagically paired, must be done by the administrator configuring jabberd and whatever app is using the socket
also, set host="void" to disable any packets or data being written to the socket

<accept>
  <ip>1.2.3.4</ip>
  <port>2020</port>
  <secret>foobar</secret>
</accept>

*/

/* simple wrapper around the pth messages to pass packets */
typedef struct
{
    pth_message_t head; /* the standard pth message header */
    dpacket p;
} *drop, _drop;

/* simple wrapper around pth message ports, so we could use something else in the future easily */
typedef struct
{
    instance i;
    pth_msgport_t mp;
    int flag_open, flag_busy;
    time_t last;
} *sink, _sink;

/* data shared for handlers related to a connection */
typedef struct
{
    int sock, flag_ok, flag_read, flag_write, flag_cond;
    pth_cond_t cond_secret;
    instance i;
    sink s;
    pool p;
    char *id;
} *acceptor, _acceptor;


/* write packets to sink */
result base_accept_phandler(instance i, dpacket p, void *arg)
{
    sink s = (sink)arg;
    drop d;
    
    d = pmalloc(p->p, sizeof(_drop));
    d->p = p;

    pth_msgport_put(s->mp, (pth_message_t *)d);

    return r_OK;
}


typedef enum {s_NONE, s_SECRET, s_WRITE, s_WAIT} state;

void *base_accept_write(void *arg)
{
    acceptor a = (acceptor)arg; /* shared data for this connection */
    dpacket dp; /* the associated packet (if any) */
    char *block; /* the data being written */
    int cur, len, ret; /* current position, total length, return code */
    pool p; /* the associated pool to the data being written (for cleanup) */
    state s = s_NONE; /* current status */
    xmlnode x;

    a->flag_write = 1; /* signal that the write thread is up */

    while(1)
    {
        if(a->s != NULL)
        {
            a->s->last = time(NULL);
            a->s->flag_busy = 0;
        }

        switch(s)
        {
        case s_NONE:
            /* create header */
            x = xstream_header("jabberd:sockets",NULL,NULL);
            p = xmlnode_pool(x);
            a->id = pstrdup(a->p,xmlnode_get_attrib(x,"id"));
            dp = NULL;
            block = xstream_header_char(x);
            len = strlen(block);
            cur = 0;
            s = s_WRITE;
            break;
        case s_SECRET:
            if(a->flag_ok == 0)
            { /* block on condition in acceptor */
            }

            if(!flag_read) break; /* socket died */

            /* based on a->flag_ok being 1 or -1, write <secret/> or fail */
            if(a->flag_ok > 0)
            {
            }else if(a->flag_ok < 0){
            }
            break;
        case s_WAIT:
            if(a->flag_write == 0)
                return; /* special case where the component doesn't want to receive anything */

            /* block on pth_msgport in sink */
            break;
        case s_WRITE:
            if(a->s != NULL)
                a->s->flag_busy = 1;
            /* block on writing to the socket */
            break;
        }
    }

    /* if there's an incomplete packet yet, return to sending pool */
    if(dp != NULL)
        base_accept_phandler(a->i, dp, (void *)(a->s));

    /* tidy up */
    close(a->sock);
    if(a->s != NULL) /* clear the flag on the sink, if any */
        a->s->flag_busy = 0;
    a->flag_write = 0;
    if(!flag_read) /* free the acceptor, if we're the last out the door */
        pool_free(a->p);
}

void base_accept_read_packets(int type, xmlnode x, void *arg)
{
    acceptor a = (acceptor)arg;
    xmlnode cur;
    char *secret;
    spool s;

    switch(type)
    {
    case XSTREAM_ROOT:
        /* setup blocking condition */
        /* spawn write thread */
        pth_spawn(PTH_ATTR_DEFAULT,base_accept_write,arg)
        break;
    case XSTREAM_NODE:
        if(a->flag_ok > 0)
        {
            deliver(dpacket_new(x), a->s->i);
        }else{
            if(j_strcmp(xmlnode_get_name(x),"secret") != 0 || (secret = xmlnode_get_data(x)) == NULL)
            {
                xmlnode_free(x);
                return;
            }

            a->flag_ok = -1;
            /* check the <secret>...</secret> against all known secrets for this port/ip */
            for(cur = xmlnode_get_firstchild(a->secrets); cur != NULL; cur = xmlnode_get_nextsibling(cur))
            {
                s = spool_new(xmlnode_pool(x));
                spooler(s,a->id,xmlnode_get_data(cur),s);
                if(j_strcmp(shahash(spool_print(s)),secret) != 0)
                    continue;

                /* setup flags in acceptor now that we're ok, and set a->s based on the right secret match */
                a->flag_ok = 1;
                a->s = (sink)xmlnode_get_vattrib(cur,"sink");
                if(j_strcmp(xmlnode_get_attrib(x,"state"),"transient") == 0)
                    a->flag_write = 0; /* special hook to tell the write thread to just do it's job and dissappear, this is a read-only connection */
                break;
            }

            /* unblock condition */
        }
        break;
    default:
    }

}

/* thread to read from socket */
void *base_accept_read(void *arg)
{
    acceptor a = (acceptor)arg;
    xstream xs;
    int len;
    char buff[1024];

    xs = xstream_new(a->p, base_accept_read_packets, arg);

    /* spin waiting on data from the socket, feeding to xstream */
    while(1)
    {
        len = pth_read(a->sock, buff, 1024);
        if(len < 0) break;

        if(xstream_eat(xs, buff, len) > XSTREAM_NODE) break;
    }

    /* just cleanup and quit */
    close(a->sock);
    a->flag_read = 0;
    if(!flag_write) /* free the acceptor, if we're the last out the door */
        pool_free(a->p);
}

/* thread to listen on a particular port/ip */
void *base_accept_listen(void *arg)
{
    acceptor a;

    /* look at the port="" and optional ip="" attribs and start listening */

    /* when we get a new socket */
    /* create acceptor */
    pth_spwan(PTH_ATTR_DEFAULT, base_accept_read, (void *)a);
}

/* cleanup routine to make sure packets are getting delivered */
result base_accept_plumber(void *arg)
{
    sink s = (sink)arg;

    while(s->flag_busy && time(NULL) - s->last > ACCEPT_TIMEOUT)
    {
        /* get the messages from the sink and bounce them intelligently */
        fprintf(stderr,"base_accept: bouncing packets\n");
    }

    return r_OK;
}

xmlnode base_accept__listeners;

result base_accept_config(instance id, xmlnode x, void *arg)
{
    char *port, *ip;
    xmlnode cur;
    sink s;

    port = xmlnode_get_data(xmlnode_get_tag(x, "port"));
    ip = xmlnode_get_data(xmlnode_get_tag(x, "ip"));
    if(id == NULL)
    {
        printf("base_accept_config validating configuration\n");
	if(port == NULL || xmlnode_get_data(xmlnode_get_tag(x, "secret")) == NULL)
	    return r_ERR;
        return r_PASS;
    }

    printf("base_accept_config performing configuration %s\n",xmlnode2str(x));

    /* look for an existing accept section that is the same */
    for(cur = xmlnode_get_firstchild(base_accept__listeners); cur != NULL; cur = xmlnode_get_nextsibling(cur))
        if(strcmp(port,xmlnode_get_attrib(cur,"port")) == 0 && (ip == NULL && xmlnode_get_attrib(cur,"ip") == NULL || strcmp(ip,xmlnode_get_attrib(cur,"ip")) == 0))
            break;

    /* create a new section for this section */
    if(cur == NULL)
    {
        cur = xmlnode_insert_tag(base_accept__listeners, "listen");
        xmlnode_put_attrib(cur,"port",port);
        xmlnode_put_attrib(cur,"ip",ip);

        /* start a new listen thread */
        pth_spawn(PTH_ATTR_DEFAULT, base_accept_listen, (void *)cur);
    }

    /* create and configure sink */
    s = pmalloc_x(id->p, sizeof(_sink));
    s->mp = pth_msgport_create("base_accept");
    s->i = id;
    s->last = time(NULL);

    /* register phandler, and register cleanup heartbeat */
    register_phandler(id, o_DELIVER, base_accept_phandler, (void *)sink);
    register_beat(10, base_accept_plumber, (void *)sink);

    /* insert secret into it and hide sink in that new secret */
    xmlnode_put_vattrib(xmlnode_insert_tag_node(cur,xmlnode_get_tag(x,"secret")),"sink",(void *)s);
}

void base_accept(void)
{
    printf("base_accept loading...\n");

    /* master list of all listen threads */
    base_accept__listeners = xmlnode_new_tag("listeners");

    register_config("accept",base_accept_config,NULL);
}
