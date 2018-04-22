/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

#define DEBUG
#define MAX_BUFF_SIZE MAX_SEG_DATA_SIZE
#define SEGMENT_HDR_SIZE sizeof(ctcp_segment_t)

#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...)\
        do { fprintf(stderr, "[DEBUG] %10s:%10d:%15s(): " fmt, __FILE__, __LINE__, __func__,## __VA_ARGS__); } while (0)
#else
#define DEBUG_PRINT(fmt, ...)  do {} while (0)
#endif

char buffer_out[MAX_BUFF_SIZE];
enum conn_state {
  DATA_TRANSFER,
  TEAR_DOWN,
  WAIT_LAST_ACK,
  WAIT_LAST_FIN,
};

struct segment_attr {
  uint16_t no_of_times;
  uint16_t time;
  ctcp_segment_t *segment;
};

typedef struct segment_attr ctcp_segment_attr_t;
typedef enum conn_state conn_state_t;
/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t          *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */

  /* FIXME: Add other needed fields. */
  conn_state_t    conn_state;
  uint32_t        seqno;              /* Current sequence number */
  uint32_t        ackno;              /* Current ack number */
  ctcp_segment_t  *received_segment;
  ctcp_segment_attr_t *sent_segment_attr;
  
  bool            wait_teardown;
  uint16_t        tim;
  uint16_t        timer;               /* How often ctcp_timer() is called, in ms */
  uint16_t        rt_timeout;          /* Retransmission timeout, in ms */
  uint32_t        fin_ackno; /* seq and ack of FIN segments */
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */


static void _segment_hton(ctcp_segment_t *segment)
{
  segment->seqno = htonl(segment->seqno);
  segment->ackno = htonl(segment->ackno);
  segment->len = htons(segment->len);
  segment->flags = htonl(segment->flags);
  segment->window = htons(segment->window);
}
static void _segment_ntoh(ctcp_segment_t *segment)
{
  segment->seqno = ntohl(segment->seqno);
  segment->ackno = ntohl(segment->ackno);
  segment->len = ntohs(segment->len);
  segment->flags = ntohl(segment->flags);
  segment->window = ntohs(segment->window);
}

static void _save_sent_segment(ctcp_state_t *state, ctcp_segment_t *segment)
{
  state->sent_segment_attr = calloc(sizeof(ctcp_segment_attr_t),1);
  state->sent_segment_attr->time = 0;
  state->sent_segment_attr->no_of_times = 0;
  state->sent_segment_attr->segment = segment;
}

static int16_t _segment_send(ctcp_state_t *state,int32_t flags, int32_t len, char* data)
{
  int32_t datalen;
  datalen = len - SEGMENT_HDR_SIZE;
  ctcp_segment_t *segment = calloc(len,1);
  segment->len = len;
  segment->seqno = state->seqno;
  segment->ackno = state->ackno;
  segment->flags = flags;
  segment->window = MAX_SEG_DATA_SIZE;
  memcpy(segment->data,data,datalen);
  _segment_hton(segment);
  segment->cksum = 0;
  int32_t sum = cksum(segment,len);
  segment->cksum = sum;
  if(conn_send(state->conn,segment,len) < 0)
  {
    return -1;
  }

  if (flags & FIN)
  {
    state->seqno ++;
    state->fin_ackno = state->ackno;
  }
  else
  {
    state->seqno += datalen;
  }
  if ((datalen > 0) || (flags & FIN))
  {
    _save_sent_segment(state,segment);
  }
  return len;
}

static int16_t _is_segment_valid(ctcp_segment_t *segment,uint16_t len)
{
  uint16_t sum;
  uint16_t segment_len = ntohs(segment->len);

  /* check if segment is truncated */
  if (segment_len > len)
  { 
    DEBUG_PRINT("Segment is truncated\n");
    return -1;
  }

  /* check if valid cksum */
  sum = segment->cksum;
  segment->cksum = 0;
  if(cksum(segment,segment_len) != sum)
  {
    DEBUG_PRINT("cksum failed %x %x\n",cksum(segment,segment_len),sum);
    return -1;
  }

  return 0;
}

static void _destroy_acked_segment(ctcp_state_t *state)
{
  if(state->sent_segment_attr != NULL)
  {
    free(state->sent_segment_attr->segment);
    free(state->sent_segment_attr);
    state->sent_segment_attr = NULL;
  }
}

void  retransmission_handler(ctcp_state_t *state)
{
  ctcp_segment_attr_t *segment_attr = state->sent_segment_attr;

  if (state->wait_teardown == 1)
  {
    state->tim += state->timer;
    if(state->tim >= (state->rt_timeout)*50)
    {
      ctcp_destroy(state);
    }
    return;
  }

  if(segment_attr != NULL) 
  {
    segment_attr->time += state->timer;
    if(segment_attr->time >= state->rt_timeout)
    {
      conn_send(state->conn,segment_attr->segment,ntohs(segment_attr->segment->len));
      segment_attr->no_of_times ++;
      segment_attr->time = 0;
      if(segment_attr->no_of_times >= 5)
      {
        state->tim = 0;
        state->wait_teardown = 1;
      }
    }
  }
}

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;
  
  /* Set fields. */
  state->conn = conn;
  state->ackno = 1;
  state->seqno = 1;
  state->tim = 0;
  state->wait_teardown = 0;
  state->timer = cfg->timer;
  state->rt_timeout = cfg->rt_timeout;
  state->conn_state = DATA_TRANSFER;
  state->sent_segment_attr = NULL;
  state->received_segment = NULL;
  /* hoangtu1: create a linked list of segment */
  return state;
}

void ctcp_destroy(ctcp_state_t *state) 
{
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */
  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  uint32_t retval,len,flags = 0;
  bzero(buffer_out,MAX_BUFF_SIZE);

  /* Only send segment when previous segment is ACKed */
  if (state->sent_segment_attr == NULL)
  {
    retval = conn_input(state->conn, buffer_out, MAX_BUFF_SIZE);
    if (-1 == retval) 
    {
      flags = FIN;
      if(_segment_send(state, flags, SEGMENT_HDR_SIZE, NULL) < 0)
      {
        DEBUG_PRINT("Fail to send FIN segment\n");
        goto exit_read;
      }
      state->conn_state=TEAR_DOWN;
    }
    else
    {
      len = retval + SEGMENT_HDR_SIZE;
      flags = ACK;
      if((retval=_segment_send(state, flags, len, buffer_out)) < 0)
      {      
        DEBUG_PRINT("Fail to send data segment\n");
      }
    }
  }

exit_read: return;
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {

  /* validate segments */
  if(_is_segment_valid(segment,(uint16_t)len) != 0)
  {
    DEBUG_PRINT("Received segment is invalid \n");
    goto exit_receive;
  }

  _segment_ntoh(segment);
  switch(state->conn_state)
  {
    case DATA_TRANSFER:
    {
      /* Check for duplicated segments */
      if( ((segment->seqno) < state->ackno))
      {
        goto exit_receive;
      }

      /* If receive a FIN, send ACK then send FIN */
      if(segment->flags & FIN)
      {
        /* Send EOF to STDOUT */
        conn_output(state->conn,NULL,0);
        state->ackno = segment->seqno + 1;
        if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
        {
          DEBUG_PRINT("Cannot send ACK of FIN segment\n");
        }

        /* Send FIN/ACK segment back */
        if(_segment_send(state,FIN,SEGMENT_HDR_SIZE,NULL) < 0)
        {
          DEBUG_PRINT("Cannot send FIN segment\n");
        }
        state->conn_state = WAIT_LAST_ACK;
      }

      else if (segment->flags & ACK)
      {
        if(segment->len == SEGMENT_HDR_SIZE)
        {
          state->ackno = segment->seqno;
          _destroy_acked_segment(state);
          free(segment);
        }
        else{
        /*Send data to STDOUT */
        state->received_segment = segment;
        ctcp_output(state);
        }
      }
      break;
    }
    case TEAR_DOWN:
    {
      if(segment->flags & ACK)
      {
        if (segment->seqno == state->fin_ackno)
          state->conn_state = WAIT_LAST_FIN;
        break;
      }
      else if (segment->flags & FIN)
      {
        state->ackno = segment->seqno + 1;
        if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
        {
          DEBUG_PRINT("Cannot send ACK segment\n");
          goto exit_receive;
        }
        ctcp_destroy(state);
        break;
      }
    }
    case WAIT_LAST_FIN:
    {
      if(segment->flags & FIN)
      {
        state->ackno = segment->seqno + 1;
        if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
        {
          DEBUG_PRINT("Cannot send ACK segment\n");
          goto exit_receive;
        }
        state->tim = 0 ;
        state->wait_teardown = 1;
        DEBUG_PRINT("WAIT TO DESTROY\n");
      }
      break;
    }
    case WAIT_LAST_ACK:
    {
      if(segment->flags & ACK)
        ctcp_destroy(state);
      break;
    }
  }
exit_receive: return;
}

void ctcp_output(ctcp_state_t *state) 
{
  uint32_t avail_buf,datalen;

  datalen = state->received_segment->len - SEGMENT_HDR_SIZE;

  /* Check if there are empty spaces */
  avail_buf = conn_bufspace(state->conn);

  if (avail_buf == 0)
  {
    DEBUG_PRINT("No free space in the buffer \n");
    free(state->received_segment);
    return;
  }

  if(avail_buf >= datalen)
  {
    if(conn_output(state->conn,state->received_segment->data,datalen) < 0)
    {
      DEBUG_PRINT("Cannot output\n");
      ctcp_destroy(state);
      return;
    }
    /*Send ACK segment*/
    state->ackno = state->received_segment->seqno + datalen;
    if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
    {
      DEBUG_PRINT("Cannot send ACK segment\n");
    }
    _destroy_acked_segment(state);
    free(state->received_segment);
  }
}

void ctcp_timer() {
  /* FIXME */
  ctcp_state_t *state = state_list;
  while(NULL != state)
  {
    retransmission_handler(state);
    state = state_list->next;
  }
}
