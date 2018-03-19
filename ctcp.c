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

char buffer[MAX_BUFF_SIZE];

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
  linked_list_t *segments;  /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  /* FIXME: Add other needed fields. */
  ctcp_config_t *cfg;
  uint32_t seqno;              /* Current sequence number */
  uint32_t next_seqno;         /* Sequence number of next segment to send */
  uint32_t ackno;              /* Current ack number */
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */
void segment_hton(ctcp_segment_t *segment)
{
  segment->seqno = htonl(segment->seqno);
  segment->ackno = htonl(segment->ackno);
  segment->len = htons(segment->len);
  segment->flags = htonl(segment->flags);
  segment->window = htons(segment->window);
  /* cksum is already in network byte order */
}

void segment_ntoh(ctcp_segment_t *segment)
{
  segment->seqno = ntohl(segment->seqno);
  segment->ackno = ntohl(segment->ackno);
  segment->len = ntohs(segment->len);
  segment->flags = ntohl(segment->flags);
  segment->window = ntohs(segment->window);
}

int16_t segment_send(ctcp_state_t *state,int32_t flags, int32_t len, char* data)
{
  ctcp_segment_t *segment = calloc(len,1);
  segment->len = len;
  segment->seqno = state->seqno + segment->len;
  segment->ackno = state->ackno;
  segment->flags |= ACK;
  segment->window = MAX_SEG_DATA_SIZE;
  segment->cksum = 0;
  memcpy(segment->data,data,len - SEGMENT_HDR_SIZE);
  int32_t sum = cksum(segment,len);
  segment->cksum = sum;
  conn_send(state->conn,segment,len);
  return 1;
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
  /* hoangtu1: create a linked list of segment */
  state->segments = ll_create();
  state->cfg = cfg;
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
  /* FIXME */
  uint32_t retval,len,flags = 0;
  bzero(buffer,MAX_BUFF_SIZE);
  retval = conn_input(state->conn, buffer, MAX_BUFF_SIZE);

  if (-1 == retval) 
  {
    flags |= FIN;
    segment_send(state, flags, SEGMENT_HDR_SIZE, NULL);
  }
  else
  {
    len = retval + SEGMENT_HDR_SIZE;
    segment_send(state, flags, len, buffer);
  }
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
  
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
}

void ctcp_timer() {
  /* FIXME */
}
