/*
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
 * \file    fsal_up_thread.c
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

#include "nfs_core.h"
#include "log.h"
#include "fsal.h"
#include "cache_inode.h"
#include "HashTable.h"
#include "fsal_up.h"
#include "sal_functions.h"
#include "nfs_rpc_callback.h"

static int32_t cb_completion_func(rpc_call_t* call, rpc_call_hook hook,
                                 void* arg, uint32_t flags)
{
  char *fh;

  LogDebug(COMPONENT_NFS_CB, "%p %s", call,
           (hook == RPC_CALL_ABORT) ?
           "RPC_CALL_ABORT" :
           "RPC_CALL_COMPLETE");
  switch (hook) {
    case RPC_CALL_COMPLETE:
        /* potentially, do something more interesting here */
        LogDebug(COMPONENT_NFS_CB, "call result: %d", call->stat);
        fh = call->cbt.v_u.v4.args.argarray.argarray_val->nfs_cb_argop4_u.opcbrecall.fh.nfs_fh4_val;
printf("xxx free maxfh %p\n", fh);

        gsh_free(fh);
        cb_compound_free(&call->cbt);
        break;
    default:
        LogDebug(COMPONENT_NFS_CB, "%p unknown hook %d", call, hook);
        break;
  }
  return (0);
}

/* Set the FSAL UP functions that will be used to process events.
 * This is called DUMB_FSAL_UP because it only invalidates cache inode
 * entires ... inode entries are not updated or refreshed through this
 * interface. */

fsal_status_t dumb_fsal_up_invalidate_step1(fsal_up_event_data_t * pevdata)
{
  cache_inode_status_t cache_status;

  LogDebug(COMPONENT_FSAL_UP,
           "FSAL_UP_DUMB: calling cache_inode_invalidate()");

  /* Lock the entry */
  cache_inode_invalidate(&pevdata->event_context.fsal_data,
                         &cache_status,
                         CACHE_INODE_INVALIDATE_CLEARBITS);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t dumb_fsal_up_invalidate_step2(fsal_up_event_data_t * pevdata)
{
  cache_inode_status_t cache_status;

  LogDebug(COMPONENT_FSAL_UP,
           "FSAL_UP_DUMB: calling cache_inode_invalidate()");

  /* Lock the entry */
  cache_inode_invalidate(&pevdata->event_context.fsal_data,
                         &cache_status,
                         CACHE_INODE_INVALIDATE_CLOSE);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t dumb_fsal_up_update(fsal_up_event_data_t * pevdata)
{
  cache_inode_status_t cache_status;

  LogFullDebug(COMPONENT_FSAL_UP,
               "FSAL_UP_DUMB: Entered dumb_fsal_up_update\n");
  if ((pevdata->type.update.upu_flags & FSAL_UP_NLINK) &&
      (pevdata->type.update.upu_stat_buf.st_nlink == 0) )
    {
      LogFullDebug(COMPONENT_FSAL_UP,
               "FSAL_UP_DUMB: nlink has become zero; close fds\n");
      cache_inode_invalidate(&pevdata->event_context.fsal_data,
                             &cache_status,
                             (CACHE_INODE_INVALIDATE_CLEARBITS |
                              CACHE_INODE_INVALIDATE_CLOSE));
    }
  else
    cache_inode_invalidate(&pevdata->event_context.fsal_data,
                           &cache_status,
                           CACHE_INODE_INVALIDATE_CLEARBITS);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t fsal_up_delegation(fsal_up_event_data_t * pevdata)
{
  cache_inode_status_t cache_status;
  cache_entry_t *pentry = NULL;
  fsal_attrib_list_t attr;
  nfs_client_id_t *clid = NULL;
  rpc_call_channel_t *chan;
  int32_t code = 0;
  nfs_cb_argop4 argop[1];
  rpc_call_t *call;
  struct glist_head  * glist;
  state_lock_entry_t * found_entry = NULL;
  fsal_handle_t *pfsal_handle = NULL;
  char *maxfh;
  compound_data_t data;
  int *tmp;

  maxfh = gsh_malloc(NFS4_FHSIZE);     // free in cb_completion_func()
printf("xxx alloc maxfh %p\n", maxfh);
  if(maxfh == NULL)
    {
      LogDebug(COMPONENT_FSAL_UP,
               "FSAL_UP_DELEG: no mem, failed.");
      /* Not an error. Expecting some nodes will not have it in cache in
       * a cluster. */
      ReturnCode(ERR_FSAL_NO_ERROR, 0);
    }

  memset(&attr, 0, sizeof(fsal_attrib_list_t));

  pentry = cache_inode_get(&pevdata->event_context.fsal_data,
                           &attr,  NULL, NULL, &cache_status);
  if(pentry == NULL)
    {
      LogDebug(COMPONENT_FSAL_UP,
               "FSAL_UP_DELEG: cache inode get failed.");
      /* Not an error. Expecting some nodes will not have it in cache in
       * a cluster. */
      ReturnCode(ERR_FSAL_NO_ERROR, 0);
    }

  LogDebug(COMPONENT_FSAL_UP,
          "FSAL_UP_DELEG: Invalidate cache found entry %p type %u",
          pentry, pentry->type);

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
          ReturnCode(ERR_FSAL_NO_ERROR, 0);
      }
      break;
  }
  pthread_rwlock_unlock(&pentry->state_lock);

  if (found_entry != NULL) {

    code  = nfs_client_id_get_confirmed(found_entry->sle_owner->so_owner.so_nfs4_owner.so_clientid, &clid);
    if (code != CLIENT_ID_SUCCESS) {
        LogCrit(COMPONENT_NFS_CB,
                "No clid record  code %d", code);
        ReturnCode(ERR_FSAL_NO_ERROR, 0);
    }
    chan = nfs_rpc_get_chan(clid, NFS_RPC_FLAG_NONE);
    if (! chan) {
        LogCrit(COMPONENT_NFS_CB, "nfs_rpc_get_chan failed");
        ReturnCode(ERR_FSAL_NO_ERROR, 0);
    }
    if (! chan->clnt) {
        LogCrit(COMPONENT_NFS_CB, "nfs_rpc_get_chan failed (no clnt)");
        ReturnCode(ERR_FSAL_NO_ERROR, 0);
    }

    /* allocate a new call--freed in completion hook */
    call = alloc_rpc_call();
    call->chan = chan;

    /* setup a compound */
    cb_compound_init_v4(&call->cbt, 6, clid->cid_cb.cb_u.v40.cb_callback_ident,
                        "brrring!!!", 10);

    memset(argop, 0, sizeof(nfs_cb_argop4));
    argop->argop = NFS4_OP_CB_RECALL;
    argop->nfs_cb_argop4_u.opcbrecall.stateid.seqid = found_entry->sle_state->state_seqid;
    memcpy(argop->nfs_cb_argop4_u.opcbrecall.stateid.other,
           found_entry->sle_state->stateid_other, OTHERSIZE);
    argop->nfs_cb_argop4_u.opcbrecall.truncate = TRUE;

tmp = (int *)&argop->nfs_cb_argop4_u.opcbrecall.stateid.other;
printf("xxx back1: %08x %08x %08x %08x\n",
 argop->nfs_cb_argop4_u.opcbrecall.stateid.seqid, tmp[0],tmp[1],tmp[2]);

tmp = (int *)&found_entry->sle_state->stateid_other;
printf("xxx back2: %08x %08x %08x %08x\n",
   found_entry->sle_state->state_seqid, tmp[0],tmp[1],tmp[2]);

    pfsal_handle = &found_entry->sle_pentry->handle;

tmp = (int *)pfsal_handle;
printf("xxx back fh: %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
   tmp[0],tmp[1],tmp[2],tmp[3],tmp[4],tmp[5],tmp[6],tmp[7],tmp[8]);
    /* Convert it to a file handle */
    argop->nfs_cb_argop4_u.opcbrecall.fh.nfs_fh4_len = 0;
    argop->nfs_cb_argop4_u.opcbrecall.fh.nfs_fh4_val = maxfh;

    data.pexport = found_entry->sle_pexport;
    if(!nfs4_FSALToFhandle(&argop->nfs_cb_argop4_u.opcbrecall.fh,
                           pfsal_handle, &data))
       ReturnCode(ERR_FSAL_NOENT, 0);

tmp = (int *)argop->nfs_cb_argop4_u.opcbrecall.fh.nfs_fh4_val;
printf("xxx back fh2: len %d\n", argop->nfs_cb_argop4_u.opcbrecall.fh.nfs_fh4_len);
printf("xxx back fh2: %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
   tmp[0],tmp[1],tmp[2],tmp[3],tmp[4],tmp[5],tmp[6],tmp[7],tmp[8]);
    /* add ops, till finished (dont exceed count) */
    cb_compound_add_op(&call->cbt, argop);

    /* set completion hook */
    call->call_hook = cb_completion_func;

    /* call it (here, in current thread context) */
    code = nfs_rpc_submit_call(call,
                               NFS_RPC_FLAG_NONE /* NFS_RPC_CALL_INLINE */);
  }
  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

#define INVALIDATE_STUB {                     \
    return dumb_fsal_up_invalidate_step1(pevdata);  \
  } while(0);

fsal_status_t dumb_fsal_up_create(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_unlink(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_rename(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_commit(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_write(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_link(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_delegation(fsal_up_event_data_t * pevdata)
{
  return fsal_up_delegation(pevdata);
}

fsal_status_t dumb_fsal_up_lock_grant(fsal_up_event_data_t * pevdata)
{
#ifdef _USE_BLOCKING_LOCKS
  cache_inode_status_t   cache_status;
  cache_entry_t        * pentry = NULL;
  fsal_attrib_list_t     attr;

  LogDebug(COMPONENT_FSAL_UP,
           "FSAL_UP_DUMB: calling cache_inode_get()");
  pentry = cache_inode_get(&pevdata->event_context.fsal_data,
                           &attr, NULL, NULL,
                           &cache_status);
  if(pentry == NULL)
    {
      LogDebug(COMPONENT_FSAL_UP,
               "FSAL_UP_DUMB: cache inode get failed.");
      /* Not an error. Expecting some nodes will not have it in cache in
       * a cluster. */
      ReturnCode(ERR_FSAL_NO_ERROR, 0);
    }

  LogDebug(COMPONENT_FSAL_UP,
           "FSAL_UP_DUMB: Lock Grant found entry %p",
           pentry);

  grant_blocked_lock_upcall(pentry,
                            pevdata->type.lock_grant.lock_owner,
                            &pevdata->type.lock_grant.lock_param);


  if(pentry)
    cache_inode_put(pentry);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
#else
  INVALIDATE_STUB;
#endif
}

fsal_status_t dumb_fsal_up_lock_avail(fsal_up_event_data_t * pevdata)
{
#ifdef _USE_BLOCKING_LOCKS
  cache_inode_status_t   cache_status;
  cache_entry_t        * pentry = NULL;
  fsal_attrib_list_t     attr;

  LogDebug(COMPONENT_FSAL_UP,
           "FSAL_UP_DUMB: calling cache_inode_get()");
  pentry = cache_inode_get(&pevdata->event_context.fsal_data,
                           &attr, NULL, NULL, &cache_status);
  if(pentry == NULL)
    {
      LogDebug(COMPONENT_FSAL_UP,
               "FSAL_UP_DUMB: cache inode get failed.");
      /* Not an error. Expecting some nodes will not have it in cache in
       * a cluster. */
      ReturnCode(ERR_FSAL_NO_ERROR, 0);
    }

  LogDebug(COMPONENT_FSAL_UP,
           "FSAL_UP_DUMB: Lock Available found entry %p",
           pentry);

  available_blocked_lock_upcall(pentry,
                                pevdata->type.lock_grant.lock_owner,
                                &pevdata->type.lock_grant.lock_param);

  if(pentry)
    cache_inode_put(pentry);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
#else
  INVALIDATE_STUB;
#endif
}

fsal_status_t dumb_fsal_up_open(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_close(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_status_t dumb_fsal_up_setattr(fsal_up_event_data_t * pevdata)
{
  INVALIDATE_STUB;
}

fsal_up_event_functions_t dumb_event_func = {
  .fsal_up_create = dumb_fsal_up_create,
  .fsal_up_unlink = dumb_fsal_up_unlink,
  .fsal_up_rename = dumb_fsal_up_rename,
  .fsal_up_commit = dumb_fsal_up_commit,
  .fsal_up_write = dumb_fsal_up_write,
  .fsal_up_link = dumb_fsal_up_link,
  .fsal_up_lock_grant = dumb_fsal_up_lock_grant,
  .fsal_up_lock_avail = dumb_fsal_up_lock_avail,
  .fsal_up_open = dumb_fsal_up_open,
  .fsal_up_close = dumb_fsal_up_close,
  .fsal_up_setattr = dumb_fsal_up_setattr,
  .fsal_up_update = dumb_fsal_up_update,
  .fsal_up_invalidate = dumb_fsal_up_invalidate_step1,
  .fsal_up_delegation = dumb_fsal_up_delegation
};

fsal_up_event_functions_t *get_fsal_up_dumb_functions()
{
  return &dumb_event_func;
}
