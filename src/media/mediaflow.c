/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <re.h>
#include <rew.h>
#include "avs_log.h"
#include "avs_version.h"
#include "avs_aucodec.h"
#include "avs_dce.h"
#include "avs_uuid.h"
#include "avs_zapi.h"
#include "avs_turn.h"
#include "avs_media.h"
#include "avs_vidcodec.h"
#include "avs_network.h"
#include "priv_mediaflow.h"
#include "avs_mediastats.h"

#ifdef __APPLE__
#       include "TargetConditionals.h"
#endif


#define MAGIC 0xed1af100
#define MAGIC_CHECK(s) \
	if (MAGIC != s->magic) {                                        \
		warning("%s: wrong magic struct=%p (magic=0x%08x)\n",   \
			__REFUNC__, s, s->magic);			\
		BREAKPOINT;                                             \
	}


enum {
	RTP_TIMEOUT_MS = 20000,
	DTLS_MTU       = 1480,
	SSRC_MAX       = 4,
	ICE_INTERVAL   = 50,    /* milliseconds */
	PORT_DISCARD   = 9,     /* draft-ietf-ice-trickle-05 */
};

enum {
	AUDIO_BANDWIDTH = 50,   /* kilobits/second */
	VIDEO_BANDWIDTH = 800,  /* kilobits/second */
};


enum sdp_state {
	SDP_IDLE = 0,
	SDP_GOFF,
	SDP_HOFF,
	SDP_DONE
};

enum {
	MQ_ERR = 0,
	MQ_RTP_START = 1,
};

struct interface {
	struct le le;

	const struct mediaflow *mf;     /* pointer to parent */
	const struct ice_lcand *lcand;  /* pointer */
	struct sa addr;
	char ifname[64];
	bool is_default;
};


struct mediaflow {

	struct mqueue *mq;

	/* common stuff */
	struct sa laddr_default;
	char tag[32];
	bool terminated;
	int af;
	int err;

	/* RTP/RTCP */
	struct udp_sock *rtp;
	struct rtp_stats audio_stats_rcv;
	struct rtp_stats audio_stats_snd;
	struct rtp_stats video_stats_rcv;
	struct rtp_stats video_stats_snd;
	struct aucodec_stats codec_stats;

	struct tmr tmr_rtp;
	bool external_rtp;
	bool enable_rtcp;
	uint32_t lssrcv[MEDIA_NUM];
	char cname[16];             /* common for audio+video */
	char msid[36];
	char *label;

	/* SDP */
	struct sdp_session *sdp;
	struct sdp_media *sdpm;
	bool sdp_offerer;
	bool got_sdp;
	bool sent_sdp;
	enum sdp_state sdp_state;
	char sdp_rtool[64];

	/* ice: */
	enum mediaflow_nat nat;

	/* union: { */

	struct tmr tmr_nat;

	struct trice *trice;
	struct stun *trice_stun;
	struct udp_helper *trice_uh;
	struct ice_candpair *sel_pair;    /* chosen candidate-pair */
	struct udp_sock *us_stun;
	struct list turnconnl;
	struct tmr tmr_error;

	/* } */

	uint64_t ice_tiebrk;
	char ice_ufrag[16];
	char ice_pwd[32];
	bool ice_ready;
	char *peer_software;
	uint64_t ts_nat_start;

	/* ice - gathering */
	struct stun_ctrans *ct_gather;
	bool ice_local_eoc;
	bool ice_remote_eoc;
	bool stun_server;
	bool stun_ok;

	/* crypto: */
	enum media_crypto cryptos_local;
	enum media_crypto cryptos_remote;
	enum media_crypto crypto;          /* negotiated crypto */
	enum media_crypto crypto_fallback;
	struct udp_helper *uh_srtp;
	struct srtp *srtp_tx;
	struct srtp *srtp_rx;
	struct tls *dtls;
	struct dtls_sock *dtls_sock;
	struct udp_helper *dtls_uh;   /* for outgoing DTLS-packet */
	struct tls_conn *tls_conn;
	struct {
		size_t headroom;
		struct sa addr;
	} dtls_peer;
	enum media_setup setup_local;
	enum media_setup setup_remote;
	bool crypto_ready;
	bool crypto_verified;
	uint64_t ts_dtls;

	/* Codec handling */
	struct media_ctx *mctx;
	struct auenc_state *aes;
	struct audec_state *ads;
	pthread_mutex_t mutex_enc;  /* protect the encoder state */
	bool started;
	bool hold;

	/* Video */
	struct {
		struct sdp_media *sdpm;
		struct media_ctx *mctx;
		struct videnc_state *ves;
		struct viddec_state *vds;

		bool has_media;
		bool started;
		char *label;
		bool has_rtp;
	} video;

	/* Data */
	struct {
		struct sdp_media *sdpm;

		struct dce *dce;
		struct dce_channel *dce_ch;

		bool has_media;
		bool ready;
		uint64_t ts_connect;
	} data;

	/* Audio */
	struct {
		bool cbr;
	} audio;
    
	/* User callbacks */
	mediaflow_localcand_h *lcandh;
	mediaflow_estab_h *estabh;
	mediaflow_close_h *closeh;
	mediaflow_rtp_state_h *rtpstateh;
	mediaflow_gather_h *gatherh;
	void *arg;

	struct {
		struct {
			uint64_t ts_first;
			uint64_t ts_last;
			size_t bytes;
		} tx, rx;

		size_t n_sdp_recv;
		size_t n_cand_recv;
		size_t n_srtp_dropped;
		size_t n_srtp_error;
	} stat;

	bool sent_rtp;
	bool got_rtp;

	struct list interfacel;

	struct mediaflow_stats mf_stats;
	bool privacy_mode;

	/* magic number check at the end of the struct */
	uint32_t magic;
};

struct vid_ref {
	struct vidcodec *vc;
	struct mediaflow *mf;
};

#undef debug
#undef info
#undef warning
#define debug(...)   mf_log(mf, LOG_LEVEL_DEBUG, __VA_ARGS__);
#define info(...)    mf_log(mf, LOG_LEVEL_INFO,  __VA_ARGS__);
#define warning(...) mf_log(mf, LOG_LEVEL_WARN,  __VA_ARGS__);


#if TARGET_OS_IPHONE
#undef OS
#define OS "ios"
#endif


/* 0.0.0.0 port 0 */
static const struct sa dummy_dtls_peer = {

	.u = {
		.in = {
			 .sin_family = AF_INET,
			 .sin_port = 0,
			 .sin_addr = {0}
		 }
	},

	.len = sizeof(struct sockaddr_in)

};


/* prototypes */
static int print_cand(struct re_printf *pf, const struct ice_cand_attr *cand);
static void add_turn_permission(struct mediaflow *mf,
				   struct turn_conn *conn,
				   const struct ice_cand_attr *rcand);
static void add_permission_to_remotes(struct mediaflow *mf);
static void add_permission_to_remotes_ds(struct mediaflow *mf,
					 struct turn_conn *conn);
static void external_rtp_recv(struct mediaflow *mf,
			      const struct sa *src, struct mbuf *mb);


static void mf_log(const struct mediaflow *mf, enum log_level level,
		   const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);

	n = re_snprintf(buf, sizeof(buf), "[%p] ", mf);
	str_ncpy(&buf[n], fmt, strlen(fmt)+1);
	fmt = buf;

	vloglv(level, fmt, ap);
	va_end(ap);
}


const char *mediaflow_nat_name(enum mediaflow_nat nat)
{
	switch (nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK: return "Trickle-Dualstack";
	default: return "?";
	}
}


enum mediaflow_nat mediaflow_nat_resolve(const char *name)
{
	if (0 == str_casecmp(name, "ice"))
		return MEDIAFLOW_TRICKLEICE_DUALSTACK;

	return (enum mediaflow_nat)-1;
}


static const char *crypto_name(enum media_crypto crypto)
{
	switch (crypto) {

	case CRYPTO_NONE:      return "None";
	case CRYPTO_DTLS_SRTP: return "DTLS-SRTP";
	case CRYPTO_SDESC:     return "SDESC";
	case CRYPTO_BOTH:      return "SDESC + DTLS-SRTP";
	default:               return "???";
	}
}


int mediaflow_cryptos_print(struct re_printf *pf, enum media_crypto cryptos)
{
	int err = 0;

	if (!cryptos)
		return re_hprintf(pf, "%s", crypto_name(CRYPTO_NONE));

	if (cryptos & CRYPTO_DTLS_SRTP) {
		err |= re_hprintf(pf, "%s ", crypto_name(CRYPTO_DTLS_SRTP));
	}
	if (cryptos & CRYPTO_SDESC) {
		err |= re_hprintf(pf, "%s ", crypto_name(CRYPTO_SDESC));
	}

	return err;
}

const char *mediaflow_setup_name(enum media_setup setup)
{
	switch (setup) {

	case SETUP_ACTPASS: return "actpass";
	case SETUP_ACTIVE:  return "active";
	case SETUP_PASSIVE: return "passive";
	default: return "?";
	}
}


static enum media_setup setup_resolve(const char *name)
{
	if (0 == str_casecmp(name, "actpass")) return SETUP_ACTPASS;
	if (0 == str_casecmp(name, "active")) return SETUP_ACTIVE;
	if (0 == str_casecmp(name, "passive")) return SETUP_PASSIVE;

	return (enum media_setup)-1;
}


static const char *sock_prefix(size_t headroom)
{
	if (headroom >= 36) return "TURN-Ind";
	if (headroom >= 4) return "TURN-Chan";

	return "Socket";
}


static bool headroom_via_turn(size_t headroom)
{
	return headroom >= 4;
}


bool mediaflow_dtls_peer_isset(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return sa_isset(&mf->dtls_peer.addr, SA_ALL);
}


static int dtls_peer_print(struct re_printf *pf, const struct mediaflow *mf)
{
	if (!mf)
		return 0;

	return re_hprintf(pf, "%s|%J",
			  sock_prefix(mf->dtls_peer.headroom),
			  &mf->dtls_peer.addr);
}


bool mediaflow_is_rtpstarted(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->sent_rtp && mf->got_rtp;
}


static bool mediaflow_is_video_started(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->video.has_rtp;
}


static void check_rtpstart(struct mediaflow *mf)
{
	if (!mf)
		return;

	if (!mf->rtpstateh)
		return;

	mf->rtpstateh(mediaflow_is_rtpstarted(mf),
		      mediaflow_is_video_started(mf),
		      mf->arg);
}


static size_t get_headroom(const struct mediaflow *mf)
{
	size_t headroom = 0;

	if (!mf)
		return 0;

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		if (!mf->trice)
			return 0;

		if (!mf->sel_pair)
			return 0;

		if (mf->sel_pair->lcand->attr.type == ICE_CAND_TYPE_RELAY)
			return 36;
		else
			return 0;
		break;

	default:
		return 0;
	}

	return headroom;
}


static void ice_error(struct mediaflow *mf, int err)
{
	warning("mediaflow: error in ICE-transport (%m)\n", err);

	mf->ice_ready = false;
	mf->err = err;

	list_flush(&mf->interfacel);

	list_flush(&mf->turnconnl);

	mf->trice_uh = mem_deref(mf->trice_uh);  /* note: destroy first */
	mf->sel_pair = mem_deref(mf->sel_pair);

	mf->terminated = true;

	if (mf->closeh)
		mf->closeh(err, mf->arg);
}


static void tmr_error_handler(void *arg)
{
	struct mediaflow *mf = arg;

	ice_error(mf, mf->err);
}


static void crypto_error(struct mediaflow *mf, int err)
{
	warning("mediaflow: error in DTLS (%m)\n", err);

	mf->crypto_ready = false;
	mf->err = err;
	mf->tls_conn = mem_deref(mf->tls_conn);

	mf->terminated = true;

	if (mf->closeh)
		mf->closeh(err, mf->arg);
}


bool mediaflow_is_ready(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		if (!mf->ice_ready)
			return false;
		break;

	default:
		break;
	}

	if (mf->cryptos_local == CRYPTO_NONE)
		return true;

	if (mf->crypto == CRYPTO_NONE)
		return false;
	else
		return mf->crypto_ready;

	return true;
}


static void update_tx_stats(struct mediaflow *mf, size_t len)
{
	uint64_t now = tmr_jiffies();

	if (!mf->stat.tx.ts_first)
		mf->stat.tx.ts_first = now;
	mf->stat.tx.ts_last = now;
	mf->stat.tx.bytes += len;
}


static void update_rx_stats(struct mediaflow *mf, size_t len)
{
	uint64_t now = tmr_jiffies();

	if (!mf->stat.rx.ts_first)
		mf->stat.rx.ts_first = now;
	mf->stat.rx.ts_last = now;
	mf->stat.rx.bytes += len;
}


static void auenc_error_handler(int err, const char *msg, void *arg)
{
	struct mediaflow *mf = arg;

	error("auenc_error_handler: %s\n", msg);

	mf->err = err;
	if (mf->closeh) {
		mf->closeh(err, mf->arg);
	}
}


static void audec_error_handler(int err, const char *msg, void *arg)
{
	struct mediaflow *mf = arg;

	error("audec_error_handler: %s\n", msg);

	mf->err = err;
	if (mf->closeh) {
		mf->closeh(err, mf->arg);
	}
}


static int voenc_rtp_handler(const uint8_t *pkt, size_t len, void *arg)
{
	struct mediaflow *mf = arg;
	int err;

	if (!mf)
		return EINVAL;

	if (!mf->sent_rtp) {
		info("mediaflow: first RTP packet sent\n");
		mqueue_push(mf->mq, MQ_RTP_START, NULL);
	}

	err = mediaflow_send_raw_rtp(mf, pkt, len);
	if (err == 0){
		mediastats_rtp_stats_update(&mf->audio_stats_snd, pkt, len, 0);
	}

	return err;
}


static int voenc_rtcp_handler(const uint8_t *pkt, size_t len, void *arg)
{
	struct mediaflow *mf = arg;

	return mediaflow_send_raw_rtcp(mf, pkt, len);
}


/* XXX: Move to mediamanager */

static int start_codecs(struct mediaflow *mf)
{
	const struct aucodec *ac;
	const struct sdp_format *fmt;
	struct aucodec_param prm;
	const char *rssrc;
	int err = 0;

	pthread_mutex_lock(&mf->mutex_enc);

	fmt = sdp_media_rformat(mf->sdpm, NULL);
	if (!fmt) {
		warning("mediaflow: no common codec\n");
		err = ENOENT;
		goto out;
	}

	ac = fmt->data;
	if (!ac) {
		warning("mediaflow: no aucodec in sdp data\n");
		err = EINVAL;
		goto out;
	}

	debug("mediaflow: starting audio codecs (%s/%u/%d)\n",
	      fmt->name, fmt->srate, fmt->ch);

	rssrc = sdp_media_rattr(mf->sdpm, "ssrc");

	prm.local_ssrc = mf->lssrcv[MEDIA_AUDIO];
	prm.remote_ssrc = rssrc ? atoi(rssrc) : 0;
	prm.pt = fmt->pt;
	prm.srate = ac->srate;
	prm.ch = ac->ch;
	prm.cbr = false;
	if(fmt->params){
		if (0 == re_regex(fmt->params, strlen(fmt->params), "cbr=1")){
			prm.cbr = true;
		}
	}
    
	if (ac->enc_alloc && !mf->aes) {
		err = ac->enc_alloc(&mf->aes, &mf->mctx, ac, NULL,
				    &prm,
				    voenc_rtp_handler,
				    voenc_rtcp_handler,
				    auenc_error_handler,
				    mf);
		if (err) {
			warning("mediaflow: encoder failed (%m)\n", err);
			goto out;
		}

		if (mf->started && ac->enc_start) {
			ac->enc_start(mf->aes);
		}
        
		mf->audio.cbr = prm.cbr;
        
	}
	mediastats_rtp_stats_init(&mf->audio_stats_snd, fmt->pt, 2000);

	if (ac->dec_alloc && !mf->ads){
		err = ac->dec_alloc(&mf->ads, &mf->mctx, ac, NULL,
				    &prm,
				    audec_error_handler,
				    mf);
		if (err) {
			warning("mediaflow: decoder failed (%m)\n", err);
			goto out;
		}

		if (mf->started && ac->dec_start) {
			ac->dec_start(mf->ads);
		}
	}
	mediastats_rtp_stats_init(&mf->audio_stats_rcv, fmt->pt, 2000);

 out:
	pthread_mutex_unlock(&mf->mutex_enc);

	return err;
}


static int videnc_rtp_handler(const uint8_t *pkt, size_t len, void *arg)
{
	struct mediaflow *mf = arg;

	int err = mediaflow_send_raw_rtp(mf, pkt, len);
	if (err == 0) {
		uint32_t bwalloc = 0;
		const struct vidcodec *vc = videnc_get(mf->video.ves);
		if (vc && vc->enc_bwalloch) {
			bwalloc = vc->enc_bwalloch(mf->video.ves);
		}
		mediastats_rtp_stats_update(&mf->video_stats_snd, pkt, len, bwalloc);
	}

	return err;
}


static int videnc_rtcp_handler(const uint8_t *pkt, size_t len, void *arg)
{
	struct mediaflow *mf = arg;

	return mediaflow_send_raw_rtcp(mf, pkt, len);
}


static void vidcodec_error_handler(int err, const char *msg, void *arg)
{
	struct mediaflow *mf = arg;

	warning("mediaflow: video-codec error '%s' (%m)\n", msg, err);

	mf->err = err;
	if (mf->closeh) {
		mf->closeh(err, mf->arg);
	}

	// TODO: should we also close video-states and ICE+DTLS ?
}


static void update_ssrc_array( uint32_t array[], size_t *count, uint32_t val)
{
	size_t i;

	for (i = 0; i < *count; i++) {
		if (val == array[i]){
			break;
		}
	}

	if ( i == *count) {
		array[*count] = val;
		(*count)++;
	}
}


static bool rssrc_handler(const char *name, const char *value, void *arg)
{
	struct vidcodec_param *prm = arg;
	struct pl pl;
	uint32_t ssrc;
	int err;

	if (prm->remote_ssrcc >= ARRAY_SIZE(prm->remote_ssrcv))
		return true;

	err = re_regex(value, strlen(value), "[0-9]+", &pl);
	if (err)
		return false;

	ssrc = pl_u32(&pl);

	update_ssrc_array( prm->remote_ssrcv, &prm->remote_ssrcc, ssrc);

	return false;
}


static const uint8_t app_label[4] = "DATA";


static int send_rtcp_app(struct mediaflow *mf, const uint8_t *pkt, size_t len)
{
	struct mbuf *mb = mbuf_alloc(256);
	int err;

	err = rtcp_encode(mb, RTCP_APP, 0, (uint32_t)0, app_label, pkt, len);
	if (err) {
		warning("mediaflow: rtcp_encode failed (%m)\n", err);
		goto out;
	}

	err = mediaflow_send_raw_rtcp(mf, mb->buf, mb->end);
	if (err) {
		warning("mediaflow_send_raw_rtcp failed (%m)\n", err);
	}

 out:
	mem_deref(mb);
	return err;
}


static int dce_send_data_handler(uint8_t *pkt, size_t len, void *arg)
{
	struct mediaflow *mf = arg;
	struct mbuf mb;
	int err = 0;
	
	if (!mediaflow_is_ready(mf)) {
		warning("mediaflow: send_data(%zu bytes): not ready"
			" [ice=%d, crypto=%d]\n",
			len, mf->ice_ready, mf->crypto_ready);
		return EINTR;
	}

	mb.buf = pkt;
	mb.pos = 0;
	mb.size = len;
	mb.end = len;

	info("mediaflow(%p): sending DCE packet: %zu\n",
	     mf, mbuf_get_left(&mb));

	switch (mf->crypto) {

	case CRYPTO_DTLS_SRTP:
		if (mf->tls_conn) {
			err = dtls_send(mf->tls_conn, &mb);
		}
		else {
			warning("mediaflow: dce_send_data:"
				" no DTLS connection\n");
			return ENOENT;
		}
		break;

	case CRYPTO_SDESC:
		err = send_rtcp_app(mf, pkt, len);
		if (err) {
			warning("mediaflow: dce_send_data:"
				" rtcp_send_app [%zu bytes] (%m)\n", len, err);
		}
		break;

	default:
		warning("mediaflow: dce_send_data: invalid crypto %d\n",
			mf->crypto);
		return EPROTO;
	};

	return err;
}


static void dce_estab_handler(void *arg)
{
	struct mediaflow *mf = arg;

	mf->mf_stats.dce_estab = tmr_jiffies() - mf->data.ts_connect;

	info("mediaflow(%p): dce established (%d ms)\n",
	     mf, mf->mf_stats.dce_estab);
    
	mf->data.ready = true;
}


static int start_video_codecs(struct mediaflow *mf)
{
	const struct vidcodec *vc;
	const struct sdp_format *fmt;
	struct vidcodec_param prm;
	struct vid_ref *vr;
	int err = 0;

	fmt = sdp_media_rformat(mf->video.sdpm, NULL);
	if (!fmt) {
		warning("mediaflow: no common video-codec\n");
		err = ENOENT;
		goto out;
	}

	vr = fmt->data;
	vc = vr->vc;
	if (!vc) {
		warning("mediaflow: no vidcodec in sdp data\n");
		err = EINVAL;
		goto out;
	}

	/* Local SSRCs */
	memcpy(prm.local_ssrcv, &mf->lssrcv[1], sizeof(prm.local_ssrcv));
	prm.local_ssrcc = 2;

	/* Remote SSRCs */
	prm.remote_ssrcc = 0;
	if (sdp_media_rattr_apply(mf->video.sdpm, "ssrc",
				  rssrc_handler, &prm)) {
		warning("mediaflow: too many remote SSRCs\n");
	}

	debug("mediaflow: starting video codecs (%s/%u/%d)"
	      " [params=%s, rparams=%s]\n",
	      fmt->name, fmt->srate, fmt->ch, fmt->params, fmt->rparams);

	if (vc->enc_alloch && !mf->video.ves) {

		err = vc->enc_alloch(&mf->video.ves, &mf->video.mctx, vc,
				     fmt->rparams, fmt->pt,
				     mf->video.sdpm, &prm,
				     videnc_rtp_handler,
				     videnc_rtcp_handler,
				     vidcodec_error_handler,
				     mf);
		if (err) {
			warning("mediaflow: video encoder failed (%m)\n", err);
			goto out;
		}

		if (mf->started && vc->enc_starth) {
			err = vc->enc_starth(mf->video.ves);
			if (err) {
				warning("mediaflow: could not start"
					" video encoder (%m)\n", err);
				goto out;
			}
		}
	}
	mediastats_rtp_stats_init(&mf->video_stats_snd, fmt->pt, 10000);

	if (vc->dec_alloch && !mf->video.vds){
		err = vc->dec_alloch(&mf->video.vds, &mf->video.mctx, vc,
				     fmt->params, fmt->pt,
				     mf->video.sdpm, &prm,
				     vidcodec_error_handler,
				     mf);
		if (err) {
			warning("mediaflow: video decoder failed (%m)\n", err);
			goto out;
		}

		if (mf->started && vc->dec_starth) {
			err = vc->dec_starth(mf->video.vds);
			if (err) {
				warning("mediaflow: could not start"
					" video decoder (%m)\n", err);
				return err;
			}
		}
	}

	mediastats_rtp_stats_init(&mf->video_stats_rcv, fmt->pt, 10000);

 out:
	return err;
}


static void timeout_rtp(void *arg)
{
	struct mediaflow *mf = arg;

	tmr_start(&mf->tmr_rtp, 5000, timeout_rtp, mf);

	if (mediaflow_is_rtpstarted(mf)) {

		int diff = tmr_jiffies() - mf->stat.rx.ts_last;

		if (diff > RTP_TIMEOUT_MS) {

			warning("mediaflow: no RTP packets recvd for"
				" %d ms -- stop\n",
				diff);

			mf->terminated = true;
			mf->ice_ready = false;

			if (mf->closeh)
				mf->closeh(ETIMEDOUT, mf->arg);
		}
	}
}


/* this function is only called once */
static void mediaflow_established_handler(struct mediaflow *mf)
{
	struct ice_rcand *rcand = NULL;

	if (mf->terminated)
		return;
	if (!mediaflow_is_ready(mf))
		return;

	info("mediaflow: ICE+DTLS established\n");

	if (!tmr_isrunning(&mf->tmr_rtp))
		tmr_start(&mf->tmr_rtp, 1000, timeout_rtp, mf);

	if (mf->estabh) {
		const struct sdp_format *fmt;

		fmt = sdp_media_rformat(mf->sdpm, NULL);

		if (mf->sel_pair)
			rcand = mf->sel_pair->rcand;

		mf->estabh(crypto_name(mf->crypto),
			   fmt ? fmt->name : "?",
			   rcand ? ice_cand_type2name(rcand->attr.type) : "",
			   rcand ? &rcand->attr.addr : NULL,
			   mf->arg);
	}
}


static bool udp_helper_send_handler_srtp(int *err, struct sa *dst,
					 struct mbuf *mb, void *arg)
{
	struct mediaflow *mf = arg;
	(void)dst;

	if (packet_is_rtp_or_rtcp(mb) && mf->srtp_tx) {

		if (packet_is_rtcp_packet(mb)) {

			/* drop short RTCP packets */
			if (mbuf_get_left(mb) <= 8)
				return true;

			*err = srtcp_encrypt(mf->srtp_tx, mb);
			if (*err) {
				warning("srtcp_encrypt() failed (%m)\n",
					*err);
			}
		}
		else {
			*err = srtp_encrypt(mf->srtp_tx, mb);
			if (*err) {
				warning("srtp_encrypt() [%zu bytes]"
					" failed (%m)\n",
					mbuf_get_left(mb), *err);
			}
		}
	}

	return false;
}


static int send_packet(struct mediaflow *mf, size_t headroom,
		       const struct sa *raddr, struct mbuf *mb_pkt,
		       enum packet pkt)
{
	struct mbuf *mb = NULL;
	size_t len = mbuf_get_left(mb_pkt);
	int err = 0;

	if (!mf)
		return EINVAL;

	info("mediaflow: <%s> send_packet `%s' (%zu bytes) via %s to %J\n",
	     mediaflow_nat_name(mf->nat),
	     packet_classify_name(pkt),
	     mbuf_get_left(mb_pkt),
	     sock_prefix(headroom), raddr);

	mb = mbuf_alloc(headroom + len);
	if (!mb)
		return ENOMEM;

	mb->pos = headroom;
	mbuf_write_mem(mb, mbuf_buf(mb_pkt), len);
	mb->pos = headroom;

	/* now invalid */
	mb_pkt = NULL;

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:

		if (mf->ice_ready && mf->sel_pair) {

			struct ice_lcand *lcand = NULL;
			void *sock;

			sock = trice_lcand_sock(mf->trice, mf->sel_pair->lcand);
			if (!sock) {
				warning("send: selected lcand %p"
					" has no sock [%H]\n",
					mf->sel_pair->lcand,
					trice_cand_print, mf->sel_pair->lcand);
				err = ENOTCONN;
				goto out;
			}

			if (AF_INET6 == sa_af(raddr)) {
				lcand = trice_lcand_find2(mf->trice,
							  ICE_CAND_TYPE_HOST,
							  AF_INET6);
				if (lcand) {
					info("mediaflow: send_packet: \n",
					     " using local IPv6 socket\n");
					sock = lcand->us;
				}
			}

			err = udp_send(sock, raddr, mb);
			if (err) {
				warning("mediaflow: send helper error"
					" raddr=%J (%m)\n",
					raddr, err);
			}
		}
		else {
			warning("mediaflow: send_packet: "
				"drop %zu bytes (ICE not ready)\n",
				len);
		}
		break;

	default:
		err = ENOTSUP;
		break;
	}

 out:
	mem_deref(mb);

	return err;
}


/* ONLY for outgoing DTLS packets! */
static bool send_dtls_handler(int *err, struct sa *dst_unused,
			      struct mbuf *mb_pkt, void *arg)
{
	struct mediaflow *mf = arg;
	const enum packet pkt = packet_classify_packet_type(mb_pkt);

	if (pkt != PACKET_DTLS) {
		warning("mediaflow: send_dtls: not a DTLS packet?\n");
		return false;
	}

	++mf->mf_stats.dtls_pkt_sent;

	info("mediaflow: dtls_helper: send DTLS packet #%u (%zu bytes)"
	     " \n",
	     mf->mf_stats.dtls_pkt_sent,
	     mbuf_get_left(mb_pkt));

	*err = send_packet(mf, mf->dtls_peer.headroom,
			   &mf->dtls_peer.addr, mb_pkt, pkt);
	if (*err) {
		warning("mediaflow: send_dtls_handler:"
			" send_packet failed (%m)\n", *err);
	}

	return true;
}


/* For Dual-stack only */
static bool udp_helper_send_handler_trice(int *err, struct sa *dst,
					 struct mbuf *mb, void *arg)
{
	struct mediaflow *mf = arg;
	enum packet pkt;
	int lerr;
	(void)dst;

	pkt = packet_classify_packet_type(mb);
	if (pkt == PACKET_DTLS) {
		warning("mediaflow: dont use this to send DTLS packets\n");
	}

	if (pkt == PACKET_STUN)
		return false;    /* continue */

	if (mf->ice_ready && mf->sel_pair) {

		void *sock;

		sock = trice_lcand_sock(mf->trice, mf->sel_pair->lcand);
		if (!sock) {
			warning("send: selected lcand %p has no sock [%H]\n",
				mf->sel_pair,
				trice_cand_print, mf->sel_pair->lcand);
		}

		lerr = udp_send(sock, &mf->sel_pair->rcand->attr.addr, mb);
		if (lerr) {
			warning("mediaflow: send helper error (%m)\n",
				lerr);
		}
	}
	else {
		warning("mediaflow: helper: cannot send"
			" %zu bytes to %J, ICE not ready! (packet=%s)\n",
			mbuf_get_left(mb), dst,
			packet_classify_name(pkt));
		*err = ENOTCONN;
	}

	return true;
}


static bool verify_fingerprint(struct mediaflow *mf,
			       const struct sdp_session *sess,
			       const struct sdp_media *media,
			       struct tls_conn *tc)
{
	struct pl hash;
	uint8_t md_sdp[32], md_dtls[32];
	size_t sz_sdp = sizeof(md_sdp);
	size_t sz_dtls;
	enum tls_fingerprint type;
	const char *attr;
	int err;

	attr = sdp_media_session_rattr(media, sess, "fingerprint");
	if (sdp_fingerprint_decode(attr, &hash, md_sdp, &sz_sdp))
		return false;

	if (0 == pl_strcasecmp(&hash, "sha-1")) {
		type = TLS_FINGERPRINT_SHA1;
		sz_dtls = 20;
	}
	else if (0 == pl_strcasecmp(&hash, "sha-256")) {
		type = TLS_FINGERPRINT_SHA256;
		sz_dtls = 32;
	}
	else {
		warning("mediaflow: dtls_srtp: unknown fingerprint"
			" '%r'\n", &hash);
		return false;
	}

	err = tls_peer_fingerprint(tc, type, md_dtls, sizeof(md_dtls));
	if (err) {
		warning("mediaflow: dtls_srtp: could not get"
			" DTLS fingerprint (%m)\n", err);
		return false;
	}

	if (sz_sdp != sz_dtls || 0 != memcmp(md_sdp, md_dtls, sz_sdp)) {
		warning("mediaflow: dtls_srtp: %r fingerprint mismatch\n",
			&hash);
		info("  SDP:  %w\n", md_sdp, sz_sdp);
		info("  DTLS: %w\n", md_dtls, sz_dtls);
		return false;
	}

	info("mediaflow: dtls_srtp: verified %r fingerprint OK\n", &hash);

	return true;
}


static int check_data_channel(struct mediaflow *mf)
{
	bool has_data = mediaflow_has_data(mf);
	int err = 0;

	info("mediaflow: dtls_estab_handler: has_data=%d active=%d\n",
	     has_data, mf->setup_local == SETUP_ACTIVE);
	
	if (has_data) {
		info("mediaflow: dce: connecting.. (%p)\n",
		     mf->data.dce);

		mf->data.ts_connect = tmr_jiffies();

		err = dce_connect(mf->data.dce, mf->setup_local == SETUP_ACTIVE);
		if (err) {
			warning("mediaflow: dce_connect failed (%m)\n",
				err);
			return err;
		}
	}

	return err;
}


static void dtls_estab_handler(void *arg)
{
	struct mediaflow *mf = arg;
	enum srtp_suite suite;
	uint8_t cli_key[30], srv_key[30];
	int err;

	if (mf->mf_stats.dtls_estab < 0 && mf->ts_dtls)
		mf->mf_stats.dtls_estab = tmr_jiffies() - mf->ts_dtls;

	info("mediaflow: DTLS established (%d ms)\n",
	     mf->mf_stats.dtls_estab);

	info("           cipher %s\n",
	     tls_cipher_name(mf->tls_conn));

	if (mf->got_sdp) {
		if (!verify_fingerprint(mf, mf->sdp, mf->sdpm, mf->tls_conn)) {
			warning("mediaflow: dtls_srtp: could not verify"
				" remote fingerprint\n");
			err = EAUTH;
			goto error;
		}
		mf->crypto_verified = true;
	}

	err = tls_srtp_keyinfo(mf->tls_conn, &suite,
			       cli_key, sizeof(cli_key),
			       srv_key, sizeof(srv_key));
	if (err) {
		warning("mediaflow: could not get SRTP keyinfo (%m)\n", err);
		goto error;
	}

	info("mediaflow: DTLS established (%s)\n", srtp_suite_name(suite));

	mf->srtp_tx = mem_deref(mf->srtp_tx);
	err = srtp_alloc(&mf->srtp_tx, suite,
			 mf->setup_local == SETUP_ACTIVE ? cli_key : srv_key,
			 sizeof(cli_key), 0);
	if (err) {
		warning("mediaflow: failed to allocate SRTP for TX (%m)\n",
			err);
		goto error;
	}

	err = srtp_alloc(&mf->srtp_rx, suite,
			 mf->setup_local == SETUP_ACTIVE ? srv_key : cli_key,
			 sizeof(srv_key), 0);
	if (err) {
		warning("mediaflow: failed to allocate SRTP for RX (%m)\n",
			err);
		goto error;
	}

	mf->crypto_ready = true;
	
	mediaflow_established_handler(mf);

	check_data_channel(mf);

	return;

 error:
	warning("mediaflow: DTLS-SRTP error (%m)\n", err);

	if (mf->closeh)
		mf->closeh(err, mf->arg);
}


static void dtls_recv_handler(struct mbuf *mb, void *arg)
{
	struct mediaflow *mf = arg;

	info("mediaflow: dtls_recv_handler: %zu bytes\n", mbuf_get_left(mb));

	if (mf->data.dce)
		dce_recv_pkt(mf->data.dce, mbuf_buf(mb), mbuf_get_left(mb));
}


static void dtls_close_handler(int err, void *arg)
{
	struct mediaflow *mf = arg;

	MAGIC_CHECK(mf);

	info("mediaflow: dtls-connection closed (%m)\n", err);

	mf->tls_conn = mem_deref(mf->tls_conn);
	mf->err = err;

	if (!mf->crypto_ready) {

		if (mf->closeh)
			mf->closeh(err, mf->arg);
	}
}


/*
 * called ONCE when we receive DTLS Client Hello from the peer
 *
 * this function is only called when the ICE-layer is established
 */
static void dtls_conn_handler(const struct sa *unused_peer, void *arg)
{
	struct mediaflow *mf = arg;
	bool okay;
	int err;

	info("mediaflow: incoming DTLS connect\n");

	/* NOTE: The DTLS peer should be set in handle_dtls_packet */
	if (!mediaflow_dtls_peer_isset(mf)) {
		warning("mediaflow: dtls_conn_handler: DTLS peer is not set\n");
	}

	/* peer is a dummy address, must not be set/used */
	if (sa_in(unused_peer) || sa_port(unused_peer)) {
		warning("mediaflow: internal error, unused peer (%J)\n",
			unused_peer);
	}

	if (mf->setup_local == SETUP_ACTPASS) {
		info("mediaflow: dtls_conn: local setup not decided yet"
		     ", drop packet\n");
		return;
	}

	if (mf->ice_ready) {

		okay = 1;
	}
	else {
		okay = 0;
	}

	if (!okay) {
		warning("mediaflow: ICE is not ready, cannot accept DTLS\n");
		return;
	}

	mf->ts_dtls = tmr_jiffies();

	if (mf->tls_conn) {
		warning("mediaflow: DTLS already accepted\n");
		return;
	}

	err = dtls_accept(&mf->tls_conn, mf->dtls, mf->dtls_sock,
			  dtls_estab_handler, dtls_recv_handler,
			  dtls_close_handler, mf);
	if (err) {
		warning("mediaflow: dtls_accept failed (%m)\n", err);
		goto error;
	}

	info("mediaflow: dtls accepted\n");

	return;

 error:
	crypto_error(mf, err);
}


static void set_dtls_peer(struct mediaflow *mf, size_t headroom,
			  const struct sa *peer)
{
       if (!mf || !peer)
               return;

       if (!mediaflow_dtls_peer_isset(mf)) {
	       info("mediaflow: dtls_peer: setting to %s|%J\n",
			 sock_prefix(headroom), peer);
       }
       else if (mf->dtls_peer.headroom != headroom ||
		!sa_cmp(&mf->dtls_peer.addr, peer, SA_ALL)) {

	       info("mediaflow: dtls peer: change from %s|%J --> %s|%J\n",
		    sock_prefix(mf->dtls_peer.headroom),
		    &mf->dtls_peer.addr,
		    sock_prefix(headroom), peer);
       }

       mf->dtls_peer.headroom = headroom;
       mf->dtls_peer.addr = *peer;
}


static int start_crypto(struct mediaflow *mf, const struct sa *peer)
{
	int err = 0;

	if (mf->crypto_ready) {
		info("mediaflow: ice-estab: crypto already ready\n");
		return 0;
	}

	switch (mf->crypto) {

	case CRYPTO_NONE:
		/* Do nothing */
		break;

	case CRYPTO_DTLS_SRTP:

		if (mf->setup_local == SETUP_ACTIVE) {

			size_t headroom = 0;

			if (mf->tls_conn) {
				info("mediaflow: dtls_connect,"
				     " already connecting ..\n");
				goto out;
			}

			/* NOTE: must be done before dtls_connect() */
			headroom = get_headroom(mf);

			info("mediaflow: dtls connect via %s to peer %J\n",
			     sock_prefix(headroom), peer);

			mf->ts_dtls = tmr_jiffies();

			set_dtls_peer(mf, headroom, peer);

			err = dtls_connect(&mf->tls_conn, mf->dtls,
					   mf->dtls_sock, &dummy_dtls_peer,
					   dtls_estab_handler,
					   dtls_recv_handler,
					   dtls_close_handler, mf);
			if (err) {
				warning("mediaflow: dtls_connect()"
					" failed (%m)\n", err);
				return err;
			}
		}
		break;

	case CRYPTO_SDESC:
		mf->crypto_ready = true;

		check_data_channel(mf);
		break;

	default:
		warning("mediaflow: established: unknown crypto '%s' (%d)\n",
			crypto_name(mf->crypto), mf->crypto);
		break;
	}

 out:
	return err;
}


/* this function is only called once */
static void ice_established_handler(struct mediaflow *mf,
				    const struct sa *peer)
{
	int err;

	info("mediaflow: ICE-transport established [got_sdp=%d]"
	     " (peer = %s.%J)\n",
	     mf->got_sdp,
	     mf->sel_pair
	      ? ice_cand_type2name(mf->sel_pair->rcand->attr.type)
	      : "?",
	     peer);

	if (mf->mf_stats.nat_estab < 0 && mf->ts_nat_start) {
		mf->mf_stats.nat_estab = tmr_jiffies() - mf->ts_nat_start;
	}

	set_dtls_peer(mf, get_headroom(mf), peer);

	if (mf->crypto_ready) {
		info("mediaflow: ice-estab: crypto already ready\n");
		goto out;
	}

	err = start_crypto(mf, peer);
	if (err) {
		crypto_error(mf, err);
	}

 out:
	mediaflow_established_handler(mf);
}


static int handle_sdes_srtp_tx(struct mediaflow *mf)
{
       uint8_t key[30];
       char buf[256];
       size_t buf_len = sizeof(buf);
       int err;

       rand_bytes(key, sizeof(key));

       err = srtp_alloc(&mf->srtp_tx, SRTP_AES_CM_128_HMAC_SHA1_80,
			key, sizeof(key), 0);
       if (err) {
	       warning("mediaflow: failed to allocate SRTP for TX (%m)\n",
		       err);
	       return err;
       }

       err = base64_encode(key, sizeof(key), buf, &buf_len);
       if (err)
	       return err;

       err = sdp_media_set_lattr(mf->sdpm, true,
				 "crypto",
				 "1 AES_CM_128_HMAC_SHA1_80 inline:%b",
				 buf, buf_len);
       if (err)
	       return err;

       return 0;
}


static bool attrib_handler(const char *name, const char *val, void *arg)
{
       struct pl idx, suite, keyprm, sessprm, key, lifemki, keyprm2;
       (void)name;
       (void)arg;

       if (!val)
               return false;

       if (re_regex(val, strlen(val),
                    "[0-9]+[ \t]+[0-9a-z_]+[ \t]+inline:[^ \t]+[^]*",
                    &idx, NULL, &suite, NULL, &keyprm, &sessprm))
               return false;

       if (re_regex(keyprm.p, keyprm.l, "[^|;]+[^;]*[;]*[^]*",
                    &key, &lifemki, NULL, &keyprm2))
               return false;

       /* MKI or multi-key not supported */
       if (pl_strchr(&lifemki, ':') || keyprm2.l > 0)
               return false;

       /* found */
       if (0 == pl_strcasecmp(&suite, "AES_CM_128_HMAC_SHA1_80"))
               return true;

       return false;
}


static int handle_sdes_srtp_rx(struct mediaflow *mf)
{
       const char *crypto;
       struct pl b;
       uint8_t key[30];
       size_t key_len = sizeof(key);
       int err;

       crypto = sdp_media_rattr_apply(mf->sdpm, "crypto", attrib_handler, 0);
       if (!crypto) {
               warning("mediaflow: crypto parameter not found\n");
               return ENOENT;
       }

       err = re_regex(crypto, str_len(crypto), "inline:[^]+", &b);
       if (err) {
               warning("mediaflow: could not get crypto key (%s)\n", crypto);
               return err;
       }

       err = base64_decode(b.p, b.l, key, &key_len);
       if (err)
               return err;

       err = srtp_alloc(&mf->srtp_rx, SRTP_AES_CM_128_HMAC_SHA1_80,
                        key, key_len, 0);
       if (err) {
               warning("mediaflow: failed to allocate SRTP for RX (%m)\n",
                       err);
               return err;
       }

       return err;
}


static void handle_dtls_packet(struct mediaflow *mf, const struct sa *src,
			       struct mbuf *mb)
{
	size_t headroom = mb->pos;

	++mf->mf_stats.dtls_pkt_recv;

	info("dtls: recv %zu bytes from %s|%J\n",
	     mbuf_get_left(mb), sock_prefix(mb->pos), src);

	if (!mf->got_sdp) {

		info("mediaflow: SDP is not ready --"
		     " drop DTLS packet from %J\n",
		     src);
		return;
	}

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK &&
	    !mediaflow_ice_ready(mf)) {

		warning("mediaflow: ICE is not ready (%s) --"
			" drop DTLS packet from %J\n",
			trice_checklist_isrunning(mf->trice)
			? "Running" : "Not-Running",
			src);
		return;
	}

	if (!mediaflow_dtls_peer_isset(mf)) {
		info("mediaflow: DTLS peer is not set --"
			" drop DTLS packet from %J\n", src);
		return;
	}

	if (headroom_via_turn(mf->dtls_peer.headroom) !=
	    headroom_via_turn(headroom)) {

		info("dtls: via turn mismatch (peer=%s, packet=%s)\n",
			sock_prefix(mf->dtls_peer.headroom),
			sock_prefix(headroom));
	}
	if (!sa_cmp(src, &mf->dtls_peer.addr, SA_ALL)) {
		info("dtls: source addr mismatch (%s|peer=%J, %s|packet=%J)\n",
			sock_prefix(mf->dtls_peer.headroom), &mf->dtls_peer.addr,
			sock_prefix(headroom), src);
	}

	dtls_recv_packet(mf->dtls_sock, &dummy_dtls_peer, mb);
}


static bool udp_helper_recv_handler_srtp(struct sa *src, struct mbuf *mb,
					 void *arg)
{
	struct mediaflow *mf = arg;
	size_t len = mbuf_get_left(mb);
	const enum packet pkt = packet_classify_packet_type(mb);
	int err;

	if (pkt == PACKET_DTLS) {
		handle_dtls_packet(mf, src, mb);
		return true;
	}

	if (packet_is_rtp_or_rtcp(mb)) {

		/* the SRTP is not ready yet .. */
		if (!mf->srtp_rx) {
			mf->stat.n_srtp_dropped++;
			goto next;
		}

		if (packet_is_rtcp_packet(mb)) {

			err = srtcp_decrypt(mf->srtp_rx, mb);
			if (err) {
				mf->stat.n_srtp_error++;
				warning("mediaflow: srtcp_decrypt failed"
					" [%zu bytes] (%m)\n", len, err);
				return true;
			}
		}
		else {
			err = srtp_decrypt(mf->srtp_rx, mb);
			if (err) {
				mf->stat.n_srtp_error++;
				if (err != EALREADY) {
					warning("mediaflow: srtp_decrypt"
						" failed"
						" [%zu bytes from %J] (%m)\n",
						len, src, err);
				}
				return true;
			}
		}

		if (packet_is_rtcp_packet(mb)) {

			struct rtcp_msg *msg = NULL;
			size_t pos = mb->pos;
			bool is_app = false;
			int r;

			r = rtcp_decode(&msg, mb);
			if (r) {
				warning("mediaflow: failed to decode"
					" incoming RTCP"
					" packet (%m)\n", r);
				goto done;
			}
			mb->pos = pos;

			if (msg->hdr.pt == RTCP_APP) {

				if (0 != memcmp(msg->r.app.name, app_label, 4)) {
					warning("invalid app name '%b'\n",
						msg->r.app.name, (size_t)4);
					goto done;
				}

				is_app = true;

				if (mf->data.dce) {
					dce_recv_pkt(mf->data.dce,
						     msg->r.app.data,
						     msg->r.app.data_len);
				}
			}

		done:
			mem_deref(msg);

			/* NOTE: dce handler might deref mediaflow */
			if (is_app)
				return true;
		}
	}

 next:
	if (packet_is_rtp_or_rtcp(mb)) {

		/* If external RTP is enabled, forward RTP/RTCP packets
		 * to the relevant au/vid-codec.
		 *
		 * otherwise just pass it up to internal RTP-stack
		 */
		if (mf->external_rtp) {
			external_rtp_recv(mf, src, mb);
			return true; /* handled */
		}
		else {
			update_rx_stats(mf, mbuf_get_left(mb));
			return false;  /* continue processing */
		}
	}

	return false;
}


/*
 * UDP helper to intercept incoming RTP/RTCP packets:
 *
 * -- send to decoder if supported by it
 */
static void external_rtp_recv(struct mediaflow *mf,
			      const struct sa *src, struct mbuf *mb)
{
	const struct aucodec *ac;
	const struct vidcodec *vc;
	struct rtp_header hdr;
	const struct sdp_format *fmt;
	size_t start = mb->pos;
	int err;

	if (!mf->started) {
		return;
	}

	ac = audec_get(mf->ads);
	vc = viddec_get(mf->video.vds);

	if (!packet_is_rtcp_packet(mb)) {
		update_rx_stats(mf, mbuf_get_left(mb));
	}
	else {
		/* RTCP is sent to both audio+video */

		if (ac && ac->dec_rtcph) {
			mb->pos = start;
			ac->dec_rtcph(mf->ads,
				      mbuf_buf(mb), mbuf_get_left(mb));
		}
		if (vc && vc->dec_rtcph) {
			mb->pos = start;
			vc->dec_rtcph(mf->video.vds,
				      mbuf_buf(mb), mbuf_get_left(mb));
		}
		goto out;
	}

	if (!mf->got_rtp) {
		info("mediaflow: first RTP packet received (%zu bytes)\n",
		     mbuf_get_left(mb));
		mf->got_rtp = true;
		check_rtpstart(mf);
	}

	err = rtp_hdr_decode(&hdr, mb);
	mb->pos = start;
	if (err) {
		warning("mediaflow: rtp header decode (%m)\n", err);
		goto out;
	}

	fmt = sdp_media_lformat(mf->sdpm, hdr.pt);
	if (fmt) {

		/* now, pass on the raw RTP/RTCP packet to the decoder */

		if (ac && ac->dec_rtph) {
			ac->dec_rtph(mf->ads,
				     mbuf_buf(mb), mbuf_get_left(mb));

			mediastats_rtp_stats_update(&mf->audio_stats_rcv,
					 mbuf_buf(mb), mbuf_get_left(mb), 0);
		}

		goto out;
	}

	fmt = sdp_media_lformat(mf->video.sdpm, hdr.pt);
	if (fmt) {
		if (!mf->video.has_rtp) {
			mf->video.has_rtp = true;
			check_rtpstart(mf);
		}
		if (vc && vc->dec_rtph) {
			vc->dec_rtph(mf->video.vds,
				     mbuf_buf(mb), mbuf_get_left(mb));

			uint32_t bwalloc = 0;
			if (vc->dec_bwalloch) {
				bwalloc = vc->dec_bwalloch(mf->video.vds);
			}
			mediastats_rtp_stats_update(&mf->video_stats_rcv,
					 mbuf_buf(mb),mbuf_get_left(mb), bwalloc);
		}

		goto out;
	}

	info("mediaflow: recv: no SDP format found"
	     " for payload type %d\n", hdr.pt);

 out:
	return;  /* stop packet here */
}


static int print_cand(struct re_printf *pf, const struct ice_cand_attr *cand)
{
	if (!cand)
		return 0;

	return re_hprintf(pf, "%s.%J",
			  ice_cand_type2name(cand->type), &cand->addr);
}


static int print_errno(struct re_printf *pf, int err)
{
	if (err == -1)
		return re_hprintf(pf, "Progress..");
	else if (err)
		return re_hprintf(pf, "%m", err);
	else
		return re_hprintf(pf, "Success");
}


static int print_candidates(struct re_printf *pf, const struct mediaflow *mf)
{
	int err = 0;

	if (!mf)
		return 0;

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {
		err = trice_debug(pf, mf->trice);
	}

	return err;
}


int mediaflow_summary(struct re_printf *pf, const struct mediaflow *mf)
{
	struct le *le;
	double dur_tx;
	double dur_rx;
	int err = 0;

	if (!mf)
		return 0;

	dur_tx = (double)(mf->stat.tx.ts_last - mf->stat.tx.ts_first) / 1000.0;
	dur_rx = (double)(mf->stat.rx.ts_last - mf->stat.rx.ts_first) / 1000.0;

	err |= re_hprintf(pf,
			  "------------- mediaflow summary -------------\n");
	err |= re_hprintf(pf, "tag:  %s\n", mf->tag);
	err |= re_hprintf(pf, "\n");
	err |= re_hprintf(pf, "sdp: state=%d, got_sdp=%d, sent_sdp=%d\n",
			  mf->sdp_state, mf->got_sdp, mf->sent_sdp);
	err |= re_hprintf(pf, "     remote_tool=%s\n", mf->sdp_rtool);

	err |= re_hprintf(pf, "nat: %s (ready=%d)\n",
			  mediaflow_nat_name(mf->nat), mf->ice_ready);
	err |= re_hprintf(pf, "remote candidates:\n");

	err |= print_candidates(pf, mf);

	if (mf->sel_pair) {
		err |= re_hprintf(pf, "selected local candidate:   %H\n",
				  trice_cand_print, mf->sel_pair->lcand);
		err |= re_hprintf(pf, "selected remote candidate:  %H\n",
				  trice_cand_print, mf->sel_pair->rcand);
	}
	err |= re_hprintf(pf, "peer_software:       %s\n", mf->peer_software);
	err |= re_hprintf(pf, "eoc:                 local=%d, remote=%d\n",
			  mf->ice_local_eoc, mf->ice_remote_eoc);
	err |= re_hprintf(pf, "\n");

	/* Crypto summary */
	err |= re_hprintf(pf,
			  "crypto: local  = %H\n"
			  "        remote = %H\n"
			  "        common = %s\n",
			  mediaflow_cryptos_print, mf->cryptos_local,
			  mediaflow_cryptos_print, mf->cryptos_remote,
			  crypto_name(mf->crypto));
	err |= re_hprintf(pf,
			  "        ready=%d\n", mf->crypto_ready);

	if (mf->crypto == CRYPTO_DTLS_SRTP) {
		err |= re_hprintf(pf,
				  "        peer = %H\n",
				  dtls_peer_print, mf);
		err |= re_hprintf(pf,
				  "        verified=%d\n"
				  "        setup_local=%s\n"
				  "        setup_remote=%s\n"
				  "",
				  mf->crypto_verified,
				  mediaflow_setup_name(mf->setup_local),
				  mediaflow_setup_name(mf->setup_remote)
				  );
		err |= re_hprintf(pf, "        setup_time=%d ms\n",
				  mf->mf_stats.dtls_estab);
		err |= re_hprintf(pf, "        packets sent=%u, recv=%u\n",
				  mf->mf_stats.dtls_pkt_sent,
				  mf->mf_stats.dtls_pkt_recv);
	}
	err |= re_hprintf(pf, "\n");

	err |= re_hprintf(pf, "RTP packets:\n");
	err |= re_hprintf(pf, "bytes sent:  %zu (%.1f bit/s)"
			  " for %.2f sec\n",
		  mf->stat.tx.bytes,
		  dur_tx ? 8.0 * (double)mf->stat.tx.bytes / dur_tx : 0,
		  dur_tx);
	err |= re_hprintf(pf, "bytes recv:  %zu (%.1f bit/s)"
			  " for %.2f sec\n",
		  mf->stat.rx.bytes,
		  dur_rx ? 8.0 * (double)mf->stat.rx.bytes / dur_rx : 0,
		  dur_rx);

	err |= re_hprintf(pf, "\n");
	err |= re_hprintf(pf, "SDP recvd:       %zu\n", mf->stat.n_sdp_recv);
	err |= re_hprintf(pf, "ICE cand recvd:  %zu\n", mf->stat.n_cand_recv);
	err |= re_hprintf(pf, "SRTP dropped:    %zu\n",
			  mf->stat.n_srtp_dropped);
	err |= re_hprintf(pf, "SRTP errors:     %zu\n",
			  mf->stat.n_srtp_error);

	err |= re_hprintf(pf, "\nvideo_media: %d\n", mf->video.has_media);

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {

		err |= re_hprintf(pf, "TURN Clients: (%u)\n",
				  list_count(&mf->turnconnl));

		for (le = mf->turnconnl.head; le; le = le->next) {
			struct turn_conn *tc = le->data;

			err |= turnconn_debug(pf, tc);
		}
	}

	err |= re_hprintf(pf, "Interfaces: (%u)\n",
			  list_count(&mf->interfacel));
	for (le = mf->interfacel.head; le; le = le->next) {
		struct interface *ifc = le->data;

		err |= re_hprintf(pf, "...%s..%s|%j\n",
				  ifc->is_default ? "*" : ".",
				  ifc->ifname, &ifc->addr);
	}

	err |= re_hprintf(pf,
			  "-----------------------------------------------\n");
	err |= re_hprintf(pf, "\n");

	return err;
}


int mediaflow_rtp_summary(struct re_printf *pf, const struct mediaflow *mf)
{
	struct aucodec_stats *voe_stats;
	int err = 0;

	if (!mf)
		return 0;

	err |= re_hprintf(pf,
			  "----------- mediaflow RTP summary ------------\n");

	voe_stats = mediaflow_codec_stats((struct mediaflow*)mf);
	err |= re_hprintf(pf,"Audio TX: \n");
	if (voe_stats) {
		err |= re_hprintf(pf,"Level (dB) %.1f %.1f %.1f \n",
				  voe_stats->in_vol.min,
				  voe_stats->in_vol.avg,
				  voe_stats->in_vol.max);
	}
	err |= re_hprintf(pf,"Bit rate (kbps) %.1f %.1f %.1f \n",
			  mf->audio_stats_snd.bit_rate_stats.min,
			  mf->audio_stats_snd.bit_rate_stats.avg,
			  mf->audio_stats_snd.bit_rate_stats.max);
	err |= re_hprintf(pf,"Packet rate (1/s) %.1f %.1f %.1f \n",
			  mf->audio_stats_snd.pkt_rate_stats.min,
			  mf->audio_stats_snd.pkt_rate_stats.avg,
			  mf->audio_stats_snd.pkt_rate_stats.max);
	err |= re_hprintf(pf,"Loss rate (pct) %.1f %.1f %.1f \n",
			  mf->audio_stats_snd.pkt_loss_stats.min,
			  mf->audio_stats_snd.pkt_loss_stats.avg,
			  mf->audio_stats_snd.pkt_loss_stats.max);

	err |= re_hprintf(pf,"Audio RX: \n");
	if (voe_stats) {
		err |= re_hprintf(pf,"Level (dB) %.1f %.1f %.1f \n",
				  voe_stats->out_vol.min,
				  voe_stats->out_vol.avg,
				  voe_stats->out_vol.max);
	}
	err |= re_hprintf(pf,"Bit rate (kbps) %.1f %.1f %.1f \n",
			  mf->audio_stats_rcv.bit_rate_stats.min,
			  mf->audio_stats_rcv.bit_rate_stats.avg,
			  mf->audio_stats_rcv.bit_rate_stats.max);
	err |= re_hprintf(pf,"Packet rate (1/s) %.1f %.1f %.1f \n",
			  mf->audio_stats_rcv.pkt_rate_stats.min,
			  mf->audio_stats_rcv.pkt_rate_stats.avg,
			  mf->audio_stats_rcv.pkt_rate_stats.max);
	err |= re_hprintf(pf,"Loss rate (pct) %.1f %.1f %.1f \n",
			  mf->audio_stats_rcv.pkt_loss_stats.min,
			  mf->audio_stats_rcv.pkt_loss_stats.avg,
			  mf->audio_stats_rcv.pkt_loss_stats.max);
	err |= re_hprintf(pf,"Mean burst length %.1f %.1f %.1f \n",
			  mf->audio_stats_rcv.pkt_mbl_stats.min,
			  mf->audio_stats_rcv.pkt_mbl_stats.avg,
			  mf->audio_stats_rcv.pkt_mbl_stats.max);
	if (voe_stats){
		err |= re_hprintf(pf,"JB size (ms) %.1f %.1f %.1f \n",
				  voe_stats->jb_size.min,
				  voe_stats->jb_size.avg,
				  voe_stats->jb_size.max);
		err |= re_hprintf(pf,"RTT (ms) %.1f %.1f %.1f \n",
				  voe_stats->rtt.min,
				  voe_stats->rtt.avg,
				  voe_stats->rtt.max);
	}
	err |= re_hprintf(pf,"Packet dropouts (#) %d \n",
			  mf->audio_stats_rcv.dropouts);
	if (mf->video.has_media){
		err |= re_hprintf(pf,"Video TX: \n");
		err |= re_hprintf(pf,"Bit rate (kbps) %.1f %.1f %.1f \n",
				  mf->video_stats_snd.bit_rate_stats.min,
				  mf->video_stats_snd.bit_rate_stats.avg,
				  mf->video_stats_snd.bit_rate_stats.max);
		err |= re_hprintf(pf,"Alloc rate (kbps) %.1f %.1f %.1f \n",
				  mf->video_stats_snd.bw_alloc_stats.min,
				  mf->video_stats_snd.bw_alloc_stats.avg,
				  mf->video_stats_snd.bw_alloc_stats.max);
		err |= re_hprintf(pf,"Frame rate (1/s) %.1f %.1f %.1f \n",
				  mf->video_stats_snd.frame_rate_stats.min,
				  mf->video_stats_snd.frame_rate_stats.avg,
				  mf->video_stats_snd.frame_rate_stats.max);
		err |= re_hprintf(pf,"Loss rate (pct) %.1f %.1f %.1f \n",
				  mf->video_stats_snd.pkt_loss_stats.min,
				  mf->video_stats_snd.pkt_loss_stats.avg,
				  mf->video_stats_snd.pkt_loss_stats.max);

		err |= re_hprintf(pf,"Video RX: \n");
		err |= re_hprintf(pf,"Bit rate (kbps) %.1f %.1f %.1f \n",
				  mf->video_stats_rcv.bit_rate_stats.min,
				  mf->video_stats_rcv.bit_rate_stats.avg,
				  mf->video_stats_rcv.bit_rate_stats.max);
		err |= re_hprintf(pf,"Alloc rate (kbps) %.1f %.1f %.1f \n",
				  mf->video_stats_rcv.bw_alloc_stats.min,
				  mf->video_stats_rcv.bw_alloc_stats.avg,
				  mf->video_stats_rcv.bw_alloc_stats.max);
		err |= re_hprintf(pf,"Frame rate (1/s) %.1f %.1f %.1f \n",
				  mf->video_stats_rcv.frame_rate_stats.min,
				  mf->video_stats_rcv.frame_rate_stats.avg,
				  mf->video_stats_rcv.frame_rate_stats.max);
		err |= re_hprintf(pf,"Loss rate (pct) %.1f %.1f %.1f \n",
				  mf->video_stats_rcv.pkt_loss_stats.min,
				  mf->video_stats_rcv.pkt_loss_stats.avg,
				  mf->video_stats_rcv.pkt_loss_stats.max);
		err |= re_hprintf(pf,"Packet dropouts (#) %d \n",
				  mf->video_stats_rcv.dropouts);
	}

	err |= re_hprintf(pf,
			  "-----------------------------------------------\n");

	return err;
}


/* NOTE: all udp-helpers must be free'd before RTP-socket */
static void destructor(void *arg)
{
	struct mediaflow *mf = arg;

	if (MAGIC != mf->magic) {
		warning("mediaflow: destructor: bad magic (0x%08x)\n",
			mf->magic);
	}

	mf->terminated = true;

	mf->estabh = NULL;
	mf->closeh = NULL;
    
	if (mf->started)
		mediaflow_stop_media(mf);

	info("mediaflow: mediaflow %p destroyed (%H) got_sdp=%d\n",
	     mf, print_errno, mf->err, mf->got_sdp);

	/* print a nice summary */
	if (mf->got_sdp) {
		info("%H\n", mediaflow_summary, mf);
		info("%H\n", mediaflow_rtp_summary, mf);
	}

	tmr_cancel(&mf->tmr_rtp);
	tmr_cancel(&mf->tmr_nat);
	tmr_cancel(&mf->tmr_error);

	/* XXX: voe is calling to mediaflow_xxx here */
	/* deref the encoders/decodrs first, as they may be multithreaded,
	 * and callback in here...
	 * Remove decoder first as webrtc might still send RTCP packets
	 */
	mf->ads = mem_deref(mf->ads);
	mf->aes = mem_deref(mf->aes);

	mf->video.ves = mem_deref(mf->video.ves);
	mf->video.vds = mem_deref(mf->video.vds);

	mf->data.dce = mem_deref(mf->data.dce);

	mf->tls_conn = mem_deref(mf->tls_conn);

	list_flush(&mf->interfacel);

	mf->trice_uh = mem_deref(mf->trice_uh);  /* note: destroy first */
	mf->sel_pair = mem_deref(mf->sel_pair);
	mf->trice = mem_deref(mf->trice);
	mf->trice_stun = mem_deref(mf->trice_stun);
	mem_deref(mf->us_stun);
	list_flush(&mf->turnconnl);

	mem_deref(mf->dtls_sock);

	mem_deref(mf->uh_srtp);

	mem_deref(mf->rtp); /* must be free'd after ICE and DTLS */
	mem_deref(mf->sdp);

	mem_deref(mf->srtp_tx);
	mem_deref(mf->srtp_rx);
	mem_deref(mf->dtls);
	mem_deref(mf->ct_gather);

	mem_deref(mf->label);
	mem_deref(mf->video.label);

	mem_deref(mf->peer_software);

	mem_deref(mf->mq);
}


/* XXX: check if we need this, or it can be moved ? */
static void stun_udp_recv_handler(const struct sa *src,
				  struct mbuf *mb, void *arg)
{
	struct mediaflow *mf = arg;
	struct stun_unknown_attr ua;
	struct stun_msg *msg = NULL;

	debug("mediaflow: stun: receive %zu bytes from %J\n",
	      mbuf_get_left(mb), src);

	if (0 == stun_msg_decode(&msg, mb, &ua) &&
	    stun_msg_method(msg) == STUN_METHOD_BINDING) {

		switch (stun_msg_class(msg)) {

		case STUN_CLASS_SUCCESS_RESP:
		case STUN_CLASS_ERROR_RESP:
			(void)stun_ctrans_recv(mf->trice_stun, msg, &ua);
			break;

		default:
			re_printf("STUN message from %J dropped\n", src);
			break;
		}
	}

	mem_deref(msg);
}


static void mq_callback(int id, void *data, void *arg)
{
	struct mediaflow *mf = arg;

	switch (id) {

	case MQ_RTP_START:
		if(!mf->sent_rtp){
			mf->sent_rtp = true;
			check_rtpstart(mf);
		}
		break;
	}
}


/*
 * See https://tools.ietf.org/html/draft-ietf-rtcweb-jsep-14#section-5.1.1
 */
static const char *sdp_profile(enum media_crypto cryptos)
{
	if (cryptos & CRYPTO_DTLS_SRTP)
		return "UDP/TLS/RTP/SAVPF";

	if (cryptos & CRYPTO_SDESC)
		return "RTP/SAVPF";

	return "RTP/SAVPF";
}


/* should not reach here */
static void rtp_recv_handler(const struct sa *src,
			     struct mbuf *mb, void *arg)
{
	struct mediaflow *mf = arg;
	(void)src;
	(void)mb;

	info("mediaflow: nobody cared about incoming packet (%zu bytes)\n",
	     mbuf_get_left(mb));
}


/**
 * Create a new mediaflow.
 *
 * No ICE candidates are added here, you need to do that explicitly.
 *
 * @param aucodecl     Optional list of audio-codecs (struct aucodec)
 * @param audio_srate  Force a specific sample-rate (optional)
 * @param audio_ch     Force a specific number of channels (optional)
 */
int mediaflow_alloc(struct mediaflow **mfp,
		    struct tls *dtls,
		    const struct list *aucodecl,
		    const struct sa *laddr_sdp,
		    enum mediaflow_nat nat,
		    enum media_crypto cryptos,
		    mediaflow_localcand_h *lcandh,
		    mediaflow_estab_h *estabh,
		    mediaflow_close_h *closeh,
		    void *arg)
{
	struct mediaflow *mf;
	struct le *le;
	struct sa laddr_rtp;
	uint16_t lport;
	bool external_rtp = true;
	int err;

	if (!mfp || !laddr_sdp)
		return EINVAL;

	if (!sa_isset(laddr_sdp, SA_ADDR))
		return EINVAL;

	mf = mem_zalloc(sizeof(*mf), destructor);
	if (!mf)
		return ENOMEM;

	mf->magic = MAGIC;
	mf->privacy_mode = false;
	mf->af = sa_af(laddr_sdp);

	mf->mf_stats.turn_alloc = -1;
	mf->mf_stats.nat_estab  = -1;
	mf->mf_stats.dtls_estab = -1;
	mf->mf_stats.dce_estab  = -1;

	err = mqueue_alloc(&mf->mq, mq_callback, mf);
	if (err)
		goto out;

	mf->dtls   = mem_ref(dtls);
	mf->nat    = nat;
	mf->setup_local    = SETUP_ACTPASS;
	mf->setup_remote   = SETUP_ACTPASS;
	mf->external_rtp = external_rtp;
	mf->cryptos_local = cryptos;
	mf->crypto_fallback = CRYPTO_DTLS_SRTP;

	mf->lcandh = lcandh;
	mf->estabh = estabh;
	mf->closeh = closeh;
	mf->arg    = arg;

	mf->ice_tiebrk = rand_u64();

	err = pthread_mutex_init(&mf->mutex_enc, NULL);
	if (err)
		goto out;

	rand_str(mf->ice_ufrag, sizeof(mf->ice_ufrag));
	rand_str(mf->ice_pwd, sizeof(mf->ice_pwd));

	/* RTP must listen on 0.0.0.0 so that we can send/recv
	   packets on all interfaces */
	sa_init(&laddr_rtp, AF_INET);

	mf->enable_rtcp = !external_rtp;

	err = udp_listen(&mf->rtp, &laddr_rtp, rtp_recv_handler, mf);
	if (err) {
		warning("mediaflow: rtp_listen failed (%m)\n", err);
		goto out;
	}

	lport = PORT_DISCARD;

	err = sdp_session_alloc(&mf->sdp, laddr_sdp);
	if (err)
		goto out;

	(void)sdp_session_set_lattr(mf->sdp, true, "tool", avs_version_str());

	err = sdp_media_add(&mf->sdpm, mf->sdp, "audio",
			    PORT_DISCARD,
			    sdp_profile(cryptos));
	if (err)
		goto out;

	sdp_media_set_lbandwidth(mf->sdpm,
				 SDP_BANDWIDTH_AS, AUDIO_BANDWIDTH);

	/* needed for new versions of WebRTC */
	err = sdp_media_set_alt_protos(mf->sdpm, 2,
				       "UDP/TLS/RTP/SAVPF", "RTP/SAVPF");
	if (err)
		goto out;

	sdp_media_set_lattr(mf->sdpm, false, "mid", "audio");

	rand_str(mf->cname, sizeof(mf->cname));
	rand_str(mf->msid, sizeof(mf->msid));
	err = uuid_v4(&mf->label);
	err |= uuid_v4(&mf->video.label);
	if (err)
		goto out;

	mf->lssrcv[MEDIA_AUDIO] = rand_u32();

	debug("mediaflow: local SSRC is %u\n", mf->lssrcv[MEDIA_AUDIO]);

	err = sdp_media_set_lattr(mf->sdpm, false, "ssrc", "%u cname:%s",
				  mf->lssrcv[MEDIA_AUDIO], mf->cname);
	if (err)
		goto out;

	/* ICE */
	if (nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {

		struct trice_conf conf = {
			.debug = false,
			.trace = false,
#if TARGET_OS_IPHONE
			.ansi = false,
#elif defined (__ANDROID__)
			.ansi = false,
#else
			.ansi = true,
#endif
			.enable_prflx = !mf->privacy_mode
		};
		bool controlling = false;  /* NOTE: this is set later */

		err = trice_alloc(&mf->trice, &conf, controlling,
				  mf->ice_ufrag, mf->ice_pwd);
		if (err) {
			warning("mediaflow: DUALSTACK trice error (%m)\n",
				err);
			goto out;
		}

		err = trice_set_software(mf->trice, avs_version_str());
		if (err)
			goto out;

		err = stun_alloc(&mf->trice_stun, NULL, NULL, NULL);
		if (err)
			goto out;

		/*
		 * tuning the STUN transaction values
		 *
		 * RTO=150 and RC=7 gives around 12 seconds timeout
		 */
		stun_conf(mf->trice_stun)->rto = 150;  /* milliseconds */
		stun_conf(mf->trice_stun)->rc =    8;  /* retransmits */

		/* Virtual socket for directing outgoing Packets */
		err = udp_register_helper(&mf->trice_uh, mf->rtp,
					  LAYER_ICE,
					  udp_helper_send_handler_trice,
					  NULL, mf);
		if (err)
			goto out;

	}

	/* populate SDP with all known audio-codecs */
	LIST_FOREACH(aucodecl, le) {
		struct aucodec *ac = list_ledata(le);

		if (external_rtp != ac->has_rtp) {
			warning("mediaflow: external_rtp=%d but "
				" aucodec '%s' has rtp=%d\n",
				external_rtp, ac->name, ac->has_rtp);
			err = EINVAL;
			goto out;
		}

		err = sdp_format_add(NULL, mf->sdpm, false,
				     ac->pt, ac->name, ac->srate, ac->ch,
				     ac->fmtp_ench, ac->fmtp_cmph, ac, false,
				     "%s", ac->fmtp);
		if (err)
			goto out;
	}

	/* Set ICE-options */
	switch (nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		sdp_session_set_lattr(mf->sdp, false, "ice-options",
				      "trickle");
		break;

	default:
		break;
	}

	/* Mandatory */
	sdp_media_set_lattr(mf->sdpm, false, "rtcp-mux", NULL);

	lport = PORT_DISCARD;
	sdp_media_set_lport_rtcp(mf->sdpm, PORT_DISCARD);

	switch (nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		sdp_media_set_lattr(mf->sdpm, false, "ice-ufrag",
				    "%s", mf->ice_ufrag);
		sdp_media_set_lattr(mf->sdpm, false, "ice-pwd",
				    "%s", mf->ice_pwd);
		break;

	default:
		break;
	}

	/* we enable support for DTLS-SRTP by default, so that the
	   SDP attributes are sent in the offer. the attributes
	   might change later though, depending on the SDP answer */

	if (cryptos & CRYPTO_DTLS_SRTP) {

		struct sa laddr_dtls;

		sa_set_str(&laddr_dtls, "0.0.0.0", 0);

		if (!mf->dtls) {
			warning("mediaflow: dtls context is missing\n");
		}

		err = dtls_listen(&mf->dtls_sock, &laddr_dtls,
				  NULL, 2, LAYER_DTLS,
				  dtls_conn_handler, mf);
		if (err) {
			warning("mediaflow: dtls_listen failed (%m)\n", err);
			goto out;
		}

		/* Virtual socket for re-directing outgoing DTLS-packet */
		err = udp_register_helper(&mf->dtls_uh,
					  dtls_udp_sock(mf->dtls_sock),
					  LAYER_DTLS_TRANSPORT,
					  send_dtls_handler,
					  NULL, mf);
		if (err)
			goto out;

		dtls_set_mtu(mf->dtls_sock, DTLS_MTU);

		err = sdp_media_set_lattr(mf->sdpm, true,
					  "fingerprint", "sha-256 %H",
					  dtls_print_sha256_fingerprint,
					  mf->dtls);
		if (err)
			goto out;

		err = sdp_media_set_lattr(mf->sdpm, true, "setup",
					  mediaflow_setup_name(mf->setup_local));
		if (err)
			goto out;
	}
	if (cryptos & CRYPTO_SDESC) {

		err  = handle_sdes_srtp_tx(mf);
		if (err)
			goto out;
	}

	/* install UDP socket helpers */
	err |= udp_register_helper(&mf->uh_srtp, mf->rtp, LAYER_SRTP,
				   udp_helper_send_handler_srtp,
				   udp_helper_recv_handler_srtp,
				   mf);
	if (err)
		goto out;

	{
		int dce_err;

		dce_err = dce_alloc(&mf->data.dce,
				    dce_send_data_handler,
				    dce_estab_handler,
				    mf);
		if (dce_err) {
			info("mediaflow: dce_alloc failed (%m)\n", dce_err);
		}
	}

	mf->laddr_default = *laddr_sdp;
	sa_set_port(&mf->laddr_default, lport);

	info("mediaflow: created new mediaflow with"
	     " local port %u and %u audio-codecs"
	     " and %s (external_rtp=%d)\n",
	     lport, list_count(aucodecl),
	     mediaflow_nat_name(mf->nat),
	     external_rtp);

 out:
	if (err)
		mem_deref(mf);
	else if (mfp)
		*mfp = mf;

	return err;
}


int mediaflow_set_setup(struct mediaflow *mf, enum media_setup setup)
{
	int err;

	if (!mf)
		return EINVAL;

	info("mediaflow: local_setup: `%s' --> `%s'\n",
	     mediaflow_setup_name(mf->setup_local), mediaflow_setup_name(setup));

	if (setup != mf->setup_local) {

		if (mf->setup_local == SETUP_ACTPASS) {

			mf->setup_local = setup;
		}
		else {
			warning("mediaflow: set_setup: Illegal transition"
				" from `%s' to `%s'\n",
				mediaflow_setup_name(mf->setup_local),
				mediaflow_setup_name(setup));
			return EPROTO;
		}
	}

	err = sdp_media_set_lattr(mf->sdpm, true, "setup",
				  mediaflow_setup_name(mf->setup_local));
	if (err)
		return err;

	if (mf->video.sdpm) {
		err = sdp_media_set_lattr(mf->video.sdpm, true,
					  "setup",
					  mediaflow_setup_name(mf->setup_local));
		if (err)
			return err;
	}

	return 0;
}


bool mediaflow_is_sdp_offerer(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->sdp_offerer;
}


enum media_setup mediaflow_local_setup(const struct mediaflow *mf)
{
	if (!mf)
		return (enum media_setup)-1;

	return mf->setup_local;
}

static bool vid_fmtp_cmp_handler(const char *params1, const char *params2,
			     void *data)
{
	struct vid_ref *vr = data;

	if (vr->vc && vr->vc->fmtp_cmph)
		return vr->vc->fmtp_cmph(params1, params2, vr->vc);

	return true;
}


static int vid_fmtp_enc_handler(struct mbuf *mb, const struct sdp_format *fmt,
				bool offer, void *data)
{
	struct sdp_format *ref_fmt = NULL;
	struct vid_ref *vr = data;

	if (!vr->vc || !vr->mf)
		return 0;
	
	if (vr->vc->codec_ref) {
		ref_fmt = sdp_media_format(vr->mf->video.sdpm, true, NULL, -1,
					   vr->vc->codec_ref->name, -1, -1);
	}

	if (vr->vc->fmtp_ench)
		return vr->vc->fmtp_ench(mb, fmt, offer, ref_fmt);
	else
		return 0;
}


int mediaflow_add_video(struct mediaflow *mf, struct list *vidcodecl)
{
	struct le *le;
	int err;

	if (!mf || !vidcodecl)
		return EINVAL;

	/* already added */
	if (mf->video.sdpm)
		return 0;

	info("mediaflow: adding video-codecs (%u)\n", list_count(vidcodecl));

	err = sdp_media_add(&mf->video.sdpm, mf->sdp, "video",
			    PORT_DISCARD,
			    sdp_profile(mf->cryptos_local));
	if (err)
		goto out;

	sdp_media_set_lbandwidth(mf->video.sdpm,
				 SDP_BANDWIDTH_AS, VIDEO_BANDWIDTH);

	/* needed for new versions of WebRTC */
	err = sdp_media_set_alt_protos(mf->video.sdpm, 2,
				       "UDP/TLS/RTP/SAVPF", "RTP/SAVPF");
	if (err)
		goto out;


	/* SDP media attributes */

	sdp_media_set_lattr(mf->video.sdpm, false, "mid", "video");
	sdp_media_set_lattr(mf->video.sdpm, false, "rtcp-mux", NULL);

	sdp_media_set_lport_rtcp(mf->video.sdpm, PORT_DISCARD);

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		sdp_media_set_lattr(mf->video.sdpm, false,
				    "ice-ufrag", "%s", mf->ice_ufrag);
		sdp_media_set_lattr(mf->video.sdpm, false,
				    "ice-pwd", "%s", mf->ice_pwd);
		break;

	default:
		break;
	}

	if (mf->dtls) {
		err = sdp_media_set_lattr(mf->video.sdpm, true,
					  "fingerprint", "sha-256 %H",
					  dtls_print_sha256_fingerprint,
					  mf->dtls);
		if (err)
			goto out;

		err = sdp_media_set_lattr(mf->video.sdpm, true,
					  "setup",
					  mediaflow_setup_name(mf->setup_local));
		if (err)
			goto out;
	}

	{
		size_t ssrcc = list_count(vidcodecl);
		uint32_t ssrcv[SSRC_MAX];
		char ssrc_group[16];
		char ssrc_fid[sizeof(ssrc_group)*SSRC_MAX + 5];
		int i = 0;
		int k = 0;

		if (ssrcc > SSRC_MAX) {
			warning("mediaflow: max %d SSRC's\n", SSRC_MAX);
			err = EOVERFLOW;
			goto out;
		}

		*ssrc_fid = '\0';

		LIST_FOREACH(vidcodecl, le) {
			struct vidcodec *vc = list_ledata(le);
			struct vid_ref *vr;
		   
			vr = mem_zalloc(sizeof(*vr), NULL);
			if (!vr)
				goto out;

			vr->mf = mf;
			vr->vc = vc;

			err = sdp_format_add(NULL, mf->video.sdpm, false,
					     vc->pt, vc->name, 90000, 1,
					     vid_fmtp_enc_handler,
					     vid_fmtp_cmp_handler,
					     vr, true,
					     "%s", vc->fmtp);
			mem_deref(vr);
			if (err) {
				
				goto out;
			}

			ssrcv[i] = rand_u32();
			re_snprintf(ssrc_group, sizeof(ssrc_group),
				    "%u ", ssrcv[i]);
			strcat(ssrc_fid, ssrc_group);
			++i;
		}
		if (strlen(ssrc_fid))
			ssrc_fid[strlen(ssrc_fid) - 1] = '\0';

		err = sdp_media_set_lattr(mf->video.sdpm, false, "ssrc-group",
					  "FID %s", ssrc_fid);
		if (err)
			goto out;

		if (ssrcc > 0)
			mf->lssrcv[MEDIA_VIDEO] = ssrcv[0];
		if (ssrcc > 1)
			mf->lssrcv[MEDIA_VIDEO_RTX] = ssrcv[1];

		for (k = 0; k < i; ++k) {
			err = sdp_media_set_lattr(mf->video.sdpm, false,
						  "ssrc", "%u cname:%s",
						  ssrcv[k], mf->cname);
			err |= sdp_media_set_lattr(mf->video.sdpm, false,
						  "ssrc", "%u msid:%s %s",
						   ssrcv[k],
						   mf->msid, mf->video.label);
			err |= sdp_media_set_lattr(mf->video.sdpm, false,
						  "ssrc", "%u mslabel:%s",
						   ssrcv[k], mf->msid);
			err |= sdp_media_set_lattr(mf->video.sdpm, false,
						  "ssrc", "%u label:%s",
						   ssrcv[k], mf->video.label);
			if (err)
				goto out;
		}
	}

 out:
	return err;
}


int mediaflow_add_data(struct mediaflow *mf)
{
	int err;

	if (!mf)
		return EINVAL;

	info("mediaflow_add_data: adding data channel\n");

	err = sdp_media_add(&mf->data.sdpm, mf->sdp, "application",
			    PORT_DISCARD,
			    "DTLS/SCTP");
	if (err)
		goto out;

	sdp_media_set_lattr(mf->data.sdpm, false, "mid", "data");

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		sdp_media_set_lattr(mf->data.sdpm, false,
				    "ice-ufrag", "%s", mf->ice_ufrag);
		sdp_media_set_lattr(mf->data.sdpm, false,
				    "ice-pwd", "%s", mf->ice_pwd);
		break;

	default:
		break;
	}

	if (mf->dtls) {
		err = sdp_media_set_lattr(mf->data.sdpm, true,
					  "fingerprint", "sha-256 %H",
					  dtls_print_sha256_fingerprint,
					  mf->dtls);
		if (err) {
			warning("mediaflow_add_data: failed to lattr "
				"'fingerprint': %m\n", err);
			
			goto out;
		}

		err = sdp_media_set_lattr(mf->data.sdpm, true,
					  "setup",
					  mediaflow_setup_name(mf->setup_local));
		if (err) {
			warning("mediaflow_add_data: failed to lattr "
				"'setup': %m\n", err);
			
			goto out;
		}
	}

	err = sdp_format_add(NULL, mf->data.sdpm, false,
			     "5000", NULL, 0, 0,
			     NULL, NULL, NULL, false, NULL);
	if (err)
		goto out;

	err = sdp_media_set_lattr(mf->data.sdpm, true,
			      "sctpmap", "5000 webrtc-datachannel 16");
	if (err) {
		warning("mediaflow_add_data: failed to add lattr: %m\n", err);
		goto out;
	}
	
 out:		
	return err;
}


void mediaflow_set_tag(struct mediaflow *mf, const char *tag)
{
	if (!mf)
		return;

	str_ncpy(mf->tag, tag, sizeof(mf->tag));
}


static int handle_setup(struct mediaflow *mf)
{
	const char *rsetup;
	enum media_setup setup_local;
	int err;

	rsetup = sdp_media_session_rattr(mf->sdpm, mf->sdp, "setup");

	info("mediaflow: remote_setup=%s\n", rsetup);

	mf->setup_remote = setup_resolve(rsetup);

	switch (mf->setup_remote) {

	case SETUP_ACTPASS:
		/* RFC 5763 setup:active is RECOMMENDED */
		if (mf->setup_local == SETUP_ACTPASS)
			setup_local = SETUP_ACTIVE;
		else
			setup_local = mf->setup_local;
		break;

	case SETUP_ACTIVE:
		setup_local = SETUP_PASSIVE;
		break;

	case SETUP_PASSIVE:
		setup_local = SETUP_ACTIVE;
		break;

	default:
		warning("mediaflow: illegal setup '%s' from remote\n", rsetup);
		return EPROTO;
	}

	info("mediaflow: local_setup=%s\n",
	     mediaflow_setup_name(mf->setup_local));

	mediaflow_set_setup(mf, setup_local);

	err = sdp_media_set_lattr(mf->sdpm, true, "setup",
				  mediaflow_setup_name(mf->setup_local));
	if (err)
		return err;

	if (mf->video.sdpm) {
		err = sdp_media_set_lattr(mf->video.sdpm, true,
					  "setup",
					  mediaflow_setup_name(mf->setup_local));
		if (err)
			return err;
	}

	if (mf->data.sdpm) {
		err = sdp_media_set_lattr(mf->data.sdpm, true,
					  "setup",
					  mediaflow_setup_name(mf->setup_local));
		if (err)
			return err;
	}

	return 0;
}


static int handle_dtls_srtp(struct mediaflow *mf)
{
	const char *fingerprint;
	struct pl fp_name;
	int err;

	fingerprint = sdp_media_session_rattr(mf->sdpm, mf->sdp,
					      "fingerprint");

	err = re_regex(fingerprint, str_len(fingerprint),
		       "[^ ]+ [0-9A-F:]*", &fp_name, NULL);
	if (err) {
		warning("mediaflow: could not parse fingerprint attr\n");
		return err;
	}

	debug("mediaflow: DTLS-SRTP fingerprint selected (%r)\n", &fp_name);

	re_printf_h *fp_printh;

	if (0 == pl_strcasecmp(&fp_name, "sha-1")) {
		fp_printh = (re_printf_h *)dtls_print_sha1_fingerprint;
	}
	else if (0 == pl_strcasecmp(&fp_name, "sha-256")) {
		fp_printh = (re_printf_h *)dtls_print_sha256_fingerprint;
	}
	else {
		warning("mediaflow: unsupported fingerprint (%r)\n", &fp_name);
		return EPROTO;
	}

	err = sdp_media_set_lattr(mf->sdpm, true, "fingerprint", "%r %H",
				  &fp_name, fp_printh, mf->dtls);
	if (err)
		return err;


	err = handle_setup(mf);
	if (err) {
		warning("mediaflow: handle_setup failed (%m)\n", err);
		return err;
	}

	debug("mediaflow: incoming SDP offer has DTLS fingerprint = '%s'\n",
	      fingerprint);

	/* DTLS has already been established, before SDP o/a */
	if (mf->crypto_ready && mf->tls_conn && !mf->crypto_verified) {

		info("mediaflow: sdp: verifying DTLS fp\n");

		if (!verify_fingerprint(mf, mf->sdp, mf->sdpm, mf->tls_conn)) {
			warning("mediaflow: dtls_srtp: could not verify"
				" remote fingerprint\n");
			return EAUTH;
		}

		mf->crypto_verified = true;
	}

	return 0;
}


static void demux_packet(struct mediaflow *mf, const struct sa *src,
			 struct mbuf *mb)
{
	enum packet pkt;
	bool hdld;

	pkt = packet_classify_packet_type(mb);

	if (mf->trice) {

		/* if the incoming UDP packet is not in the list of
		 * remote ICE candidates, we should not trust it.
		 * note that new remote candidates are added dynamically
		 * as PRFLX in the ICE-layer.
		 */
		if (!trice_rcand_find(mf->trice, ICE_COMPID_RTP,
				      IPPROTO_UDP, src)) {

			debug("mediaflow: demux: unauthorized"
				" %s packet from %J"
				" (rcand-list=%u)\n",
				packet_classify_name(pkt), src,
				list_count(trice_rcandl(mf->trice)));
		}
	}

	switch (pkt) {

	case PACKET_RTP:
	case PACKET_RTCP:
		hdld = udp_helper_recv_handler_srtp((struct sa *)src, mb, mf);
		if (!hdld) {
			warning("mediaflow: rtp packet not handled\n");
		}
		break;

	case PACKET_DTLS:
		handle_dtls_packet(mf, src, mb);
		break;

	case PACKET_STUN:
		stun_udp_recv_handler(src, mb, mf);
		break;

	default:
		warning("   @@@ udp: dropping %zu bytes from %J\n",
			  mbuf_get_left(mb), src);
		break;
	}
}


static void trice_udp_recv_handler(const struct sa *src, struct mbuf *mb,
				   void *arg)
{
	struct mediaflow *mf = arg;

	demux_packet(mf, src, mb);
}


static void interface_destructor(void *data)
{
	struct interface *ifc = data;

	list_unlink(&ifc->le);
	/*mem_deref(ifc->lcand);*/
}


/* NOTE: only ADDRESS portion of 'addr' is used */
int mediaflow_add_local_host_candidate(struct mediaflow *mf,
				       const char *ifname,
				       const struct sa *addr)
{
	// XXX: adjust local-preference here for v4/v6
	uint32_t prio = ice_cand_calc_prio(ICE_CAND_TYPE_HOST, 0, 1);
	int err = 0;

	if (!mf || !addr)
		return EINVAL;

	if (!sa_isset(addr, SA_ADDR)) {
		warning("mediaflow: add_cand: address not set\n");
		return EINVAL;
	}
	if (sa_port(addr)) {
		warning("mediaflow: add_local_host: Port should not be set\n");
		return EINVAL;
	}

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {
		struct ice_lcand *lcand = NULL;
		struct interface *ifc;

		ifc = mem_zalloc(sizeof(*ifc), interface_destructor);
		if (!ifc)
			return ENOMEM;

		if (!mf->privacy_mode) {
			err = trice_lcand_add(&lcand, mf->trice,
					      ICE_COMPID_RTP,
					      IPPROTO_UDP, prio, addr, NULL,
					      ICE_CAND_TYPE_HOST, NULL,
					      0,     /* tcptype */
					      NULL,  /* sock */
					      0);
			if (err) {
				warning("mediaflow: add_local_host[%j]"
					" failed (%m)\n",
					addr, err);
				return err;
			}

			/* hijack the UDP-socket of the local candidate
			 *
			 * NOTE: this must be done for all local candidates
			 */
			udp_handler_set(lcand->us, trice_udp_recv_handler, mf);

			err = sdp_media_set_lattr(mf->sdpm, false,
						  "candidate",
						  "%H",
						  ice_cand_attr_encode, lcand);
			if (err)
				return err;

			if (ifname) {
				str_ncpy(lcand->ifname, ifname,
					 sizeof(lcand->ifname));
			}
		}

		list_append(&mf->interfacel, &ifc->le, ifc);
		ifc->lcand = lcand;
		ifc->addr = *addr;
		if (ifname)
			str_ncpy(ifc->ifname, ifname, sizeof(ifc->ifname));
		ifc->is_default = sa_cmp(addr, &mf->laddr_default, SA_ADDR);
		ifc->mf = mf;
	}
	else {
		warning("mediaflow: add_local_host: invalid nat %d\n",
			mf->nat);
		return ENOTSUP;
	}

	return err;
}


static void set_ice_role(struct mediaflow *mf, bool controlling)
{
	if (!mf)
		return;

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {
		if (sdp_media_session_rattr(mf->sdpm, mf->sdp, "ice-lite")) {
			info("mediaflow: remote side is ice-lite"
			     " -- force controlling\n");
			controlling = true;
		}
	}

	if (mf->trice)
		trice_set_controlling(mf->trice, controlling);
}


int mediaflow_generate_offer(struct mediaflow *mf, char *sdp, size_t sz)
{
	bool offer = true;
	struct mbuf *mb = NULL;
	int err = 0;

	if (!mf || !sdp)
		return EINVAL;

	if (mf->sdp_state != SDP_IDLE) {
		warning("mediaflow: invalid sdp state %d (%s)\n",
			mf->sdp_state, __func__);
	}
	mf->sdp_state = SDP_GOFF;

	mf->sdp_offerer = true;

	set_ice_role(mf, true);

	/* for debugging */
	sdp_session_set_lattr(mf->sdp, true,
			      offer ? "x-OFFER" : "x-ANSWER", NULL);

	/* Setup the bundle, depending on usage of video or data */
	if (mf->video.sdpm && mf->data.sdpm) {
		sdp_session_set_lattr(mf->sdp, true,
				      "group", "BUNDLE audio video data");
	}
	else if (mf->video.sdpm) {
		sdp_session_set_lattr(mf->sdp, true,
				      "group", "BUNDLE audio video");	
	}
	else if (mf->data.sdpm) {
		sdp_session_set_lattr(mf->sdp, true,
				      "group", "BUNDLE audio data");		
	}

	err = sdp_encode(&mb, mf->sdp, offer);
	if (err) {
		warning("mediaflow: sdp encode(offer) failed (%m)\n", err);
		goto out;
	}

	if (re_snprintf(sdp, sz, "%b", mb->buf, mb->end) < 0) {
		err = ENOMEM;
		goto out;
	}

	debug("---------- generate SDP offer ---------\n");
	debug("%s", sdp);
	debug("---------------------------------------\n");

	mf->sent_sdp = true;

 out:
	mem_deref(mb);

	return err;
}


int mediaflow_generate_answer(struct mediaflow *mf, char *sdp, size_t sz)
{
	bool offer = false;
	struct mbuf *mb = NULL;
	int err = 0;

	if (!mf || !sdp)
		return EINVAL;

	if (mf->sdp_state != SDP_HOFF) {
		warning("mediaflow: invalid sdp state (%s)\n", __func__);
	}
	mf->sdp_state = SDP_DONE;

	mf->sdp_offerer = false;

	set_ice_role(mf, false);

	/* for debugging */
	sdp_session_set_lattr(mf->sdp, true,
			      offer ? "x-OFFER" : "x-ANSWER", NULL);

	err = sdp_encode(&mb, mf->sdp, offer);
	if (err)
		goto out;

	if (re_snprintf(sdp, sz, "%b", mb->buf, mb->end) < 0) {
		err = ENOMEM;
		goto out;
	}

	debug("---------- generate SDP answer ---------\n");
	debug("%s", sdp);
	debug("----------------------------------------\n");

	mf->sent_sdp = true;

 out:
	mem_deref(mb);

	return err;
}


/* after the SDP has been parsed,
   we can start to analyze it
   (this must be done _after_ sdp_decode() )
*/
static int post_sdp_decode(struct mediaflow *mf)
{
	const char *mid, *tool;
	int err = 0;

	if (0 == sdp_media_rport(mf->sdpm)) {
		warning("mediaflow: sdp medialine port is 0 - disabled\n");
		return EPROTO;
	}

	tool = sdp_session_rattr(mf->sdp, "tool");
	if (tool) {
		str_ncpy(mf->sdp_rtool, tool, sizeof(mf->sdp_rtool));
	}

	if (mf->trice) {

		const char *rufrag, *rpwd;

		rufrag = sdp_media_session_rattr(mf->sdpm, mf->sdp,
						 "ice-ufrag");
		rpwd   = sdp_media_session_rattr(mf->sdpm, mf->sdp,
						 "ice-pwd");
		if (!rufrag || !rpwd) {
			warning("mediaflow: post_sdp_decode: missing remote"
				" ice-ufrag/ice-pwd\n");
			warning("%H\n", sdp_session_debug, mf->sdp);
		}

		err |= trice_set_remote_ufrag(mf->trice, rufrag);
		err |= trice_set_remote_pwd(mf->trice, rpwd);
		if (err)
			goto out;

		if (sdp_media_rattr(mf->sdpm, "end-of-candidates"))
			mf->ice_remote_eoc = true;
	}

	mid = sdp_media_rattr(mf->sdpm, "mid");
	if (mid) {
		debug("mediaflow: updating mid-value to '%s'\n", mid);
		sdp_media_set_lattr(mf->sdpm, true, "mid", mid);
	}

	if (!sdp_media_rattr(mf->sdpm, "rtcp-mux")) {
		warning("mediaflow: no 'rtcp-mux' attribute in SDP"
			" -- rejecting\n");
		err = EPROTO;
		goto out;
	}

	if (mf->video.sdpm) {
		const char *group;

		mid = sdp_media_rattr(mf->video.sdpm, "mid");
		if (mid) {
			debug("mediaflow: updating video mid-value "
			      "to '%s'\n", mid);
			sdp_media_set_lattr(mf->video.sdpm,
					    true, "mid", mid);
		}

		group = sdp_session_rattr(mf->sdp, "group");
		if (group) {
			sdp_session_set_lattr(mf->sdp, true, "group", group);
		}
	}

	if (mf->data.sdpm) {
		mid = sdp_media_rattr(mf->data.sdpm, "mid");
		if (mid) {
			debug("mediaflow: updating data mid-value "
			      "to '%s'\n", mid);
			sdp_media_set_lattr(mf->data.sdpm,
					    true, "mid", mid);
		}
	}

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {
		if (sdp_media_session_rattr(mf->sdpm, mf->sdp, "ice-lite")) {
			info("mediaflow: remote side is ice-lite"
			     " -- force controlling\n");
			set_ice_role(mf, true);
		}
	}

	/*
	 * Handle negotiation about a common crypto-type
	 */

	mf->cryptos_remote = 0;
	if (sdp_media_session_rattr(mf->sdpm, mf->sdp, "fingerprint")) {

		mf->cryptos_remote |= CRYPTO_DTLS_SRTP;
	}

	if (sdp_media_rattr(mf->sdpm, "crypto")) {

		mf->cryptos_remote |= CRYPTO_SDESC;
	}

	mf->crypto = mf->cryptos_local & mf->cryptos_remote;

	info("mediaflow: negotiated crypto = %s\n",
	     crypto_name(mf->crypto));

	if (mf->cryptos_local && !mf->cryptos_remote) {
		warning("mediaflow: we offered crypto, but got none\n");
		return EPROTO;
	}

	/* check for a common crypto here, reject if nothing in common
	 */
	if (mf->cryptos_local && mf->cryptos_remote) {

		if (!mf->crypto) {

			warning("mediaflow: no common crypto in SDP offer"
				" -- rejecting\n");
			err = EPROTO;
			goto out;
		}
	}

	if (mf->crypto & CRYPTO_DTLS_SRTP &&
	    mf->crypto & CRYPTO_SDESC) {

		info("mediaflow: negotiated both cryptos, fallback to '%s'\n",
		     crypto_name(mf->crypto_fallback));
		mf->crypto = mf->crypto_fallback;
	}

	if (mf->crypto & CRYPTO_DTLS_SRTP) {

		err = handle_dtls_srtp(mf);
		if (err) {
			warning("mediaflow: handle_dtls_srtp failed (%m)\n",
				err);
			goto out;
		}

	}

	// XXX if "data"
	err = handle_setup(mf);
	if (err) {
		warning("mediaflow: handle_setup failed (%m)\n", err);
		return err;
	}

	if (mf->crypto & CRYPTO_SDESC) {
		err |= handle_sdes_srtp_rx(mf);
		if (err)
			goto out;
	}

 out:
	return err;
}


int mediaflow_handle_offer(struct mediaflow *mf, const char *sdp)
{
	struct mbuf *mbo = NULL;
	int err = 0;

	if (!mf || !sdp)
		return EINVAL;

	if (mf->sdp_state != SDP_IDLE) {
		warning("mediaflow: invalid sdp state %d (%s)\n",
			mf->sdp_state, __func__);
		return EPROTO;
	}
	mf->sdp_state = SDP_HOFF;

	++mf->stat.n_sdp_recv;

	mf->sdp_offerer = false;

	set_ice_role(mf, false);

	mbo = mbuf_alloc(1024);
	if (!mbo)
		return ENOMEM;

	err = mbuf_write_str(mbo, sdp);
	if (err)
		goto out;

	mbo->pos = 0;

	debug("---------- recv SDP offer ----------\n");
	debug("%s", sdp);
	debug("------------------------------------\n");

	err = sdp_decode(mf->sdp, mbo, true);
	if (err) {
		warning("mediaflow: could not parse SDP offer"
			" [%zu bytes] (%m)\n",
			mbo->end, err);
		goto out;
	}

	mf->got_sdp = true;

	/* after the SDP offer has been parsed,
	   we can start to analyze it */

	err = post_sdp_decode(mf);
	if (err)
		goto out;


	start_codecs(mf);

	if (sdp_media_rformat(mf->video.sdpm, NULL)) {

		info("mediaflow: SDP has video enabled\n");

		mf->video.has_media = true;
		start_video_codecs(mf);
	}
	else {
		info("mediaflow: video is disabled\n");
	}

	if (sdp_media_rformat(mf->data.sdpm, NULL)) {
		info("mediaflow: SDP has data channel\n");
		mf->data.has_media = true;
	}

 out:
	mem_deref(mbo);

	return err;
}


int mediaflow_handle_answer(struct mediaflow *mf, const char *sdp)
{
	struct mbuf *mb;
	bool offer = false;
	int err = 0;

	if (!mf || !sdp)
		return EINVAL;

	if (mf->sdp_state != SDP_GOFF) {
		warning("mediaflow: invalid sdp state (%s)\n", __func__);
	}
	mf->sdp_state = SDP_DONE;

	++mf->stat.n_sdp_recv;

	mb = mbuf_alloc(1024);
	if (!mb)
		return ENOMEM;

	err = mbuf_write_str(mb, sdp);
	if (err)
		goto out;

	mb->pos = 0;

	debug("---------- recv SDP answer ----------\n");
	debug("%s", sdp);
	debug("------------------------------------\n");

	err = sdp_decode(mf->sdp, mb, offer);
	if (err) {
		warning("mediaflow: could not parse SDP answer"
			" [%zu bytes] (%m)\n", mb->end, err);
		goto out;
	}

	mf->got_sdp = true;

	/* after the SDP has been parsed,
	   we can start to analyze it
	   (this must be done _after_ sdp_decode() )
	*/

	err = post_sdp_decode(mf);
	if (err)
		goto out;

	start_codecs(mf);

	if (sdp_media_rformat(mf->video.sdpm, NULL)) {

		info("mediaflow: SDP has video enabled\n");

		mf->video.has_media = true;
		start_video_codecs(mf);
	}
	else {
		info("mediaflow: video is disabled\n");
	}


	if (sdp_media_rformat(mf->data.sdpm, NULL)) {

		info("mediaflow: SDP has data channel\n");

		mf->data.has_media = true;
	}
	else {
		info("mediaflow: no data channel\n");
	}

 out:
	mem_deref(mb);

	return err;
}


/*
 * This function does 2 things:
 *
 * - handle offer
 * - generate answer
 */
int mediaflow_offeranswer(struct mediaflow *mf,
			  char *answer, size_t answer_sz,
			  const char *offer)
{
	int err = 0;

	if (!mf || !answer || !offer)
		return EINVAL;

	err = mediaflow_handle_offer(mf, offer);
	if (err)
		return err;

	err = mediaflow_generate_answer(mf, answer, answer_sz);
	if (err)
		return err;

	return 0;
}


void mediaflow_sdpstate_reset(struct mediaflow *mf)
{
	if (!mf)
		return;

	mf->sdp_state = SDP_IDLE;

	sdp_session_del_lattr(mf->sdp, "x-OFFER");
	sdp_session_del_lattr(mf->sdp, "x-ANSWER");

	mf->got_sdp = false;
	mf->sent_sdp = false;
}


int mediaflow_send_rtp(struct mediaflow *mf, const struct rtp_header *hdr,
		       const uint8_t *pld, size_t pldlen)
{
	struct mbuf *mb;
	size_t headroom = 0;
	int err = 0;

	if (!mf || !pld || !pldlen || !hdr)
		return EINVAL;

	MAGIC_CHECK(mf);

	/* check if media-stream is ready for sending */
	if (!mediaflow_is_ready(mf)) {
		warning("mediaflow: send_rtp: not ready\n");
		return EINTR;
	}

	headroom = get_headroom(mf);

	mb = mbuf_alloc(headroom + 256);
	if (!mb)
		return ENOMEM;

	mb->pos = headroom;
	err  = rtp_hdr_encode(mb, hdr);
	err |= mbuf_write_mem(mb, pld, pldlen);
	if (err)
		goto out;

	mb->pos = headroom;

	update_tx_stats(mf, pldlen); /* This INCLUDES the rtp header! */

	err = udp_send(mf->rtp, &mf->sel_pair->rcand->attr.addr, mb);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	return err;
}


/* NOTE: might be called from different threads */
int mediaflow_send_raw_rtp(struct mediaflow *mf, const uint8_t *buf,
			   size_t len)
{
	struct mbuf *mb;
	size_t headroom;
	int err;

	if (!mf || !buf)
		return EINVAL;

	MAGIC_CHECK(mf);

	/* check if media-stream is ready for sending */
	if (!mediaflow_is_ready(mf)) {
		warning("mediaflow: send_raw_rtp(%zu bytes): not ready"
			" [ice=%d, crypto=%d]\n",
			len, mf->ice_ready, mf->crypto_ready);
		return EINTR;
	}

	pthread_mutex_lock(&mf->mutex_enc);

	headroom = get_headroom(mf);

	mb = mbuf_alloc(headroom + len);
	if (!mb) {
		err = ENOMEM;
		goto out;
	}

	mb->pos = headroom;
	err = mbuf_write_mem(mb, buf, len);
	if (err)
		goto out;
	mb->pos = headroom;

	if (len >= RTP_HEADER_SIZE)
		update_tx_stats(mf, len - RTP_HEADER_SIZE);

	err = udp_send(mf->rtp, &mf->sel_pair->rcand->attr.addr, mb);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	pthread_mutex_unlock(&mf->mutex_enc);

	return err;
}


void mediaflow_rtp_start_send(struct mediaflow *mf)
{
	if (!mf)
		return;

	if (!mf->sent_rtp) {
		info("mediaflow: first RTP packet sent\n");
		mf->sent_rtp = true;
		check_rtpstart(mf);
	}
}


int mediaflow_send_raw_rtcp(struct mediaflow *mf,
			    const uint8_t *buf, size_t len)
{
	struct mbuf *mb;
	size_t headroom;
	int err;

	if (!mf || !buf || !len)
		return EINVAL;

	MAGIC_CHECK(mf);

	/* check if media-stream is ready for sending */
	if (!mediaflow_is_ready(mf)) {
		warning("mediaflow: send_raw_rtcp(%zu bytes): not ready"
			" [ice=%d, crypto=%d]\n",
			len, mf->ice_ready, mf->crypto_ready);
		return EINTR;
	}

	pthread_mutex_lock(&mf->mutex_enc);

	headroom = get_headroom(mf);

	mb = mbuf_alloc(headroom + 256);
	if (!mb) {
		err = ENOMEM;
		goto out;
	}

	mb->pos = headroom;
	err = mbuf_write_mem(mb, buf, len);
	if (err)
		goto out;
	mb->pos = headroom;

	err = udp_send(mf->rtp, &mf->sel_pair->rcand->attr.addr, mb);
	if (err)
		goto out;

 out:
	mem_deref(mb);

	pthread_mutex_unlock(&mf->mutex_enc);

	return err;
}


static bool rcandidate_handler(const char *name, const char *val, void *arg)
{
	struct mediaflow *mf = arg;
	struct ice_cand_attr rcand;
	int err;

	err = ice_cand_attr_decode(&rcand, val);
	if (err || rcand.compid != ICE_COMPID_RTP ||
	    rcand.proto != IPPROTO_UDP)
		goto out;

	err = trice_rcand_add(NULL, mf->trice, rcand.compid,
			      rcand.foundation, rcand.proto, rcand.prio,
			      &rcand.addr, rcand.type, rcand.tcptype);
	if (err) {
		warning("mediaflow: rcand: trice_rcand_add failed"
			" [%J] (%m)\n",
			&rcand.addr, err);
	}

 out:
	return false;
}


static void turnc_chan_handler(void *arg)
{
	struct mediaflow *mf = arg;
	(void)mf;

	info("mediaflow: TURN channel added.\n");
}


static void trice_estab_handler(struct ice_candpair *pair,
				const struct stun_msg *msg, void *arg)
{
	struct mediaflow *mf = arg;
	void *sock;
	int err;

	info("mediaflow: ice pair established  %H\n",
	     trice_candpair_debug, pair);

	/* verify local candidate */
	sock = trice_lcand_sock(mf->trice, pair->lcand);
	if (!sock) {
		warning("mediaflow: estab: lcand has no sock [%H]\n",
			trice_cand_print, pair->lcand);
		return;
	}

	/* We use the first pair that is working */
	if (!mf->ice_ready) {
		struct stun_attr *attr;
		struct turn_conn *conn;

		mem_deref(mf->sel_pair);
		mf->sel_pair = mem_ref(pair);

		mf->ice_ready = true;

		attr = stun_msg_attr(msg, STUN_ATTR_SOFTWARE);
		if (attr && !mf->peer_software) {
			(void)str_dup(&mf->peer_software, attr->v.software);
		}

		info("mediaflow: trice: setting peer to %H [%s]\n",
		     print_cand, pair->rcand,
		     mf->peer_software);

#if 1
		// TODO: extra for PRFLX
		udp_handler_set(pair->lcand->us, trice_udp_recv_handler, mf);
#endif


		// TODO: iterate over all TURN-connections
		conn = turnconn_find_allocated(&mf->turnconnl,
					       IPPROTO_UDP);
		if (conn) {
			info("mediaflow: adding TURN channel to %J\n",
			     &pair->rcand->attr.addr);

			err = turnc_add_chan(conn->turnc,
					     &pair->rcand->attr.addr,
					     turnc_chan_handler, mf);
			if (err) {
				warning("mediaflow: could not add TURN"
					" channel (%m)\n", err);
			}
		}

		ice_established_handler(mf, &pair->rcand->attr.addr);
	}
}


static bool all_failed(const struct list *lst)
{
	struct le *le;

	if (list_isempty(lst))
		return false;

	for (le = list_head(lst); le; le = le->next) {

		struct ice_candpair *pair = le->data;

		if (pair->state != ICE_CANDPAIR_FAILED)
			return false;
	}

	return true;
}


static void trice_failed_handler(int err, uint16_t scode,
				 struct ice_candpair *pair, void *arg)
{
	struct mediaflow *mf = arg;

	info("mediaflow: candpair not working [%H]\n",
	     trice_candpair_debug, pair);

	/* check if checklist is complete AND EOC */
	if (mediaflow_have_eoc(mf)) {

		if (!list_isempty(trice_validl(mf->trice)))
			return;

		if (all_failed(trice_checkl(mf->trice))) {

			int to = (int)(tmr_jiffies() - mf->ts_nat_start);

			warning("mediaflow: all pairs failed"
				" after %d milliseconds"
				" (checklist=%u, validlist=%u)\n",
				to,
				list_count(trice_checkl(mf->trice)),
				list_count(trice_validl(mf->trice))
				);

			mf->ice_ready = false;
			mf->err = EPROTO;

			tmr_start(&mf->tmr_error, 0, tmr_error_handler, mf);
		}
	}
}


/*
 * Start the mediaflow state-machine.
 *
 * this should be called after SDP exchange is complete. we will now
 * start sending ICE connectivity checks to all known remote candidates
 */
int mediaflow_start_ice(struct mediaflow *mf)
{
	struct le *le;

	if (!mf)
		return EINVAL;

	MAGIC_CHECK(mf);

	mf->ts_nat_start = tmr_jiffies();

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {

		int err;

		sdp_media_rattr_apply(mf->sdpm, "candidate",
				      rcandidate_handler, mf);

		/* add permission for ALL TURN-Clients */
		for (le = mf->turnconnl.head; le; le = le->next) {
			struct turn_conn *conn = le->data;

			if (conn->turnc && conn->turn_allocated) {
				add_permission_to_remotes_ds(mf, conn);
			}
		}

		info("mediaflow: start_ice: starting ICE checklist with"
		     " %u remote candidates\n",
		     list_count(trice_rcandl(mf->trice)));

		err = trice_checklist_start(mf->trice, mf->trice_stun,
					    ICE_INTERVAL, true,
					    trice_estab_handler,
					    trice_failed_handler,
					    mf);
		if (err) {
			warning("could not start ICE checklist (%m)\n", err);
			return err;
		}
	}

	return 0;
}


int mediaflow_add_rcand(struct mediaflow *mf, const char *sdp,
			const char *mid, int idx)
{
	struct ice_cand_attr rcand;
	struct le *le;
	struct pl pl;
	char attr[256];
	int err;

	if (!mf)
		return EINVAL;

	if (0 == str_casecmp(sdp, "a=end-of-candidates")) {
		mf->ice_remote_eoc = true;
		return 0;
	}

	if (re_regex(sdp, strlen(sdp), "candidate:[^\r\n]+", &pl)) {
		pl_set_str(&pl, sdp);
	}

	pl_strcpy(&pl, attr, sizeof(attr));

	/* ignore candidates that we cannot decode */
	if (ice_cand_attr_decode(&rcand, attr) ||
	    rcand.compid != ICE_COMPID_RTP ||
	    rcand.proto != IPPROTO_UDP)
		return 0;

	++mf->stat.n_cand_recv;

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		info("mediaflow: new remote candidate (%H)\n",
		     trice_cand_print, &rcand);

		err = trice_rcand_add(NULL, mf->trice, rcand.compid,
				      rcand.foundation, rcand.proto,
				      rcand.prio,
				      &rcand.addr, rcand.type, rcand.tcptype);
		if (err) {
			warning("mediaflow: add_rcand: trice_rcand_add failed"
				" [%J] (%m)\n",
				&rcand.addr, err);
		}

		/* add permission for ALL TURN-Clients */
		for (le = mf->turnconnl.head; le; le = le->next) {
			struct turn_conn *tc = le->data;

			if (tc->turnc && tc->turn_allocated)
				add_turn_permission(mf, tc, &rcand);
		}

		/* NOTE: checklist must be re-started for every new
		 *       remote candidate
		 */

		info("mediaflow: start_ice: starting ICE checklist with"
		     " %u remote candidates\n",
		     list_count(trice_rcandl(mf->trice)));

		err = trice_checklist_start(mf->trice, mf->trice_stun,
					    ICE_INTERVAL, true,
					    trice_estab_handler,
					    trice_failed_handler,
					    mf);
		if (err) {
			warning("could not start ICE checklist (%m)\n",
				err);
			return err;
		}
		break;

	default:
		break;
	}

	return 0;
}

static int start_audio(struct mediaflow *mf)
{
	const struct aucodec *ac;
	int err = 0;
    
	if (mf->aes == NULL)
		return ENOSYS;

	ac = auenc_get(mf->aes);
	if (ac && ac->enc_start)
		err = ac->enc_start(mf->aes);

	ac = audec_get(mf->ads);
	if (ac && ac->dec_start)
		err |= ac->dec_start(mf->ads);

	return err;
}


static int stop_audio(struct mediaflow *mf)
{
	const struct aucodec *ac;

	if (!mf)
		return EINVAL;

	/* audio */
	ac = auenc_get(mf->aes);
	if (ac && ac->enc_stop)
		ac->enc_stop(mf->aes);

	ac = audec_get(mf->ads);
	if (ac && ac->get_stats)
		ac->get_stats(mf->ads, &mf->codec_stats);
	if (ac && ac->dec_stop)
		ac->dec_stop(mf->ads);

	return 0;
}


static int hold_video(struct mediaflow *mf, bool hold)
{
	const struct vidcodec *vc;

	vc = viddec_get(mf->video.vds);
	if (vc && vc->dec_holdh) {
		info("mediaflow: hold_media: holding"
		     " video decoder (%s)\n", vc->name);

		vc->dec_holdh(mf->video.vds, hold);
	}

	vc = videnc_get(mf->video.ves);
	if (vc && vc->enc_holdh) {
		info("mediaflow: hold_media: holding"
		     " video encoder (%s)\n", vc->name);

		vc->enc_holdh(mf->video.ves, hold);
	}

	return 0;
}


int mediaflow_hold_media(struct mediaflow *mf, bool hold)
{
	int err = 0;

	if (!mf)
		return EINVAL;

	err = hold ? stop_audio(mf) : start_audio(mf);
	err |= hold_video(mf, hold);

	mf->hold = hold;

	return err;
}


int mediaflow_start_media(struct mediaflow *mf)
{
	int err = 0;

	if (!mf)
		return EINVAL;

	if (mf->hold && mf->started)
		return mediaflow_hold_media(mf, false);

	if (mf->started)
		return 0;

	mf->started = true;

	err = start_audio(mf);
	if(err){
		return err;
	}
    
	if (mf->video.has_media) {
		const struct vidcodec *vc;

		vc = viddec_get(mf->video.vds);
		if (vc && vc->dec_starth) {
			info("mediaflow: start_media: starting"
			     " video decoder (%s)\n", vc->name);

			err = vc->dec_starth(mf->video.vds);
			if (err) {
				warning("mediaflow: could not start"
					" video decoder (%m)\n", err);
				err = 0;
			}
		}

		if (mf->video.started)
			mediaflow_set_video_send_active(mf, mf->video.started);
	}

	if (!tmr_isrunning(&mf->tmr_rtp))
		tmr_start(&mf->tmr_rtp, 5000, timeout_rtp, mf);

	return err;
}

int mediaflow_set_video_send_active(struct mediaflow *mf, bool video_active)
{
	const struct vidcodec *vc;
	int err = 0;

	if (!mf->video.has_media) {
		return ENODEV;
	}

	if (video_active) {
		vc = videnc_get(mf->video.ves);
		if (vc && vc->enc_starth) {
			info("mediaflow: start_media: starting"
			     " video encoder (%s)\n", vc->name);

			err = vc->enc_starth(mf->video.ves);
			if (err) {
				warning("mediaflow: could not start"
					" video encoder (%m)\n", err);
				return err;
			}
			mf->video.started = true;
		}
	}
	else {

		vc = videnc_get(mf->video.ves);
		if (vc && vc->enc_stoph) {
			info("mediaflow: stop_media: stopping"
			     " video encoder (%s)\n", vc->name);
			vc->enc_stoph(mf->video.ves);
		}
		mf->video.started = false;
	}

	return err;
}


bool mediaflow_is_sending_video(struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->video.started;
}

void mediaflow_stop_media(struct mediaflow *mf)
{
	const struct aucodec *ac;
	const struct vidcodec *vc;

	if (!mf)
		return;

	if (!mf->started)
		return;

	mf->started = false;

	/* audio */
	ac = auenc_get(mf->aes);
	if (ac && ac->enc_stop)
		ac->enc_stop(mf->aes);

	ac = audec_get(mf->ads);
	if (ac && ac->get_stats)
		ac->get_stats(mf->ads, &mf->codec_stats);
	if (ac && ac->dec_stop)
		ac->dec_stop(mf->ads);

	/* video */
	vc = videnc_get(mf->video.ves);
	if (vc && vc->enc_stoph) {
		info("mediaflow: stop_media: stopping"
		     " video encoder (%s)\n", vc->name);
		vc->enc_stoph(mf->video.ves);
	}

	vc = viddec_get(mf->video.vds);
	if (vc && vc->dec_stoph) {
		info("mediaflow: stop_media: stopping"
		     " video decoder (%s)\n", vc->name);
		vc->dec_stoph(mf->video.vds);
	}

	tmr_cancel(&mf->tmr_rtp);
	mf->sent_rtp = false;
	mf->got_rtp = false;
}

void mediaflow_reset_media(struct mediaflow *mf)
{
	if (!mf)
		return;

	mf->ads = mem_deref(mf->ads);
	mf->aes = mem_deref(mf->aes);
	mf->mctx = NULL;

	mf->video.ves = mem_deref(mf->video.ves);
	mf->video.vds = mem_deref(mf->video.vds);
	mf->video.mctx = NULL;
}


static uint32_t calc_prio(enum ice_cand_type type, int af,
			  int turn_proto, bool turn_secure)
{
	uint16_t lpref = 0;

	switch (turn_proto) {

	case IPPROTO_UDP:
		lpref = 3;
		break;

	case IPPROTO_TCP:
		if (turn_secure)
			lpref = 1;
		else
			lpref = 2;
		break;
	}

	return ice_cand_calc_prio(type, lpref, ICE_COMPID_RTP);
}


static void submit_local_candidate(struct mediaflow *mf,
				   enum ice_cand_type type,
				   const struct sa *addr,
				   const struct sa *rel_addr, bool eoc,
				   int turn_proto, bool turn_secure,
				   void **sockp, struct udp_sock *sock)
{
	struct ice_cand_attr attr = {
		.foundation = "1",  /* NOTE: same foundation for all */
		.compid     = ICE_COMPID_RTP,
		.proto      = IPPROTO_UDP,
		.prio       = 0,
		.addr       = *addr,
		.type       = type,
		.tcptype    = 0,
	};
	char cand[512];
	size_t candc = eoc ? 2 : 1;

	struct zapi_candidate candv[] = {
		{
			.mid = "audio",
			.mline_index = 0,
			.sdp = cand,
		},
		{
			.mid = "audio",
			.mline_index = 0,
			.sdp = "a=end-of-candidates",
		},
	};

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {

		struct ice_lcand *lcand;
		int err;
		bool add;

		switch (type) {

		case ICE_CAND_TYPE_RELAY:
			add = true;
			break;

		default:
			add = !mf->privacy_mode;
			break;
		}

		if (!add) {
			debug("mediaflow: NOT adding cand %s (privacy mode)\n",
			      ice_cand_type2name(type));
			return;
		}

		attr.prio = calc_prio(type, sa_af(addr),
				      turn_proto, turn_secure);

		if (turn_proto == IPPROTO_UDP) {

		}
		else
			sock = NULL;


		err = trice_lcand_add(&lcand, mf->trice, attr.compid,
				      attr.proto, attr.prio, addr, NULL,
				      attr.type, rel_addr,
				      0 /* tcptype */,
				      sock, LAYER_ICE);
		if (err) {
			warning("mediaflow: add local cand failed (%m)\n",
				err);
			return;
		}

		if (sockp)
			*sockp = lcand->us;

		/* hijack the UDP-socket of the local candidate
		 *
		 * NOTE: this must be done for all local candidates
		 */
		udp_handler_set(lcand->us, trice_udp_recv_handler, mf);

		re_snprintf(cand, sizeof(cand), "a=candidate:%H",
			    ice_cand_attr_encode, lcand);

		/* also add the candidate to SDP */

		if (add) {
			err = sdp_media_set_lattr(mf->sdpm, false,
						  "candidate",
						  "%H",
						  ice_cand_attr_encode, lcand);
			if (err)
				return;
		}

	}
	else {
		if (rel_addr)
			attr.rel_addr = *rel_addr;

		re_snprintf(cand, sizeof(cand), "a=candidate:%H",
			    ice_cand_attr_encode, &attr);
	}

	if (mf->lcandh)
		mf->lcandh(candv, candc, mf->arg);
}


static void gather_stun_resp_handler(int err, uint16_t scode,
				     const char *reason,
				     const struct stun_msg *msg, void *arg)
{
	struct mediaflow *mf = arg;
	struct stun_attr *map = NULL, *attr;

	if (err) {
		warning("mediaflow: stun_resp %m\n", err);
		goto error;
	}

	if (scode) {
		warning("mediaflow: stun_resp %u %s\n", scode, reason);
		goto error;
	}

	map = stun_msg_attr(msg, STUN_ATTR_XOR_MAPPED_ADDR);
	if (!map) {
		warning("mediaflow: xor_mapped_addr attr missing\n");
		goto error;
	}

	mf->stun_ok = true;

	attr = stun_msg_attr(msg, STUN_ATTR_SOFTWARE);
	info("mediaflow: STUN allocation OK"
	     " (mapped=%J) [%s]\n",
	     &map->v.xor_mapped_addr,
	     attr ? attr->v.software : "");

	submit_local_candidate(mf, ICE_CAND_TYPE_SRFLX,
			       &map->v.xor_mapped_addr, &mf->laddr_default,
			       true, IPPROTO_UDP, false, NULL, mf->us_stun);

	mf->ice_local_eoc = true;
	sdp_media_set_lattr(mf->sdpm, true, "end-of-candidates", NULL);

	if (mf->gatherh)
		mf->gatherh(mf->arg);

	return;

 error:
	/* NOTE: only flag an error if ICE is not established yet */
	if (!mf->ice_ready)
		ice_error(mf, err ? err : EPROTO);
}


// TODO: should be done PER interface
int mediaflow_gather_stun(struct mediaflow *mf, const struct sa *stun_srv)
{
	struct stun *stun = NULL;
	struct sa laddr;
	void *sock = NULL;
	int err;

	if (!mf || !stun_srv)
		return EINVAL;

	if (mf->ct_gather)
		return EALREADY;

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		sa_init(&laddr, sa_af(stun_srv));

		if (!mf->trice)
			return EINVAL;

		err = udp_listen(&mf->us_stun, &laddr,
				 stun_udp_recv_handler, mf);
		if (err)
			return err;

		stun = mf->trice_stun;
		sock = mf->us_stun;
		break;

	default:
		return EINVAL;
	}

	if (!stun || !sock) {
		warning("mediaflow: gather_stun: no STUN/SOCK instance\n");
		return EINVAL;
	}

	err = stun_request(&mf->ct_gather, stun, IPPROTO_UDP,
			   sock, stun_srv, 0,
			   STUN_METHOD_BINDING, NULL, 0, false,
			   gather_stun_resp_handler, mf, 0);
	if (err) {
		warning("mediaflow: stun_request failed (%m)\n", err);
		return err;
	}

	mf->stun_server = true;

	return 0;
}


static void add_turn_permission(struct mediaflow *mf,
				struct turn_conn *conn,
				const struct ice_cand_attr *rcand)
{
	bool add;
	int err;

	if (!mf || !rcand)
		return;

	if (AF_INET != sa_af(&rcand->addr))
		return;

	if (rcand->type == ICE_CAND_TYPE_HOST)
		add = !sa_ipv4_is_private(&rcand->addr);
	else
		add = true;

	if (add) {
		info("mediaflow: adding TURN permission"
		     " to remote address %s.%j <turnconn=%p>\n",
		     ice_cand_type2name(rcand->type),
		     &rcand->addr, conn);

		err = turnconn_add_permission(conn, &rcand->addr);
		if (err) {
			warning("mediaflow: failed to"
				" add permission (%m)\n",
				err);
		}
	}
}


static void add_permissions(struct mediaflow *mf, struct turn_conn *conn)
{
	struct le *le;

	for (le = list_head(trice_rcandl(mf->trice)); le; le = le->next) {

		struct ice_rcand *rcand = le->data;

		add_turn_permission(mf, conn, &rcand->attr);
	}
}


static void add_permission_to_remotes(struct mediaflow *mf)
{
	struct turn_conn *conn;
	struct le *le;

	if (!mf)
		return;

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:

		if (!mf->trice)
			return;

		for (le = mf->turnconnl.head; le; le = le->next) {

			conn = le->data;

			if (conn->turn_allocated)
				add_permissions(mf, conn);
		}
		break;

	default:
		break;
	}
}


static void add_permission_to_remotes_ds(struct mediaflow *mf,
					 struct turn_conn *conn)
{
	struct le *le;

	if (!mf->trice)
		return;

	for (le = list_head(trice_rcandl(mf->trice)); le; le = le->next) {

		struct ice_rcand *rcand = le->data;

		add_turn_permission(mf, conn, &rcand->attr);
	}
}


/* all outgoing UDP-packets must be sent via
 * the TCP-connection to the TURN server
 */
static bool turntcp_send_handler(int *err, struct sa *dst,
				 struct mbuf *mb, void *arg)
{
	struct turn_conn *tc = arg;

	*err = turnc_send(tc->turnc, dst, mb);
	if (*err) {
		re_printf("mediaflow: turnc_send failed (%zu bytes to %J)\n",
			mbuf_get_left(mb), dst);
	}

	return true;
}


static void add_permission_to_relays(struct mediaflow *mf, struct turn_conn *conn)
{
	struct le *le;
	int err;

	for (le = mf->turnconnl.head; le; le = le->next) {

		struct turn_conn *conn_perm = le->data;

		info("mediaflow: turn: add permission to relay %j\n",
		     &conn_perm->turn_srv);

		if (AF_INET == sa_af(&conn_perm->turn_srv)) {

			err = turnconn_add_permission(conn,
						      &conn_perm->turn_srv);
			if (err) {
				warning("mediaflow: failed to"
					" add permission to %j (%m)\n",
					&conn_perm->turn_srv, err);
			}
		}
	}
}


static void turnconn_estab_handler(struct turn_conn *conn,
				   const struct sa *relay_addr,
				   const struct sa *mapped_addr,
				   const struct stun_msg *msg, void *arg)
{
	struct mediaflow *mf = arg;
	void *sock = NULL;
	int err;
	(void)msg;

	info("mediaflow: TURN established (%J)\n", relay_addr);

	if (mf->mf_stats.turn_alloc < 0 &&
	    conn->ts_turn_resp &&
	    conn->ts_turn_req) {

		mf->mf_stats.turn_alloc = conn->ts_turn_resp
			- conn->ts_turn_req;
	}

	if (mf->nat == MEDIAFLOW_TRICKLEICE_DUALSTACK) {

		sdp_media_set_laddr(mf->sdpm, relay_addr);
		sdp_media_set_laddr(mf->video.sdpm, relay_addr);

		add_permission_to_relays(mf, conn);
	}


	/* NOTE: important to ship the SRFLX before RELAY cand. */

	if (conn->proto == IPPROTO_UDP) {
		submit_local_candidate(mf, ICE_CAND_TYPE_SRFLX,
				       mapped_addr, &mf->laddr_default, false,
				       conn->proto, conn->secure, NULL,
				       conn->us_turn);
	}

	submit_local_candidate(mf, ICE_CAND_TYPE_RELAY,
			       relay_addr, mapped_addr, true,
			       conn->proto, conn->secure, &sock,
			       conn->us_turn);

	if (conn->proto == IPPROTO_TCP) {
		/* NOTE: this is needed to snap up outgoing UDP-packets */
		conn->us_app = mem_ref(sock);
		err = udp_register_helper(&conn->uh_app, sock, LAYER_TURN,
					  turntcp_send_handler, NULL, conn);
		if (err) {
			warning("mediaflow: TURN failed to register UDP-helper"
				" (%m)\n", err);
			goto error;
		}
	}

	mf->ice_local_eoc = true;
	sdp_media_set_lattr(mf->sdpm, true, "end-of-candidates", NULL);

	add_permission_to_remotes_ds(mf, conn);
	add_permission_to_remotes(mf);

	/* NOTE: must be called last, since app might deref mediaflow */
	if (mf->gatherh)
		mf->gatherh(mf->arg);


	return;

 error:
	/* NOTE: only flag an error if ICE is not established yet */
	if (!mf->ice_ready)
		ice_error(mf, err ? err : EPROTO);
}


/* incoming packets over TURN - demultiplex to the right module */
static void turnconn_data_handler(struct turn_conn *conn, const struct sa *src,
				  struct mbuf *mb, void *arg)
{
	struct mediaflow *mf = arg;
	struct ice_lcand *lcand;
	enum packet pkt;

	pkt = packet_classify_packet_type(mb);

	if (pkt == PACKET_STUN) {

		debug("mediaflow: incoming STUN-packet via TURN\n");

		// TODO: this supports only one TURN-client for now
		//       add support for multiple clients
		lcand = trice_lcand_find2(mf->trice,
					  ICE_CAND_TYPE_RELAY, sa_af(src));
		if (lcand) {

			/* forward packet to ICE */
			trice_lcand_recv_packet(lcand, src, mb);
		}
		else {
			debug("mediaflow: turnconn: no local candidate\n");
			demux_packet(mf, src, mb);
		}
	}
	else {
		demux_packet(mf, src, mb);
	}
}


static void turnconn_error_handler(int err, void *arg)
{
	struct mediaflow *mf = arg;

	warning("mediaflow: turnconn_error:  turnconnl=%u  (%m)\n",
		list_count(&mf->turnconnl), err);

	if (list_count(&mf->turnconnl) > 1 ||
	    turnconn_is_one_allocated(&mf->turnconnl)) {

		info("mediaflow: ignoring turn error, already have 1\n");
		return;
	}

	/* NOTE: only flag an error if ICE is not established yet */
	if (!mf->ice_ready)
		ice_error(mf, err ? err : EPROTO);
}


/*
 * Gather RELAY and SRFLX candidates (UDP only)
 */
int mediaflow_gather_turn(struct mediaflow *mf, const struct sa *turn_srv,
			  const char *username, const char *password)
{
	struct sa turn_srv6;
	void *sock = NULL;
	int err;

	if (!mf || !turn_srv)
		return EINVAL;

	if (!sa_isset(turn_srv, SA_ALL)) {
		warning("mediaflow: gather_turn: TURN server is not set\n");
		return EINVAL;
	}

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		if (!mf->trice)
			return EINVAL;

		/* NOTE: this should only be done if we detect that
		 *       we are behind a NAT64
		 */
		if (mf->af != sa_af(turn_srv)) {

			err = sa_translate_nat64(&turn_srv6, turn_srv);
			if (err) {
				warning("gather_turn: sa_translate_nat64(%j)"
					" failed (%m)\n",
					turn_srv, err);
				return err;
			}

			info("mediaflow: Dualstack: TRANSLATE NAT64"
			     " (%J ----> %J)\n",
			     turn_srv, &turn_srv6);

			turn_srv = &turn_srv6;
		}
		break;

	default:
		return EINVAL;
	}

	info("mediaflow: gather_turn: username='%s' srv=%J\n",
	     username, turn_srv);

	err = turnconn_alloc(NULL, &mf->turnconnl,
			     turn_srv, IPPROTO_UDP, false,
			     username, password,
			     mf->af, sock,
			     LAYER_STUN, LAYER_TURN,
			     turnconn_estab_handler,
			     turnconn_data_handler,
			     turnconn_error_handler, mf
			     );
	if (err) {
		warning("mediaflow: turnc_alloc failed (%m)\n", err);
		return err;
	}

	return 0;
}


/*
 * Add a new TURN-server and gather RELAY candidates (TCP or TLS)
 */
int mediaflow_gather_turn_tcp(struct mediaflow *mf, const struct sa *turn_srv,
			      const char *username, const char *password,
			      bool secure)
{
	struct turn_conn *tc;
	int err = 0;

	if (!mf || !turn_srv)
		return EINVAL;

	if (mf->nat != MEDIAFLOW_TRICKLEICE_DUALSTACK) {
		warning("gather_turn_tcp: only implemented for DS\n");
		return EINVAL;
	}

	err = turnconn_alloc(&tc, &mf->turnconnl,
			     turn_srv, IPPROTO_TCP, secure,
			     username, password,
			     mf->af, NULL,
			     LAYER_STUN, LAYER_TURN,
			     turnconn_estab_handler,
			     turnconn_data_handler,
			     turnconn_error_handler, mf
			     );
	if (err)
		return err;

	return err;
}


size_t mediaflow_remote_cand_count(const struct mediaflow *mf)
{
	if (!mf)
		return 0;

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		return list_count(trice_rcandl(mf->trice));

	default:
		return 0;
	}
}


void mediaflow_set_fallback_crypto(struct mediaflow *mf, enum media_crypto cry)
{
	if (!mf)
		return;

	mf->crypto_fallback = cry;
}


enum media_crypto mediaflow_crypto(const struct mediaflow *mf)
{
	return mf ? mf->crypto : CRYPTO_NONE;
}


struct auenc_state *mediaflow_encoder(const struct mediaflow *mf)
{
	return mf ? mf->aes : NULL;
}


struct audec_state *mediaflow_decoder(const struct mediaflow *mf)
{
	return mf ? mf->ads : NULL;
}


struct videnc_state *mediaflow_video_encoder(const struct mediaflow *mf)
{
	return mf ? mf->video.ves : NULL;
}


struct viddec_state *mediaflow_video_decoder(const struct mediaflow *mf)
{
	return mf ? mf->video.vds : NULL;
}


int mediaflow_debug(struct re_printf *pf, const struct mediaflow *mf)
{
	struct ice_rcand *rcand = NULL;
	int err = 0;
	char nat_letter = ' ';

	if (!mf)
		return 0;

	nat_letter = mf->ice_ready ? 'I' : ' ';

	if (mf->sel_pair)
		rcand = mf->sel_pair->rcand;

	err = re_hprintf(pf, "%c%c%c%c%c ice=%s-%s.%J [%s] tx=%zu rx=%zu",
			 mf->got_sdp ? 'S' : ' ',
			 nat_letter,
			 mf->crypto_ready ? 'D' : ' ',
			 mediaflow_is_rtpstarted(mf) ? 'R' : ' ',
			 mf->data.ready ? 'C' : ' ',
			 mediaflow_lcand_name(mf),
			 rcand ? ice_cand_type2name(rcand->attr.type) : "?",
			 rcand ? &rcand->attr.addr : NULL,
			 mf->peer_software,
			 mf->stat.tx.bytes,
			 mf->stat.rx.bytes);

	return err;
}


void mediaflow_set_rtpstate_handler(struct mediaflow *mf,
				      mediaflow_rtp_state_h *rtpstateh)
{
	if (!mf)
		return;

	mf->rtpstateh = rtpstateh;
}


const char *mediaflow_peer_software(const struct mediaflow *mf)
{
	return mf ? mf->peer_software : NULL;
}


bool mediaflow_has_video(const struct mediaflow *mf)
{
	return mf ? mf->video.has_media : false;
}


bool mediaflow_has_data(const struct mediaflow *mf)
{
	return mf ? mf->data.sdpm != NULL : false;
}

int mediaflow_video_debug(struct re_printf *pf, const struct mediaflow *mf)
{
	if (!mf)
		return 0;

	if (mf->video.vds) {
		const struct vidcodec *vc = viddec_get(mf->video.vds);

		if (vc->dec_debugh)
			return vc->dec_debugh(pf, mf->video.vds);
	}

	return 0;
}


const struct tls_conn *mediaflow_dtls_connection(const struct mediaflow *mf)
{
	return mf ? mf->tls_conn : NULL;
}


bool mediaflow_is_started(const struct mediaflow *mf)
{
	return mf ? mf->started : false;
}


void mediaflow_set_gather_handler(struct mediaflow *mf,
				  mediaflow_gather_h *gatherh)
{
	if (!mf)
		return;

	mf->gatherh = gatherh;
}


bool mediaflow_got_sdp(const struct mediaflow *mf)
{
	return mf ? mf->got_sdp : false;
}


/*
 * return TRUE if one SDP sent AND one SDP received
 */
bool mediaflow_sdp_is_complete(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->got_sdp && mf->sent_sdp;
}


bool mediaflow_is_gathered(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	debug("mediaflow: is_gathered:  turnconnl=%u  stun=%d/%d\n",
	      list_count(&mf->turnconnl),
	      mf->stun_server, mf->stun_ok);

	switch (mf->nat) {

	case MEDIAFLOW_TRICKLEICE_DUALSTACK:
		if (!list_isempty(&mf->turnconnl))
			return turnconn_is_one_allocated(&mf->turnconnl);

		if (mf->stun_server)
			return mf->stun_ok;
		return true;

	default:
		return false;
	}

	return true;
}


uint32_t mediaflow_get_local_ssrc(struct mediaflow *mf, enum media_type type)
{
	if (!mf || type >= MEDIA_NUM)
		return 0;

	return mf->lssrcv[type];
}


int mediaflow_get_remote_ssrc(const struct mediaflow *mf, enum media_type type,
			      uint32_t *ssrcp)
{
	struct sdp_media *sdpm;
	const char *rssrc;
	struct pl pl_ssrc;
	int err;

	sdpm = type == MEDIA_AUDIO ? mf->sdpm : mf->video.sdpm;

	rssrc = sdp_media_rattr(sdpm, "ssrc");
	if (!rssrc)
		return ENOENT;

	err = re_regex(rssrc, str_len(rssrc), "[0-9]+", &pl_ssrc);
	if (err)
		return err;

	*ssrcp = pl_u32(&pl_ssrc);

	return 0;
}


bool mediaflow_dtls_ready(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->crypto_ready;
}


bool mediaflow_ice_ready(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->ice_ready;
}


const struct rtp_stats* mediaflow_rcv_audio_rtp_stats(const struct mediaflow *mf)
{
	if (!mf)
		return NULL;

	return &mf->audio_stats_rcv;
}


const struct rtp_stats* mediaflow_snd_audio_rtp_stats(const struct mediaflow *mf)
{
	if (!mf)
		return NULL;

	return &mf->audio_stats_snd;
}


const struct rtp_stats* mediaflow_rcv_video_rtp_stats(const struct mediaflow *mf)
{
	if (!mf)
		return NULL;

	return &mf->video_stats_rcv;
}


const struct rtp_stats* mediaflow_snd_video_rtp_stats(const struct mediaflow *mf)
{
	if (!mf)
		return NULL;

	return &mf->video_stats_snd;
}


struct aucodec_stats *mediaflow_codec_stats(struct mediaflow *mf)
{
	const struct aucodec *ac;

	if (!mf)
		return NULL;

	ac = audec_get(mf->ads);
	if (ac && ac->get_stats)
		ac->get_stats(mf->ads, &mf->codec_stats);

	return &mf->codec_stats;
}


const struct mediaflow_stats *mediaflow_stats_get(const struct mediaflow *mf)
{
	return mf ? &mf->mf_stats : NULL;
}

int32_t mediaflow_get_media_time(const struct mediaflow *mf)
{
	if (!mf)
		return -1;
    
	int dur_rx = (mf->stat.rx.ts_last - mf->stat.rx.ts_first);
    
	return dur_rx;
}

void mediaflow_set_local_eoc(struct mediaflow *mf)
{
	if (!mf)
		return;

	mf->ice_local_eoc = true;
	sdp_media_set_lattr(mf->sdpm, true, "end-of-candidates", NULL);
}


bool mediaflow_have_eoc(const struct mediaflow *mf)
{
	if (!mf)
		return false;

	return mf->ice_local_eoc && mf->ice_remote_eoc;
}


void mediaflow_enable_privacy(struct mediaflow *mf, bool enabled)
{
	if (!mf)
		return;

	mf->privacy_mode = enabled;

	if (mf->trice) {
		trice_conf(mf->trice)->enable_prflx = !enabled;
	}
}


const char *mediaflow_lcand_name(const struct mediaflow *mf)
{
	struct ice_lcand *lcand;

	if (!mf)
		return NULL;

	if (!mf->sel_pair)
		return "???";

	lcand = mf->sel_pair->lcand;

	if (lcand)
		return ice_cand_type2name(lcand->attr.type);
	else
		return "???";
}


const char *mediaflow_rcand_name(const struct mediaflow *mf)
{
	if (!mf)
		return NULL;

	if (!mf->sel_pair)
		return "???";

	return ice_cand_type2name(mf->sel_pair->rcand->attr.type);
}


struct dce *mediaflow_get_dce(const struct mediaflow *mf)
{
	if (!mf || !mf->data.dce)
		return NULL;
    
	return mf->data.dce;
}

bool mediaflow_get_audio_cbr(const struct mediaflow *mf)
{
	if (!mf)
		return false;
    
	return mf->audio.cbr;
}
