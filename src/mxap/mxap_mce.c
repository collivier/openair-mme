/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file m2ap_mce.c
  \brief
  \author Dincer BEKEN
  \company Blackned GmbH
  \email: dbeken@blackned.de
*/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>


#include "bstrlib.h"
#include "queue.h"
#include "tree.h"

#include "hashtable.h"
#include "log.h"
#include "msc.h"
#include "assertions.h"
#include "conversions.h"
#include "intertask_interface.h"
#include "timer.h"
#include "itti_free_defined_msg.h"
#include "mxap_mce.h"
#include "m2ap_mce_decoder.h"
#include "m2ap_mce_handlers.h"
#include "mxap_mce_procedures.h"
#include "m2ap_mce_retransmission.h"
#include "mxap_mce_itti_messaging.h"
#include "dynamic_memory_check.h"
#include "3gpp_23.003.h"
#include "mme_config.h"


#if M2AP_DEBUG_LIST
#  define eNB_LIST_OUT(x, args...) OAILOG_DEBUG (LOG_MXAP, "[eNB]%*s"x"\n", 4*indent, "", ##args)
#  define MBMS_LIST_OUT(x, args...)  OAILOG_DEBUG (LOG_MXAP, "[MBMS] %*s"x"\n", 4*indent, "", ##args)
#else
#  define eNB_LIST_OUT(x, args...)
#  define MBMS_LIST_OUT(x, args...)
#endif


uint32_t                                nb_m2ap_enb_associated = 0;

hash_table_ts_t g_m2ap_enb_coll = {.mutex = PTHREAD_MUTEX_INITIALIZER, 0}; // contains eNB_description_s, key is eNB_description_s.enb_id (uint32_t);
hash_table_ts_t g_m2ap_mbms_coll = {.mutex = PTHREAD_MUTEX_INITIALIZER, 0}; // contains MBMS_description_s, key is MBMS_description_s.mbms_m2ap_id (uint24_t);
/** An MBMS Service can be associated to multiple SCTP IDs. */

static int                              indent = 0;
extern struct mme_config_s              mme_config;
void *m2ap_mce_thread (void *args);

//------------------------------------------------------------------------------
static int m2ap_send_init_sctp (void)
{
  // Create and alloc new message
  MessageDef                             *message_p = NULL;

  message_p = itti_alloc_new_message (TASK_MXAP, SCTP_INIT_MSG);
  if (message_p) {
    message_p->ittiMsg.sctpInit.port = M2AP_PORT_NUMBER;
    message_p->ittiMsg.sctpInit.ppid = M2AP_SCTP_PPID;
    message_p->ittiMsg.sctpInit.ipv4 = 1;
    message_p->ittiMsg.sctpInit.ipv6 = 1;
    message_p->ittiMsg.sctpInit.nb_ipv4_addr = 0;
    message_p->ittiMsg.sctpInit.nb_ipv6_addr = 0;
    message_p->ittiMsg.sctpInit.ipv4_address[0].s_addr = mme_config.ip.mc_mme_v4.s_addr;

    if (mme_config.ip.mc_mme_v4.s_addr != INADDR_ANY) {
      message_p->ittiMsg.sctpInit.nb_ipv4_addr = 1;
    }
    if(memcmp(&mme_config.ip.mc_mme_v6.s6_addr, (void*)&in6addr_any, sizeof(mme_config.ip.mc_mme_v6.s6_addr)) != 0) {
      message_p->ittiMsg.sctpInit.nb_ipv6_addr = 1;
      memcpy(message_p->ittiMsg.sctpInit.ipv6_address[0].s6_addr, mme_config.ip.mc_mme_v6.s6_addr, 16);
    }
    /*
     * SR WARNING: ipv6 multi-homing fails sometimes for localhost.
     * Disable it for now.
     * todo : ?!?!?
     */
//    message_p->ittiMsg.sctpInit.nb_ipv6_addr = 0;
    return itti_send_msg_to_task (TASK_SCTP, INSTANCE_DEFAULT, message_p);
  }
  return RETURNerror;
}

//------------------------------------------------------------------------------
static bool m2ap_mbms_remove_sctp_assoc_id_cb (__attribute__((unused)) const hash_key_t keyP,
                                      void * const elementP, void * parameterP, void **resultP)
{
  mbms_description_t                       *mbms_ref           = (mbms_description_t*)elementP;
  hashtable_ts_free(&mbms_ref->g_m2ap_assoc_id2mce_enb_id_coll, keyP);
  m2ap_enb_description_t * m2ap_enb_ref = m2ap_is_enb_assoc_id_in_list((sctp_assoc_id_t)keyP);
  m2ap_enb_ref->nb_mbms_associated--;
  return false;
}

//------------------------------------------------------------------------------
static void
m2ap_remove_enb (
  void ** enb_ref)
{
  m2ap_enb_description_t                      *m2ap_enb_description = NULL;
  if (*enb_ref ) {
	  m2ap_enb_description = (m2ap_enb_description_t*)(*enb_ref);
	  /** Go through the MBMS Services, and remove the SCTP association. */
	  if(m2ap_enb_description->nb_mbms_associated) {
	    mbms_description_t                   *mbms_ref = NULL;
	    sctp_assoc_id_t			 	 		 *sctp_assoc_id_p = &m2ap_enb_description->sctp_assoc_id;
	    hashtable_ts_apply_callback_on_elements((hash_table_ts_t * const)&g_m2ap_mbms_coll,
	    		m2ap_mbms_remove_sctp_assoc_id_cb, (void*)sctp_assoc_id_p, (void**)&mbms_ref);
	  }
	  DevAssert(!m2ap_enb_description->nb_mbms_associated);
	  free_wrapper(enb_ref);
	  nb_m2ap_enb_associated--;
  }
  return;
}

//------------------------------------------------------------------------------
static
bool m2ap_enb_compare_by_mbms_sai_cb (__attribute__((unused)) const hash_key_t keyP,
                                    void * const elementP,
                                    void * parameterP, void **resultP)
{
  const mbms_service_area_id_t * const mbms_sai_p  = (const mbms_service_area_id_t *const)parameterP;
  m2ap_enb_description_t       * m2ap_enb_ref  	    = (m2ap_enb_description_t*)elementP;
  for(int i = 0; i < m2ap_enb_ref->mbms_sa_list.num_service_area; i++) {
    if (*mbms_sai_p == m2ap_enb_ref->mbms_sa_list.serviceArea[i]) {
      *resultP = elementP;
      return true;
    }
  }
  return false;
}

//------------------------------------------------------------------------------
static bool m2ap_mbms_compare_by_tmgi_cb (__attribute__((unused)) const hash_key_t keyP,
                                      void * const elementP, void * parameterP, void **resultP)
{
  mce_mbms_m2ap_id_t                      * mce_mbms_m2ap_id_p = (mce_mbms_m2ap_id_t*)parameterP;
  mbms_description_t                       *mbms_ref           = (mbms_description_t*)elementP;
  if ( *mce_mbms_m2ap_id_p == mbms_ref->mce_mbms_m2ap_id ) {
    *resultP = elementP;
    OAILOG_TRACE(LOG_MXAP, "Found mbms_ref %p mce_mbms_m2ap_id " MCE_MBMS_M2AP_ID_FMT "\n", mbms_ref, mbms_ref->mce_mbms_m2ap_id);
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
static bool m2ap_mbms_compare_by_mce_mbms_id_cb (__attribute__((unused)) const hash_key_t keyP,
                                      void * const elementP, void * parameterP, void **resultP)
{
  mce_mbms_m2ap_id_t                      * mce_mbms_m2ap_id_p = (mce_mbms_m2ap_id_t*)parameterP;
  mbms_description_t                       *mbms_ref           = (mbms_description_t*)elementP;
  if ( *mce_mbms_m2ap_id_p == mbms_ref->mce_mbms_m2ap_id ) {
    *resultP = elementP;
    OAILOG_TRACE(LOG_MXAP, "Found mbms_ref %p mce_mbms_m2ap_id " MCE_MBMS_M2AP_ID_FMT "\n", mbms_ref, mbms_ref->mce_mbms_m2ap_id);
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
void                                   *
m2ap_mce_thread (
  __attribute__((unused)) void *args)
{
  itti_mark_task_ready (TASK_MXAP);
//  OAILOG_START_USE ();
//  MSC_START_USE ();

  while (1) {
    MessageDef                             *received_message_p = NULL;
    MessagesIds                             message_id = MESSAGES_ID_MAX;
    /*
     * Trying to fetch a message from the message queue.
     * * * * If the queue is empty, this function will block till a
     * * * * message is sent to the task.
     */
    itti_receive_msg (TASK_MXAP, &received_message_p);
    DevAssert(received_message_p != NULL);

    switch (ITTI_MSG_ID (received_message_p)) {
    case ACTIVATE_MESSAGE:{
    	if (m2ap_send_init_sctp () < 0) {
    		OAILOG_CRITICAL (LOG_MXAP, "Error while sending SCTP_INIT_MSG to SCTP\n");
    	}
    }
    break;

    case MESSAGE_TEST:{
    	OAI_FPRINTF_INFO("TASK_M2AP received MESSAGE_TEST\n");
    }
    break;

    // MBMS session messages from MCE_APP task (M3 --> should trigger MBMS service for all eNB in the service area - not eNB specific).
    case M3AP_MBMS_SESSION_START_REQUEST:{
    	m3ap_handle_mbms_session_start_request (&M3AP_MBMS_SESSION_START_REQUEST (received_message_p));
    }
    break;

    case M3AP_MBMS_SESSION_STOP_REQUEST:{
    	m3ap_handle_mbms_session_stop_request (&M3AP_MBMS_SESSION_STOP_REQUEST (received_message_p));
    }
    break;

    case M3AP_MBMS_SESSION_UPDATE_REQUEST:{
    	m3ap_handle_mbms_session_update_request (&M3AP_MBMS_SESSION_UPDATE_REQUEST (received_message_p));
    }
    break;

    // ToDo: Leave the stats collection in the MXAP layer for now.
    case MCE_APP_M3_MBMS_SERVICE_COUNTING_REQ:{
    	/*
    	 * New message received from MCE_APP task.
    	 */
    	m3ap_handle_mbms_service_counting_req(MCE_APP_M3_MBMS_SERVICE_COUNTING_REQ (received_message_p));
    }
    break;

    // From SCTP layer, notifies M2AP of disconnection of a peer (eNB).
    case SCTP_CLOSE_ASSOCIATION:{
    	m2ap_handle_sctp_disconnection(SCTP_CLOSE_ASSOCIATION (received_message_p).assoc_id,
    		SCTP_CLOSE_ASSOCIATION (received_message_p).reset);
    }
    break;
//
//    // From SCTP
//    case SCTP_DATA_CNF:
//    	m2ap_mce_itti_nas_downlink_cnf(SCTP_DATA_CNF (received_message_p).mce_mbms_m2ap_id, SCTP_DATA_CNF (received_message_p).is_success);
//    break;

    // From SCTP
    case SCTP_DATA_IND:{
    	/*
    	 * New message received from SCTP layer.
    	 * Decode and handle it.
    	 */
    	M2AP_M2AP_PDU_t                            pdu = {0};

    	/*
    	 * Invoke M2AP message decoder
    	 */
    	if (m2ap_mce_decode_pdu (&pdu, SCTP_DATA_IND (received_message_p).payload) < 0) {
    		// TODO: Notify eNB of failure with right cause
    		OAILOG_ERROR (LOG_MXAP, "Failed to decode new buffer\n");
    	} else {
    		m2ap_mce_handle_message (SCTP_DATA_IND (received_message_p).assoc_id, SCTP_DATA_IND (received_message_p).stream, &pdu);
    	}

    	/*
    	 * Free received PDU array
    	 */
    	bdestroy_wrapper (&SCTP_DATA_IND (received_message_p).payload);
    }
    break;
//
//    case MCE_APP_M2AP_MME_MBMS_ID_NOTIFICATION:{
//    	m2ap_handle_mce_mbms_id_notification (&MCE_APP_M2AP_MME_MBMS_ID_NOTIFICATION (received_message_p));
//    }
//    break;

    case M3AP_ENB_INITIATED_RESET_ACK:{
    	m3ap_handle_enb_initiated_reset_ack (&M3AP_ENB_INITIATED_RESET_ACK (received_message_p));
    }
    break;

    case TIMER_HAS_EXPIRED:{
    	mbms_description_t                       *ue_ref_p = NULL;
    	if (received_message_p->ittiMsg.timer_has_expired.arg != NULL) {
//    		todo: enb_m2ap_id_key_t enb_m2ap_id_key = (enb_m2ap_id_key_t)(received_message_p->ittiMsg.timer_has_expired.arg);
    		enb_mbms_m2ap_id_t enb_mbms_m2ap_id = 0; // todo: MCE_APP_ENB_M2AP_ID_KEY2ENB_M2AP_ID(enb_m2ap_id_key);
    		uint32_t enb_id = 0; // todo ((enb_m2ap_id_key >> 24) & 0xFFFFFFFFFF);

//              /** Check if the MBMS still exists. */
//              ue_ref_p = m2ap_is_enb_mbms_m2ap_id_in_list_per_enb(enb_mbms_m2ap_id, enb_id);
//              if (!ue_ref_p) {
//                OAILOG_WARNING (LOG_MXAP, "Timer with id 0x%lx expired but no associated MBMS context!\n", received_message_p->ittiMsg.timer_has_expired.timer_id);
//                break;
//              }
//              OAILOG_WARNING (LOG_MXAP, "Processing expired timer with id 0x%lx for ueId "MCE_MBMS_M2AP_ID_FMT " with m2ap_mbms_context_rel_timer_id 0x%lx !\n",
//            		  received_message_p->ittiMsg.timer_has_expired.timer_id,
//                  ue_ref_p->mce_mbms_m2ap_id, ue_ref_p->m2ap_mbms_context_rel_timer.id);
//              if (received_message_p->ittiMsg.timer_has_expired.timer_id == ue_ref_p->m2ap_mbms_context_rel_timer.id) {
//                // MBMS context release complete timer expiry handler
//                m2ap_mce_handle_mbms_context_rel_comp_timer_expiry (ue_ref_p);
//              } else if (received_message_p->ittiMsg.timer_has_expired.timer_id == ue_ref_p->m2ap_handover_completion_timer.id) {
//                m2ap_mce_handle_mce_mobility_completion_timer_expiry(ue_ref_p);
//              }
    	}
    	/* TODO - Commenting out below function as it is not used as of now.
    	 * Need to handle it when we support other timers in M2AP
    	 */

    	//m2ap_handle_timer_expiry (&received_message_p->ittiMsg.timer_has_expired);
    }
    break;

    // From SCTP layer, notifies M2AP of connection of a peer (eNB).
    case SCTP_NEW_ASSOCIATION:{
    	m2ap_handle_new_association (&received_message_p->ittiMsg.sctp_new_peer);
    }
    break;

    case TERMINATE_MESSAGE:{
    	m2ap_mce_exit();
    	itti_free_msg_content(received_message_p);
    	itti_free (ITTI_MSG_ORIGIN_ID (received_message_p), received_message_p);
    	OAI_FPRINTF_INFO("TASK_MXAP terminated\n");
    	itti_exit_task ();
    }
    break;

    default:{
    	OAILOG_ERROR (LOG_MXAP, "Unknown message ID %d:%s\n", ITTI_MSG_ID (received_message_p), ITTI_MSG_NAME (received_message_p));
    }
          break;
    }

    itti_free_msg_content(received_message_p);
    itti_free (ITTI_MSG_ORIGIN_ID (received_message_p), received_message_p);
    received_message_p = NULL;
  }

  return NULL;
}

//------------------------------------------------------------------------------
int
mxap_mce_init(void)
{
  OAILOG_DEBUG (LOG_MXAP, "Initializing M2AP interface\n");

  if (get_asn1c_environment_version () < ASN1_MINIMUM_VERSION) {
    OAILOG_ERROR (LOG_MXAP, "ASN1C version %d found, expecting at least %d\n", get_asn1c_environment_version (), ASN1_MINIMUM_VERSION);
    return RETURNerror;
  } else {
    OAILOG_DEBUG (LOG_MXAP, "ASN1C version %d\n", get_asn1c_environment_version ());
  }

  OAILOG_DEBUG (LOG_MXAP, "M2AP Release v%s\n", M2AP_VERSION);

  /** Free the M2AP eNB list. */
  // 16 entries for n eNB.
  bstring bs1 = bfromcstr("m2ap_eNB_coll");
  hash_table_ts_t* h = hashtable_ts_init (&g_m2ap_enb_coll, mme_config.max_m2_enbs, NULL, m2ap_remove_enb, bs1); /**< Use a better removal handler. */
  bdestroy_wrapper (&bs1);
  if (!h) return RETURNerror;

  /** Free all MBMS Services and clear the association list. */
  // 16 entries for n eNB.
  bs1 = bfromcstr("m2ap_MBMS_coll");
  h = hashtable_ts_init (&g_m2ap_mbms_coll, mme_config.max_mbms_services, NULL, m2ap_remove_mbms, bs1); /**< Use a better removal handler. */
  bdestroy_wrapper (&bs1);
  if (!h) return RETURNerror;

  if (itti_create_task (TASK_MXAP, &m2ap_mce_thread, NULL) < 0) {
    OAILOG_ERROR (LOG_MXAP, "Error while creating M2AP task\n");
    return RETURNerror;
  }
  return RETURNok;
}
//------------------------------------------------------------------------------
void mxap_mce_exit (void)
{
  OAILOG_DEBUG (LOG_MXAP, "Cleaning MXAP\n");
  if (hashtable_ts_destroy(&g_m2ap_mbms_coll) != HASH_TABLE_OK) {
    OAILOG_ERROR(LOG_MXAP, "An error occurred while destroying MBMS Service hash table. \n");
  }

  if (hashtable_ts_destroy(&g_m2ap_enb_coll) != HASH_TABLE_OK) {
    OAILOG_ERROR(LOG_MXAP, "An error occurred while destroying M2 eNB hash table. \n");
  }
  OAILOG_DEBUG (LOG_MXAP, "Cleaning MXAP: DONE\n");
}

//------------------------------------------------------------------------------
void
m2ap_dump_enb_list (
  void)
{
  hashtable_ts_apply_callback_on_elements(&g_m2ap_enb_coll, m2ap_dump_enb_hash_cb, NULL, NULL);
}

//------------------------------------------------------------------------------
bool m2ap_dump_enb_hash_cb (__attribute__((unused))const hash_key_t keyP,
               void * const eNB_void,
               void __attribute__((unused)) *unused_parameterP,
               void __attribute__((unused)) **unused_resultP)
{
  const m2ap_enb_description_t * const enb_ref = (const m2ap_enb_description_t *)eNB_void;
  if (enb_ref == NULL) {
    return false;
  }
  m2ap_dump_enb(enb_ref);
  return false;
}

//------------------------------------------------------------------------------
void
m2ap_dump_enb (
  const m2ap_enb_description_t * const m2ap_enb_ref)
{
#  ifdef M2AP_DEBUG_LIST
  //Reset indentation
  indent = 0;

  if (enb_ref == NULL) {
    return;
  }

  eNB_LIST_OUT ("");
  eNB_LIST_OUT ("eNB name:           %s", m2ap_enb_ref->m2ap_enb_name == NULL ? "not present" : m2ap_enb_ref->m2ap_enb_name);
  eNB_LIST_OUT ("eNB ID:             %07x", m2ap_enb_ref->m2ap_enb_id);
  eNB_LIST_OUT ("SCTP assoc id:      %d", m2ap_enb_ref->sctp_assoc_id);
  eNB_LIST_OUT ("SCTP instreams:     %d", m2ap_enb_ref->instreams);
  eNB_LIST_OUT ("SCTP outstreams:    %d", m2ap_enb_ref->outstreams);
  eNB_LIST_OUT ("MBMS active on eNB: %d", m2ap_enb_ref->nb_mbms_associated);
  indent++;
  hashtable_ts_apply_callback_on_elements((hash_table_ts_t * const)&enb_ref->mbms_coll, m2ap_dump_mbms_hash_cb, NULL, NULL);
  indent--;
  eNB_LIST_OUT ("");
#  else
  m2ap_dump_mbms (NULL);
#  endif
}

//------------------------------------------------------------------------------
bool m2ap_dump_mbms_hash_cb (__attribute__((unused)) const hash_key_t keyP,
               void * const mbms_void,
               void __attribute__((unused)) *unused_parameterP,
               void __attribute__((unused)) **unused_resultP)
{
  mbms_description_t * mbms_ref = (mbms_description_t *)mbms_void;
  if (mbms_ref == NULL) {
    return false;
  }
  m2ap_dump_mbms(mbms_ref);
  return false;
}

//------------------------------------------------------------------------------
void
m2ap_dump_mbms (const mbms_description_t * const mbms_ref)
{
#  ifdef M2AP_DEBUG_LIST

  if (mbms_ref == NULL)
    return;

  UE_LIST_OUT ("eNB MBMS m2ap id:   0x%06x", mbms_ref->enb_mbms_m2ap_id);
  UE_LIST_OUT ("MCE MBMS m2ap id:   0x%08x", mbms_ref->mce_mbms_m2ap_id);
  UE_LIST_OUT ("SCTP stream recv: 0x%04x", mbms_ref->sctp_stream_recv);
  UE_LIST_OUT ("SCTP stream send: 0x%04x", mbms_ref->sctp_stream_send);
# endif
}

//------------------------------------------------------------------------------
bool m2ap_enb_compare_by_m2ap_enb_id_cb (__attribute__((unused)) const hash_key_t keyP,
                                    void * const elementP,
                                    void * parameterP, void **resultP)
{
  const uint32_t                  * const m2ap_enb_id_p = (const uint32_t*const)parameterP;
  m2ap_enb_description_t          * m2ap_enb_ref  = (m2ap_enb_description_t*)elementP;
  if (*m2ap_enb_id_p == m2ap_enb_ref->m2ap_enb_id ) {
    *resultP = elementP;
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
m2ap_enb_description_t                      *
m2ap_is_enb_id_in_list (
  const uint32_t m2ap_enb_id)
{
  m2ap_enb_description_t                 *m2ap_enb_ref = NULL;
  uint32_t                               *m2ap_enb_id_p  = (uint32_t*)&m2ap_enb_id;
  hashtable_ts_apply_callback_on_elements((hash_table_ts_t * const)&g_m2ap_enb_coll, m2ap_enb_compare_by_m2ap_enb_id_cb, (void *)m2ap_enb_id_p, (void**)&m2ap_enb_ref);
  return m2ap_enb_ref;
}

//------------------------------------------------------------------------------
void m2ap_is_mbms_sai_in_list (
  const mbms_service_area_id_t mbms_sai,
  int *num_m2ap_enbs,
  m2ap_enb_description_t ** m2ap_enbs)
{
  m2ap_enb_description_t                 *m2ap_enb_ref 	= NULL;
  mbms_service_area_id_t                 *mbms_sai_p  	= (mbms_service_area_id_t*)&mbms_sai;

  /** Collect all M2AP eNBs for the given MBMS Service Area Id. */
  hashtable_element_array_t              ea;
  memset(&ea, 0, sizeof(hashtable_element_array_t));
  ea.elements = m2ap_enbs;
  hashtable_ts_apply_list_callback_on_elements((hash_table_ts_t * const)&g_m2ap_enb_coll, m2ap_enb_compare_by_mbms_sai_cb, (void *)mbms_sai_p, &ea);
  OAILOG_DEBUG(LOG_MXAP, "Found %d matching m2ap_enb references based on the received MBMS SAI" MBMS_SERVICE_AREA_ID_FMT". \n", ea.num_elements, mbms_sai);
  *num_m2ap_enbs = ea.num_elements;
}

//------------------------------------------------------------------------------
m2ap_enb_description_t                      *
m2ap_is_enb_assoc_id_in_list (
  const sctp_assoc_id_t sctp_assoc_id)
{
  m2ap_enb_description_t                      *enb_ref = NULL;
  hashtable_ts_get(&g_m2ap_enb_coll, (const hash_key_t)sctp_assoc_id, (void**)&enb_ref);
  return enb_ref;
}

//------------------------------------------------------------------------------
// TODO: (amar) unused function check with OAI.
bool m2ap_mbms_compare_by_enb_mbms_m2ap_id_cb (__attribute__((unused)) const hash_key_t keyP, void * elementP, void * parameterP,
                                           void **resultP)
{
  enb_mbms_m2ap_id_t                       *enb_mbms_m2ap_id = (enb_mbms_m2ap_id_t*)parameterP;
  mbms_description_t                       *mbms_ref           = (mbms_description_t*)elementP;
  if ( *enb_mbms_m2ap_id == mbms_ref->enb_mbms_m2ap_id ) {
    *resultP = elementP;
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
mbms_description_t                       *
m2ap_is_mbms_mce_id_in_list (
  const mce_mbms_m2ap_id_t mce_mbms_m2ap_id)
{
  mbms_description_t                       *mbms_ref = NULL;
  mce_mbms_m2ap_id_t                       *mce_mbms_m2ap_id_p = (mce_mbms_m2ap_id_t*)&mce_mbms_m2ap_id;

  hashtable_ts_apply_callback_on_elements((hash_table_ts_t * const)&g_m2ap_mbms_coll, m2ap_mbms_compare_by_mce_mbms_id_cb, (void*)mce_mbms_m2ap_id_p, (void**)&mbms_ref);
  if (mbms_ref) {
    OAILOG_TRACE(LOG_MXAP, "Found mbms_ref %p mce_mbms_m2ap_id " MCE_MBMS_M2AP_ID_FMT "\n", mbms_ref, mbms_ref->mce_mbms_m2ap_id);
  }
  return mbms_ref;
}

//------------------------------------------------------------------------------
mbms_description_t                       *
m2ap_is_tmgi_in_list (
  const tmgi_t * const tmgi, const mbms_service_area_id_t mbms_sai)
{
  mbms_description_t                     *mbms_ref = NULL;
  m2ap_tmgi_t						   	  m2ap_tmgi = {.tmgi = *tmgi, .mbms_service_area_id_t = mbms_sai};
  m2ap_tmgi_t				 	 		 *m2ap_tmgi_p = &m2ap_tmgi;

  hashtable_ts_apply_callback_on_elements((hash_table_ts_t * const)&g_m2ap_mbms_coll, m2ap_mbms_compare_by_tmgi_cb, (void*)m2ap_tmgi_p, (void**)&mbms_ref);
  if (mbms_ref) {
    OAILOG_TRACE(LOG_MXAP, "Found mbms_ref %p for TMGI " TMGI_FMT " and MBMS-SAI " MBMS_SERVICE_AREA_ID_FMT ". \n", mbms_ref, TMGI_ARG(tmgi), mbms_sai);
  }
  return mbms_ref;
}

//------------------------------------------------------------------------------
m2ap_enb_description_t *m2ap_new_enb (void)
{
  m2ap_enb_description_t                      *m2ap_enb_ref = NULL;

  m2ap_enb_ref = calloc (1, sizeof (m2ap_enb_description_t));
  /*
   * Something bad happened during malloc...
   * * * * May be we are running out of memory.
   * * * * TODO: Notify eNB with a cause like Hardware Failure.
   */
  DevAssert (m2ap_enb_ref != NULL);
  /** No table for MBMS. */
  return m2ap_enb_ref;
}

//------------------------------------------------------------------------------
mbms_description_t                       *
m2ap_new_mbms (
  const tmgi_t * const tmgi, const mbms_service_area_id_t mbms_sai)
{
  m2ap_enb_description_t                   *m2ap_enb_ref = NULL;
  mbms_description_t                       *mbms_ref = NULL;

  mce_mbms_m2ap_id_t						mce_mbms_m2ap_id = INVALID_MCE_MBMS_M2AP_ID;

  /** Try to generate a new MBMS M2AP Id. */
  mce_mbms_m2ap_id = generate_new_mce_mbms_m2ap_id();
  if(mce_mbms_m2ap_id == INVALID_MCE_MBMS_M2AP_ID){
	return NULL;
  }
  /** Assert that it is not reused. */
  DevAssert(!m2ap_is_mbms_mce_id_in_list(mce_mbms_m2ap_id));

  /** No eNB reference needed at this stage. */
  mbms_ref = calloc (1, sizeof (mbms_description_t));
  /*
   * Something bad happened during malloc...
   * * * * May be we are running out of memory.
   * * * * TODO: Notify eNB with a cause like Hardware Failure.
   */
  DevAssert (mbms_ref != NULL);
  mbms_ref->mce_mbms_m2ap_id = mce_mbms_m2ap_id;
  memcpy((void*)&mbms_ref->tmgi, (void*)tmgi, sizeof(tmgi_t));
  mbms_ref->mbms_service_area_id = mbms_sai;  /**< Only supporting a single MBMS Service Area ID. */
  /**
   * Create and initialize the SCTP eNB MBMS M2aP Id map.
   */
//  mbms_ref->g_m2ap_assoc_id2mce_enb_id_coll.mutex = PTHREAD_MUTEX_INITIALIZER; // contains enb MBMS M2AP Id, key is sctp_assoc;
  bstring bs2 = bfromcstr("m2ap_assoc_id2enb_mbms_id_coll");
  hash_table_ts_t* h = hashtable_ts_init (&mbms_ref->g_m2ap_assoc_id2mce_enb_id_coll, mme_config.max_mbms_services, NULL, hash_free_int_func, bs2);
  bdestroy_wrapper (&bs2);
  if (!h) return RETURNerror;
  /** MBMS is not inserted into any M2AP eNB or and vice versa at this stage. */
  return mbms_ref;
}

//------------------------------------------------------------------------------
void
m2ap_remove_mbms (
  mbms_description_t * mbms_ref)
{
  m2ap_enb_description_t                      *m2ap_enb_ref = NULL;
  /*
   * NULL reference...
   */
  if (mbms_ref == NULL)
    return;
  mce_mbms_m2ap_id_t mce_mbms_m2ap_id = mbms_ref->mce_mbms_m2ap_id;
  /** Stop MBMS Action timer,if running. */
  if (mbms_ref->m2ap_action_timer.id != M2AP_TIMER_INACTIVE_ID) {
    if (timer_remove (mbms_ref->m2ap_action_timer.id, NULL)) {
      OAILOG_ERROR (LOG_MXAP, "Failed to stop m2ap mbms context action timer for MBMS id  " MCE_MBMS_M2AP_ID_FMT " \n",
    	mbms_ref->mce_mbms_m2ap_id);
    }
    mbms_ref->m2ap_action_timer.id = M2AP_TIMER_INACTIVE_ID;
  }
  /** Remove the MBMS Bearer and IP MC information. */
  DevAssert(!mbms_ref->mbms_bc.eps_bearer_context.esm_ebr_context.tft);
  /** We will try to remove the SCTP association too, but it will anyways be set after the handover is completed. */
  hashtable_key_array_t * enb_sctp_ids = hashtable_ts_get_keys (&mbms_ref->g_m2ap_assoc_id2mce_enb_id_coll);
  if(enb_sctp_ids){
	for(int i = 0; i < enb_sctp_ids->num_keys; i++) {
	  /** Get the eNB from the SCTP association. */
	  m2ap_enb_ref = m2ap_is_enb_assoc_id_in_list(enb_sctp_ids->keys[i]);
	  if(m2ap_enb_ref) {
	    /** Updating number of MBMS Services of the MBMS eNB. */
	    DevAssert(m2ap_enb_ref->nb_mbms_associated > 0);
	    m2ap_enb_ref->nb_mbms_associated--;
	    if (!m2ap_enb_ref->nb_mbms_associated) {
	      if (m2ap_enb_ref->m2_state == M2AP_RESETING) {
	        OAILOG_INFO(LOG_MXAP, "Moving M2AP eNB state to M2AP_INIT");
	        m2ap_enb_ref->m2_state = M2AP_INIT;
	        update_mce_app_stats_connected_enb_sub();
	      } else if (m2ap_enb_ref->m2_state == M2AP_SHUTDOWN) {
	    	OAILOG_INFO(LOG_MXAP, "Deleting eNB");
	    	hashtable_ts_free (&g_m2ap_enb_coll, m2ap_enb_ref->sctp_assoc_id);
	      }
	    }
	  }
	  /** Remove it from list. */
	  hashtable_ts_free(&mbms_ref->g_m2ap_assoc_id2mce_enb_id_coll, enb_sctp_ids->keys[i]);
	}
    free_wrapper(&enb_sctp_ids);
  }
  /** Destroy the Map. */
  if (hashtable_ts_destroy(&mbms_ref->g_m2ap_assoc_id2mce_enb_id_coll) != HASH_TABLE_OK) {
    OAILOG_ERROR(LOG_MXAP, "An error occurred while destroying enb_mbms_id hash table. \n");
  }
}