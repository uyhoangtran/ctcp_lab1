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

#define MAX_BUFF_SIZE MAX_SEG_DATA_SIZE
#define SEGMENT_HDR_SIZE sizeof(ctcp_segment_t)

//char buffer_in[MAX_BUFF_SIZE];
char buffer_out[MAX_BUFF_SIZE];
enum conn_state {
  DATA_TRANSFER,
  WAIT_LAST_ACK,
  RETRANSMITT,
};

struct segment_attr {
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

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  linked_list_t *segments_send;  /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */
  

  /* FIXME: Add other needed fields. */
  conn_state_t conn_state;
  uint32_t seqno;              /* Current sequence number */
  uint32_t next_seqno;         /* Sequence number of next segment to send */
  uint32_t ackno;              /* Current ack number */
  ctcp_segment_t *received_segment;
  uint16_t tim;
  uint16_t timer;               /* How often ctcp_timer() is called, in ms */
  uint16_t rt_timeout;          /* Retransmission timeout, in ms */
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */

static inline void perr(char* str)
{
  fprintf(stderr,"Error: %s\n",str);
  exit(0);
}
static void _segment_hton(ctcp_segment_t *segment)
{
  segment->seqno = htonl(segment->seqno);
  segment->ackno = htonl(segment->ackno);
  segment->len = htons(segment->len);
  segment->flags = htonl(segment->flags);
  segment->window = htons(segment->window);
  /* cksum is already in network byte order */
}

static void _segment_ntoh(ctcp_segment_t *segment)
{
  segment->seqno = ntohl(segment->seqno);
  segment->ackno = ntohl(segment->ackno);
  segment->len = ntohs(segment->len);
  segment->flags = ntohl(segment->flags);
  segment->window = ntohs(segment->window);
}

static void _save_sent_segment(linked_list_t *sent_segment_list, ctcp_segment_t *segment)
{
  ctcp_segment_attr_t *segment_attr = calloc(sizeof(ctcp_segment_attr_t),1);
  segment_attr->time = 0;
  ll_add(sent_segment_list,(void *)segment_attr);
}

static int16_t _segment_send(ctcp_state_t *state,int32_t flags, int32_t len, char* data)
{
  int32_t datalen;
  datalen = len - SEGMENT_HDR_SIZE;
  ctcp_segment_t *segment = calloc(len,1);
  segment->len = len;
  segment->seqno = state->seqno;
  if (flags&FIN){
    state->seqno ++;
  } else
  segment->ackno = state->ackno;
  segment->flags = flags;
  segment->window = MAX_SEG_DATA_SIZE;
  segment->cksum = 0;
  memcpy(segment->data,data,datalen);
  int32_t sum = cksum(segment,len);
  segment->cksum = sum;
  _segment_hton(segment);
  if(conn_send(state->conn,segment,len) < 0)
  {
    return -1;
  }
  state->seqno += datalen;
  _save_sent_segment(state->segments_send,segment);
  return 0;
}

static int16_t _is_segment_valid(ctcp_segment_t *segment,size_t len)
{
  int32_t sum;
  /* check if segment is truncated */
  if (segment->len > len)
    return -1;
  
  /* check if valid cksum */
  sum = segment->cksum;
  segment->cksum = 0;
  if(cksum(segment,segment->len) != sum)
    return -1;
  else
    return 0;
}

static void _destroy_acked_segment(ctcp_state_t *state,uint32_t seqno)
{
 // uint16_t i;
  ll_node_t *node_index;
  ctcp_segment_attr_t *segment_attr;
  ctcp_segment_t *segment;
  node_index = state->segments_send->head;
 // for(i=0; i<(state->segments_send->length); i++)
   while(node_index != NULL)

  {
    segment_attr = (ctcp_segment_attr_t *)node_index->object;
    segment = segment_attr->segment;
    if(segment->ackno == seqno)
    {
      free(segment);
      free(segment_attr);
      ll_remove(state->segments_send,node_index);
      break;
    }
    node_index = node_index->next;
  }
}

void  retransmission_handler(ctcp_state_t *state)
{
  //uint16_t i;
  ll_node_t *node_index;
  ctcp_segment_attr_t *segment_attr;
  ctcp_segment_t *segment;
  node_index = state->segments_send->head;
  //for(i=0; i<(state->segments_send->length); i++)
  while(node_index != NULL)
  {
    segment_attr = (ctcp_segment_attr_t *)node_index->object;
    segment = segment_attr->segment;
    segment_attr->time += state->timer;
    if(segment_attr->time >= state->rt_timeout)
    {
      _segment_send(state,segment->flags,segment->len,segment->data);
    }
    node_index = node_index->next;
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
  state->timer = cfg->timer;
  state->rt_timeout = cfg->rt_timeout;
  /* hoangtu1: create a linked list of segment */
  state->segments_send = ll_create();
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
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
  retval = conn_input(state->conn, buffer_out, MAX_BUFF_SIZE);

  if (-1 == retval) 
  {
    flags = FIN;
    if(_segment_send(state, flags, SEGMENT_HDR_SIZE, NULL) < 0)
    {
      goto exit_read;
    }
    state->conn_state = WAIT_LAST_ACK;
  }
  else
  {
    len = retval + SEGMENT_HDR_SIZE;
    flags = ACK;
    if(_segment_send(state, flags, len, buffer_out) < 0)
    {
      
    }
  }
exit_read: return;
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  if(_is_segment_valid(segment,len) != 0)
  {
    fprintf(stderr,"Received segment is invalid \n");
    goto exit_receive;
  }
  
  _segment_ntoh(segment);
  _destroy_acked_segment(state,segment->seqno);

  if(state->conn_state == DATA_TRANSFER)
  {
    if(segment->ackno <= state->seqno)
    {
      goto exit_receive;
    }
    if(segment->flags & FIN)
    {
#ifdef DEBUG
      fprintf(stderr,"Send ACK of FIN segment\n");
#endif
/* Send EOF to STDOUT */
      conn_output(state->conn,NULL,0);
      state->ackno = segment->seqno + 1;
      if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
      {
        perr("Cannot send ACK of FIN segment");
      }
#ifdef DEBUG
      fprintf(stderr,"Send FIN segment\n");
#endif
/* Send FIN/ACK segment back */
      if(_segment_send(state,FIN|ACK,SEGMENT_HDR_SIZE,NULL) < 0)
      {
        perr("Cannot send FIN segment");
      }
    }
    else if (segment->flags & ACK)
    {
/*Send data to STDOUT */
      state->received_segment = segment;
      ctcp_output(state);
    }
  }
  if(state->conn_state == DATA_TRANSFER)
  {
    if(segment->flags & ACK)
      ctcp_destroy(state);
  }

exit_receive: free(segment);
}

void ctcp_output(ctcp_state_t *state) {
  uint32_t avail_buf,datalen;
  datalen = state->received_segment->len - SEGMENT_HDR_SIZE;
  avail_buf = conn_bufspace(state->conn);
  if (avail_buf == 0)
  {
    return;
  }
  if(avail_buf >= datalen)
  {
    if(conn_output(state->conn,state->received_segment->data,datalen) < 0)
    {
      ctcp_destroy(state);
      return;
    }
    /*Send ACK segment*/
    state->ackno = state->received_segment->seqno + datalen;
    if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
    {
      perr("Cannot send ACK segment");
    }
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
