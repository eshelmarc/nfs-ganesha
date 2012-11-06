/*
 * Copyright (C) 2012, The Linux Box Corporation
 * Contributor : Matt Benjamin <matt@linuxbox.com>
 *
 * Some portions Copyright CEA/DAM/DIF  (2008)
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
 * -------------
 */

/**
 * @file nfs_rpc_callback.c
 * @author Matt Benjamin <matt@linuxbox.com>
 * @author Lee Dobryden <lee@linuxbox.com>
 * @author Adam C. Emerson <aemerson@linuxbox.com>
 * @brief RPC callback dispatch package
 *
 * This module implements APIs for submission, and dispatch of NFSv4.0
 * and NFSv4.1 callbacks.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif /* _SOLARIS */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <arpa/inet.h>
#include "nlm_list.h"
#include "fsal.h"
#include "nfs_core.h"
#include "nfs_req_queue.h"
#include "log.h"
#include "nfs_rpc_callback.h"
#include "nfs4.h"
#include "gssd.h"
#include "gss_util.h"
#include "krb5_util.h"
#include "sal_data.h"

/**
 * @brief Pool for allocating callbacks.
 */
static pool_t *rpc_call_pool;

extern char host_name[MAXHOSTNAMELEN];

/**
 * @brief Initialize the callback credential cache
 *
 * @param[in] ccache Location of credential cache
 */

static inline void nfs_rpc_cb_init_ccache(const char *ccache)
{
	int code = 0;

	mkdir(ccache, 700); /* XXX */
	ccachesearch[0] = nfs_param.krb5_param.ccache_dir;

	code = gssd_refresh_krb5_machine_credential(
		host_name, NULL, nfs_param.krb5_param.svc.principal);
	if (code) {
		LogDebug(COMPONENT_INIT,
			 "gssd_refresh_krb5_machine_credential "
			 "failed (%d:%d)", code, errno);
		goto out;
	}

out:
	return;
}

/**
 * @brief Initialize callback subsystem
 */

void nfs_rpc_cb_pkginit(void)
{
	/* Create a pool of rpc_call_t */
	rpc_call_pool = pool_init("RPC Call Pool",
				  sizeof(rpc_call_t),
				  pool_basic_substrate,
				  NULL,
				  nfs_rpc_init_call,
				  NULL);
	if (!(rpc_call_pool)) {
		LogCrit(COMPONENT_INIT,
			"Error while allocating rpc call pool");
		LogError(COMPONENT_INIT, ERR_SYS, ERR_MALLOC, errno);
		Fatal();
	}

	/* ccache */
	nfs_rpc_cb_init_ccache(nfs_param.krb5_param.ccache_dir);

	/* sanity check GSSAPI */
	if (gssd_check_mechs() != 0)
		LogCrit(COMPONENT_INIT,
			"sanity check: gssd_check_mechs() failed");

	return;
}

/**
 * @brief Shutdown callback subsystem
 */
void nfs_rpc_cb_pkgshutdown(void)
{
	return;
}

/**
 * @brief Convert a netid label
 *
 * @todo This is automatically redundant, but in fact upstream TI-RPC is
 * not up-to-date with RFC 5665, will fix (Matt)
 *
 * @copyright 2012, Linux Box Corp
 *
 * @param[in] netid The netid label dictating the protocol
 *
 * @return The numerical protocol identifier.
 */

nc_type nfs_netid_to_nc(const char *netid)
{
	if (!strncmp(netid, netid_nc_table[_NC_TCP].netid,
		     netid_nc_table[_NC_TCP].netid_len))
		return(_NC_TCP);

	if (!strncmp(netid, netid_nc_table[_NC_TCP6].netid,
		     netid_nc_table[_NC_TCP6].netid_len))
		return(_NC_TCP6);

	if (!strncmp(netid, netid_nc_table[_NC_UDP].netid,
		     netid_nc_table[_NC_UDP].netid_len))
		return _NC_UDP;

	if (!strncmp(netid, netid_nc_table[_NC_UDP6].netid,
		     netid_nc_table[_NC_UDP6].netid_len))
		return _NC_UDP6;

	if (!strncmp(netid, netid_nc_table[_NC_RDMA].netid,
		     netid_nc_table[_NC_RDMA].netid_len))
		return _NC_RDMA;

	if (!strncmp(netid, netid_nc_table[_NC_RDMA6].netid,
		     netid_nc_table[_NC_RDMA6].netid_len))
		return _NC_RDMA6;

	if (!strncmp(netid, netid_nc_table[_NC_SCTP].netid,
		     netid_nc_table[_NC_SCTP].netid_len))
		return _NC_SCTP;

	if (!strncmp(netid, netid_nc_table[_NC_SCTP6].netid,
		     netid_nc_table[_NC_SCTP6].netid_len))
		return _NC_SCTP6;

	return _NC_ERR;
}

/**
 * @brief Convert string format address to sockaddr
 *
 * This function takes the host.port format used in the NFSv4.0
 * clientaddr4 and converts it to a POSIX sockaddr structure stored in
 * the callback information of the clientid.
 *
 * @param[in,out] clientid The clientid in which to store the sockaddr
 * @param[in]     uaddr    na_r_addr from the clientaddr4
 */

static inline void setup_client_saddr(nfs_client_id_t *clientid,
				      const char *uaddr)
{
	char addr_buf[SOCK_NAME_MAX];
	uint32_t bytes[11];
	int code;

	assert(clientid->cid_minorversion == 0);

	memset(&clientid->cid_cb.v40.cb_addr.ss, 0,
	       sizeof(struct sockaddr_storage));

	switch (clientid->cid_cb.v40.cb_addr.nc) {
	case _NC_TCP:
	case _NC_RDMA:
	case _NC_SCTP:
	case _NC_UDP:
		/* IPv4 (ws inspired) */
		if (sscanf(uaddr, "%u.%u.%u.%u.%u.%u",
			   &bytes[1], &bytes[2], &bytes[3], &bytes[4],
			   &bytes[5], &bytes[6]) == 6) {
			struct sockaddr_in *sin =
				((struct sockaddr_in *)
				 &clientid->cid_cb.v40.cb_addr.ss);
			snprintf(addr_buf, SOCK_NAME_MAX, "%u.%u.%u.%u",
				 bytes[1], bytes[2],
				 bytes[3], bytes[4]);
			sin->sin_family = AF_INET;
			sin->sin_port = htons((bytes[5]<<8) | bytes[6]);
			code = inet_pton(AF_INET, addr_buf, &sin->sin_addr);
			if (code != 1) {
				LogDebug(COMPONENT_NFS_CB,
					 "inet_pton failed (%d %s)",
					 code, addr_buf);
			} else {
				LogDebug(COMPONENT_NFS_CB,
					 "client callback addr:port %s:%d",
					 addr_buf, ntohs(sin->sin_port));
			}
		}
		break;
	case _NC_TCP6:
	case _NC_RDMA6:
	case _NC_SCTP6:
	case _NC_UDP6:
		/* IPv6 (ws inspired) */
		if (sscanf(uaddr, "%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x.%u.%u",
			   &bytes[1], &bytes[2], &bytes[3],
			   &bytes[4], &bytes[5], &bytes[6],
			   &bytes[7], &bytes[8], &bytes[9],
			   &bytes[10]) == 10) {
			struct sockaddr_in6 *sin6 =
				((struct sockaddr_in6 *)
				 &clientid->cid_cb.v40.cb_addr.ss);
			snprintf(addr_buf, SOCK_NAME_MAX,
				 "%2x:%2x:%2x:%2x:%2x:%2x:%2x:%2x",
				 bytes[1], bytes[2], bytes[3],
				 bytes[4], bytes[5], bytes[6],
				 bytes[7], bytes[8]);
			code = inet_pton(AF_INET6, addr_buf, &sin6->sin6_addr);
			sin6->sin6_port = htons((bytes[9]<<8) | bytes[10]);
			sin6->sin6_family = AF_INET6;
			if (code != 1) {
				LogDebug(COMPONENT_NFS_CB,
					 "inet_pton failed (%d %s)",
					 code, addr_buf);
			} else {
				LogDebug(COMPONENT_NFS_CB,
					 "client callback addr:port %s:%d",
					 addr_buf, ntohs(sin6->sin6_port));
			}
		}
		break;
	default:
		/* unknown netid */
		break;
	};
}

/**
 * @brief Set the callback location for an NFSv4.0 clientid
 *
 * @param[in,out] clientid The clientid in which to set the location
 * @param[in]     addr4    The client's supplied callback address
 */

void nfs_set_client_location(nfs_client_id_t *clientid,
			     const clientaddr4 *addr4)
{
	assert(clientid->cid_minorversion == 0);
	clientid->cid_cb.v40.cb_addr.nc = nfs_netid_to_nc(addr4->r_netid);
	strlcpy(clientid->cid_cb.v40.cb_client_r_addr, addr4->r_addr,
		SOCK_NAME_MAX);
	setup_client_saddr(clientid,
			   clientid->cid_cb.v40.cb_client_r_addr);
}

/**
 * @brief Get the fd of an NFSv4.0 callback connection
 *
 * @param[in]  clientid The clientid to query
 * @param[out] fd       The file descriptor
 * @param[out] proto    The protocol used on this connection
 *
 * @return 0 or values of errno.
 */

static inline int32_t nfs_clid_connected_socket(nfs_client_id_t *clientid,
						int *fd, int *proto)
{
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	int nfd, code = 0;

	assert(clientid->cid_minorversion == 0);

	*fd = 0;
	*proto = -1;

	switch (clientid->cid_cb.v40.cb_addr.ss.ss_family) {
	case AF_INET:
		sin = (struct sockaddr_in *) &clientid->cid_cb.v40.cb_addr.ss;
		switch (clientid->cid_cb.v40.cb_addr.nc) {
		case _NC_TCP:
			nfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
			*proto = IPPROTO_TCP;
			break;
		case _NC_UDP:
			nfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
			*proto = IPPROTO_UDP;
			break;
		default:
			code = EINVAL;
			goto out;
			break;
		}

		code = connect(nfd, (struct sockaddr *) sin,
			       sizeof(struct sockaddr_in));
		if (code == -1) {
			LogDebug(COMPONENT_NFS_CB,
				 "connect fail errno %d",
				 errno);
			goto out;
		}
		*fd = nfd;
		break;
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *) &clientid->cid_cb
			.v40.cb_addr.ss;
		switch (clientid->cid_cb.v40.cb_addr.nc) {
		case _NC_TCP6:
			nfd = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP);
			*proto = IPPROTO_TCP;
			break;
		case _NC_UDP6:
			nfd = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
			*proto = IPPROTO_UDP;
			break;
		default:
			code = EINVAL;
			goto out;
			break;
		}
		code = connect(nfd, (struct sockaddr *) sin6,
			       sizeof(struct sockaddr_in6));
		if (code == -1) {
			LogDebug(COMPONENT_NFS_CB,
				 "connect fail errno %d", errno);
			goto out;
		}
		*fd = nfd;
		break;
	default:
		code = EINVAL;
		break;
	}

out:
	return code;
}

/* end refactorable RPC code */

/**
 * @brief Check if an authentication flavor is supported
 *
 * @param[in] flavor RPC authentication flavor
 *
 * @retval true if supported.
 * @retval false if not.
 */

static inline bool supported_auth_flavor(int flavor)
{
	bool code = false;

	switch (flavor) {
	case RPCSEC_GSS:
	case AUTH_SYS:
	case AUTH_NONE:
		code = true;
		break;
	default:
		break;
	};

	return code;
}

/**
 * @brief Kerberos OID
 *
 * This value comes from kerberos source, gssapi_krb5.c (Umich).
 */

gss_OID_desc krb5oid = {9, "\052\206\110\206\367\022\001\002\002"};

/**
 * @brief Format a principal name for an RPC call channel
 *
 * @param[in]  chan Call channel
 * @param[out] buf  Buffer to hold formatted name
 * @param[in]  len  Size of buffer
 *
 * @return The principle or NULL.
 */

static inline char *format_host_principal(rpc_call_channel_t *chan,
					  char *buf,
					  size_t len)
{
	char addr_buf[SOCK_NAME_MAX];
	const char *host = NULL;
	char *princ = NULL;

	switch (chan->type) {
	case RPC_CHAN_V40:
	{
		nfs_client_id_t *clientid = chan->source.clientid;
		switch (clientid->cid_cb.v40.cb_addr.ss.ss_family) {
		case AF_INET:
		{
			struct sockaddr_in *sin = (struct sockaddr_in *)
				&clientid->cid_cb.v40.cb_addr.ss;
			host = inet_ntop(AF_INET, &sin->sin_addr, addr_buf,
					 INET_ADDRSTRLEN);
			break;
		}
		case AF_INET6:
		{
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)
				&clientid->cid_cb.v40.cb_addr.ss;
			host = inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf,
					 INET6_ADDRSTRLEN);
			break;
		}
		default:
			break;
		}
		break;
	}
	case RPC_CHAN_V41:
		/* XXX implement */
		goto out;
		break;
	}

	if (host) {
		snprintf(buf, len, "nfs@%s", host);
		princ = buf;
	}

out:
	return princ;
}

/**
 * @brief Set up GSS on a callback channel
 *
 * @param[in,out] chan Channel on which to set up GSS
 * @param[in]     cred GSS Credential
 */

static inline void nfs_rpc_callback_setup_gss(rpc_call_channel_t *chan,
					      nfs_client_cred_t *cred)
{
	AUTH *auth;
	char hprinc[MAXPATHLEN];
	int32_t code = 0;

	assert(cred->flavor == RPCSEC_GSS);

	/* MUST RFC 3530bis, section 3.3.3 */
	chan->gss_sec.svc = cred->auth_union.auth_gss.svc;
	chan->gss_sec.qop = cred->auth_union.auth_gss.qop;

	/* the GSSAPI k5 mech needs to find an unexpired credential
	 * for nfs/hostname in an accessible k5ccache */
	code = gssd_refresh_krb5_machine_credential(
		host_name, NULL, nfs_param.krb5_param.svc.principal);
	if (code) {
		LogDebug(COMPONENT_NFS_CB,
			 "gssd_refresh_krb5_machine_credential failed (%d:%d)",
			 code, errno);
		goto out;
	}

	if (!format_host_principal(chan, hprinc, MAXPATHLEN)) {
		LogCrit(COMPONENT_NFS_CB, "format_host_principal failed");
		goto out;
	}

	chan->gss_sec.cred = GSS_C_NO_CREDENTIAL;
	chan->gss_sec.req_flags = 0;

	if (chan->gss_sec.svc != RPCSEC_GSS_SVC_NONE) {
		/* no more lipkey, spkm3 */
		chan->gss_sec.mech = (gss_OID) &krb5oid;
		chan->gss_sec.req_flags = GSS_C_MUTUAL_FLAG; /* XXX */
		auth =
			authgss_create_default(chan->clnt,
					       hprinc,
					       &chan->gss_sec);
		/* authgss_create and authgss_create_default return NULL on
		 * failure, don't assign NULL to clnt->cl_auth */
		if (auth)
			chan->auth = auth;
	}

out:
	return;
}

/**
 * @brief Create a channel for an NFSv4.0 client
 *
 * @param[in] clientid Client record
 * @param[in] flags     Currently unused
 *
 * @return Status code.
 */

int nfs_rpc_create_chan_v40(nfs_client_id_t *clientid,
			    uint32_t flags)
{
	struct netbuf raddr;
	int fd, proto, code = 0;
	rpc_call_channel_t *chan = &clientid->cid_cb.v40.cb_chan;

	assert(!chan->clnt);
	assert(clientid->cid_minorversion == 0);

	/* XXX we MUST error RFC 3530bis, sec. 3.3.3 */
	if (!supported_auth_flavor(clientid->cid_credential.flavor)) {
		code = EINVAL;
		goto out;
	}

	chan->type = RPC_CHAN_V40;
	chan->source.clientid = clientid;

	code = nfs_clid_connected_socket(clientid, &fd, &proto);
	if (code) {
		LogDebug(COMPONENT_NFS_CB,
			 "Failed creating socket");
		goto out;
	}

	raddr.buf = &clientid->cid_cb.v40.cb_addr.ss;

	switch (proto) {
	case IPPROTO_TCP:
		raddr.maxlen = raddr.len = sizeof(struct sockaddr_in);
		chan->clnt = clnt_vc_create(fd,
					    &raddr,
					    clientid->cid_cb.v40.cb_program,
					    1 /* Errata ID: 2291 */,
					    0, 0);
		break;
	case IPPROTO_UDP:
		raddr.maxlen = raddr.len = sizeof(struct sockaddr_in6);
		chan->clnt = clnt_dg_create(fd,
					    &raddr,
					    clientid->cid_cb.v40.cb_program,
					    1 /* Errata ID: 2291 */,
					    0, 0);
		break;
	default:
		break;
	}

	if (!chan->clnt) {
		code = EINVAL;
		goto out;
	}

	/* channel protection */
	switch (clientid->cid_credential.flavor) {
	case RPCSEC_GSS:
		nfs_rpc_callback_setup_gss(chan,
					   &clientid->cid_credential);
		break;
	case AUTH_SYS:
		if (!(chan->auth = authunix_create_default())) {
			code = EINVAL;
		}
		code = 0;
		break;
	case AUTH_NONE:
		break;
	default:
		code = EINVAL;
		break;
	}

out:
	return code;
}

/**
 * @brief Create a channel for an NFSv4.1 session
 *
 * This function creates a channel on an NFSv4.1 session, using the
 * given security parameters.  If a channel already exists, it is
 * removed and replaced.
 *
 * @param[in,out] session       The session on which to create the
 *                              back channel
 * @parma[in]     num_sec_parms Length of sec_parms list
 * @param[in]     sec_parms     Allowable security parameters
 *
 * @return 0 or POSIX error code.
 */

int nfs_rpc_create_chan_v41(nfs41_session_t *session,
			    int num_sec_parms,
			    callback_sec_parms4 *sec_parms)

{
	int code = 0;
	rpc_call_channel_t *chan = &session->cb_chan;
	int i = 0;
	bool authed = false;
	struct timeval cb_timeout = {15, 0};

	if (chan->clnt) {
		/* Something better later. */
		return EEXIST;
	}

	chan->type = RPC_CHAN_V41;
	chan->source.session = session;

	assert(session->xprt);

	/* connect an RPC client */
	chan->clnt = clnt_vc_create_svc(session->xprt,
					session->cb_program,
					4, SVC_VC_CREATE_BOTHWAYS);

	if (!chan->clnt) {
		code = EINVAL;
		goto out;
	}


	for (i = 0; i < num_sec_parms; ++i) {
		if (sec_parms[i].cb_secflavor == AUTH_NONE) {
			authed = true;
			break;
		} else if (sec_parms[i].cb_secflavor == AUTH_SYS) {
			struct authunix_parms *sys_parms =
				&sec_parms[i].callback_sec_parms4_u
				.cbsp_sys_cred;
			if (!(chan->auth
			      = authunix_create(sys_parms->aup_machname,
						sys_parms->aup_uid,
						sys_parms->aup_gid,
						sys_parms->aup_len,
						sys_parms->aup_gids))) {
				continue;
			}
			authed = true;
			break;
		} else if (sec_parms[i].cb_secflavor == RPCSEC_GSS) {

			/**
			 * @todo ACE: Come back later and implement
			 * GSS.
			 */
			continue;
		} else  {
			LogMajor(COMPONENT_NFS_CB,
				 "Client sent unknown auth type.");
			continue;
		}
	}

	if (!authed) {
		code = EPERM;
		LogMajor(COMPONENT_NFS_CB,
			 "No working auth in sec_params.");
		goto out;
	}

	if (rpc_cb_null(chan,
			cb_timeout) != RPC_SUCCESS) {
		code = EBADFD;
		goto out;
	}

	session->cb_chan_up = true;
	code = 0;

out:
	if ((code != 0) &&
	    chan->clnt) {
		nfs_rpc_destroy_chan(chan);
	}

	return code;
}

/**
 * @brief Get a backchannel for a clientid
 *
 * This function works for both NFSv4.0 and NFSv4.1.  For NFSv4.0, if
 * the channel isn't up, it tries to create it.
 *
 * @param[in,out] clientid The clientid to use
 * @param[out]    flags    Unused
 *
 * @return The back channel or NULL if none existed or could be
 *         established.
 */

rpc_call_channel_t *nfs_rpc_get_chan(nfs_client_id_t *clientid,
				     uint32_t flags)
{
	rpc_call_channel_t *chan = NULL;

	if (clientid->cid_minorversion == 0) {
		chan = &clientid->cid_cb.v40.cb_chan;
		if (!chan->clnt) {
			if (nfs_rpc_create_chan_v40(clientid,
						    flags) == 0) {
			}
		}
		} else { /* 1 and higher */
		struct glist_head *glist = NULL;

		/* Get the first working back channel we have */
		glist_for_each(glist,
			       &clientid->cid_cb.v41.cb_session_list) {
			nfs41_session_t *session =
				glist_entry(glist,
					    nfs41_session_t,
					    session_link);
			if (session->cb_chan_up) {
				chan = &session->cb_chan;
			}
		}
	}

	return chan;
}

/**
 * @brief Dispose of a channel
 *
 * @param[in] chan The channel to dispose of
 */

void nfs_rpc_destroy_chan(rpc_call_channel_t *chan)
{
	assert(chan);

	/* XXX lock, wait for outstanding calls, etc */

	switch (chan->type) {
	case RPC_CHAN_V40:
		/* channel has a dedicated RPC client */
		if (chan->clnt) {
			/* clean up auth, if any */
			if (chan->auth) {
				AUTH_DESTROY(chan->auth);
				chan->auth = NULL;
			}
			/* destroy it */
			clnt_destroy(chan->clnt);
		}
		break;
	case RPC_CHAN_V41:
		if (chan->auth) {
			AUTH_DESTROY(chan->auth);
			chan->auth = NULL;
		}
		chan->clnt->cl_ops->cl_release(chan->clnt);

		break;
	}

	chan->clnt = NULL;
	chan->last_called = 0;
}

/**
 * Call the NFSv4 client's CB_NULL procedure.
 *
 * @param[in] chan    Channel on which to call
 * @param[in] timeout Timeout for client call
 *
 * @return Client status.
 */

enum clnt_stat rpc_cb_null(rpc_call_channel_t *chan,
			   struct timeval timeout)
{
	enum clnt_stat stat = RPC_SUCCESS;

	/* XXX TI-RPC does the signal masking */
	pthread_mutex_lock(&chan->mtx);

	if (!chan->clnt) {
		stat = RPC_INTR;
		goto unlock;
	}

	stat = clnt_call(chan->clnt, chan->auth, CB_NULL,
			 (xdrproc_t) xdr_void, NULL,
			 (xdrproc_t) xdr_void, NULL, timeout);

	/* If a call fails, we have to assume path down, or equally fatal
	 * error.  We may need back-off. */
	if (stat != RPC_SUCCESS) {
		if (chan->clnt) {
			if (chan->auth) {
				AUTH_DESTROY(chan->auth);
				chan->auth = NULL;
			}
			clnt_destroy(chan->clnt);
			chan->clnt = NULL;
		}
	}

unlock:
	pthread_mutex_unlock(&chan->mtx);

	return stat;
}

/**
 * @brief Free callback arguments
 *
 * @param[in] op The argop to free
 */

static inline void free_argop(nfs_cb_argop4 *op)
{
	gsh_free(op);
}

/**
 * @brief Free callback result
 *
 * @param[in] op The resop to free
 */

static inline void free_resop(nfs_cb_resop4 *op)
{
	gsh_free(op);
}

/**
 * @brief Allocate an RPC call
 *
 * @return The newly allocated call or NULL.
 */

rpc_call_t *alloc_rpc_call(void)
{
	rpc_call_t *call;

	call = pool_alloc(rpc_call_pool, NULL);

	return call;
}

/**
 * @brief Fre an RPC call
 *
 * @param[in] call The call to free
 */

void free_rpc_call(rpc_call_t *call)
{
	free_argop(call->cbt.v_u.v4.args.argarray.argarray_val);
	free_resop(call->cbt.v_u.v4.res.resarray.resarray_val);
	pool_free(rpc_call_pool, call);
}

/**
 * @brief Completion hook
 *
 * If a call has been supplied to handle the result, call the supplied
 * hook. Otherwise, a no-op.
 *
 * @param[in] call  The RPC call
 * @param[in] hook  The call hook
 * @param[in] arg   Supplied arguments
 * @param[in] flags Any flags
 */

static inline void RPC_CALL_HOOK(rpc_call_t *call, rpc_call_hook hook,
				 void* arg, uint32_t flags)
{
	if (call) {
	    call->call_hook(call, hook, arg, flags);
	}
}

/**
 * @brief Fire off an RPC call
 *
 * @param[in] call  The constructed call
 * @param[in] flags Control flags for call
 *
 * @return 0 or POSIX error codes.
 */

int32_t nfs_rpc_submit_call(rpc_call_t *call,
			    uint32_t flags)
{
	int32_t code = 0;
	request_data_t *nfsreq = NULL;
	rpc_call_channel_t *chan = call->chan;

	assert(chan);

	if (flags & NFS_RPC_CALL_INLINE) {
		code = nfs_rpc_dispatch_call(call, NFS_RPC_CALL_NONE);
	} else {
		nfsreq = nfs_rpc_get_nfsreq(0 /* flags */);
		pthread_mutex_lock(&call->we.mtx);
		call->states = NFS_CB_CALL_QUEUED;
		nfsreq->rtype = NFS_CALL;
		nfsreq->r_u.call = call;
		nfs_rpc_enqueue_req(nfsreq);
		pthread_mutex_unlock(&call->we.mtx);
	}

	return code;
}

/**
 * @brief Dispatch a call
 *
 * @param[in,out] call  The call to dispatch
 * @param[in]     flags Flags governing call
 *
 * @return 0 or POSIX errors.
 */

int32_t nfs_rpc_dispatch_call(rpc_call_t *call, uint32_t flags)
{
	int code = 0;
	struct timeval CB_TIMEOUT = {15, 0}; /* XXX */

	/* send the call, set states, wake waiters, etc */
	pthread_mutex_lock(&call->we.mtx);

	switch (call->states) {
	case NFS_CB_CALL_DISPATCH:
	case NFS_CB_CALL_FINISHED:
		/* XXX invalid entry states for nfs_rpc_dispatch_call */
		abort();
	}

	call->states = NFS_CB_CALL_DISPATCH;
	pthread_mutex_unlock(&call->we.mtx);

	/* XXX TI-RPC does the signal masking */
	pthread_mutex_lock(&call->chan->mtx);

	if (!call->chan->clnt) {
		call->stat = RPC_INTR;
		goto unlock;
	}

	call->stat = clnt_call(call->chan->clnt,
			       call->chan->auth,
			       CB_COMPOUND,
			       (xdrproc_t) xdr_CB_COMPOUND4args,
			       &call->cbt.v_u.v4.args,
			       (xdrproc_t) xdr_CB_COMPOUND4res,
			       &call->cbt.v_u.v4.res,
			       CB_TIMEOUT);

	/* If a call fails, we have to assume path down, or equally fatal
	 * error.  We may need back-off. */
	if (call->stat != RPC_SUCCESS) {
		if (call->chan->clnt) {
			if (call->chan->auth) {
				AUTH_DESTROY(call->chan->auth);
				call->chan->auth = NULL;
			}
			clnt_destroy(call->chan->clnt);
			call->chan->clnt = NULL;
		}
	}

unlock:
	pthread_mutex_unlock(&call->chan->mtx);

	/* signal waiter(s) */
	pthread_mutex_lock(&call->we.mtx);
	call->states |= NFS_CB_CALL_FINISHED;

	/* broadcast will generally be inexpensive */
	if (call->flags & NFS_RPC_CALL_BROADCAST)
		pthread_cond_broadcast(&call->we.cv);
	pthread_mutex_unlock(&call->we.mtx);

	/* call completion hook */
	RPC_CALL_HOOK(call, RPC_CALL_COMPLETE, NULL, NFS_RPC_CALL_NONE);

	return code;
}

/**
 * @brief Abort a call
 *
 * @param[in] call The call to abort
 *
 * @todo function doesn't seem to do anything.
 *
 * @return But it does it successfully.
 */

int32_t nfs_rpc_abort_call(rpc_call_t *call)
{
	return 0;
}
