/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006      Sandia National Laboratories. All rights
 *                         reserved.
 * Copyright (c) 2007      The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013-2014 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "opal_config.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "opal/prefetch.h"
#include "opal/types.h"

#include "btl_usnic.h"
#include "btl_usnic_endpoint.h"
#include "btl_usnic_module.h"
#include "btl_usnic_frag.h"
#include "btl_usnic_proc.h"
#include "btl_usnic_util.h"
#include "btl_usnic_ack.h"
#include "btl_usnic_send.h"

/*
 * Construct/destruct an endpoint structure.
 */
static void endpoint_construct(mca_btl_base_endpoint_t* endpoint)
{
    int i;

    endpoint->endpoint_module = NULL;
    endpoint->endpoint_proc = NULL;
    endpoint->endpoint_proc_index = -1;
    endpoint->endpoint_exiting = false;
    endpoint->endpoint_connectivity_checked = false;

    for (i=0; i<USNIC_NUM_CHANNELS; ++i) {
        endpoint->endpoint_remote_addr.qp_num[i] = 0;
    }
    endpoint->endpoint_remote_addr.gid.global.subnet_prefix = 0;
    endpoint->endpoint_remote_addr.gid.global.interface_id = 0;
    endpoint->endpoint_remote_ah = NULL;

    endpoint->endpoint_send_credits = 8;

    /* list of fragments queued to be sent */
    OBJ_CONSTRUCT(&endpoint->endpoint_frag_send_queue, opal_list_t);

    endpoint->endpoint_next_frag_id = 1;
    endpoint->endpoint_acktime = 0;

    /* endpoint starts not-ready-to-send */
    endpoint->endpoint_ready_to_send = 0;
    endpoint->endpoint_ack_needed = false;

    /* clear sent/received sequence number array */
    memset(endpoint->endpoint_sent_segs, 0,
            sizeof(endpoint->endpoint_sent_segs));
    memset(endpoint->endpoint_rcvd_segs, 0,
            sizeof(endpoint->endpoint_rcvd_segs));

    /*
     * Make a new OPAL hotel for this module
     * "hotel" is a construct used for triggering segment retransmission
     * due to timeout
     */
    OBJ_CONSTRUCT(&endpoint->endpoint_hotel, opal_hotel_t);
    opal_hotel_init(&endpoint->endpoint_hotel,
                    WINDOW_SIZE,
                    mca_btl_usnic_component.retrans_timeout,
                    0,
                    opal_btl_usnic_ack_timeout);

    /* Setup this endpoint's list links */
    OBJ_CONSTRUCT(&(endpoint->endpoint_ack_li), opal_list_item_t);
    OBJ_CONSTRUCT(&(endpoint->endpoint_endpoint_li), opal_list_item_t);
    endpoint->endpoint_ack_needed = false;

    /* fragment reassembly info */
    endpoint->endpoint_rx_frag_info =
        calloc(sizeof(struct opal_btl_usnic_rx_frag_info_t), MAX_ACTIVE_FRAGS);
    assert(NULL != endpoint->endpoint_rx_frag_info);
    if (OPAL_UNLIKELY(endpoint->endpoint_rx_frag_info == NULL)) {
        BTL_ERROR(("calloc returned NULL -- this should not happen!"));
        opal_btl_usnic_exit(endpoint->endpoint_module);
        /* Does not return */
    }
}

static void endpoint_destruct(mca_btl_base_endpoint_t* endpoint)
{
    int rc;
    opal_btl_usnic_proc_t *proc;

    /* We should not get here for an endpoint that is on the ACK list,
       so it should be safe to unconditionally destruct the ack_li */
    OBJ_DESTRUCT(&(endpoint->endpoint_ack_li));

    /* Remove this endpoint from module->all_endpoints list, then
       destruct the list_item_t */
    opal_mutex_lock(&endpoint->endpoint_module->all_endpoints_lock);
    opal_list_remove_item(&endpoint->endpoint_module->all_endpoints,
                          &endpoint->endpoint_endpoint_li);
    opal_mutex_unlock(&endpoint->endpoint_module->all_endpoints_lock);
    OBJ_DESTRUCT(&(endpoint->endpoint_endpoint_li));

    if (endpoint->endpoint_hotel.rooms != NULL) {
        OBJ_DESTRUCT(&(endpoint->endpoint_hotel));
    }

    OBJ_DESTRUCT(&endpoint->endpoint_frag_send_queue);

    /* release owning proc */
    proc = endpoint->endpoint_proc;
    if (NULL != proc) {
        proc->proc_endpoints[endpoint->endpoint_proc_index] = NULL;
        OBJ_RELEASE(proc);
    }

    free(endpoint->endpoint_rx_frag_info);

    if (NULL != endpoint->endpoint_remote_ah) {
        rc = ibv_destroy_ah(endpoint->endpoint_remote_ah);
        if (rc) {
            BTL_ERROR(("failed to ibv_destroy_ah, err=%d (%s)",
                       rc, strerror(rc)));
        }
    }
}

OBJ_CLASS_INSTANCE(opal_btl_usnic_endpoint_t,
                   opal_list_item_t,
                   endpoint_construct,
                   endpoint_destruct);

/*
 * Forcibly drain all pending output on an endpoint, without waiting for
 * actual completion.
 */
void
opal_btl_usnic_flush_endpoint(
    opal_btl_usnic_endpoint_t *endpoint)
{
    opal_btl_usnic_send_frag_t *frag;

    /* First, free all pending fragments */
    while (!opal_list_is_empty(&endpoint->endpoint_frag_send_queue)) {
        frag = (opal_btl_usnic_send_frag_t *)opal_list_remove_first(
                &endpoint->endpoint_frag_send_queue);

        /* _cond still needs to check ownership, but make sure the
         * fragment is marked as done.
         */
        frag->sf_ack_bytes_left = 0;
        frag->sf_seg_post_cnt = 0;
        opal_btl_usnic_send_frag_return_cond(endpoint->endpoint_module, frag);
    }

    /* Now, ACK everything that is pending */
    opal_btl_usnic_handle_ack(endpoint, endpoint->endpoint_next_seq_to_send-1);
}
