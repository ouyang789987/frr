/*	$OpenBSD$ */

/*
 * Copyright (c) 2017 Rafael Zalamena <rzalamena at opensourcerouting.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <zebra.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>

#include <errno.h>

#include "ldpd.h"
#include "ldpe.h"
#include "lde.h"
#include "log.h"
#include "ldp_vty.h"
#include "ldp_debug.h"

#include "lib/json.h"
#include "lib/openbsd-queue.h"

int ldpd_afx_if_parse(int, struct json_object *, struct ldpd_conf *);
int ldpd_afx_addr_parse(int, struct json_object *, struct ldpd_conf *);
int ldpd_afx_parse(int, struct json_object *, struct ldpd_conf *);
int ldpd_af_parse(struct json_object *, struct ldpd_conf *);
int ldpd_nb_parse(struct json_object *, struct ldpd_conf *);
int ldpd_global_parse(struct json_object *, struct ldpd_conf *);
int json2ldpdconf(const char *, struct ldpd_conf *);

int ldpd_afx_if_parse(int af, struct json_object *jo, struct ldpd_conf *conf)
{
	struct json_object_iterator joi, join;
	const char *key, *sval;
	struct json_object *jo_val;
	uint64_t lval;
	struct iface *iface;
	struct iface_af *ia;
	int error = 0;

	if (json_object_object_get_ex(jo, "name", &jo_val) != 1) {
		log_warnx("\t\t\t\tfailed to find interface name");
		return -1;
	}

	sval = json_object_get_string(jo_val);
	iface = if_lookup_name(conf, sval);
	if (iface == NULL) {
		iface = if_new(sval);
		RB_INSERT(iface_head, &conf->iface_tree, iface);
	}
	ia = iface_af_get(iface, af);
	ia->enabled = 1;

	log_debug("\t\t\t\tname: %s", sval);

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strmatch(key, "link-hello-holdtime")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"link-hello-holdtime");
				continue;
			}

			ia->hello_holdtime = lval;
			log_debug("\t\t\t\t\tlink-hello-holdtime: %lu", lval);
			continue;
		} else if (strmatch(key, "link-hello-interval")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"link-hello-interval");
				continue;
			}

			ia->hello_interval = lval;
			log_debug("\t\t\t\t\tlink-hello-interval: %lu", lval);
			continue;
		} else {
			/* Handled outside the loop. */
			if (strmatch(key, "name"))
				continue;

			sval = json_object_get_string(jo_val);
			error++;
			log_warnx("\t\t\t\t(unhandled) %s: %s", key, sval);
		}
	}

	return error;
}

int ldpd_afx_addr_parse(int af, struct json_object *jo, struct ldpd_conf *conf)
{
	struct json_object *jo_val;
	const char *sval;
	union ldpd_addr	addr;
	struct tnbr *tnbr;

	if (json_object_object_get_ex(jo, "address", &jo_val) != 1) {
		log_warnx("\t\t\t\tfailed to find neighbor address");
		return -1;
	}

	sval = json_object_get_string(jo_val);
	if (inet_pton(af, sval, &addr) != 1) {
		log_warnx("\t\t\t\tfailed to convert address: %s", sval);
		return -1;
	}

	tnbr = tnbr_find(conf, af, &addr);
	if (tnbr == NULL) {
		tnbr = tnbr_new(af, &addr);
		tnbr->flags |= F_TNBR_CONFIGURED;
		RB_INSERT(tnbr_head, &conf->tnbr_tree, tnbr);
	}

	log_debug("\t\t\t\taddress: %s", sval);
	return 0;
}

int ldpd_afx_parse(int af, struct json_object *jo, struct ldpd_conf *conf)
{
	struct json_object_iterator joi, join;
	const char *key, *sval;
	struct json_object *jo_val, *jo_idx;
	struct ldpd_af_conf *afconf;
	uint32_t ival;
	uint64_t lval;
	int allen, idx;
	int error = 0;

	afconf = (af == AF_INET) ? &conf->ipv4 : &conf->ipv6;
	SET_FLAG(afconf->flags, F_LDPD_AF_ENABLED);

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strmatch(key, "gtsm")) {
			ival = json_object_get_boolean(jo_val);
			if (ival)
				UNSET_FLAG(afconf->flags, F_LDPD_AF_NO_GTSM);
			else
				SET_FLAG(afconf->flags, F_LDPD_AF_NO_GTSM);
			log_debug("\t\t\tgtsm: %s", ival ? "true" : "false");
			continue;
		} else if (strmatch(key, "explicit-null")) {
			ival = json_object_get_boolean(jo_val);
			if (ival)
				SET_FLAG(afconf->flags, F_LDPD_AF_EXPNULL);
			else
				UNSET_FLAG(afconf->flags, F_LDPD_AF_EXPNULL);
			log_debug("\t\t\texplicit-null: %s",
				  ival ? "true" : "false");
			continue;
		} else if (strmatch(key, "keepalive")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"keepalive");
				continue;
			}

			afconf->keepalive = lval;
			log_debug("\t\t\tkeepalive: %lu", lval);
			continue;
		} else if (strmatch(key, "link-hello-holdtime")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"link-hello-holdtime");
				continue;
			}

			afconf->lhello_holdtime = lval;
			log_debug("\t\t\tlink-hello-holdtime: %lu", lval);
			continue;
		} else if (strmatch(key, "link-hello-interval")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"link-hello-interval");
				continue;
			}

			afconf->lhello_interval = lval;
			log_debug("\t\t\tlink-hello-interval: %lu", lval);
			continue;
		} else if (strmatch(key, "targeted-hello-holdtime")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"targeted-hello-holdtime");
				continue;
			}

			afconf->thello_holdtime = lval;
			log_debug("\t\t\ttargeted-hello-holdtime: %lu", lval);
			continue;
		} else if (strmatch(key, "targeted-hello-interval")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"targeted-hello-interval");
				continue;
			}

			afconf->thello_interval = lval;
			log_debug("\t\t\ttargeted-hello-interval: %lu", lval);
			continue;
		} else if (strmatch(key, "targeted-hello-accept")) {
			ival = json_object_get_boolean(jo_val);
			if (ival)
				SET_FLAG(afconf->flags,
					 F_LDPD_AF_THELLO_ACCEPT);
			else
				UNSET_FLAG(afconf->flags,
					   F_LDPD_AF_THELLO_ACCEPT);
			log_debug("\t\t\ttargeted-hello-accept: %s",
				  ival ? "true" : "false");
			continue;
		} else if (strmatch(key, "transport-address")) {
			sval = json_object_get_string(jo_val);
			if (inet_pton(af, sval, &afconf->trans_addr)
			    != 1) {
				log_warnx(
					"failed to convert transport-address"
					": %s",
					sval);
				error++;
			}
			log_debug("\t\t\ttransport-address: %s", sval);
			continue;
		} else if (strmatch(key, "targeted-neighbors")) {
			allen = json_object_array_length(jo_val);
			log_debug("\t\t\ttargeted-neighbors (%d):", allen);
			for (idx = 0; idx < allen; idx++) {
				jo_idx = json_object_array_get_idx(jo_val, idx);
				error +=
					ldpd_afx_addr_parse(af, jo_idx, conf);
			}
			continue;
		} else if (strmatch(key, "interfaces")) {
			allen = json_object_array_length(jo_val);
			log_debug("\t\t\tinterfaces (%d):", allen);
			for (idx = 0; idx < allen; idx++) {
				jo_idx = json_object_array_get_idx(jo_val, idx);
				error += ldpd_afx_if_parse(af, jo_idx, conf);
			}
			continue;
		} else {
			sval = json_object_get_string(jo_val);
			error++;
			log_warnx("\t\t\t(unhandled) %s: %s", key, sval);
		}
	}

	return error;
}

int ldpd_af_parse(struct json_object *jo, struct ldpd_conf *conf)
{
	struct json_object_iterator joi, join;
	const char *key, *sval;
	struct json_object *jo_val;
	int error = 0;

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strmatch(key, "ipv4")) {
			log_debug("\t\tipv4:");
			error += ldpd_afx_parse(AF_INET, jo_val, conf);
			continue;
		} else if (strmatch(key, "ipv6")) {
			log_debug("\t\tipv6:");
			error += ldpd_afx_parse(AF_INET6, jo_val, conf);
			continue;
		} else {
			error++;
			sval = json_object_get_string(jo_val);
			log_warnx("\t\t(unhandled) %s: %s", key, sval);
		}
	}

	return error;
}

int ldpd_nb_parse(struct json_object *jo, struct ldpd_conf *conf)
{
	struct json_object_iterator joi, join;
	const char *key, *sval;
	struct json_object *jo_val;
	struct nbr_params *nbrp;
	struct in_addr in;
	uint32_t ival;
	uint64_t lval;
	int error = 0;

	/*
	 * We need to get LSR-ID first to know where to store the
	 * configuration.
	 */
	if (json_object_object_get_ex(jo, "lsr-id", &jo_val) != 1) {
		log_warnx("\tfailed to find neighbor lsr-id");
		return -1;
	}

	sval = json_object_get_string(jo_val);
	if (inet_pton(AF_INET, sval, &in) != 1) {
		log_warn(
			"failed to convert "
			"lsr-id");
		return -1;
	}

	nbrp = nbr_params_find(conf, in);
	if (nbrp == NULL) {
		nbrp = nbr_params_new(in);
		RB_INSERT(nbrp_head, &conf->nbrp_tree, nbrp);
	}

	log_debug("\t\tlsr-id: %s", sval);

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strmatch(key, "gtsm")) {
			ival = json_object_get_boolean(jo_val);
			nbrp->gtsm_enabled = ival;
			SET_FLAG(nbrp->flags, F_NBRP_GTSM);
			log_debug("\t\tgtsm: %s", ival ? "true" : "false");
			continue;
		} else if (strmatch(key, "gtsm-hops")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"gtsm-hops");
				continue;
			}

			nbrp->gtsm_hops = lval;
			SET_FLAG(nbrp->flags, F_NBRP_GTSM_HOPS);
			log_debug("\t\tgtsm-hops: %lu", lval);
			continue;
		} else if (strmatch(key, "keepalive")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"keepalive");
				continue;
			}

			nbrp->keepalive = lval;
			SET_FLAG(nbrp->flags, F_NBRP_KEEPALIVE);
			log_debug("\t\tkeepalive: %lu", lval);
			continue;
		} else if (strmatch(key, "password")) {
			sval = json_object_get_string(jo_val);
			nbrp->auth.md5key_len =
				strlcpy(nbrp->auth.md5key, sval,
					sizeof(nbrp->auth.md5key));
			nbrp->auth.method = AUTH_MD5SIG;
			log_debug("\t\tpassword: %s", nbrp->auth.md5key);
			continue;
		} else {
			/* Handled outside the loop. */
			if (strmatch(key, "lsr-id"))
				continue;

			sval = json_object_get_string(jo_val);
			log_warnx("\t\t(unhandled) %s: %s", key, sval);
			error++;
		}
	}

	return error;
}

int ldpd_global_parse(struct json_object *jo, struct ldpd_conf *conf)
{
	struct json_object_iterator joi, join;
	const char *key, *sval;
	struct json_object *jo_val, *jo_idx;
	uint32_t ival;
	uint64_t lval;
	int allen, idx;
	int error = 0;

	SET_FLAG(conf->flags, F_LDPD_ENABLED);

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strmatch(key, "dual-stack-cisco-interop")) {
			ival = json_object_get_boolean(jo_val);
			if (ival)
				SET_FLAG(conf->flags, F_LDPD_DS_CISCO_INTEROP);
			else
				UNSET_FLAG(conf->flags,
					   F_LDPD_DS_CISCO_INTEROP);
			log_debug("\tdual-stack-cisco-interop: %s",
				  ival ? "true" : "false");
			continue;
		} else if (strmatch(key, "link-hello-holdtime")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"link-hello-holdtime");
				continue;
			}

			conf->lhello_holdtime = lval;
			log_debug("\tlink-hello-holdtime: %lu", lval);
			continue;
		} else if (strmatch(key, "link-hello-interval")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"link-hello-interval");
				continue;
			}

			conf->lhello_interval = lval;
			log_debug("\tlink-hello-interval: %lu", lval);
			continue;
		} else if (strmatch(key, "targeted-hello-holdtime")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"targeted-hello-holdtime");
				continue;
			}

			conf->thello_holdtime = lval;
			log_debug("\ttargeted-hello-holdtime: %lu", lval);
			continue;
		} else if (strmatch(key, "targeted-hello-interval")) {
			errno = 0;
			lval = json_object_get_int64(jo_val);
			if (lval == 0 && errno != 0) {
				error++;
				log_warn(
					"failed to convert "
					"targeted-hello-interval");
				continue;
			}

			conf->thello_interval = lval;
			log_debug("\ttargeted-hello-interval: %lu", lval);
			continue;
		} else if (strmatch(key, "router-id")) {
			sval = json_object_get_string(jo_val);
			if (inet_pton(AF_INET, sval, &conf->rtr_id) != 1) {
				error++;
				log_warnx(
					"failed to convert router-id"
					": %s",
					sval);
				continue;
			}
			log_debug("\trouter-id: %s", sval);
			continue;
		} else if (strmatch(key, "transport-preference")) {
			sval = json_object_get_string(jo_val);
			conf->trans_pref = strmatch(key, "ipv6")
						   ? DUAL_STACK_LDPOV6
						   : DUAL_STACK_LDPOV4;
			log_debug("\ttransport-preference: %s", sval);
			continue;
		} else if (strmatch(key, "address-families")) {
			log_debug("\taddress-families:");
			error += ldpd_af_parse(jo_val, conf);
			continue;
		} else if (strmatch(key, "neighbors")) {
			allen = json_object_array_length(jo_val);
			log_debug("\tneighbors (%d):", allen);
			for (idx = 0; idx < allen; idx++) {
				jo_idx = json_object_array_get_idx(jo_val, idx);
				error += ldpd_nb_parse(jo_idx, conf);
			}
			continue;
		} else {
			sval = json_object_get_string(jo_val);
			log_warnx("\t(unhandled) %s: %s", key, sval);
			error++;
		}
	}

	return error;
}

int json2ldpdconf(const char *json, struct ldpd_conf *conf)
{
	struct json_object_iterator joi, join;
	const char *key;
	struct json_object *jo, *jo_val;
	int error = 0;

	jo = json_tokener_parse(json);
	if (jo == NULL)
		return -1;

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strmatch(key, "ldp-process")) {
			error += ldpd_global_parse(jo_val, conf);
			continue;
		}
		if (strmatch(key, "l2vpns")) {
			/* TODO l2vpns */
			continue;
		}
	}

	return 0;
}

/* json socket part */

#define LJC_MAX_SIZE 67107840

struct ldpd_json_conn;
TAILQ_HEAD(ljc_head, ldpd_json_conn);

struct ldpd_json_conn {
	TAILQ_ENTRY(ldpd_json_conn) entry;
	struct ljc_head *ljch;
	struct thread *t;
	int sd;
	struct ibuf *ibuf;
};

struct ldpd_json_ctx {
	struct thread *t;
	int sd;
	struct ljc_head ljclist;
};

void ljc_free(struct ldpd_json_conn *);
int ldpd_json_read(struct thread *);
int ldpd_json_accept(struct thread *);


void ljc_free(struct ldpd_json_conn *ljc)
{
	ibuf_free(ljc->ibuf);
	THREAD_READ_OFF(ljc->t);
	close(ljc->sd);
	TAILQ_REMOVE(ljc->ljch, ljc, entry);
}

int ldpd_json_read(struct thread *t)
{
	struct ldpd_json_conn *ljc = THREAD_ARG(t);
	ssize_t tread;
	int nread;
	char *buf;
	struct ldpd_conf *conf;

	ljc->t = NULL;
	thread_add_read(master, ldpd_json_read, ljc, ljc->sd, &ljc->t);

	conf = config_new_empty();
	if (conf == NULL) {
		log_warnx("%s: config_new_empty", __FUNCTION__);
		return -1;
	}

	if (ioctl(ljc->sd, FIONREAD, &nread) == -1) {
		log_warn("%s: ioctl(FIONREAD): %s", __FUNCTION__,
			 strerror(errno));
		return -1;
	}

	if (nread == 0) {
		ljc_free(ljc);
		return -1;
	}

	log_debug("%s: expecting %d bytes", __FUNCTION__, nread);

	buf = ibuf_reserve(ljc->ibuf, nread + 1);
	if (buf == NULL) {
		log_warn("%s: ibuf_reserve: %s", __FUNCTION__, strerror(errno));
		return -1;
	}

	tread = recv(ljc->sd, buf, nread, MSG_DONTWAIT);
	while (tread < nread) {
		if (tread <= 0) {
			log_warn("%s: socket closed", __FUNCTION__);
			ljc_free(ljc);
			return -1;
		}
		buf += tread;
		nread -= tread;
		tread = recv(ljc->sd, buf, nread, MSG_DONTWAIT);
	}

	if (json2ldpdconf((char *)ljc->ibuf->buf, conf) == -1) {
		/* TODO improve this */
		log_warnx("%s: configuration incomplete or wrong",
			  __FUNCTION__);
		ljc_free(ljc);
		return -1;
	}

	ldp_config_apply(NULL, conf);

	ibuf_free(ljc->ibuf);

	ljc->ibuf = ibuf_dynamic(65535, LJC_MAX_SIZE);
	if (ljc->ibuf == NULL) {
		ljc_free(ljc);
		return -1;
	}

	return 0;
}

int ldpd_json_accept(struct thread *t)
{
	struct ldpd_json_ctx *ljctx = THREAD_ARG(t);
	struct ldpd_json_conn *ljc;
	int sd;
	struct sockaddr_in sin;
	socklen_t slen;

	ljctx->t = NULL;
	thread_add_read(master, ldpd_json_accept, ljctx, ljctx->sd, &ljctx->t);

	slen = sizeof(sin);
	sd = accept(ljctx->sd, &sin, &slen);
	if (sd == -1) {
		log_warn("failed to allocate ldpd json context");
		return -1;
	}

#if 1
	{
		char buf[256];
		inet_ntop(AF_INET, &sin.sin_addr, buf, sizeof(buf));
		log_debug("<- %s", buf);
	}
#endif

	ljc = calloc(1, sizeof(*ljc));
	if (ljc == NULL) {
		log_warn("failed to allocate ldpd json context");
		close(sd);
		return -1;
	}

	ljc->ibuf = ibuf_dynamic(65535, LJC_MAX_SIZE);
	if (ljc->ibuf == NULL) {
		log_warn("failed to allocate ldpd json context");
		close(sd);
		free(ljc);
		return -1;
	}

	ljc->sd = sd;
	ljc->ljch = &ljctx->ljclist;
	TAILQ_INSERT_HEAD(&ljctx->ljclist, ljc, entry);

	thread_add_read(master, ldpd_json_read, ljc, sd, &ljc->t);

	return 0;
}

int ldpd_json_init(void)
{
	int sd;
	struct ldpd_json_ctx *ljctx;
	struct sockaddr_in sin;

	ljctx = calloc(1, sizeof(*ljctx));
	if (ljctx == NULL) {
		log_warn("failed to allocate ldpd json context");
		return -1;
	}
	TAILQ_INIT(&ljctx->ljclist);

	sd = socket(AF_INET, SOCK_STREAM, PF_UNSPEC);
	if (sd == -1) {
		log_warn("failed to open json socket");
		return -1;
	}

	sockopt_reuseaddr(sd);
	sockopt_reuseport(sd);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(12345);
	if (bind(sd, &sin, sizeof(sin)) == -1) {
		log_warn("failed to open json socket");
		return -1;
	}

	if (listen(sd, SOMAXCONN) == -1) {
		log_warn("failed to open json socket");
		return -1;
	}

	ljctx->sd = sd;
	thread_add_read(master, ldpd_json_accept, ljctx, sd, &ljctx->t);

	return 0;
}
