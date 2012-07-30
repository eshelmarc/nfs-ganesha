/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
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
 * ---------------------------------------
 */

/**
 * \file    nfs4_op_delegreturn.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:50 $
 * \version $Revision: 1.10 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs4_op_delegreturn.c : Routines used for managing the NFS4 COMPOUND functions.
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
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include "HashData.h"
#include "HashTable.h"
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_proto_functions.h"
#include "nfs_tools.h"
#include "sal_functions.h"

/**
 * nfs4_op_delegreturn: The NFS4_OP_DELEGRETURN
 * 
 * Implements the NFS4_OP_DELEGRETURN. 
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 * 
 * @return NFS4_OK if ok, any other value show an error.
 *
 */
#define arg_DELEGRETURN4 op->nfs_argop4_u.opdelegreturn
#define res_DELEGRETURN4 resp->nfs_resop4_u.opdelegreturn

int nfs4_op_delegreturn(struct nfs_argop4 *op,
                        compound_data_t * data, struct nfs_resop4 *resp)
{
  char __attribute__ ((__unused__)) funcname[] = "nfs4_op_delegreturn";
  state_status_t      state_status;
  state_t           * pstate_found = NULL;
  state_owner_t     * plock_owner;
  fsal_lock_param_t   lock_desc;
  unsigned int         rc = 0;
  struct glist_head  * glist;
  cache_entry_t      * pentry = NULL;
  state_lock_entry_t * found_entry = NULL;
  const char         * tag = "DELEGRETURN";

#ifdef _WITH_NO_NFSV4_DELEGATIONS

  /* Lock are not supported */
  resp->resop = NFS4_OP_DELEGRETURN;
  res_DELEGRETURN4.status = NFS4_OK;

#else

  LogDebug(COMPONENT_NFS_V4_LOCK,
           "Entering NFS v4 DELEGRETURN handler -----------------------------------------------------");

  /* Initialize to sane default */
  resp->resop = NFS4_OP_DELEGRETURN;

  /* If there is no FH */
  if(nfs4_Is_Fh_Empty(&(data->currentFH)))
    {
      res_DELEGRETURN4.status = NFS4ERR_NOFILEHANDLE;
      return res_DELEGRETURN4.status;
    }

  /* If the filehandle is invalid */
  if(nfs4_Is_Fh_Invalid(&(data->currentFH)))
    {
      res_DELEGRETURN4.status = NFS4ERR_BADHANDLE;
      return res_DELEGRETURN4.status;
    }

  /* Tests if the Filehandle is expired (for volatile filehandle) */
  if(nfs4_Is_Fh_Expired(&(data->currentFH)))
    {
      res_DELEGRETURN4.status = NFS4ERR_FHEXPIRED;
      return res_DELEGRETURN4.status;
    }

  /* Delegation is done only on a file */
  if(data->current_filetype != REGULAR_FILE)
    {
      /* Type of the entry is not correct */
      switch (data->current_filetype)
        {
        case DIRECTORY:
          res_DELEGRETURN4.status = NFS4ERR_ISDIR;
          return res_DELEGRETURN4.status;

        default:
          res_DELEGRETURN4.status = NFS4ERR_INVAL;
          return res_DELEGRETURN4.status;
        }
    }
  /* Only read delegations */
  lock_desc.lock_type = FSAL_LOCK_R;
  lock_desc.lock_start = 0;
  lock_desc.lock_length = 0;
  lock_desc.lock_sle_type = FSAL_LEASE_LOCK;

  /* Check stateid correctness and get pointer to state */
  if((rc = nfs4_Check_Stateid(&arg_DELEGRETURN4.deleg_stateid,
                              data->current_entry,
                              &pstate_found,
                              data,
                              STATEID_SPECIAL_FOR_LOCK,
                              tag)) != NFS4_OK)
    {
      res_DELEGRETURN4.status = rc;
      return res_DELEGRETURN4.status;
    }

#if 0 //??? temp fix
  /* Check seqid (lock_seqid or open_seqid) */
  if(!Check_nfs4_seqid(plock_owner,
                       arg_DELEGRETURN4.deleg_stateid.seqid,
                       op,
                       data,
                       resp,
                       tag))
    {
      /* Response is all setup for us and LogDebug told what was wrong */
      return res_DELEGRETURN4.status;
    }
#endif
  pentry = data->current_entry;

  LogDebug(COMPONENT_NFS_CB,"xxx pentry %p", pentry);

  pthread_rwlock_wrlock(&pentry->state_lock);

  glist_for_each(glist, &pentry->object.file.lock_list)
  {
      found_entry = glist_entry(glist, state_lock_entry_t, sle_list);

      if (found_entry != NULL)
      {
          LogDebug(COMPONENT_NFS_CB,"xxx found_entry %p", found_entry);
      }
      else
      {
          LogDebug(COMPONENT_NFS_CB,"xxx list is empty %p", found_entry);
          pthread_rwlock_unlock(&pentry->state_lock);
          res_DELEGRETURN4.status = NFS4ERR_BAD_STATEID;
          return res_DELEGRETURN4.status;
      }
      break;
  }
  pthread_rwlock_unlock(&pentry->state_lock);

  plock_owner = found_entry->sle_owner;

  LogLock(COMPONENT_NFS_V4_LOCK, NIV_FULL_DEBUG,
          tag,
          data->current_entry,
          data->pcontext,
          plock_owner,
          &lock_desc);

  /* Now we have a lock owner and a stateid.
   * Go ahead and push unlock into SAL (and FSAL).
   */
  if(state_unlock(data->current_entry,
                  data->pcontext,
                  data->pexport,
                  plock_owner,
                  pstate_found,
                  &lock_desc,
                  &state_status,
                  LEASE_LOCK) != STATE_SUCCESS)
    {
      res_DELEGRETURN4.status = nfs4_Errno_state(state_status);

#if 0 //???
      /* Save the response in the lock owner */
      Copy_nfs4_state_req(plock_owner, arg_DELEGRETURN4.deleg_stateid.seqid, op, data, resp, tag);
#endif

      return res_DELEGRETURN4.status;
    }

  /* Successful exit */
  res_DELEGRETURN4.status = NFS4_OK;

#if 0 //???
  /* Save the response in the lock owner */
  Copy_nfs4_state_req(plock_owner, arg_DELEGRETURN4.deleg_stateid.seqid, op, data, resp, tag);
#endif

#endif

  return res_DELEGRETURN4.status;
}                               /* nfs4_op_delegreturn */

/**
 * nfs4_op_delegreturn_Free: frees what was allocared to handle nfs4_op_delegreturn.
 * 
 * Frees what was allocared to handle nfs4_op_delegreturn.
 *
 * @param resp  [INOUT]    Pointer to nfs4_op results
 *
 * @return nothing (void function )
 * 
 */
void nfs4_op_delegreturn_Free(DELEGRETURN4res * resp)
{
  /* Nothing to be done */
  return;
}                               /* nfs4_op_delegreturn_Free */
