/*
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */

/**
 *
 * \file    fsal_fsinfo.c
 * \author  $Author: leibovic $
 * \date    $Date: 2006/01/17 14:20:07 $
 * \version $Revision: 1.7 $
 * \brief   functions for retrieving filesystem info.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fsal.h"
#include "fsal_internal.h"
#include "fsal_convert.h"
#include <sys/statvfs.h>

/**
 * FSAL_static_fsinfo:
 * Return static filesystem info such as
 * behavior, configuration, supported operations...
 *
 * \param filehandle (input):
 *        Handle of an object in the filesystem
 *        whom info is to be retrieved.
 * \param cred (input):
 *        Authentication context for the operation (user,...).
 * \param staticinfo (output):
 *        Pointer to the static info of the filesystem.
 *
 * \return Major error codes:
 *      - ERR_FSAL_NO_ERROR: no error.
 *      - ERR_FSAL_FAULT: NULL pointer passed as input parameter.
 *      - ERR_FSAL_SERVERFAULT: Unexpected error.
 */
fsal_status_t LUSTREFSAL_static_fsinfo(lustrefsal_handle_t * p_filehandle,      /* IN */
                                       lustrefsal_op_context_t * p_context,     /* IN */
                                       fsal_staticfsinfo_t * p_staticinfo       /* OUT */
    )
{
  /* sanity checks. */
  if(!p_staticinfo)
    Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_static_fsinfo);

  /* returning static info about the filesystem */
  (*p_staticinfo) = global_fs_info;

  Return(ERR_FSAL_NO_ERROR, 0, INDEX_FSAL_static_fsinfo);

}

/**
 * FSAL_dynamic_fsinfo:
 * Return dynamic filesystem info such as
 * used size, free size, number of objects...
 *
 * \param filehandle (input):
 *        Handle of an object in the filesystem
 *        whom info is to be retrieved.
 * \param cred (input):
 *        Authentication context for the operation (user,...).
 * \param dynamicinfo (output):
 *        Pointer to the static info of the filesystem.
 *
 * \return Major error codes:
 *      - ERR_FSAL_NO_ERROR: no error.
 *      - ERR_FSAL_FAULT: NULL pointer passed as input parameter.
 *      - ERR_FSAL_SERVERFAULT: Unexpected error.
 */
fsal_status_t LUSTREFSAL_dynamic_fsinfo(lustrefsal_handle_t * p_filehandle,     /* IN */
                                        lustrefsal_op_context_t * p_context,    /* IN */
                                        fsal_dynamicfsinfo_t * p_dynamicinfo    /* OUT */
    )
{
  fsal_path_t pathfsal;
  fsal_status_t status;
  struct statvfs buffstatvfs;
  int rc, errsv;
  /* sanity checks. */
  if(!p_filehandle || !p_dynamicinfo || !p_context)
    Return(ERR_FSAL_FAULT, 0, INDEX_FSAL_dynamic_fsinfo);

  status = fsal_internal_Handle2FidPath(p_context, p_filehandle, &pathfsal);
  if(FSAL_IS_ERROR(status))
    Return(status.major, status.minor, INDEX_FSAL_dynamic_fsinfo);

  TakeTokenFSCall();
  rc = statvfs(pathfsal.path, &buffstatvfs);
  errsv = errno;
  ReleaseTokenFSCall();
  if(rc)
    Return(posix2fsal_error(errsv), errsv, INDEX_FSAL_dynamic_fsinfo);

  p_dynamicinfo->total_bytes = buffstatvfs.f_frsize * buffstatvfs.f_blocks;
  p_dynamicinfo->free_bytes = buffstatvfs.f_frsize * buffstatvfs.f_bfree;
  p_dynamicinfo->avail_bytes = buffstatvfs.f_frsize * buffstatvfs.f_bavail;

  p_dynamicinfo->total_files = buffstatvfs.f_files;
  p_dynamicinfo->free_files = buffstatvfs.f_ffree;
  p_dynamicinfo->avail_files = buffstatvfs.f_favail;

  p_dynamicinfo->time_delta.seconds = 1;
  p_dynamicinfo->time_delta.nseconds = 0;

  Return(ERR_FSAL_NO_ERROR, 0, INDEX_FSAL_dynamic_fsinfo);

}
