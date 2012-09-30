
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define PACKET_HEADER_LENGTH 12
#define ACK_HEADER_LENGTH 8
struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;
    
    conn_t *c;			/* This is the connection object */
    
    /* Add your own data fields below this */
    int seqno;
    int ackno;
    char data[500];
};
rel_t *rel_list;





/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
            const struct config_common *cc)
{
    rel_t *r;
    
    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));
    
    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }
    
    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
        rel_list->prev = &r->next;
    rel_list = r;
    
    /* Do any other initialization you need here */
    r->ackno = 0;
    r->seqno = 0;
    
    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next)
        r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);
    
    /* Free any other allocated memory here */
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
           const struct sockaddr_storage *ss,
           packet_t *pkt, size_t len)
{
}

void send_packet(rel_t *s, void* _packet, char type) {
    int len;
    if(type == 'a') {       //dealing with ack_packet
        struct ack_packet* ack = (struct ack_packet*)_packet;
        
        len = ack->len;
        ack->cksum = cksum(&ack, ack->len);
        
        htons(ack->len);
        htonl(ack->ackno);
    
        conn_sendpkt (s->c, ack, len);
    } else if (type == 'p') {//dealing with regular packet
        packet_t* packet = (packet_t*)_packet;
        
        len = packet->len;
        packet->cksum = cksum(packet->data, len);
        
        htonl(packet->seqno);
        htonl(packet->ackno);
        htons(packet->len);
        
        conn_sendpkt (s->c, packet, len);
    }
}

void send_ackno(rel_t *r){
    int sent;
    if (r->ackno == r->seqno) {
        sent = rel_read(r);
    }
    if (sent < 1) {
        struct ack_packet ack = {0, ACK_HEADER_LENGTH, htonl(r->ackno)};
        send_packet(r, &ack, 'a');
    }
}

void send_end_connection(rel_t *s) {
    packet_t packet = {0, PACKET_HEADER_LENGTH, s->ackno, s->seqno};
    send_packet(s, &packet, 'p');
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n){
    //process packet?
    int old_chksum = ntohs(pkt->cksum);
    pkt->cksum = 0;
    cksum(pkt, n);
    
    memcpy(r->data, pkt->data, pkt->len);
}


int
rel_read (rel_t *s)
{
    if (s->seqno == s->ackno) {
        s->seqno++;
        packet_t packet = {0, PACKET_HEADER_LENGTH, s->ackno, s->seqno};
        int data_len = conn_input(s->c, packet.data, 500);
        if (data_len == 0) {
            return 0;
        } else if (data_len == -1) {
            //deal with EOF or error
            //tear down the connection!
            send_end_connection(s);
            return -1;
        }
        packet.len += data_len;
        send_packet(s, &packet, 'p');
        return 1;
    }
    return 0;
}

void
rel_output (rel_t *r)
{
    // r->ackno++;
    // send_ackno(r);
    
}

void
rel_timer ()
{
    /* Retransmit any packets that need to be retransmitted */
    
}
