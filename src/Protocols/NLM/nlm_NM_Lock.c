/*
 * Copyright IBM Corporation, 2010
 *  Contributor: Aneesh Kumar K.v  <aneesh.kumar@linux.vnet.ibm.com>
 *
 * --------------------------
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "rpc.h"
#include "log.h"
#include "stuff_alloc.h"
#include "nlm4.h"
#include "sal_functions.h"
#include "nlm_util.h"
#include "nlm_async.h"

/**
 * nlm4_Lock: Set a range lock
 *
 *  @param parg        [IN]
 *  @param pexportlist [IN]
 *  @param pcontextp   [IN]
 *  @param pclient     [INOUT]
 *  @param ht          [INOUT]
 *  @param preq        [IN]
 *  @param pres        [OUT]
 *
 */

int nlm4_NM_Lock(nfs_arg_t * parg /* IN     */ ,
                 exportlist_t * pexport /* IN     */ ,
                 fsal_op_context_t * pcontext /* IN     */ ,
                 cache_inode_client_t * pclient /* INOUT  */ ,
                 struct svc_req *preq /* IN     */ ,
                 nfs_res_t * pres /* OUT    */ )
{
  nlm4_lockargs      * arg = &parg->arg_nlm4_lock;
  cache_entry_t      * pentry;
  state_status_t       state_status = STATE_SUCCESS;
  char                 buffer[MAXNETOBJ_SZ * 2];
  state_nsm_client_t * nsm_client;
  state_nlm_client_t * nlm_client;
  state_owner_t      * nlm_owner, * holder;
  fsal_lock_param_t    lock, conflict;
  int                  rc;
  state_block_data_t * pblock_data;

  netobj_to_string(&arg->cookie, buffer, 1024);
  LogDebug(COMPONENT_NLM,
           "REQUEST PROCESSING: Calling nlm4_NM_Lock svid=%d off=%llx len=%llx cookie=%s reclaim=%s",
           (int) arg->alock.svid,
           (unsigned long long) arg->alock.l_offset,
           (unsigned long long) arg->alock.l_len, buffer,
           arg->reclaim ? "yes" : "no");

  if(!copy_netobj(&pres->res_nlm4test.cookie, &arg->cookie))
    {
      pres->res_nlm4.stat.stat = NLM4_FAILED;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_NM_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }

  /* allow only reclaim lock request during recovery */
  if(nfs_in_grace() && !arg->reclaim)
    {
      pres->res_nlm4.stat.stat = NLM4_DENIED_GRACE_PERIOD;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_NM_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }

  if(!nfs_in_grace() && arg->reclaim)
    {
      pres->res_nlm4.stat.stat = NLM4_DENIED_GRACE_PERIOD;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_NM_Lock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }

  rc = nlm_process_parameters(preq,
                              arg->exclusive,
                              &arg->alock,
                              &lock,
                              &pentry,
                              pcontext,
                              pclient,
                              CARE_NO_MONITOR,
                              &nsm_client,
                              &nlm_client,
                              &nlm_owner,
                              &pblock_data);

  if(rc >= 0)
    {
      /* Present the error back to the client */
      pres->res_nlm4.stat.stat = (nlm4_stats)rc;
      LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_Unlock %s",
               lock_result_str(pres->res_nlm4.stat.stat));
      return NFS_REQ_OK;
    }

  /* Cast the state number into a state pointer to protect
   * locks from a client that has rebooted from the SM_NOTIFY
   * that will release old locks
   */
  if(state_lock(pentry,
                pcontext,
                pexport,
                nlm_owner,
                (void *) (ptrdiff_t) arg->state,
                arg->block ? STATE_NLM_BLOCKING : STATE_NON_BLOCKING,
                pblock_data,
                &lock,
                &holder,
                &conflict,
                pclient,
                &state_status) != STATE_SUCCESS)
    {
      pres->res_nlm4test.test_stat.stat = nlm_convert_state_error(state_status);

      if(state_status == STATE_LOCK_CONFLICT)
        {
          nlm_process_conflict(&pres->res_nlm4test.test_stat.nlm4_testrply_u.holder,
                               holder,
                               &conflict,
                               pclient);
        }

      /* If we didn't block, release the block data */
      if(state_status != STATE_LOCK_BLOCKED && pblock_data != NULL)
        Mem_Free(pblock_data);
    }
  else
    {
      pres->res_nlm4.stat.stat = NLM4_GRANTED;
    }

  /* Release the NLM Client and NLM Owner references we have */
  dec_nsm_client_ref(nsm_client);
  dec_nlm_client_ref(nlm_client);
  dec_state_owner_ref(nlm_owner, pclient);

  LogDebug(COMPONENT_NLM, "REQUEST RESULT: nlm4_NM_Lock %s",
           lock_result_str(pres->res_nlm4.stat.stat));
  return NFS_REQ_OK;
}

/**
 * nlm4_NM_Lock_Free: Frees the result structure allocated for nlm4_NM_Lock
 *
 * Frees the result structure allocated for nlm4_NM_Lock. Does Nothing in fact.
 *
 * @param pres        [INOUT]   Pointer to the result structure.
 *
 */
void nlm4_NM_Lock_Free(nfs_res_t * pres)
{
  netobj_free(&pres->res_nlm4test.cookie);
  return;
}