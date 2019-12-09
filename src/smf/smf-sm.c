/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "smf-sm.h"
#include "smf-context.h"
#include "smf-event.h"
#include "smf-gtp-path.h"
#include "smf-fd-path.h"
#include "smf-pfcp-path.h"
#include "smf-s5c-handler.h"
#include "smf-gx-handler.h"

void smf_state_initial(ogs_fsm_t *s, smf_event_t *e)
{
    smf_sm_debug(e);

    ogs_assert(s);

    OGS_FSM_TRAN(s, &smf_state_operational);
}

void smf_state_final(ogs_fsm_t *s, smf_event_t *e)
{
    smf_sm_debug(e);

    ogs_assert(s);
}

void smf_state_operational(ogs_fsm_t *s, smf_event_t *e)
{
    int rv;
    ogs_pkbuf_t *recvbuf = NULL;
    ogs_pkbuf_t *copybuf = NULL;
    uint16_t copybuf_len = 0;
    smf_sess_t *sess = NULL;

    ogs_gtp_message_t *gtp_message = NULL;
    ogs_pkbuf_t *gtpbuf = NULL;
    ogs_gtp_node_t *gnode = NULL;
    ogs_gtp_xact_t *gxact = NULL;

    ogs_pkbuf_t *gxbuf = NULL;
    ogs_diam_gx_message_t *gx_message = NULL;

#if 0
    ogs_pfcp_message_t *pfcp_message = NULL;
    ogs_pkbuf_t *pfcpbuf = NULL;
    ogs_pfcp_node_t *pnode = NULL;
    ogs_pfcp_xact_t *pxact = NULL;
#endif

    smf_sm_debug(e);

    ogs_assert(s);

    switch (e->id) {
    case OGS_FSM_ENTRY_SIG:
        rv = smf_gtp_open();
        if (rv != OGS_OK) {
            ogs_fatal("Can't establish S11-GTP path");
            break;
        }
        rv = smf_pfcp_open();
        if (rv != OGS_OK) {
            ogs_fatal("Can't establish N4-PFCP path");
            break;
        }
        break;
    case OGS_FSM_EXIT_SIG:
        smf_gtp_close();
        smf_pfcp_close();
        break;
    case SMF_EVT_S5C_MESSAGE:
        ogs_assert(e);
        recvbuf = e->gtpbuf;
        ogs_assert(recvbuf);

        copybuf_len = sizeof(ogs_gtp_message_t);
        copybuf = ogs_pkbuf_alloc(NULL, copybuf_len);
        ogs_pkbuf_put(copybuf, copybuf_len);
        gtp_message = (ogs_gtp_message_t *)copybuf->data;
        ogs_assert(gtp_message);

        rv = ogs_gtp_parse_msg(gtp_message, recvbuf);
        ogs_assert(rv == OGS_OK);

        /*
         * 5.5.2 in spec 29.274
         *
         * If a peer's TEID is not available, the TEID field still shall be
         * present in the header and its value shall be set to "0" in the
         * following messages:
         *
         * - Create Session Request message on S2a/S2b/S5/S8
         *
         * - Create Session Request message on S4/S11, if for a given UE,
         *   the SGSN/MME has not yet obtained the Control TEID of the SGW.
         *
         * - If a node receives a message and the TEID-C in the GTPv2 header of
         *   the received message is not known, it shall respond with
         *   "Context not found" Cause in the corresponding response message
         *   to the sender, the TEID used in the GTPv2-C header in the response
         *   message shall be then set to zero.
         *
         * - If a node receives a request message containing protocol error,
         *   e.g. Mandatory IE missing, which requires the receiver to reject
         *   the message as specified in clause 7.7, it shall reject
         *   the request message. For the response message, the node should
         *   look up the remote peer's TEID and accordingly set the GTPv2-C
         *   header TEID and the message cause code. As an implementation
         *   option, the node may not look up the remote peer's TEID and
         *   set the GTPv2-C header TEID to zero in the response message.
         *   However in this case, the cause code shall not be set to
         *   "Context not found".
         */
        if (gtp_message->h.teid != 0) {
            /* Cause is not "Context not found" */
            sess = smf_sess_find_by_teid(gtp_message->h.teid);
        }

        if (sess) {
            gnode = sess->gnode;
            ogs_assert(gnode);
        } else {
            gnode = e->gnode;
            ogs_assert(gnode);
        }

        rv = ogs_gtp_xact_receive(gnode, &gtp_message->h, &gxact);
        if (rv != OGS_OK) {
            ogs_pkbuf_free(recvbuf);
            ogs_pkbuf_free(copybuf);
            break;
        }

        switch(gtp_message->h.type) {
        case OGS_GTP_CREATE_SESSION_REQUEST_TYPE:
            if (gtp_message->h.teid == 0) {
                ogs_assert(!sess);
                sess = smf_sess_add_by_message(gtp_message);
                if (sess)
                    OGS_SETUP_GTP_NODE(sess, gnode);
            }
            smf_s5c_handle_create_session_request(
                sess, gxact, copybuf, &gtp_message->create_session_request);
            break;
        case OGS_GTP_DELETE_SESSION_REQUEST_TYPE:
            smf_s5c_handle_delete_session_request(
                sess, gxact, copybuf, &gtp_message->delete_session_request);
            break;
        case OGS_GTP_CREATE_BEARER_RESPONSE_TYPE:
            smf_s5c_handle_create_bearer_response(
                sess, gxact, &gtp_message->create_bearer_response);
            ogs_pkbuf_free(copybuf);
            break;
        case OGS_GTP_UPDATE_BEARER_RESPONSE_TYPE:
            smf_s5c_handle_update_bearer_response(
                sess, gxact, &gtp_message->update_bearer_response);
            ogs_pkbuf_free(copybuf);
            break;
        case OGS_GTP_DELETE_BEARER_RESPONSE_TYPE:
            smf_s5c_handle_delete_bearer_response(
                sess, gxact, &gtp_message->delete_bearer_response);
            ogs_pkbuf_free(copybuf);
            break;
        default:
            ogs_warn("Not implmeneted(type:%d)", gtp_message->h.type);
            ogs_pkbuf_free(copybuf);
            break;
        }
        ogs_pkbuf_free(recvbuf);
        break;

    case SMF_EVT_GX_MESSAGE:
        ogs_assert(e);

        gxbuf = e->gxbuf;
        ogs_assert(gxbuf);
        gx_message = (ogs_diam_gx_message_t *)gxbuf->data;
        ogs_assert(gx_message);

        sess = e->sess;
        ogs_assert(sess);

        switch(gx_message->cmd_code) {
        case OGS_DIAM_GX_CMD_CODE_CREDIT_CONTROL:
            gxact = e->gxact;
            ogs_assert(gxact);

            gtpbuf = e->gtpbuf;
            ogs_assert(gtpbuf);
            gtp_message = (ogs_gtp_message_t *)gtpbuf->data;

            if (gx_message->result_code == ER_DIAMETER_SUCCESS) {
                switch(gx_message->cc_request_type) {
                case OGS_DIAM_GX_CC_REQUEST_TYPE_INITIAL_REQUEST:
                    smf_gx_handle_cca_initial_request(
                            sess, gx_message, gxact, 
                            &gtp_message->create_session_request);
                    break;
                case OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST:
                    smf_gx_handle_cca_termination_request(
                            sess, gx_message, gxact,
                            &gtp_message->delete_session_request);
                    break;
                default:
                    ogs_error("Not implemented(%d)",
                            gx_message->cc_request_type);
                    break;
                }
            } else
                ogs_error("Diameter Error(%d)", gx_message->result_code);

            ogs_pkbuf_free(gtpbuf);
            break;
        case OGS_DIAM_GX_CMD_RE_AUTH:
            smf_gx_handle_re_auth_request(sess, gx_message);
            break;
        default:
            ogs_error("Invalid type(%d)", gx_message->cmd_code);
            break;
        }

        ogs_diam_gx_message_free(gx_message);
        ogs_pkbuf_free(gxbuf);
        break;
    case SMF_EVT_N4_MESSAGE:
        ogs_assert(e);
        recvbuf = e->pfcpbuf;
        ogs_assert(recvbuf);

#if 0
        if (pfcp_message.h.seid != 0) {
            /* Cause is not "Context not found" */
            sess = sess_find_by_seid(pfcp_message.h.seid);
        }

        if (sess) {
            pnode = sess->pnode;
            ogs_assert(pnode);

        } else {
            pnode = e->pnode;
            ogs_assert(pnode);
        }

        rv = ogs_pfcp_xact_receive(pnode, &pfcp_message.h, &pxact);
        if (rv != OGS_OK) {
            ogs_pkbuf_free(recvbuf);
            break;
        }

        switch (pfcp_message.h.type) {
        }
#endif

        ogs_pkbuf_free(recvbuf);
        break;
    default:
        ogs_error("No handler for event %s", smf_event_get_name(e));
        break;
    }
}