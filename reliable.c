
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
#define DATA_LEN 500
#define true 1
#define false 0

typedef int bool;

typedef struct _receiver {
    int len;
    int num_packets_accepted; //recieved packets accepted
    char data[DATA_LEN];
} receiver;

typedef struct _sender {
    packet_t packet;
    int num_packets_accepted; //sent packets accepted
    bool is_empty;
} sender;

struct reliable_state {
    rel_t *next;            /* Linked list for traversing all connections */
    rel_t **prev;
    conn_t *c;          /* This is the connection object */
    /* Add your own data fields below this */
    bool end_connection;
    receiver recv;
    sender send;
};
rel_t *rel_list;

void myPrintPacket(char* func_name, int hex, packet_t* packet) {
    char* fstring;
    if (!hex) fstring = "cksum:%d, len:%d, ackno:%d, seqno:%d, %s_data: %s";
    if (hex)  fstring = "cksum:%x, len:%d, ackno:%x, seqno:%x, %s_data: %s";
    fprintf(stderr, fstring,
            packet->cksum,
            packet->len,
            packet->ackno,
            packet->seqno,
            func_name,
            packet->data);
}

void init_receiver(receiver* r) {
    r->len = 0;
    r->num_packets_accepted = 0;
}

void init_sender(sender* s) {
    s->packet = (packet_t) { .cksum = 0,
        .len = PACKET_HEADER_LENGTH,
        .ackno = 1,
        .seqno = 0
    };
    s->is_empty = true;
    s->num_packets_accepted = 0;
}
//could consider passing a function, but probably not worth it
//returns: 1  if packet
//         0  if ack
//         -1 if cksum fails
//         2  if eof indicator
int ntoh_packet(packet_t* pkt, size_t net_len) {
    // packet_t * pkt = ((packet_t*)_pkt);
    
    int old_cksum = pkt->cksum;
    int pkt_len = ntohs(pkt->len);
    pkt->cksum = 0;
    if(net_len < pkt_len || (cksum(pkt, pkt_len) != old_cksum)) { // can't read packet/cksum should fail
        return -1;
    }
    // do conversions
    pkt->len = pkt_len;
    pkt->ackno = ntohl(pkt->ackno);
    if(pkt->len == ACK_HEADER_LENGTH) {
        return 0;
    }
    if (pkt->len == PACKET_HEADER_LENGTH) { //means teardown
        return 2;
    }
    
    pkt->seqno = ntohl(pkt->seqno);
    return 1;
}
//TODO: make this take a packet pointer
void hton_packet(void* _packet, bool is_packet) {
    packet_t* packet = (packet_t*)_packet;
    int len = packet->len;
    packet->len = htons(packet->len);
    packet->ackno = htonl(packet->ackno);
    if(is_packet) {
        packet->seqno = htonl(packet->seqno);
    }
    packet->cksum = 0;
    packet->cksum = cksum(packet, len);
}

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
    init_receiver(&r->recv);
    init_sender(&r->send);
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

inline int need_an_ack(rel_t *r) {
    return r->send.num_packets_accepted <= r->send.packet.seqno;
}

//TODO: change send_packet to take a packet struct, no bool, and check length
void send_packet(rel_t *s, void* _packet, bool is_packet) {
    packet_t packet = *(packet_t*)_packet;
    int len = packet.len;
    
    hton_packet(&packet, is_packet);
    if (conn_sendpkt (s->c, &packet, len) != len) {
        fprintf(stderr, "%s\n", "trouble sending packet");
        exit(1);
    }
}



void send_ackno(rel_t *r){
    if (r->send.is_empty) {
        r->send.packet = (packet_t){.cksum = 0,
            .len   = ACK_HEADER_LENGTH,
        };
    }
    r->send.packet.ackno = r->recv.num_packets_accepted++;
    myPrintPacket("send_ackno", 0, &r->send.packet);
    send_packet(r, &r->send.packet, false);
    //    if (r->send.is_empty) {
    //        sent = rel_read(r);
    //
    //        } else if (sent < 0) {
    //            //teardown connection
    //        }
    //    }
}

void send_end_connection(rel_t *s) {
    packet_t packet = {0, PACKET_HEADER_LENGTH, s->recv.num_packets_accepted+1, s->send.packet.seqno};
    send_packet(s, &packet, true);
}

void do_tear_down(rel_t *r) {
    //    if (r->recv.len == 0) {
    //        conn_output(r->c, r->recv.data, 0);
    //    }
    //    send eof
    //    send acks until done
    //    You have read an EOF from the other side (i.e., a Data packet of len 12, where the payload field is 0 bytes).
    //    You have read an EOF or error from your input (conn_input returned -1).
    //    All packets you have sent have been acknowledged.
    //    You have written all output data with conn_output.
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
    int packet_type = ntoh_packet(pkt, n);//destructive modification on pkt
    if (packet_type == -1) return; //it's corrupted
    if (packet_type >= 0 &&
        r->send.num_packets_accepted == pkt->ackno-2) { // it's ackno
        r->send.num_packets_accepted++;
        r->send.is_empty = !r->send.is_empty;
    } if (packet_type == 1 && r->recv.len == 0) { //it's a packet
        memcpy(r->recv.data, pkt->data, pkt->len);
        r->recv.len = pkt->len - PACKET_HEADER_LENGTH;
        rel_output(r);
    }
    
}


int rel_read (rel_t *s) {
    if (!s->send.is_empty) {
        return 0;
    }
    s->send.packet.cksum = 0;
    s->send.packet.len = PACKET_HEADER_LENGTH;
    s->send.packet.ackno = s->recv.num_packets_accepted+1;
    int data_len = conn_input(s->c, s->send.packet.data, DATA_LEN);
    if (data_len > 0) {
        
        //data has been read, incr seqno and close buffer for use
        s->send.packet.seqno++;
        s->send.is_empty = false;
        s->send.packet.len += data_len;
        send_packet(s, &s->send.packet, true);
        return 1;
    } else if (data_len == -1) {
        //deal with EOF or error
        //tear down the connection!
        send_end_connection(s);
        return -1;
    }
    return 0;
}

void
rel_output (rel_t *r)
{
    // fprintf(stderr, "r->recv.len: %d, r->recv.data: %s",
    // r->recv.len, r->recv.data);
    if (r->recv.len) {
        if (conn_bufspace(r->c) >= r->recv.len) {
            conn_output(r->c, r->recv.data, r->recv.len);
            send_ackno(r);
        }
    }
    
    // send_ackno(r);
    //try to output any data we have
}

void
rel_timer ()
{
    rel_t *rel = rel_list;
    //rel_list isn't a circle!
    while (rel != NULL) {
        if (!rel->send.is_empty) {
            send_packet(rel, &rel->send.packet, true);
        }
        rel = rel->next;
    }
    /* Retransmit any packets that need to be retransmitted */
    
}
