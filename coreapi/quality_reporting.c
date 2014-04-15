/*
linphone
Copyright (C) 2014 - Belledonne Communications, Grenoble, France

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include "linphonecore.h"
#include "private.h"
#include "sal/sal.h"


#include "ortp/rtpsession.h"

#include <math.h>

/***************************************************************************
 *  				TODO / REMINDER LIST
 ****************************************************************************/
	// TO_CHECK: only if call succeeded and ran (busy should NOT call this)
	// TO_CHECK: executed AFTER BYE's "OK" response has been received

	// TO_CHECK: configurable global publish_call_statistics linphone_proxy_config_set_statistics_collector enable_collector
 	
 	// For codecs that are able to change sample rates, the lowest and highest sample rates MUST be reported (e.g., 8000;16000).

 // range 0 - 255 au lieu de 0 - 5 metrics->quality_estimates.rcq, metrics->quality_estimates.moslq, metrics->quality_estimates.moscq);
 // abstraction audio video
 // tests liblinphonetester
 	// à voir ++ :
		// video : que se passe t-il si on arrete / resume la vidéo (new stream)
 		// valeurs instanannées : moyenne ? valeur extreme ?
 		// rlq: il faut un algo
 // #define PRINTF printf
 #define PRINTF(...) 
/***************************************************************************
 *  				END OF TODO / REMINDER LIST
 ****************************************************************************/

#define strass(dest, src) {\
	if (dest != NULL) \
		ms_free(dest); \
	dest = src; \
}

// since printf family functions are LOCALE dependent, float separator may differ
// depending on the user's locale (LC_NUMERIC env var).
static char * float_to_one_decimal_string(float f) {
	float rounded_f = floorf(f * 10 + .5f) / 10;

	int floor_part = (int) rounded_f;
	int one_decimal_part = floorf (10 * (rounded_f - floor_part) + .5f);

	return ms_strdup_printf(_("%d.%d"), floor_part, one_decimal_part);
}

static void append_to_buffer_valist(char **buff, size_t *buff_size, size_t *offset, const char *fmt, va_list args) {
	belle_sip_error_code ret;
	size_t prevoffset = *offset;

	#ifndef WIN32
        va_list cap;/*copy of our argument list: a va_list cannot be re-used (SIGSEGV on linux 64 bits)*/
        va_copy(cap,args);
		ret = belle_sip_snprintf_valist(*buff, *buff_size, offset, fmt, cap);
        va_end(cap);
	#else
		ret = belle_sip_snprintf_valist(*buff, *buff_size, offset, fmt, args);
	#endif

	// if we are out of memory, we add some size to buffer
	if (ret == BELLE_SIP_BUFFER_OVERFLOW) {
		ms_warning("Buffer was too small to contain the whole report - doubling its size from %lu to %lu", *buff_size, 2 * *buff_size);
		*buff_size += 2048;
		*buff = (char *) ms_realloc(*buff, *buff_size);

		*offset = prevoffset;
		// recall myself since we did not write all things into the buffer but
		// only a part of it
		append_to_buffer_valist(buff, buff_size, offset, fmt, args);
	}
}

static void append_to_buffer(char **buff, size_t *buff_size, size_t *offset, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	append_to_buffer_valist(buff, buff_size, offset, fmt, args);
	va_end(args);
}


#define APPEND_IF_NOT_NULL_STR(buffer, size, offset, fmt, arg) if (arg != NULL) append_to_buffer(buffer, size, offset, fmt, arg)
#define APPEND_IF_NUM_IN_RANGE(buffer, size, offset, fmt, arg, inf, sup) if (inf <= arg && arg <= sup) append_to_buffer(buffer, size, offset, fmt, arg)
#define APPEND_IF(buffer, size, offset, fmt, arg, cond) if (cond) append_to_buffer(buffer, size, offset, fmt, arg)
#define IF_NUM_IN_RANGE(num, inf, sup, statement) if (inf <= num && num <= sup) statement

static void append_metrics_to_buffer(char ** buffer, size_t * size, size_t * offset, const reporting_content_metrics_t rm) {
	char * timestamps_start_str = NULL;
	char * timestamps_stop_str = NULL;
	char * network_packet_loss_rate_str = NULL;
	char * jitter_buffer_discard_rate_str = NULL;
	// char * gap_loss_density_str = NULL;
	char * moslq_str = NULL;
	char * moscq_str = NULL;

	if (rm.timestamps.start > 0) 
		timestamps_start_str = linphone_timestamp_to_rfc3339_string(rm.timestamps.start);
	if (rm.timestamps.stop > 0) 
		timestamps_stop_str = linphone_timestamp_to_rfc3339_string(rm.timestamps.stop);

	IF_NUM_IN_RANGE(rm.packet_loss.network_packet_loss_rate, 0, 255, network_packet_loss_rate_str = float_to_one_decimal_string(rm.packet_loss.network_packet_loss_rate / 256));
	IF_NUM_IN_RANGE(rm.packet_loss.jitter_buffer_discard_rate, 0, 255, jitter_buffer_discard_rate_str = float_to_one_decimal_string(rm.packet_loss.jitter_buffer_discard_rate / 256));
	// IF_NUM_IN_RANGE(rm.burst_gap_loss.gap_loss_density, 0, 10, gap_loss_density_str = float_to_one_decimal_string(rm.burst_gap_loss.gap_loss_density));
	IF_NUM_IN_RANGE(rm.quality_estimates.moslq, 1, 5, moslq_str = float_to_one_decimal_string(rm.quality_estimates.moslq));
	IF_NUM_IN_RANGE(rm.quality_estimates.moscq, 1, 5, moscq_str = float_to_one_decimal_string(rm.quality_estimates.moscq));

	append_to_buffer(buffer, size, offset, "Timestamps:");
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " START=%s", timestamps_start_str);
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " STOP=%s", timestamps_stop_str);

	append_to_buffer(buffer, size, offset, "\r\nSessionDesc:");
		APPEND_IF(buffer, size, offset, " PT=%d", rm.session_description.payload_type, rm.session_description.payload_type != -1);
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " PD=%s", rm.session_description.payload_desc);
		APPEND_IF(buffer, size, offset, " SR=%d", rm.session_description.sample_rate, rm.session_description.sample_rate != -1);
		APPEND_IF(buffer, size, offset, " FD=%d", rm.session_description.frame_duration, rm.session_description.frame_duration != -1);
		// append_to_buffer(buffer, size, offset, " FO=%d", rm.session_description.frame_ocets);
		// append_to_buffer(buffer, size, offset, " FPP=%d", rm.session_description.frames_per_sec);
		// append_to_buffer(buffer, size, offset, " PPS=%d", rm.session_description.packets_per_sec);
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " FMTP=\"%s\"", rm.session_description.fmtp);
		APPEND_IF(buffer, size, offset, " PLC=%d", rm.session_description.packet_loss_concealment, rm.session_description.packet_loss_concealment != -1);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " SSUP=%s", rm.session_description.silence_suppression_state);

	append_to_buffer(buffer, size, offset, "\r\nJitterBuffer:");
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " JBA=%d", rm.jitter_buffer.adaptive, 0, 3);
		// APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " JBR=%d", rm.jitter_buffer.rate, 0, 15);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " JBN=%d", rm.jitter_buffer.nominal, 0, 65535);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " JBM=%d", rm.jitter_buffer.max, 0, 65535);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " JBX=%d",  rm.jitter_buffer.abs_max, 0, 65535); 

	append_to_buffer(buffer, size, offset, "\r\nPacketLoss:");
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " NLR=%s", network_packet_loss_rate_str);
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " JDR=%s", jitter_buffer_discard_rate_str);

	// append_to_buffer(buffer, size, offset, "\r\nBurstGapLoss:");
	// 	append_to_buffer(buffer, size, offset, " BLD=%d", rm.burst_gap_loss.burst_loss_density);
	// 	append_to_buffer(buffer, size, offset, " BD=%d", rm.burst_gap_loss.burst_duration);
	// 	APPEND_IF_NOT_NULL_STR(buffer, size, offset, " GLD=%s", gap_loss_density_str);
	// 	append_to_buffer(buffer, size, offset, " GD=%d", rm.burst_gap_loss.gap_duration);
	// 	append_to_buffer(buffer, size, offset, " GMIN=%d", rm.burst_gap_loss.min_gap_threshold); 

	append_to_buffer(buffer, size, offset, "\r\nDelay:");
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " RTD=%d", rm.delay.round_trip_delay, 0, 65535);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " ESD=%d", rm.delay.end_system_delay, 0, 65535);
		// APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " OWD=%d", rm.delay.one_way_delay, 0, 65535);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " SOWD=%d", rm.delay.symm_one_way_delay, 0, 65535);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " IAJ=%d", rm.delay.interarrival_jitter, 0, 65535);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " MAJ=%d", rm.delay.mean_abs_jitter, 0, 65535);

	append_to_buffer(buffer, size, offset, "\r\nSignal:");
		APPEND_IF(buffer, size, offset, " SL=%d", rm.signal.level, rm.signal.level != 127);
		APPEND_IF(buffer, size, offset, " NL=%d", rm.signal.noise_level, rm.signal.noise_level != 127);
		// append_to_buffer(buffer, size, offset, " RERL=%d", rm.signal.residual_echo_return_loss);

	append_to_buffer(buffer, size, offset, "\r\nQualityEst:");
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " RLQ=%d", rm.quality_estimates.rlq, 1, 120);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " RLQEstAlg=%s", rm.quality_estimates.rlqestalg);
		APPEND_IF_NUM_IN_RANGE(buffer, size, offset, " RCQ=%d", rm.quality_estimates.rcq, 1, 120);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " RCQEstAlgo=%s", rm.quality_estimates.rcqestalg);
		// append_to_buffer(buffer, size, offset, " EXTRI=%d", rm.quality_estimates.extri);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " ExtRIEstAlg=%s", rm.quality_estimates.extriestalg);
		// append_to_buffer(buffer, size, offset, " EXTRO=%d", rm.quality_estimates.extro);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " ExtROEstAlg=%s", rm.quality_estimates.extroutestalg);
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " MOSLQ=%s", moslq_str);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " MOSLQEstAlgo=%s", rm.quality_estimates.moslqestalg);
		APPEND_IF_NOT_NULL_STR(buffer, size, offset, " MOSCQ=%s", moscq_str);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " MOSCQEstAlgo=%s", rm.quality_estimates.moscqestalg);
		// APPEND_IF_NOT_NULL_STR(buffer, size, offset, " QoEEstAlg=%s", rm.quality_estimates.qoestalg);
	append_to_buffer(buffer, size, offset, "\r\n");

	ms_free(timestamps_start_str);
	ms_free(timestamps_stop_str);
	ms_free(network_packet_loss_rate_str);
	ms_free(jitter_buffer_discard_rate_str);
	// ms_free(gap_loss_density_str);
	ms_free(moslq_str);
	ms_free(moscq_str);
}

static void reporting_publish(const LinphoneCall* call, const reporting_session_report_t * report) {
	PRINTF("static reporting_publish\n");

	LinphoneContent content = {0};
 	LinphoneAddress *addr;
	int expires = -1;
	size_t offset = 0;
	size_t size = 2048;
	char * buffer;

	buffer = (char *) ms_malloc(size);

	content.type = ms_strdup("application");
	content.subtype = ms_strdup("vq-rtcpxr");

	append_to_buffer(&buffer, &size, &offset, "VQSessionReport: CallTerm\r\n");
	append_to_buffer(&buffer, &size, &offset, "CallID: %s\r\n", report->info.call_id);
	append_to_buffer(&buffer, &size, &offset, "LocalID: %s\r\n", report->info.local_id);
	append_to_buffer(&buffer, &size, &offset, "RemoteID: %s\r\n", report->info.remote_id);
	append_to_buffer(&buffer, &size, &offset, "OrigID: %s\r\n", report->info.orig_id);

	APPEND_IF_NOT_NULL_STR(&buffer, &size, &offset, "LocalGroup: %s\r\n", report->info.local_group);
	APPEND_IF_NOT_NULL_STR(&buffer, &size, &offset, "RemoteGroup: %s\r\n", report->info.remote_group);
	append_to_buffer(&buffer, &size, &offset, "LocalAddr: IP=%s PORT=%d SSRC=%d\r\n", report->info.local_addr.ip, report->info.local_addr.port, report->info.local_addr.ssrc);
	APPEND_IF_NOT_NULL_STR(&buffer, &size, &offset, "LocalMAC: %s\r\n", report->info.local_mac_addr);
	append_to_buffer(&buffer, &size, &offset, "RemoteAddr: IP=%s PORT=%d SSRC=%d\r\n", report->info.remote_addr.ip, report->info.remote_addr.port, report->info.remote_addr.ssrc);
	APPEND_IF_NOT_NULL_STR(&buffer, &size, &offset, "RemoteMAC: %s\r\n", report->info.remote_mac_addr);
	
	append_to_buffer(&buffer, &size, &offset, "LocalMetrics:\r\n");
	append_metrics_to_buffer(&buffer, &size, &offset, report->local_metrics);
		
	append_to_buffer(&buffer, &size, &offset, "RemoteMetrics:\r\n");
	append_metrics_to_buffer(&buffer, &size, &offset, report->remote_metrics);
	APPEND_IF_NOT_NULL_STR(&buffer, &size, &offset, "DialogID: %s\r\n", report->dialog_id);

	content.data = buffer;
	content.size = strlen((char*)content.data);


	addr = linphone_address_new(call->dest_proxy->reg_statistics_collector);
	if (addr != NULL) {
		linphone_core_publish(call->core, addr, "vq-rtcpxr", expires, &content);
		linphone_address_destroy(addr);
	
		// for debug purpose only
		PRINTF("%s\n", (char*) content.data);
	} else {
		ms_warning("Asked to submit reporting statistics but no collector address found");
		PRINTF("Asked to submit reporting statistics but no collector address found\n");
	}

	linphone_content_uninit(&content);
}

static const SalStreamDescription * get_media_stream_for_desc(const SalMediaDescription * remote_smd, SalStreamType sal_stream_type) {
	if (remote_smd != NULL) {
		int count;
		for (count = 0; count < remote_smd->n_total_streams; ++count) {
			if (remote_smd->streams[count].type == sal_stream_type) {
				return &remote_smd->streams[count];
			}
		}
		if (remote_smd == NULL || count == remote_smd->n_total_streams) {
			ms_warning("Could not find the associated stream of type %d for remote desc", sal_stream_type);
		}
	}

	return NULL;
}

static void reporting_update_ip(LinphoneCall * call, int stats_type) {
	SalStreamType sal_stream_type = (stats_type == LINPHONE_CALL_STATS_AUDIO) ? SalAudio : SalVideo;
	if (call->log->reports[stats_type] != NULL) {
		const SalStreamDescription * local_desc = get_media_stream_for_desc(call->localdesc, sal_stream_type);
		const SalStreamDescription * remote_desc = get_media_stream_for_desc(sal_call_get_remote_media_description(call->op), sal_stream_type);
		
		// local info are always up-to-date and correct
		if (local_desc != NULL) {
			call->log->reports[stats_type]->info.local_addr.port = local_desc->rtp_port;
			strass(call->log->reports[stats_type]->info.local_addr.ip, ms_strdup(local_desc->rtp_addr));
		}

		if (remote_desc != NULL) {
			// port is always stored in stream description struct
			call->log->reports[stats_type]->info.remote_addr.port = remote_desc->rtp_port;

			// for IP it can be not set if we are using a direct route
			if (remote_desc->rtp_addr != NULL && strlen(remote_desc->rtp_addr) > 0) {
				strass(call->log->reports[stats_type]->info.remote_addr.ip, ms_strdup(remote_desc->rtp_addr));
			} else {
				strass(call->log->reports[stats_type]->info.remote_addr.ip, ms_strdup(sal_call_get_remote_media_description(call->op)->addr));
			}
		}
	}
}

static bool_t reporting_enabled(const LinphoneCall * call) {
	return (call->dest_proxy != NULL && linphone_proxy_config_send_statistics_enabled(call->dest_proxy));
}

void linphone_reporting_update_ip(LinphoneCall * call) {
	// This function can be called in two different cases:
	// - 1) at start when call is starting, remote ip/port info might be the proxy ones to which callee is registered
	// - 2) later, if we found a direct route between caller and callee with ICE/Stun, ip/port are updated for the direct route access

	PRINTF("linphone_reporting_update_remote_ip\n");

	if (! reporting_enabled(call)) 
		return;

	reporting_update_ip(call, LINPHONE_CALL_STATS_AUDIO);

	if (linphone_call_params_video_enabled(linphone_call_get_current_params(call))) {
		reporting_update_ip(call, LINPHONE_CALL_STATS_VIDEO);
	}
}

void linphone_reporting_update(LinphoneCall * call, int stats_type) {
	reporting_session_report_t * report = call->log->reports[stats_type];
	MediaStream * stream = NULL;
	const PayloadType * local_payload = NULL;
	const PayloadType * remote_payload = NULL;
	const LinphoneCallParams * current_params = linphone_call_get_current_params(call);

	PRINTF("linphone_reporting_call_stats_updated type=%d\n", stats_type);

	if (! reporting_enabled(call)) 
		return;

	strass(report->info.call_id, ms_strdup(call->log->call_id));
	strass(report->info.local_group, ms_strdup_printf(_("linphone-%s-%s"), linphone_core_get_user_agent_name(), report->info.call_id));
	strass(report->info.remote_group, ms_strdup_printf(_("linphone-%s-%s"), linphone_call_get_remote_user_agent(call), report->info.call_id));

	if (call->dir == LinphoneCallIncoming) {
		strass(report->info.remote_id, linphone_address_as_string(call->log->from));
		strass(report->info.local_id, linphone_address_as_string(call->log->to));
		strass(report->info.orig_id, ms_strdup(report->info.remote_id));
	} else {
		strass(report->info.remote_id, linphone_address_as_string(call->log->to));
		strass(report->info.local_id, linphone_address_as_string(call->log->from));
		strass(report->info.orig_id, ms_strdup(report->info.local_id));
	}

	strass(report->dialog_id, sal_op_get_dialog_id(call->op));

	report->local_metrics.timestamps.start = call->log->start_date_time;
	report->local_metrics.timestamps.stop = call->log->start_date_time + linphone_call_get_duration(call);

	//we use same timestamps for remote too
	report->remote_metrics.timestamps.start = call->log->start_date_time;
	report->remote_metrics.timestamps.stop = call->log->start_date_time + linphone_call_get_duration(call);
	
	// yet we use the same payload config for local and remote, since this is the largest case
	if (stats_type == LINPHONE_CALL_STATS_AUDIO && call->audiostream != NULL) {
		stream = &call->audiostream->ms;
		local_payload = linphone_call_params_get_used_audio_codec(current_params);
		remote_payload = local_payload;
	} else if (stats_type == LINPHONE_CALL_STATS_VIDEO && call->videostream != NULL) {
		stream = &call->videostream->ms;
		local_payload = linphone_call_params_get_used_video_codec(current_params);
		remote_payload = local_payload;
	}

	if (stream != NULL) {
		RtpSession * session = stream->sessions.rtp_session;

		report->info.local_addr.ssrc = rtp_session_get_send_ssrc(session);
		report->info.remote_addr.ssrc = rtp_session_get_recv_ssrc(session);
	}

	if (local_payload != NULL) {
		report->local_metrics.session_description.payload_type = local_payload->type;
		strass(report->local_metrics.session_description.payload_desc, ms_strdup(local_payload->mime_type));
		report->local_metrics.session_description.sample_rate = local_payload->clock_rate;
		strass(report->local_metrics.session_description.fmtp, ms_strdup(local_payload->recv_fmtp));
	}

	if (remote_payload != NULL) {
		report->remote_metrics.session_description.payload_type = remote_payload->type;
		strass(report->remote_metrics.session_description.payload_desc, ms_strdup(remote_payload->mime_type));
		report->remote_metrics.session_description.sample_rate = remote_payload->clock_rate;
		strass(report->remote_metrics.session_description.fmtp, ms_strdup(remote_payload->recv_fmtp));
	}
}

void linphone_reporting_call_stats_updated(LinphoneCall *call, int stats_type) {
	reporting_session_report_t * report = call->log->reports[stats_type];
	reporting_content_metrics_t * metrics = NULL;

	LinphoneCallStats stats = call->stats[stats_type];
    mblk_t *block = NULL;

    if (! reporting_enabled(call)) 
		return;

    if (stats.updated == LINPHONE_CALL_STATS_RECEIVED_RTCP_UPDATE) {
		metrics = &report->remote_metrics;
        if (rtcp_is_XR(stats.received_rtcp) == TRUE) {
            block = stats.received_rtcp;
        }
    } else if (stats.updated == LINPHONE_CALL_STATS_SENT_RTCP_UPDATE) {
		metrics = &report->local_metrics;
        if (rtcp_is_XR(stats.sent_rtcp) == TRUE) {
            block = stats.sent_rtcp;
        }
    }
    if (block != NULL) {
        switch (rtcp_XR_get_block_type(block)) {
            case RTCP_XR_VOIP_METRICS: {
                metrics->quality_estimates.rcq = rtcp_XR_voip_metrics_get_r_factor(block);
                metrics->quality_estimates.moslq = rtcp_XR_voip_metrics_get_mos_lq(block);
                metrics->quality_estimates.moscq = rtcp_XR_voip_metrics_get_mos_cq(block);
                metrics->jitter_buffer.nominal = rtcp_XR_voip_metrics_get_jb_nominal(block);
                metrics->jitter_buffer.max = rtcp_XR_voip_metrics_get_jb_maximum(block);
                metrics->jitter_buffer.abs_max = rtcp_XR_voip_metrics_get_jb_abs_max(block);
                metrics->packet_loss.network_packet_loss_rate = rtcp_XR_voip_metrics_get_loss_rate(block);
                metrics->packet_loss.jitter_buffer_discard_rate = rtcp_XR_voip_metrics_get_discard_rate(block);

                uint8_t config = rtcp_XR_voip_metrics_get_rx_config(block);
                metrics->session_description.packet_loss_concealment = (config >> 6) & 0x3;
                metrics->jitter_buffer.adaptive = (config >> 4) & 0x3;
                break;
            } default: {
                break;
            }
        }
    }
}

void linphone_reporting_publish(LinphoneCall* call) {
	PRINTF("linphone_reporting_publish\n");

	if (! reporting_enabled(call))
		return;


	if (call->log->reports[LINPHONE_CALL_STATS_AUDIO] != NULL) {
		reporting_publish(call, call->log->reports[LINPHONE_CALL_STATS_AUDIO]);
	}

	if (call->log->reports[LINPHONE_CALL_STATS_VIDEO] != NULL 
		&& linphone_call_params_video_enabled(linphone_call_get_current_params(call))) {
		reporting_publish(call, call->log->reports[LINPHONE_CALL_STATS_VIDEO]);
	}
}

reporting_session_report_t * linphone_reporting_new() {
	int i;
	reporting_session_report_t * rm = ms_new0(reporting_session_report_t,1);

	reporting_content_metrics_t * metrics[2] = {&rm->local_metrics, &rm->remote_metrics};
	for (i = 0; i < 2; i++) {
		metrics[i]->session_description.payload_type = -1;
		metrics[i]->session_description.sample_rate = -1;
		metrics[i]->session_description.frame_duration = -1;

		metrics[i]->packet_loss.network_packet_loss_rate = -1;
		metrics[i]->packet_loss.jitter_buffer_discard_rate = -1;

		metrics[i]->session_description.packet_loss_concealment = -1;

		metrics[i]->jitter_buffer.adaptive = -1;
		// metrics[i]->jitter_buffer.rate = -1;
		metrics[i]->jitter_buffer.nominal = -1;
		metrics[i]->jitter_buffer.max = -1;
		metrics[i]->jitter_buffer.abs_max = -1;

		metrics[i]->delay.round_trip_delay = -1;
		metrics[i]->delay.end_system_delay = -1;
		// metrics[i]->delay.one_way_delay = -1;
		metrics[i]->delay.symm_one_way_delay = -1;
		metrics[i]->delay.interarrival_jitter = -1;
		metrics[i]->delay.mean_abs_jitter = -1;

		metrics[i]->signal.level = 127;
		metrics[i]->signal.noise_level = 127;
	}
	return rm;
}

void linphone_reporting_destroy(reporting_session_report_t * report) {
	if (report->info.call_id != NULL) ms_free(report->info.call_id);
	if (report->info.local_id != NULL) ms_free(report->info.local_id);
	if (report->info.remote_id != NULL) ms_free(report->info.remote_id);
	if (report->info.orig_id != NULL) ms_free(report->info.orig_id);
	if (report->info.local_addr.ip != NULL) ms_free(report->info.local_addr.ip);
	if (report->info.remote_addr.ip != NULL) ms_free(report->info.remote_addr.ip);
	if (report->info.local_group != NULL) ms_free(report->info.local_group);
	if (report->info.remote_group != NULL) ms_free(report->info.remote_group);
	if (report->info.local_mac_addr != NULL) ms_free(report->info.local_mac_addr);
	if (report->info.remote_mac_addr != NULL) ms_free(report->info.remote_mac_addr);
	if (report->dialog_id != NULL) ms_free(report->dialog_id);
	if (report->local_metrics.session_description.fmtp != NULL) ms_free(report->local_metrics.session_description.fmtp);
	if (report->local_metrics.session_description.payload_desc != NULL) ms_free(report->local_metrics.session_description.payload_desc);
	if (report->remote_metrics.session_description.fmtp != NULL) ms_free(report->remote_metrics.session_description.fmtp);
	if (report->remote_metrics.session_description.payload_desc != NULL) ms_free(report->remote_metrics.session_description.payload_desc);

	ms_free(report);
}
