/*
 * Copyright (C) 2016 Michael Richardson <mcr@sandelman.ca>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

#include "grasp.h"
#include "debug.h"
#include "cbor.h"
#include "hexdump.c"

/* for generating random numbers, see:
   https://tls.mbed.org/kb/how-to/add-a-random-generator
*/

#include "mbedtls/ctr_drbg.h"

bool grasp_client::open_connection(const char *serverip,
                                   unsigned int port)
{
    if(this->infd != this->outfd && this->outfd != -1) close(this->outfd);
    if(this->infd != -1) close(this->infd);
    this->infd = -1;
    this->outfd = -1;

    struct addrinfo hints;
    struct addrinfo *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family  = AF_UNSPEC;
    hints.ai_socktype= SOCK_STREAM;
    hints.ai_protocol= 0;

    int ret = getaddrinfo(serverip, NULL, &hints, &res);
    if(ret != 0) {
        deb->info("can not lookup %s error: %s\n",
                  serverip, gai_strerror(ret));
        return false;
    }

    int lasterr = 0;

    struct addrinfo *rp;
    int sfd;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype,
                     rp->ai_protocol);
        if (sfd == -1)
            continue;

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;                  /* Success */
        }
        lasterr = errno;

        close(sfd);
    }

    if (rp == NULL) {               /* No address succeeded */
        deb->error("Could not connect: %s", strerror(lasterr));
        return false;
    }

    freeaddrinfo(res);           /* No longer needed */
    this->infd  = sfd;
    this->outfd = sfd;
}


bool grasp_client::send_cbor(cbor_item_t *cb)
{
    unsigned char * buffer;
    size_t buffer_size,
        length = cbor_serialize_alloc(cb, &buffer, &buffer_size);

    if(write(outfd, buffer, length) != length) {
        deb->error("can not write %u bytes: %s",
                   length, strerror(errno));
        return false;
    }

    hexdump(buffer, 0, length);
    free(buffer);
    return true;
}

cbor_item_t *grasp_client::read_cbor(void)
{
    unsigned char buf[256];
    struct cbor_load_result res;
    unsigned int cnt = read(infd, buf, 256);

    cbor_item_t *reply = cbor_load(buf, cnt, &res);

    /* XXX checking res */
    if(!reply) {
        deb->error("can not load reply: decode details");
        return NULL;
    }

    return reply;
}


grasp_session_id grasp_client::start_query_for_aro(unsigned char eui64[8])
{
    grasp_session_id gsi = generate_random_sessionid(true);

    /* we are writing a GRASP: 3.7.5.  Request Messages */
    /*  request-negotiation-message = [M_REQ_NEG, session-id, objective]*/

    /* Preallocate the new array structure */
    cbor_item_t * root = cbor_new_definite_array(3);
    {
        cbor_item_t * req_neg = cbor_build_uint8(M_REQ_NEG);
        cbor_array_set(root, 0, cbor_move(req_neg));
    }

    {
        cbor_item_t * req_seqno = cbor_build_uint32(gsi);
        cbor_array_set(root, 1, cbor_move(req_seqno));
    }

    /*
     *
     * objective = [objective-name, objective-flags, loop-count, ?any]
     * objective-name = text ;see specification for uniqueness rules
     * loop-count = 0..255
     */
    {
        cbor_item_t * objective = cbor_new_definite_array(4);
        {
            /* Sandelman Software Works PEN
             * http://pen.iana.org/pen/app?page=AssignPenComplete&service=external&sp=S70753
             * with objective name: "6JOIN"
             */
            cbor_item_t * objname   = cbor_build_string("46930:6JOIN");
            cbor_array_set(objective, 0, cbor_move(objname));
        }
        {
            cbor_item_t * objflag   = cbor_build_uint8(1 << F_NEG);
            cbor_array_set(objective, 1, cbor_move(objflag));
        }
        {
            cbor_item_t * loopcount = cbor_build_uint8(2);
            cbor_array_set(objective, 2, cbor_move(loopcount));
        }

        {
            cbor_item_t * aro_box   = cbor_new_definite_array(2);
            cbor_item_t * aro       = cbor_build_bytestring(eui64, 8);
            cbor_array_set(aro_box,   0, cbor_move(aro));
            cbor_array_set(objective, 3, cbor_move(aro_box));

            cbor_array_set(aro_box,   1, cbor_move(cbor_build_string("6p-ipip")));
        }

        cbor_array_set(root, 2, cbor_move(objective));
    }

    send_cbor(root);
    cbor_decref(&root);

    return gsi;
}


grasp_session_id grasp_client::generate_random_sessionid(bool init)
{
    grasp_session_id newrand;
    int ret = mbedtls_ctr_drbg_random(&ctr_drbg, (unsigned char *)&newrand, sizeof(newrand));
    if( ret != 0 )
    {
        deb->error("can not generate sessionid");
        return 0;
    }

    if(init) {
        /* force upper bit to zero */
        newrand &= 0x7fffffff;
    } else {
        /* force upper bit to one  */
        newrand |= 0x80000000;
    }
    return newrand;
}

void grasp_client::init_random(void)
{
    entropy_init = true;

    /* set up the entropy source */
    mbedtls_entropy_init( &entropy );

    const char *personalization = "unstrung grasp client";
    mbedtls_ctr_drbg_init( &ctr_drbg );
}


/*
 * Local Variables:
 * c-basic-offset:4
 * c-style: whitesmith
 * compile-command: "make programs"
 * End:
 */
