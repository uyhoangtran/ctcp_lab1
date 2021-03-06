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
#define DEBUG 1
#define MAX_BUFF_SIZE MAX_SEG_DATA_SIZE
#define SEGMENT_HDR_SIZE sizeof(ctcp_segment_t)


//char buffer_in[MAX_BUFF_SIZE];
char buffer_out[MAX_BUFF_SIZE];
enum conn_state {
  WAIT_INPUT,
  DATA_TRANSFER,
  TEAR_DOWN,
  WAIT_LAST_ACK,
  WAIT_LAST_FIN,
 // RETRANSMITT,
};

enum teardown_state {
  NOT_TEARDOWN,
  WAIT_DESTROY,
  DESTROYED,
};

struct segment_attr {
  uint16_t no_of_times;
  uint16_t time;
  ctcp_segment_t *segment;
};

struct tear_down_nums {
  uint32_t fin_seq_no;
  uint32_t fin_ack_no;
};
typedef struct tear_down_nums ctcp_tear_down_nums_t;
typedef struct segment_attr ctcp_segment_attr_t;
typedef enum conn_state conn_state_t;
typedef enum teardown_state td_state_t;
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
 // uint32_t net_seqno;          /* Sequence number of connected host */
  uint32_t ackno;              /* Current ack number */
  ctcp_segment_t *received_segment;
  ctcp_segment_attr_t *sent_segment_attr;
  td_state_t td_state;
  uint16_t tim;
  uint16_t timer;               /* How often ctcp_timer() is called, in ms */
  uint16_t rt_timeout;          /* Retransmission timeout, in ms */
  ctcp_tear_down_nums_t* tear_down_nums; /* seq and ack of FIN segments */
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
  #ifdef DEBUG
  fprintf(stderr,"Error: %s\n",str);
  #endif
}

static inline void _print_segment_info(ctcp_segment_t *segment)
{
  fprintf(stderr,"   SeqNo: %x\n   AckNo: %x\n   len: %x\n   flags: %d\n   cksum: %d\n",segment->seqno,segment->ackno,segment->len,segment->flags,segment->cksum);
}

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
  fprintf(stderr,"Sent segment:\n");
  _print_segment_info(segment);
  fprintf(stderr,"\n");
 if (flags&FIN){
    state->seqno ++;
    state->tear_down_nums = calloc(sizeof(ctcp_tear_down_nums_t),1);
    state->tear_down_nums->fin_seq_no = state->seqno;
    state->tear_down_nums->fin_ack_no = state->ackno;
  }
  else{
  state->seqno += datalen;
  }
  if((datalen > 0) || flags&FIN)
  _save_sent_segment(state,segment);
  return len;
}

static int16_t _is_segment_valid(ctcp_segment_t *segment,uint16_t len)
{
  uint16_t sum;
  uint16_t segment_len = ntohs(segment->len);
  /* check if segment is truncated */
  if (segment_len > len)
  { 
    return -1;
  }
  /* check if valid cksum */
  sum = segment->cksum;
  segment->cksum = 0;
  if(cksum(segment,segment_len) != sum)
  {
    fprintf(stderr,"cksum failed %x %x\n",cksum(segment,segment_len),sum);
    return -1;
  }
    return 0;
}

static void _destroy_acked_segment(ctcp_state_t *state)
{
  if(state->sent_segment_attr != NULL){
    free(state->sent_segment_attr->segment);
    free(state->sent_segment_attr);
    state->sent_segment_attr = NULL;
  }
}

void  retransmission_handler(ctcp_state_t *state)
{
  ctcp_segment_attr_t *segment_attr = state->sent_segment_attr;
  switch (state->td_state)
  {
    case NOT_TEARDOWN:
      break;
    case WAIT_DESTROY:
    {
      state->tim += state->timer;
      if(state->tim >= (state->rt_timeout)*50){
        ctcp_destroy(state);
        return;
      }
      break;
    }
    case DESTROYED:
    {
      break;
    }
  }
  if(segment_attr != NULL) {
  segment_attr->time += state->timer;
  if(segment_attr->time >= state->rt_timeout)
  {
    conn_send(state->conn,segment_attr->segment,ntohs(segment_attr->segment->len));
    segment_attr->no_of_times ++;
    if(segment_attr->no_of_times >= 5)
      _destroy_acked_segment(state);
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
  state->td_state = NOT_TEARDOWN;
  state->timer = cfg->timer;
  state->rt_timeout = cfg->rt_timeout;
  state->conn_state = DATA_TRANSFER;
  state->sent_segment_attr = NULL;
  /* hoangtu1: create a linked list of segment */
 // state->sent_segment_attr = calloc(sizeof(ctcp_segment_attr_t),1);
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
  ll_destroy(state->segments_send);
  free(state->tear_down_nums);
  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  uint32_t retval,len,flags = 0;
  bzero(buffer_out,MAX_BUFF_SIZE);
  if (state->sent_segment_attr == NULL){
  retval = conn_input(state->conn, buffer_out, MAX_BUFF_SIZE);
  if (-1 == retval) 
  {
    flags = FIN;
    if(_segment_send(state, flags, SEGMENT_HDR_SIZE, NULL) < 0)
    {
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

    }
  }}
exit_read: return;
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  
  fprintf(stderr,"Received segment:\n");
  _print_segment_info(segment);  
  fprintf(stderr,"\n");

  if(_is_segment_valid(segment,(uint16_t)len) != 0)
  {
    fprintf(stderr,"Received segment is invalid \n");
    goto exit_receive;
  }
  _segment_ntoh(segment);
  switch(state->conn_state){
    case DATA_TRANSFER:
  {
    if( ((segment->seqno) < state->ackno))
    {
      goto exit_receive;
    }
    if(segment->flags & FIN)
    {
      if(DEBUG)
        fprintf(stderr,"Send ACK of FIN segment\n");
/* Send EOF to STDOUT */
      conn_output(state->conn,NULL,0);
      state->ackno = segment->seqno + 1;
      if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
      {
        perr("Cannot send ACK of FIN segment");
      }
      if(DEBUG)
        fprintf(stderr,"Send FIN segment\n");
/* Send FIN/ACK segment back */
      if(_segment_send(state,FIN,SEGMENT_HDR_SIZE,NULL) < 0)
      {
        perr("Cannot send FIN segment");
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
      if (segment->flags & FIN)
      {
        state->conn_state = WAIT_LAST_ACK;
      }
      else
      {
        state->conn_state = WAIT_LAST_FIN;
      }
      break;
    }
    else if (segment->flags & FIN)
    {
      state->ackno = segment->seqno + 1;
      if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
      {
        perr("Cannot send last ACK segment");
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
        perr("Cannot send last ACK segment");
      }
    //    ctcp_destroy(state);
      state->tim = 0 ;
      state->td_state = WAIT_DESTROY;
    }
    break;
  }
  case WAIT_LAST_ACK:
  {
    if(segment->flags & ACK)
      ctcp_destroy(state);
    break;
  }
  default : {

  }

  }
exit_receive: return;
}

void ctcp_output(ctcp_state_t *state) {
  uint32_t avail_buf,datalen;
  datalen = state->received_segment->len - SEGMENT_HDR_SIZE;
  avail_buf = conn_bufspace(state->conn);
  if (avail_buf == 0)
  {
    fprintf(stderr,"No available buffer \n");
    free(state->received_segment);
    return;
  }
  if(avail_buf >= datalen)
  {
    if(datalen == 0)
      {
        state->ackno = state->received_segment->seqno;
        _destroy_acked_segment(state);
        free(state->received_segment);
      }
    else {
    if(conn_output(state->conn,state->received_segment->data,datalen) < 0)
    {
      fprintf(stderr,"Cannot output\n");
      ctcp_destroy(state);
      return;
    }
    /*Send ACK segment*/
    state->ackno = state->received_segment->seqno + datalen;
    if(_segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL) < 0)
    {
      perr("Cannot send ACK segment\n");
    }
    _destroy_acked_segment(state);
    free(state->received_segment);
    }
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
