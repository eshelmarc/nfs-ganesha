/*
 *
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file nfs_fh.h
 * @brief Prototypes for the file handle in v3 and v4
 */

#ifndef NFS_FH_H
#define NFS_FH_H

#include "byteswap.h"

extern uint16_t mixed_endian;

/*
 * Structure of the filehandle
 * these structures must be naturally aligned.  The xdr buffer from/to which
 * they come/go are 4 byte aligned.
 */

#define GANESHA_FH_VERSION 0x41
#define FH3_BIG_ENDIAN          0x80 /*< on if big endian */
#define FH3_LITTLE_ENDIAN       0x40 /*< on if little endian */
#define FH3_LEN_MASK            0x3F /*< fh v3 length mask */

/**
 * @brief An NFSv3 handle
 *
 * This may be up to 64 bytes long, aligned on 32 bits
 */

typedef struct file_handle_v3 {
	uint8_t fhversion;	/*< Set to 0x41 to separate from Linux knfsd */
	uint8_t xattr_pos;	/*< Used for xattr management */
	uint16_t exportid;	/*< Must be correlated to exportlist_t::id */
	uint8_t fs_v3len;	/*< Actual length of opaque handle */
	uint8_t fsopaque[];	/*< Persistent part of FSAL handle,
				    <= 59 bytes */
} file_handle_v3_t;


#define FILE_HANDLE_V4_FLAG_DS 0x0001 /*< handle for a DS */
#define FH_BIG_ENDIAN          0x8080 /*< on if big endian */
#define FH_LITTLE_ENDIAN       0x4040 /*< on if little endian */

/**
 * @brief An NFSv4 filehandle
 *
 * This may be up to 128 bytes, aligned on 32 bits.
 */

typedef struct __attribute__ ((__packed__)) file_handle_v4 {
	uint8_t fhversion;	/*< Set to 0x41 to separate from Linux knfsd */
	union {
		uint16_t exports;	/*< FSAL exports, export_by_id */
		uint16_t servers;	/*< FSAL servers, server_by_id */
	} id;
	uint16_t flags;		/*< To replace things like ds_flag */
	uint8_t fs_len;		/*< Length of opaque handle */
	uint8_t fsopaque[];	/*< FSAL handle */
} file_handle_v4_t;

#endif				/* NFS_FH_H */
