/* The API definition */
#include <portals4.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

/* System headers */
#include <stdlib.h>		       /* for malloc() */
#include <assert.h>		       /* for assert() */
#include <string.h>		       /* for memcpy() */

#include <stdio.h>

/* Internals */
#include "ptl_visibility.h"
#include "ptl_internal_queues.h"
#include "ptl_internal_DM.h"
#include "ptl_internal_MD.h"
#include "ptl_internal_commpad.h"
#include "ptl_internal_atomic.h"
#include "ptl_internal_pid.h"
#include "ptl_internal_nit.h"
#include "ptl_internal_handles.h"
#include "ptl_internal_fragments.h"
#include "ptl_internal_EQ.h"
#include "ptl_internal_LE.h"
#include "ptl_internal_error.h"

typedef union {
    struct {
    } put;
    struct {
	ptl_internal_handle_converter_t md_handle;
	ptl_size_t local_offset;
    } get;
    struct {
    } atomic;
    struct {
	ptl_internal_handle_converter_t get_md_handle;
	ptl_size_t local_get_offset;
	ptl_internal_handle_converter_t put_md_handle;
	ptl_size_t local_put_offset;
    } fetchatomic;
    struct {
	ptl_internal_handle_converter_t get_md_handle;
	ptl_size_t local_get_offset;
	ptl_internal_handle_converter_t put_md_handle;
	ptl_size_t local_put_offset;
    } swap;
} ptl_internal_srcdata_t;

static uint32_t spawned;
static pthread_t catcher, ack_catcher;

static void *PtlInternalDMCatcher(void * __attribute__((unused)) junk) Q_NORETURN
{
    while (1) {
	ptl_pid_t src;
	ptl_internal_header_t * hdr = PtlInternalFragmentReceive();
	assert(hdr != NULL);
	//printf("%u ~> got a header! %p points to ni %i\n", (unsigned int)proc_number, hdr, hdr->ni);
	src = hdr->src;
	assert(nit.tables != NULL);
	if (nit.tables[hdr->ni] != NULL) {
	    ptl_table_entry_t *table_entry = &(nit.tables[hdr->ni][hdr->pt_index]);
	    if (table_entry->status != 0) {
		assert(pthread_mutex_lock(&table_entry->lock) == 0);
		switch (PtlInternalPTValidate(table_entry)) {
		    case 1: // uninitialized
			fprintf(stderr, "sent to an uninitialized PT!\n");
			abort();
			break;
		    case 2: // disabled
			fprintf(stderr, "sent to a disabled PT!\n");
			abort();
			break;
		}
		//printf("%u ~> received NI = %u, pt_index = %u, priority=%p, overflow=%p\n", (unsigned int)proc_number, (unsigned int)hdr->ni, hdr->pt_index, table_entry->priority.head, table_entry->overflow.head);
		switch (hdr->ni) {
		    case 0: case 2: // Matching (ME)
			fprintf(stderr, "Matching delivery not handled yet, sorry\n");
			break;
		    case 1: case 3: // Non-matching (LE)
			//printf("delivering to LE table\n");
			switch (hdr->src = PtlInternalLEDeliver(table_entry, hdr)) {
			    case 0: // who knows, we must not send an ack
				//printf("%u ~> not sending an ack\n", (unsigned int)proc_number);
				break;
			    case 1: // success
				//printf("LE delivery success!\n");
				//printf("%u ~> LE delivery success!\n", (unsigned int)proc_number);
				break;
			    case 2: // overflow
				//printf("%u ~> LE delivery overflow!\n", (unsigned int)proc_number);
				break;
			    case 4: // nothing matched, report error
				//printf("%u ~> LE says nothing matched!\n", (unsigned int)proc_number);
				hdr->length = 0;
				break;
			    case 3: // Permission Violation
				//printf("%u ~> LE says permission violation!\n", (unsigned int)proc_number);
				hdr->length = 0;
				break;
			}
			break;
		}
		//printf("unlocking\n");
		assert(pthread_mutex_unlock(&table_entry->lock) == 0);
	    } else {
		hdr->src = 9999;
		//printf("%u ~> table_entry->status == 0\n", (unsigned int)proc_number);
	    }
	} else { // uninitialized NI
	    hdr->src = 99999;
	}
	//printf("returning fragment\n");
	/* Now, return the fragment to the sender */
	PtlInternalFragmentAck(hdr, src);
	//printf("back to the beginning\n");
    }
}

static void *PtlInternalDMAckCatcher(void * __attribute__((unused)) junk)
{
    while (1) {
	ptl_internal_header_t * hdr = PtlInternalFragmentAckReceive();
	ptl_md_t *mdptr = NULL;
	ptl_handle_md_t md_handle = PTL_INVALID_HANDLE.md;
	ptl_internal_srcdata_t *einfo = NULL;
	//printf("%u +> got an ACK (%p)\n", (unsigned int)proc_number, hdr);
	/* first, figure out what to do with the ack */
	switch(hdr->type) {
	    case HDR_TYPE_PUT:
		//printf("%u +> it's an ACK for a PUT\n", (unsigned int)proc_number);
		md_handle = (ptl_handle_md_t)(uintptr_t)(hdr->src_data_ptr);
		mdptr = PtlInternalMDFetch(md_handle);
		break;
	    case HDR_TYPE_GET:
		//printf("%u +> it's an ACK for a GET\n", (unsigned int)proc_number);
		einfo = hdr->src_data_ptr;
		md_handle = einfo->get.md_handle.a.md;
		mdptr = PtlInternalMDFetch(md_handle);
		/* pull the data out of the reply */
		//printf("replied with %i data\n", (int)hdr->length);
		if (mdptr != NULL && (hdr->src == 1 || hdr->src == 0)) {
		    memcpy((uint8_t*)(mdptr->start) + einfo->get.local_offset, hdr->data, hdr->length);
		}
		break;
	    case HDR_TYPE_ATOMIC:
		//printf("%u +> it's an ACK for an ATOMIC\n", (unsigned int)proc_number);
		md_handle = (ptl_handle_md_t)(uintptr_t)(hdr->src_data_ptr);
		mdptr = PtlInternalMDFetch(md_handle);
		break;
	    case HDR_TYPE_FETCHATOMIC:
		//printf("%u +> it's an ACK for a FETCHATOMIC\n", (unsigned int)proc_number);
		einfo = hdr->src_data_ptr;
		assert(einfo != NULL);
		md_handle = einfo->fetchatomic.get_md_handle.a.md;
		if (einfo->fetchatomic.put_md_handle.a.md != PTL_INVALID_HANDLE.md) {
		    PtlInternalMDCleared(einfo->fetchatomic.put_md_handle.a.md);
		}
		mdptr = PtlInternalMDFetch(md_handle);
		/* pull the data out of the reply */
		//printf("replied with %i data\n", (int)hdr->length);
		if (mdptr != NULL && (hdr->src == 1 || hdr->src == 0)) {
		    memcpy((uint8_t*)(mdptr->start) + einfo->fetchatomic.local_get_offset, hdr->data, hdr->length);
		}
		break;
	    case HDR_TYPE_SWAP:
		fprintf(stderr, "SWAP REPLY unimplemented\n");
		abort();
		break;
	    default: // impossible
		*(int*)0 = 0;
	}
	/* Report the ack */
	switch(hdr->src) {
	    case 0: // Pretend we didn't recieve an ack
		//printf("%u +> it's a secret ACK\n", (unsigned int)proc_number);
		break;
	    case 1: // success
	    case 2: // overflow
		//printf("%u +> it's a successful/overflow ACK (%p)\n", (unsigned int)proc_number, mdptr);
		if (mdptr != NULL) {
		    int ct_enabled = 0;
		    if (hdr->type == HDR_TYPE_PUT) {
			ct_enabled = mdptr->options & PTL_MD_EVENT_CT_ACK;
		    } else {
			ct_enabled = mdptr->options & PTL_MD_EVENT_CT_REPLY;
		    }
		    if (mdptr->ct_handle != PTL_CT_NONE && ct_enabled != 0) {
			if ((mdptr->options & PTL_MD_EVENT_CT_BYTES) == 0) {
			    ptl_ct_event_t cte = {1, 0};
			    PtlCTInc(mdptr->ct_handle, cte);
			} else {
			    ptl_ct_event_t cte = {hdr->length, 0};
			    PtlCTInc(mdptr->ct_handle, cte);
			}
		    }
		    if (mdptr->eq_handle != PTL_EQ_NONE && (mdptr->options & (PTL_MD_EVENT_DISABLE | PTL_MD_EVENT_SUCCESS_DISABLE)) == 0) {
			ptl_event_t e;
			switch (hdr->type) {
			    case HDR_TYPE_PUT:
				e.type = PTL_EVENT_ACK;
				break;
			    default:
				e.type = PTL_EVENT_REPLY;
				break;
			}
			e.event.ievent.mlength = hdr->length;
			e.event.ievent.offset = hdr->dest_offset;
			e.event.ievent.user_ptr = hdr->user_ptr;
			e.event.ievent.ni_fail_type = PTL_NI_OK;
			PtlInternalEQPush(mdptr->eq_handle, &e);
		    }
		}
		//printf("%u +> finished notification of successful ACK\n", (unsigned int)proc_number);
		break;
	    case 99999: // NI not initialized
		//printf("%u +> ACK says NI not initialized\n", (unsigned int)proc_number);
		//goto reporterror;
	    case 9999: // PT not allocated
		//printf("%u +> ACK says PT-not-allocated\n", (unsigned int)proc_number);
		//goto reporterror;
	    case 3: // Permission Violation
		//printf("%u +> ACK says permission violation\n", (unsigned int)proc_number);
		//goto reporterror;
	    case 4: // nothing matched, report error
//reporterror:
		//printf("%u +> ACK says nothing matched!\n", (unsigned int)proc_number);
		if (mdptr != NULL) {
		    int ct_enabled = 0;
		    switch (hdr->type) {
			case HDR_TYPE_PUT: ct_enabled = mdptr->options & PTL_MD_EVENT_CT_ACK; break;
			default: ct_enabled = mdptr->options & PTL_MD_EVENT_CT_REPLY; break;
		    }
		    if (ct_enabled) {
			if (mdptr->ct_handle != PTL_CT_NONE) {
			    ptl_ct_event_t cte = {0, 1};
			    ptl_ct_event_t ctc;
			    PtlCTGet(mdptr->ct_handle, &ctc);
			    //printf("%u +> ct before inc = {%u,%u}\n", (unsigned int)proc_number, (unsigned int)ctc.success, (unsigned int)ctc.failure);
			    PtlCTInc(mdptr->ct_handle, cte);
			    PtlCTGet(mdptr->ct_handle, &ctc);
			    //printf("%u +> ct after inc = {%u,%u}\n", (unsigned int)proc_number, (unsigned int)ctc.success, (unsigned int)ctc.failure);
			} else {
			    fprintf(stderr, "enabled CT counting, but no CT!\n");
			}
		    }
		    if (mdptr->eq_handle != PTL_EQ_NONE && (mdptr->options & (PTL_MD_EVENT_DISABLE)) == 0) {
			ptl_event_t e;
			e.type = PTL_EVENT_ACK;
			e.event.ievent.mlength = hdr->length;
			e.event.ievent.offset = hdr->dest_offset;
			e.event.ievent.user_ptr = hdr->user_ptr;
			switch (hdr->src) {
			    case 3:
				e.event.ievent.ni_fail_type = PTL_NI_PERM_VIOLATION;
				break;
			    case 9999: // bad PT
			    case 99999: // bad NI
				e.event.ievent.ni_fail_type = PTL_NI_UNDELIVERABLE;
				break;
			    default:
				e.event.ievent.ni_fail_type = PTL_NI_OK;
			}
			PtlInternalEQPush(mdptr->eq_handle, &e);
		    }
		}
		break;
	}
	if (mdptr != NULL && md_handle != PTL_INVALID_HANDLE.md) {
	    //printf("%u +> clearing ACK's md_handle\n", (unsigned int)proc_number);
	    PtlInternalMDCleared(md_handle);
	}
	if (einfo != NULL) {
	    free(einfo);
	}
	//printf("%u +> freeing fragment (%p)\n", (unsigned int)proc_number, hdr);
	/* now, put the fragment back in the freelist */
	PtlInternalFragmentFree(hdr);
    }
}

void INTERNAL PtlInternalDMSetup(
    void)
{
    if (PtlInternalAtomicInc(&spawned, 1) == 0) {
	assert(pthread_create(&catcher, NULL, PtlInternalDMCatcher, NULL) == 0);
	assert(pthread_create(&ack_catcher, NULL, PtlInternalDMAckCatcher, NULL) == 0);
    }
}

void INTERNAL PtlInternalDMTeardown(
    void)
{
    if (PtlInternalAtomicInc(&spawned, -1) == 1) {
	assert(pthread_cancel(catcher) == 0);
	assert(pthread_join(catcher, NULL) == 0);
	assert(pthread_cancel(ack_catcher) == 0);
	assert(pthread_join(ack_catcher, NULL) == 0);
    }
}

int API_FUNC PtlPut(
    ptl_handle_md_t md_handle,
    ptl_size_t local_offset,
    ptl_size_t length,
    ptl_ack_req_t ack_req,
    ptl_process_t target_id,
    ptl_pt_index_t pt_index,
    ptl_match_bits_t match_bits,
    ptl_size_t remote_offset,
    void *user_ptr,
    ptl_hdr_data_t hdr_data)
{
    ptl_internal_header_t *hdr;
    ptl_md_t *mdptr;
    const ptl_internal_handle_converter_t md = { md_handle };
#ifndef NO_ARG_VALIDATION
    if (comm_pad == NULL) {
	return PTL_NO_INIT;
    }
    if (PtlInternalMDHandleValidator(md_handle,1)) {
	VERBOSE_ERROR("Invalid md_handle\n");
	return PTL_ARG_INVALID;
    }
    if (length == 0) {
	VERBOSE_ERROR("Zero length operations cannot detect errors/success reliably.\n");
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDLength(md_handle) < local_offset + length) {
	VERBOSE_ERROR("MD too short for local_offset\n");
	return PTL_ARG_INVALID;
    }
    switch (md.s.ni) {
	case 0:		       // Logical
	case 1:		       // Logical
	    if (PtlInternalLogicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (rank=%u)\n", (unsigned int)target_id.rank);
		return PTL_ARG_INVALID;
	    }
	    break;
	case 2:		       // Physical
	case 3:		       // Physical
	    if (PtlInternalPhysicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (pid=%u, nid=%u)\n", (unsigned int)target_id.phys.pid, (unsigned int)target_id.phys.nid);
		return PTL_ARG_INVALID;
	    }
	    break;
    }
#endif
    PtlInternalMDPosted(md_handle);
    /* step 1: get a local memory fragment */
    hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t) + length);
    //printf("got fragment %p, commpad = %p\n", hdr, comm_pad);
    /* step 2: fill the op structure */
    hdr->type = HDR_TYPE_PUT;
    hdr->ni = md.s.ni;
    //printf("hdr->NI = %u, md.s.ni = %u\n", (unsigned int)hdr->ni, (unsigned int)md.s.ni);
    hdr->src = proc_number;
    hdr->pt_index = pt_index;
    hdr->match_bits = match_bits;
    hdr->dest_offset = remote_offset;
    hdr->length = length;
    hdr->user_ptr = user_ptr;
    assert(sizeof(void*) >= sizeof(ptl_handle_md_t));
    hdr->src_data_ptr = (void*)(intptr_t)md_handle;
    hdr->info.put.hdr_data = hdr_data;
    hdr->info.put.ack_req = ack_req;
    /* step 3: load up the data */
    if (PtlInternalFragmentSize(hdr) - sizeof(ptl_internal_header_t) >= length) {
	memcpy(hdr->data, PtlInternalMDDataPtr(md_handle) + local_offset, length);
    } else {
#warning supersize messages need to be handled
	fprintf(stderr, "need to implement rendezvous protocol (got a %llu-byte fragment, need to send %llu bytes)\n", (unsigned long long)PtlInternalFragmentSize(hdr), (unsigned long long) length);
	abort();
    }
    /* step 4: enqueue the op structure on the target */
    switch (md.s.ni) {
	case 0:
	case 1:		       // Logical
	    PtlInternalFragmentToss(hdr, target_id.rank);
	    break;
	case 2:
	case 3:		       // Physical
	    PtlInternalFragmentToss(hdr, target_id.phys.pid);
	    break;
    }
    /* step 5: report the send event */
    mdptr = PtlInternalMDFetch(md_handle);
    if (mdptr->options & PTL_MD_EVENT_CT_SEND) {
	if ((mdptr->options & PTL_MD_EVENT_CT_BYTES) == 0) {
	    ptl_ct_event_t cte = {1, 0};
	    PtlCTInc(mdptr->ct_handle, cte);
	} else {
	    ptl_ct_event_t cte = {length, 0};
	    PtlCTInc(mdptr->ct_handle, cte);
	}
    }
    if ((mdptr->options & (PTL_MD_EVENT_DISABLE | PTL_MD_EVENT_SUCCESS_DISABLE)) == 0) {
	ptl_event_t e;
	e.type = PTL_EVENT_SEND;
	e.event.ievent.mlength = length;
	e.event.ievent.offset = local_offset;
	e.event.ievent.user_ptr = user_ptr;
	e.event.ievent.ni_fail_type = PTL_NI_OK;
	PtlInternalEQPush(mdptr->eq_handle, &e);
    }
    return PTL_OK;
}

int API_FUNC PtlGet(
    ptl_handle_md_t md_handle,
    ptl_size_t local_offset,
    ptl_size_t length,
    ptl_process_t target_id,
    ptl_pt_index_t pt_index,
    ptl_match_bits_t match_bits,
    void *user_ptr,
    ptl_size_t remote_offset)
{
    const ptl_internal_handle_converter_t md = { md_handle };
    ptl_internal_srcdata_t *extra_info;
    ptl_internal_header_t *hdr;
#ifndef NO_ARG_VALIDATION
    if (comm_pad == NULL) {
	return PTL_NO_INIT;
    }
    if (PtlInternalMDHandleValidator(md_handle,1)) {
	VERBOSE_ERROR("Invalid md_handle\n");
	return PTL_ARG_INVALID;
    }
    if (length == 0) {
	VERBOSE_ERROR("Zero length operations cannot detect errors/success reliably.\n");
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDLength(md_handle) < local_offset + length) {
	VERBOSE_ERROR("MD too short for local_offset\n");
	return PTL_ARG_INVALID;
    }
    switch (md.s.ni) {
	case 0:		       // Logical
	case 1:		       // Logical
	    if (PtlInternalLogicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (rank=%u)\n", (unsigned int)target_id.rank);
		return PTL_ARG_INVALID;
	    }
	    break;
	case 2:		       // Physical
	case 3:		       // Physical
	    if (PtlInternalPhysicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (pid=%u, nid=%u)\n", (unsigned int)target_id.phys.pid, (unsigned int)target_id.phys.nid);
		return PTL_ARG_INVALID;
	    }
	    break;
    }
#endif
    PtlInternalMDPosted(md_handle);
    /* step 1: get a local memory fragment */
    hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t) + length);
    /* step 2: fill the op structure */
    hdr->type = HDR_TYPE_GET;
    hdr->ni = md.s.ni;
    hdr->src = proc_number;
    hdr->pt_index = pt_index;
    hdr->match_bits = match_bits;
    hdr->dest_offset = remote_offset;
    hdr->length = length;
    hdr->user_ptr = user_ptr;
    extra_info = malloc(sizeof(ptl_internal_srcdata_t));
    assert(extra_info);
    extra_info->get.md_handle.a.md = md_handle;
    extra_info->get.local_offset = local_offset;
    hdr->src_data_ptr = extra_info;
    /* step 3: enqueue the op structure on the target */
    switch (md.s.ni) {
	case 0:
	case 1:		       // Logical
	    PtlInternalFragmentToss(hdr, target_id.rank);
	    break;
	case 2:
	case 3:		       // Physical
	    PtlInternalFragmentToss(hdr, target_id.phys.pid);
	    break;
    }
    /* no send event to report */
    return PTL_OK;
}

int API_FUNC PtlAtomic(
    ptl_handle_md_t md_handle,
    ptl_size_t local_offset,
    ptl_size_t length,
    ptl_ack_req_t ack_req,
    ptl_process_t target_id,
    ptl_pt_index_t pt_index,
    ptl_match_bits_t match_bits,
    ptl_size_t remote_offset,
    void *user_ptr,
    ptl_hdr_data_t hdr_data,
    ptl_op_t operation,
    ptl_datatype_t datatype)
{
    ptl_internal_header_t *hdr;
    ptl_md_t *mdptr;
    const ptl_internal_handle_converter_t md = { md_handle };
#ifndef NO_ARG_VALIDATION
    if (comm_pad == NULL) {
	return PTL_NO_INIT;
    }
    if (length == 0) {
	VERBOSE_ERROR("Zero length operations cannot detect errors/success reliably.\n");
	return PTL_ARG_INVALID;
    }
    if (length > nit_limits.max_atomic_size) {
	VERBOSE_ERROR("Length (%u) is bigger than max_atomic_size (%u)\n", (unsigned int)length, (unsigned int)nit_limits.max_atomic_size);
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDHandleValidator(md_handle,1)) {
	VERBOSE_ERROR("Invalid MD\n");
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDLength(md_handle) < local_offset + length) {
	VERBOSE_ERROR("MD too short for local_offset\n");
	return PTL_ARG_INVALID;
    }
    switch (md.s.ni) {
	case 0:		       // Logical
	case 1:		       // Logical
	    if (PtlInternalLogicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (rank=%u)\n", (unsigned int)target_id.rank);
		return PTL_ARG_INVALID;
	    }
	    break;
	case 2:		       // Physical
	case 3:		       // Physical
	    if (PtlInternalPhysicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (pid=%u, nid=%u)\n", (unsigned int)target_id.phys.pid, (unsigned int)target_id.phys.nid);
		return PTL_ARG_INVALID;
	    }
	    break;
    }
    switch (operation) {
	case PTL_SWAP: case PTL_CSWAP: case PTL_MSWAP:
	    VERBOSE_ERROR("SWAP/CSWAP/MSWAP invalid optypes for PtlAtomic()\n");
	    return PTL_ARG_INVALID;
	case PTL_LOR: case PTL_LAND: case PTL_LXOR:
	case PTL_BOR: case PTL_BAND: case PTL_BXOR:
	    switch (datatype) {
		case PTL_DOUBLE: case PTL_FLOAT:
		    VERBOSE_ERROR("PTL_DOUBLE/PTL_FLOAT invalid datatypes for logical/binary operations\n");
		    return PTL_ARG_INVALID;
		default:
		    break;
	    }
	default:
	    break;
    }
#endif
    PtlInternalMDPosted(md_handle);
    /* step 1: get a local memory fragment */
    hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t) + length);
    /* step 2: fill the op structure */
    hdr->type = HDR_TYPE_ATOMIC;
    hdr->ni = md.s.ni;
    hdr->src = proc_number;
    hdr->pt_index = pt_index;
    hdr->match_bits = match_bits;
    hdr->dest_offset = remote_offset;
    hdr->length = length;
    hdr->user_ptr = user_ptr;
    assert(sizeof(void*) >= sizeof(ptl_handle_md_t));
    hdr->src_data_ptr = (void*)(intptr_t)md_handle;
    hdr->info.atomic.hdr_data = hdr_data;
    hdr->info.atomic.ack_req = ack_req;
    hdr->info.atomic.operation = operation;
    hdr->info.atomic.datatype = datatype;
    /* step 3: load up the data */
    memcpy(hdr->data, PtlInternalMDDataPtr(md_handle) + local_offset, length);
    /* step 4: enqueue the op structure on the target */
    switch (md.s.ni) {
	case 0:
	case 1:		       // Logical
	    PtlInternalFragmentToss(hdr, target_id.rank);
	    break;
	case 2:
	case 3:		       // Physical
	    PtlInternalFragmentToss(hdr, target_id.phys.pid);
	    break;
    }
    /* step 5: report the send event */
    mdptr = PtlInternalMDFetch(md_handle);
    if (mdptr->options & PTL_MD_EVENT_CT_SEND) {
	if ((mdptr->options & PTL_MD_EVENT_CT_BYTES) == 0) {
	    ptl_ct_event_t cte = {1, 0};
	    PtlCTInc(mdptr->ct_handle, cte);
	} else {
	    ptl_ct_event_t cte = {length, 0};
	    PtlCTInc(mdptr->ct_handle, cte);
	}
    }
    if ((mdptr->options & PTL_MD_EVENT_DISABLE) == 0) {
	if ((mdptr->options & PTL_MD_EVENT_SUCCESS_DISABLE) == 0) {
	    ptl_event_t e;
	    e.type = PTL_EVENT_SEND;
	    e.event.ievent.mlength = length;
	    e.event.ievent.offset = local_offset;
	    e.event.ievent.user_ptr = user_ptr;
	    e.event.ievent.ni_fail_type = PTL_NI_OK;
	    PtlInternalEQPush(mdptr->eq_handle, &e);
	}
    }
    return PTL_OK;
}

int API_FUNC PtlFetchAtomic(
    ptl_handle_md_t get_md_handle,
    ptl_size_t local_get_offset,
    ptl_handle_md_t put_md_handle,
    ptl_size_t local_put_offset,
    ptl_size_t length,
    ptl_process_t target_id,
    ptl_pt_index_t pt_index,
    ptl_match_bits_t match_bits,
    ptl_size_t remote_offset,
    void *user_ptr,
    ptl_hdr_data_t hdr_data,
    ptl_op_t operation,
    ptl_datatype_t datatype)
{
    ptl_internal_header_t *hdr;
    ptl_md_t *mdptr;
    ptl_internal_srcdata_t *extra_info;
    const ptl_internal_handle_converter_t get_md = { get_md_handle };
    const ptl_internal_handle_converter_t put_md = { put_md_handle };
#ifndef NO_ARG_VALIDATION
    if (comm_pad == NULL) {
	return PTL_NO_INIT;
    }
    if (PtlInternalMDHandleValidator(get_md_handle,1)) {
	VERBOSE_ERROR("Invalid get_md_handle\n");
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDHandleValidator(put_md_handle,1)) {
	VERBOSE_ERROR("Invalid put_md_handle\n");
	return PTL_ARG_INVALID;
    }
    if (length == 0) {
	VERBOSE_ERROR("Zero length operations cannot detect errors/success reliably.\n");
	return PTL_ARG_INVALID;
    }
    if (length > nit_limits.max_atomic_size) {
	VERBOSE_ERROR("Length (%u) is bigger than max_atomic_size (%u)\n", (unsigned int)length, (unsigned int)nit_limits.max_atomic_size);
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDLength(get_md_handle) < local_get_offset + length) {
	VERBOSE_ERROR("FetchAtomic saw get_md too short for local_offset\n");
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDLength(put_md_handle) < local_put_offset + length) {
	VERBOSE_ERROR("FetchAtomic saw put_md too short for local_offset\n");
	return PTL_ARG_INVALID;
    }
    if (get_md.s.ni != put_md.s.ni) {
	VERBOSE_ERROR("MDs *must* be on the same NI\n");
	return PTL_ARG_INVALID;
    }
    switch (get_md.s.ni) {
	case 0:		       // Logical
	case 1:		       // Logical
	    if (PtlInternalLogicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (rank=%u)\n", (unsigned int)target_id.rank);
		return PTL_ARG_INVALID;
	    }
	    break;
	case 2:		       // Physical
	case 3:		       // Physical
	    if (PtlInternalPhysicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (pid=%u, nid=%u)\n", (unsigned int)target_id.phys.pid, (unsigned int)target_id.phys.nid);
		return PTL_ARG_INVALID;
	    }
	    break;
    }
    switch (operation) {
	case PTL_CSWAP: case PTL_MSWAP:
	    VERBOSE_ERROR("MSWAP/CSWAP should be performed with PtlSwap\n");
	    return PTL_ARG_INVALID;
	case PTL_LOR: case PTL_LAND: case PTL_LXOR:
	case PTL_BOR: case PTL_BAND: case PTL_BXOR:
	    switch (datatype) {
		case PTL_DOUBLE: case PTL_FLOAT:
		    VERBOSE_ERROR("PTL_DOUBLE/PTL_FLOAT invalid datatypes for logical/binary operations\n");
		    return PTL_ARG_INVALID;
		default:
		    break;
	    }
	default:
	    break;
    }
#endif
    PtlInternalMDPosted(put_md_handle);
    PtlInternalMDPosted(get_md_handle);
    /* step 1: get a local memory fragment */
    hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t) + length);
    /* step 2: fill the op structure */
    hdr->type = HDR_TYPE_FETCHATOMIC;
    hdr->ni = get_md.s.ni;
    hdr->src = proc_number;
    hdr->pt_index = pt_index;
    hdr->match_bits = match_bits;
    hdr->dest_offset = remote_offset;
    hdr->length = length;
    hdr->user_ptr = user_ptr;
    extra_info = malloc(sizeof(ptl_internal_srcdata_t));
    assert(extra_info);
    extra_info->fetchatomic.get_md_handle.a.md = get_md_handle;
    extra_info->fetchatomic.local_get_offset = local_get_offset;
    extra_info->fetchatomic.put_md_handle.a.md = put_md_handle;
    extra_info->fetchatomic.local_put_offset = local_put_offset;
    hdr->src_data_ptr = extra_info;
    hdr->info.fetchatomic.hdr_data = hdr_data;
    hdr->info.fetchatomic.operation = operation;
    hdr->info.fetchatomic.datatype = datatype;
    /* step 3: load up the data */
    memcpy(hdr->data, PtlInternalMDDataPtr(put_md_handle) + local_put_offset,
	   length);
    /* step 4: enqueue the op structure on the target */
    switch (put_md.s.ni) {
	case 0:
	case 1:		       // Logical
	    PtlInternalFragmentToss(hdr, target_id.rank);
	    break;
	case 2:
	case 3:		       // Physical
	    PtlInternalFragmentToss(hdr, target_id.phys.pid);
	    break;
    }
    /* step 5: report the send event */
    mdptr = PtlInternalMDFetch(put_md_handle);
    if (mdptr->options & PTL_MD_EVENT_CT_SEND) {
	if ((mdptr->options & PTL_MD_EVENT_CT_BYTES) == 0) {
	    ptl_ct_event_t cte = {1, 0};
	    PtlCTInc(mdptr->ct_handle, cte);
	} else {
	    ptl_ct_event_t cte = {length, 0};
	    PtlCTInc(mdptr->ct_handle, cte);
	}
    }
    if ((mdptr->options & PTL_MD_EVENT_DISABLE) == 0) {
	if ((mdptr->options & PTL_MD_EVENT_SUCCESS_DISABLE) == 0) {
	    ptl_event_t e;
	    e.type = PTL_EVENT_SEND;
	    e.event.ievent.mlength = length;
	    e.event.ievent.offset = local_put_offset;
	    e.event.ievent.user_ptr = user_ptr;
	    e.event.ievent.ni_fail_type = PTL_NI_OK;
	    PtlInternalEQPush(mdptr->eq_handle, &e);
	}
    }
    return PTL_OK;
}

int API_FUNC PtlSwap(
    ptl_handle_md_t get_md_handle,
    ptl_size_t local_get_offset,
    ptl_handle_md_t put_md_handle,
    ptl_size_t local_put_offset,
    ptl_size_t length,
    ptl_process_t target_id,
    ptl_pt_index_t pt_index,
    ptl_match_bits_t match_bits,
    ptl_size_t remote_offset,
    void *user_ptr,
    ptl_hdr_data_t hdr_data,
    void *operand,
    ptl_op_t operation,
    ptl_datatype_t datatype)
{
    const ptl_internal_handle_converter_t get_md = { get_md_handle };
    const ptl_internal_handle_converter_t put_md = { put_md_handle };
    ptl_internal_header_t *hdr;
    ptl_internal_srcdata_t *extra_info;
#ifndef NO_ARG_VALIDATION
    if (comm_pad == NULL) {
	return PTL_NO_INIT;
    }
    if (PtlInternalMDHandleValidator(get_md_handle,1)) {
	VERBOSE_ERROR("Swap saw invalid get_md_handle\n");
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDHandleValidator(put_md_handle,1)) {
	VERBOSE_ERROR("Swap saw invalid put_md_handle\n");
	return PTL_ARG_INVALID;
    }
    if (length == 0) {
	VERBOSE_ERROR("Zero length operations cannot detect errors/success reliably.\n");
	return PTL_ARG_INVALID;
    }
    if (length > nit_limits.max_atomic_size) {
	VERBOSE_ERROR("Length (%u) is bigger than max_atomic_size (%u)\n", (unsigned int)length, (unsigned int)nit_limits.max_atomic_size);
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDLength(get_md_handle) < local_get_offset + length) {
	VERBOSE_ERROR("Swap saw get_md too short for local_offset\n");
	return PTL_ARG_INVALID;
    }
    if (PtlInternalMDLength(put_md_handle) < local_put_offset + length) {
	VERBOSE_ERROR("Swap saw put_md too short for local_offset\n");
	return PTL_ARG_INVALID;
    }
    if (get_md.s.ni != put_md.s.ni) {
	VERBOSE_ERROR("MDs *must* be on the same NI\n");
	return PTL_ARG_INVALID;
    }
    switch (get_md.s.ni) {
	case 0:		       // Logical
	case 1:		       // Logical
	    if (PtlInternalLogicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (rank=%u)\n", (unsigned int)target_id.rank);
		return PTL_ARG_INVALID;
	    }
	    break;
	case 2:		       // Physical
	case 3:		       // Physical
	    if (PtlInternalPhysicalProcessValidator(target_id)) {
		VERBOSE_ERROR("Invalid target_id (rank=%u)\n", (unsigned int)target_id.rank);
		return PTL_ARG_INVALID;
	    }
	    break;
    }
    switch (operation) {
	case PTL_SWAP:
	    break;
	case PTL_CSWAP: case PTL_MSWAP:
	    switch (datatype) {
		case PTL_DOUBLE: case PTL_FLOAT:
		    VERBOSE_ERROR("PTL_DOUBLE/PTL_FLOAT invalid datatypes for CSWAP/MSWAP\n");
		    return PTL_ARG_INVALID;
		default:
		    break;
	    }
	default:
	    VERBOSE_ERROR("Only PTL_SWAP/CSWAP/MSWAP may be used with PtlSwap\n");
	    return PTL_ARG_INVALID;
    }
#endif
    PtlInternalMDPosted(put_md_handle);
    PtlInternalMDPosted(get_md_handle);
    /* step 1: get a local memory fragment */
    hdr = PtlInternalFragmentFetch(sizeof(ptl_internal_header_t) + length + 8);
    /* step 2: fill the op structure */
    hdr->type = HDR_TYPE_SWAP;
    hdr->ni = get_md.s.ni;
    hdr->src = proc_number;
    hdr->pt_index = pt_index;
    hdr->match_bits = match_bits;
    hdr->dest_offset = remote_offset;
    hdr->length = length;
    hdr->user_ptr = user_ptr;
    extra_info = malloc(sizeof(ptl_internal_srcdata_t));
    assert(extra_info);
    extra_info->swap.get_md_handle.a.md = get_md_handle;
    extra_info->swap.local_get_offset = local_get_offset;
    extra_info->swap.put_md_handle.a.md = put_md_handle;
    extra_info->swap.local_put_offset = local_put_offset;
    hdr->src_data_ptr = extra_info;
    hdr->info.swap.hdr_data = hdr_data;
    hdr->info.swap.operation = operation;
    hdr->info.swap.datatype = datatype;
    /* step 3: load up the data */
    {
	char *dataptr = hdr->data;
	if (operation == PTL_CSWAP || operation == PTL_MSWAP) {
	    switch (datatype) {
		case PTL_CHAR:
		case PTL_UCHAR:
		    memcpy(dataptr, operand, 1);
		    break;
		case PTL_SHORT:
		case PTL_USHORT:
		    memcpy(dataptr, operand, 2);
		    break;
		case PTL_INT:
		case PTL_UINT:
		case PTL_FLOAT:
		    memcpy(dataptr, operand, 4);
		    break;
		case PTL_LONG:
		case PTL_ULONG:
		case PTL_DOUBLE:
		    memcpy(dataptr, operand, 8);
		    break;
	    }
	}
	dataptr += 8;
	memcpy(dataptr, PtlInternalMDDataPtr(put_md_handle) + local_put_offset,
		length);
    }
    /* step 4: enqueue the op structure on the target */
    switch (get_md.s.ni) {
	case 0:
	case 1:		       // Logical
	    PtlInternalFragmentToss(hdr, target_id.rank);
	    break;
	case 2:
	case 3:		       // Physical
	    PtlInternalFragmentToss(hdr, target_id.phys.pid);
	    break;
    }
    /* step 5: report the send event */
    {
	ptl_md_t *mdptr = PtlInternalMDFetch(put_md_handle);
	if (mdptr->options & PTL_MD_EVENT_CT_SEND) {
	    if ((mdptr->options & PTL_MD_EVENT_CT_BYTES) == 0) {
		ptl_ct_event_t cte = {1, 0};
		PtlCTInc(mdptr->ct_handle, cte);
	    } else {
		ptl_ct_event_t cte = {length, 0};
		PtlCTInc(mdptr->ct_handle, cte);
	    }
	}
	if ((mdptr->options & PTL_MD_EVENT_DISABLE) == 0) {
	    if ((mdptr->options & PTL_MD_EVENT_SUCCESS_DISABLE) == 0) {
		ptl_event_t e;
		e.type = PTL_EVENT_SEND;
		e.event.ievent.mlength = length;
		e.event.ievent.offset = local_put_offset;
		e.event.ievent.user_ptr = user_ptr;
		e.event.ievent.ni_fail_type = PTL_NI_OK;
		PtlInternalEQPush(mdptr->eq_handle, &e);
	    }
	}
    }
    return PTL_OK;
}
