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
  uint16_t datalen;
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
  
  linked_list_t *segments_receive;
  /* FIXME: Add other needed fields. */
  conn_state_t conn_state;
  uint32_t seqno;              /* Current sequence number */
  uint32_t ackno;              /* Current ack number */

  uint16_t recv_window;
  uint16_t send_window;

  td_state_t td_state;
  uint16_t datasize_in;
  uint16_t datasize_out;
//  uint32_t exp_seqno;           /* Expected sequence number to receive */
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

static void _save_sent_segment(ctcp_state_t *state, ctcp_segment_t *sent_segment)
{
  ctcp_segment_attr_t *sent_segment_attr;

  sent_segment_attr = calloc(sizeof(ctcp_segment_attr_t),1);
  sent_segment_attr->time = 0;
  sent_segment_attr->no_of_times = 0;
  sent_segment_attr->segment = sent_segment;
  sent_segment_attr->datalen = ntohs(sent_segment->len) - SEGMENT_HDR_SIZE;

  state->datasize_out += sent_segment_attr->datalen;
  ll_add(state->segments_send,(void *)sent_segment_attr);
}

/**
 * Save received segment to linked list:
 *  - Locate the received segment in the linked list.
 *  - Add the received segment in the right position.
 * 
 * Received segment members are in host byte order
 */
static void _save_received_segment(ctcp_state_t *state, ctcp_segment_t *received_segment)
{
  ctcp_segment_t *segment;
  ll_node_t *ll_node;
  uint32_t received_seqno;

  ll_node = state->segments_receive->head;
  received_seqno = received_segment->seqno;

  while (NULL != ll_node)
  {
    segment = (ctcp_segment_t *)ll_node->object;

    if (segment->seqno > received_seqno)
      break;

    ll_node = ll_node->next;
  }

  if (NULL == ll_node)
  {
    ll_add(state->segments_receive,received_segment);
  }
  else
    if (ll_node == state->segments_receive->head)
    {
      ll_add_front(state->segments_receive,received_segment);
    }
  else
  {
    ll_add_after(state->segments_receive,ll_node->prev,received_segment);
  }

  state->datasize_in += received_segment->len - SEGMENT_HDR_SIZE;
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
  segment->window = state->recv_window;
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

ll_node_t *find_ll_node_with_ack(linked_list_t *ll_list, uint32_t ackno)
{
  ll_node_t *ll_node;
  ctcp_segment_attr_t *segment_attr;
  ll_node = ll_list->head;
  while(NULL != ll_node)
  {
    segment_attr = (ctcp_segment_attr_t *)ll_node->object;
    
    if(ntohl(segment_attr->segment->ackno) == ackno)
      return ll_node;
    else ll_node = ll_node->next;
  }
  return NULL;
}


/**
 * Destroy ACKed segment:
 * Find the segment with ackno in segments_send linked list, free and nullify the segment but still hold
 * segment's node in the linked list
 * 
 * Check from head and delete the nullified segments.
 * 
 *  state: state structure
 *  ackno: ACK number of the segment to be destroy
 * 
 *  return: number of removed linked list node (0 if the destroyed segment is not at the head of the linked list)
**/
static int _destroy_acked_segment(ctcp_state_t *state, uint32_t ackno)
{
  int ret = 0;
  ll_node_t *ll_node;
  ctcp_segment_attr_t *sent_segment_attr;

  ll_node = find_ll_node_with_ack(state->segments_send,ackno);
  sent_segment_attr = (ctcp_segment_attr_t *)ll_node->object;
  free(sent_segment_attr->segment);
  sent_segment_attr->segment = NULL;

  ll_node = state->segments_send->head;
  sent_segment_attr = (ctcp_segment_attr_t *)ll_node->object;
  while(NULL == sent_segment_attr->segment)
  {
    state->datasize_out -= sent_segment_attr->datalen;
    free(sent_segment_attr);
    ll_remove(state->segments_send,ll_node);

    ret++;

    ll_node = state->segments_send->head;
    if (NULL == ll_node)
      break;
    sent_segment_attr = (ctcp_segment_attr_t *)ll_node->object;
  }

  return ret;
}

void  retransmission_handler(ctcp_state_t *state)
{
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

  ll_node_t *ll_node;
  ctcp_segment_attr_t *segment_attr;
  ll_node = state->segments_send->head;
  while(ll_node != NULL) 
  {
    segment_attr = (ctcp_segment_attr_t *)ll_node->object;
    segment_attr->time += state->timer;
    if(segment_attr->time >= state->rt_timeout)
    {
      conn_send(state->conn,segment_attr->segment,ntohs(segment_attr->segment->len));
      segment_attr->no_of_times ++;
      if(segment_attr->no_of_times >= 5)
      {
        if(0 != _destroy_acked_segment(state,ntohl(segment_attr->segment->ackno)))
        {
          ll_node = state->segments_send->head;
        }
        else
        {
          ll_node = ll_node->next;
        }
      }
      else
      {
        ll_node = ll_node->next;
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
//  state->exp_seqno = 1;
  state->recv_window = cfg->recv_window;
  state->send_window = cfg->send_window;
  state->tim = 0;
  state->datasize_in = 0;
  state->datasize_out = 0;
  state->td_state = NOT_TEARDOWN;
  state->timer = cfg->timer;
  state->rt_timeout = cfg->rt_timeout;
  state->conn_state = DATA_TRANSFER;
  /* hoangtu1: create a linked list of segment */
 // state->sent_segment_attr = calloc(sizeof(ctcp_segment_attr_t),1);
  state->segments_send = ll_create();
  state->segments_receive = ll_create();
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
  /* call conn_input only if there are enough space (MAX_BUFF_SIZE bytes) in the send_window */
  if ((state->send_window - state->datasize_out) >= MAX_BUFF_SIZE)
  {
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
    }
  }
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
  
    if (segment->seqno < state->ackno) /* Ignore too-old segment */
    {
      goto exit_receive;
    }

    /* Ignore segment if there is not enough space */
    if ((state->datasize_in + segment->len - SEGMENT_HDR_SIZE) > state->recv_window)
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
      if (segment->len == SEGMENT_HDR_SIZE)
      {
        _destroy_acked_segment(state,segment->seqno);
        free(segment);
      }
      else
      {
      _save_received_segment(state,segment);
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

void ctcp_output(ctcp_state_t *state) 
{
  ll_node_t      *ll_node;
  ctcp_segment_t *segment;
  uint16_t       datalen,avail_buff;

  ll_node = state->segments_receive->head;
  segment = (ctcp_segment_t *)ll_node->object;
  datalen = segment->len - SEGMENT_HDR_SIZE;
  
  while(segment->seqno == state->ackno)
  {
    datalen = segment->len - SEGMENT_HDR_SIZE;
    avail_buff = conn_bufspace(state->conn);
    if (datalen > avail_buff)
    {
      fprintf(stderr,"Not enough buffer available \n");
      break;
    }
    if (datalen <= avail_buff)
    {
      conn_output(state->conn,segment->data,datalen);
      state->ackno += datalen;
      fprintf(stderr,"before send ACK \n");
      _segment_send(state,ACK,SEGMENT_HDR_SIZE,NULL);
      
      fprintf(stderr,"befor free\n");
      free(segment);
          
      fprintf(stderr,"after free \n");
      if (NULL == ll_node->next)
      {
        ll_remove(state->segments_receive,ll_node);
        break;
      }
      else
      {
        ll_node = ll_node->next;
        ll_remove(state->segments_receive,ll_node->prev);
        segment = (ctcp_segment_t *)ll_node->object;
      }
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
