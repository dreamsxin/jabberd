#include "jabberd.h"

/* ---------------------------------------------------------
   base_exec - Starts a specified coprocess and exchanges 
               xmlnodes with it via piped IO
   ---------------------------------------------------------*/

int exec_and_capture(const char* exe, int* in, int* out)
{
     int left_fds[2], right_fds[2];
     int pid;

     /* Create left and right pipes */
     if (pipe(left_fds) < 0 || pipe(right_fds) < 0)
	  return r_ERR;

     pid = fork();
     if (pid < 0)
	  return r_ERR;
     else if (pid > 0)		/* Parent */
     {
	  /* Close unneeded file handles */
	  close(left_fds[STDIN_FILENO]);
	  close(right_fds[STDOUT_FILENO]);
	  /* Return the in and out file descriptors */
	  *in = right_fds[STDIN_FILENO];
	  *out = left_fds[STDOUT_FILENO];
	  return 0;
     }
     else			/* Child */
     {
	  /* Close unneeded file handles */
	  close(left_fds[STDOUT_FILENO]);
	  close(right_fds[STDIN_FILENO]);
	  /* Map left's STDIN to the child's STDIN */
	  if (left_fds[STDIN_FILENO] != STDIN_FILENO)
	  {
	       dup2(left_fds[STDIN_FILENO], STDIN_FILENO);
	       close(left_fds[STDIN_FILENO]);
	  }
	  /* Map right's STDOUT to the child's STDOUT */
	  if (right_fds[STDOUT_FILENO] != STDOUT_FILENO)
	  {
	       dup2(right_fds[STDOUT_FILENO], STDOUT_FILENO);
	       close(right_fds[STDOUT_FILENO]);
	  }
	  /* Execute another process */
	  if( execl(exe, exe, (char*) 0) < 0)
	       exit(1);
     }
     return 0;
}

/* Structures -------------------------------------------------------------------------------*/

/* process states */
typedef enum { p_OPEN, p_CLOSED } pstate;

/* process_info - stores thread data for a coprocess */
typedef struct
{
     char*         processcmd;	   /* Process command string */
     pstate        state;	   /* Process state flag*/
     pool          mempool;	   /* Memory pool for this structt */
     instance      inst;	   /* Instance this coprocess is assoc. with */
     int           stdin;	   /* Coprocess stdin filehandle */
     int           stdout;	   /* "     "   stdout "      " */
     pth_msgport_t write_queue;	   /* Queue of write_buf packets which need to be written */
     pth_event_t   e_write;	   /* Event set when data is available to be written */
     pth_event_t   e_read;	   /* Event set when data is available to be read */
     pth_event_t   events;	   /* Event ring for e_write & e_read */
} *process_info, _process_info;


/* process_write_buf - stores a dpacket that needs to be written to the coprocess */
typedef struct
{
     pth_message_t head;
     dpacket       packet;
} *process_write_buf, _process_write_buf;



/* Deliver packets to the coprocess*/
result base_exec_deliver(instance i, dpacket p, void* args)
{
     process_info pi = (process_info)args;
     process_write_buf wb = NULL;

     /* Ensure this process is open */
     if (pi->state == p_OPEN)
     {
	  /* Allocate a new write buffer */
	  wb         = pmalloco(p->p, sizeof(_process_write_buf));
	  wb->packet = p;
	  
	  /* Send the buffer to the processing thread */
	  pth_msgport_put(pi->write_queue, (pth_message_t*)wb);

	  return r_OK;   
     }
     /* Otherwise, release the process info and unreg this handler */
     /* FIXME: Should i bounce the packet? */
     else
     {
	  pth_msgport_destroy(pi->write_queue);
	  pool_free(pi->mempool);
	  return r_UNREG;
     }
}

void base_exec_handle_xstream_event(int type, xmlnode x, void* arg)
{
     process_info pi = (process_info)arg;

     switch(type)
     {
     case XSTREAM_ROOT:
	  /* Return a fake root tag */
	  pth_write(pi->stdout, "<root>\n", strlen("<root>\n"));
	  /* Hook the event for delivering messages to the coprocess */
  	  pi->e_write = pth_event(PTH_EVENT_MSG, pi->write_queue);  
  	  pi->events  = pth_event_concat(pi->e_read, pi->e_write, NULL);  
	  /* Validate namespace */
	  break;
     case XSTREAM_NODE:
	  /* Deliver the packet */
	  deliver(dpacket_new(x), pi->inst);
	  break;
     case XSTREAM_CLOSE:
	  /* Set the closed state  */
	  pi->state = p_CLOSED;
     case XSTREAM_ERR:
	  /* FIXME: Who knows? The _SHADOW_ knows. */
     }

}

/* Process incoming data from the coprocess */
void* base_exec_process_io(void* threadarg)
{
     process_info pi = (process_info)threadarg;
     char     readbuf[1024];	   /* Raw buffer to read into */
     int      readlen = 0;	   /* Amount of data read into readbuf */

     xstream  xs;		   /* XMLStream */

     process_write_buf pwb;	   /* Process write buffer */
     char*             writebuf;   /* Raw buffer to write */
     
     /* Setup event ring for this coprocess */
     pi->e_read  = pth_event(PTH_EVENT_FD|PTH_UNTIL_FD_READABLE, pi->stdin);
     pi->events  = pth_event_concat(pi->e_read, NULL);

     /* Allocate an xstream for this coprocess */
     xs = xstream_new(pi->mempool, base_exec_handle_xstream_event, threadarg);

     /* Loop on events */
     while (pth_wait(pi->events) > 0)
     {
	  /* Data is available from coprocess */
	  if (pth_event_occurred(pi->e_read))
	  {
	       readlen = pth_read(pi->stdin, readbuf, sizeof(readbuf));
	       if (readlen <= 0)
	       {
		    printf("base_exec_process_io Read error!\n");
		    break;
	       }
	       if (xstream_eat(xs, readbuf, readlen) > XSTREAM_NODE)
		    break;
	  }
	  /* Data is available to be written to the coprocess, and the coprocess is ready */
	  if (pth_event_occurred(pi->e_write))
	  {
	       /* Get the packet.. */
	       pwb = (process_write_buf)pth_msgport_get(pi->write_queue);
	       
	       /* Serialize the packet.. */
	       writebuf = xmlnode2tstr(pwb->packet->x);

	       /* Write the raw buffer */
	       if (pth_write(pi->stdout, writebuf, strlen(writebuf)) < 0)
	       {
		    printf("base_exec_process_io Write error.\n");
		    break;
	       }
	       
	       /* Data is sent, release the packet */
	       pool_free(pwb->packet->p);
	  }
     }

     /* Cleanup... */
     close(pi->stdout);
     close(pi->stdin);
     pth_event_free(pi->e_read, PTH_FREE_THIS);
     pth_event_free(pi->e_write, PTH_FREE_THIS);

     /* If the process didn't send a closing XML tag, assume it crashed
	and restart it */
     if (pi->state == p_OPEN)
     {
	  /* Exec and capture the STDIN/STDOUT */
	  exec_and_capture(pi->processcmd, &(pi->stdin), &(pi->stdout));

	  /* Recreate the thread */
	  pth_spawn(PTH_ATTR_DEFAULT, base_exec_process_io, (void*) pi);
     }

     return NULL;
}

result base_exec_config(instance id, xmlnode x, void *arg)
{
    process_info pi = NULL;
    int   stdin, stdout;
	  
    if(id == NULL)
    {	 
	 if (xmlnode_get_data(x) == NULL)
	 {
	      printf("base_exec_config error: no script provided\n");
	      return r_ERR;
	 }
	 printf("base_exec_config validating configuration\n");
	 return r_PASS;
    }

    /* Exec and capture the STDIN/STDOUT of the child process */
    exec_and_capture(xmlnode_get_data(x), &stdin, &stdout);

    /* Allocate an info structure, and associate with the
       instance pool */
    pi = pmalloco(id->p, sizeof(_process_info));
    pi->processcmd  = pstrdup(id->p, xmlnode_get_data(x));
    pi->inst        = id;
    pi->mempool     = id->p;
    pi->stdin       = stdin;
    pi->stdout      = stdout;
    pi->write_queue = pth_msgport_create(id->id);   
    pi->state       = p_OPEN;

    /* Spawn a new thread to handle IO for this coprocess */
    pth_spawn(PTH_ATTR_DEFAULT, base_exec_process_io, (void*) pi);

    /* Register a handler to recieve inbound data */
    register_phandler(id, o_DELIVER, base_exec_deliver, (void*) pi);

    printf("base_exec_config performing configuration %s\n",xmlnode2str(x));
    return r_OK;
}

void base_exec(void)
{
    printf("base_exec loading...\n");

    register_config("exec",base_exec_config,NULL);
}
