/*!
 * \file chan_rtmp.c
 * \author ichramm
 *
 * Created on March 22, 2013, 3:38 PM
 *
 * This file contains code implementing a RTMP-based tech-driver for asterisk.
 */

#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>

#include <librtmp/rtmp.h>
#include <librtmp/http.h>
#include <librtmp/amf.h>
#include <librtmp/log.h>

#if GCC_VERSION >= 46
 #pragma GCC diagnostic push
#endif
#pragma GCC diagnostic ignored "-Wold-style-declaration"

#include <asterisk.h>
#include <asterisk/options.h>
#include <asterisk/lock.h>
#include <asterisk/utils.h>
#include <asterisk/frame.h>
#include <asterisk/config.h>
#include <asterisk/module.h>
#include <asterisk/channel.h>
#include <asterisk/strings.h>
#include <asterisk/pbx.h>
#include <asterisk/causes.h>
#include <asterisk/cli.h>
#include <asterisk/musiconhold.h>
#include <asterisk/astobj2.h>

#if GCC_VERSION >= 46
 #pragma GCC diagnostic pop
#endif

#define __min(a, b)  ( (a) < (b) ? (a) : (b) )

#define UNUSED(expr) do { \
	(void)(expr); \
} while (0)

// we'll use a shorter version
#undef _A_
#define _A_ "chan_rtmp.c", __LINE__, __FUNCTION__

#ifndef ast_verb // since asterisk 1.6
 #define ast_verb(level, ...) if (option_verbose >= level) do {\
 	ast_verbose( __VA_ARGS__ ); \
 } while (0)
#endif // ast_verb

// wrapper for asterisk 1.4
#define ast_cli2(fd, fmt, ...) ast_cli(fd, (char*)(fmt), ## __VA_ARGS__)

// to avoid the classic if-without-braces block in the code
#define ast_log_if(expr, ...) if (expr) do { \
	ast_log(__VA_ARGS__); \
} while (0)

#ifndef ao2_unref
 #define ao2_unref(obj) if (obj) do { \
	ao2_ref(obj, -1); \
	obj = NULL; \
 } while(0)
#endif

#define __AVC(name) { (char*)name, sizeof(name)-1 }

#define STR2AVAL(av, str) do { \
	av.av_val = (char*)(str); \
	av.av_len = strlen(av.av_val); \
} while (0)

#define AVAL2STR(str, av, size) do { \
	int max_chars = __min((int)size-1, av.av_len); \
	strncpy(str, av.av_val, max_chars); \
	str[max_chars] = '\0'; \
} while (0)

/*!
 * Writes begin-object mark in \p buff
 */
#define _AMF_OBJECT_BEGIN(buff) do { \
	*buff++ = AMF_OBJECT; \
} while (0)

/*!
 * Writes end-object mark in \p buff
 */
#define _AMF_OBJECT_END(buff) do { \
	*buff++ = 0; \
	*buff++ = 0; \
	*buff++ = AMF_OBJECT_END; \
} while (0)


#define RTMP_DEFAULT_PORT    1935
#define RTMP_DEFULT_CONTEXT  "default"
#define RTMP_WINDOW_SIZE     (250*1024)  // determines how many unacknowledged bytes can be sent/received
#define RTMP_MAX_CAPS        (AST_FORMAT_ULAW | AST_FORMAT_ALAW | AST_FORMAT_SPEEX)
#define THREAD_STACKSIZE     (((sizeof(void *) * 8 * 8) - 16) * 512)

#define RTMP_FLAG_DEBUG        (1 << 0)
#define RTMP_FLAG_DEBUG_AIN    (1 << 1)
#define RTMP_FLAG_DEBUG_AOUT   (1 << 2)

#define FRAME_MARKER_SLINEAR    ((uint8_t)0x06)
#define FRAME_MARKER_NELLYMOSER ((uint8_t)0x52)
#define FRAME_MARKER_ALAW       ((uint8_t)0x72)
#define FRAME_MARKER_ULAW       ((uint8_t)0x82)
#define FRAME_MARKER_SPEEX      ((uint8_t)0xB2)
#define FRAME_MARKER_DTMF       ((uint8_t)0x01)

/* Audio codecs supported by the client */
#define SUPPORT_SND_G711A   ((uint16_t)0x0080)
#define SUPPORT_SND_G711U   ((uint16_t)0x0100)
#define SUPPORT_SND_SPEEX   ((uint16_t)0x0800)


static const char configfile[] = "rtmp.conf";

// connection parameters
static const AVal av_connect              = __AVC("connect"); // connect command
static const AVal av_app                  = __AVC("app");
static const AVal av_flashVer             = __AVC("flashVer");
static const AVal av_swfUrl               = __AVC("swfUrl");
static const AVal av_pageUrl              = __AVC("pageUrl");
static const AVal av_tcUrl                = __AVC("tcUrl");
static const AVal av_fpad                 = __AVC("fpad");
static const AVal av_capabilities         = __AVC("capabilities");
static const AVal av_audioCodecs          = __AVC("audioCodecs");
static const AVal av_videoCodecs          = __AVC("videoCodecs");
static const AVal av_videoFunction        = __AVC("videoFunction");
static const AVal av_objectEncoding       = __AVC("objectEncoding");
static const AVal av_mode                 = __AVC("mode");
static const AVal av_secureToken          = __AVC("secureToken");
static const AVal av_uri                  = __AVC("uri");
// rpc results
static const AVal av__result              = __AVC("_result"); // operation succeeded, this is the result
static const AVal av__error               = __AVC("_error");  // operation failed, this is the error
// server data
static const AVal av_fmsVer               = __AVC("fmsVer");
static const AVal av_fmsVerVal            = __AVC("FMS/3,5,1,525");
// events
static const AVal av_onStatus             = __AVC("onStatus");
static const AVal av_level                = __AVC("level");       // event level
static const AVal av_status               = __AVC("status");      // event level is status (notification)
static const AVal av_error                = __AVC("error");       // event level is error
static const AVal av_code                 = __AVC("code");        // status code
static const AVal av_description          = __AVC("description"); // status description
// stream functions
static const AVal av_play                 = __AVC("play");
static const AVal av_publish              = __AVC("publish");
static const AVal av_createStream         = __AVC("createStream");
static const AVal av_closeStream          = __AVC("closeStream");
static const AVal av_deleteStream         = __AVC("deleteStream");
static const AVal av_getStreamLength      = __AVC("getStreamLength");
// stream events
static const AVal av_NetStream_Play_Start = __AVC("NetStream.Play.Start");
static const AVal av_NetStream_Play_Stop  = __AVC("NetStream.Play.Stop");
static const AVal av_Started_playing      = __AVC("Started playing");
static const AVal av_Stopped_playing      = __AVC("Stopped playing");
static const AVal av_clientid             = __AVC("clientid");
static const AVal av_details              = __AVC("details");
// application events
static const AVal av_invited              = __AVC("invited");   // invited(from: String, to:String)
static const AVal av_ringing              = __AVC("ringing");   // ringing()
static const AVal av_accepted             = __AVC("accepted");  // accepted(audioCodec: String, videoCodec: String)
static const AVal av_rejected             = __AVC("rejected");  // rejected(reason: String)
static const AVal av_cancelled            = __AVC("cancelled"); // cancelled()
static const AVal av_byed                 = __AVC("byed");      // byed()
static const AVal av_onhold               = __AVC("onhold");    // onhold(putOnHold: Boolean)
static const AVal av_ondtmf               = __AVC("ondtmf");    // ondtmf(digit: String)
static const AVal av_codecChanged         = __AVC("codecChanged"); // codecChanged(newcodec: String)
// application commands
static const AVal av_invite               = __AVC("invite");   // c.invite(destination)
static const AVal av_accept               = __AVC("accept");   // c.accept()
static const AVal av_reject               = __AVC("reject");   // c.reject()
static const AVal av_hangup               = __AVC("hangup");   // c.hangup()
static const AVal av_hold                 = __AVC("hold");
static const AVal av_sendDTMF             = __AVC("sendDTMF");
// connection and stream events
static const AVal av_NetStream_Publish_Start        = __AVC("NetStream.Publish.Start");
static const AVal av_NetStream_Publish_Stop         = __AVC("NetStream.Publish.Stop");
static const AVal av_NetConnection_Connect_Success  = __AVC("NetConnection.Connect.Success");
static const AVal av_Connection_succeeded           = __AVC("Connection succeeded");
static const AVal av_NetConnection_Connect_Rejected = __AVC("NetConnection.Connect.Rejected");
static const AVal av_Connection_rejected            = __AVC("Connection rejected");

/*!
 * RTMP User Control Messages
 */
enum rtmp_control_message
{
	/*!
	 * The server sends this event to notify the client that a stream has become
	 * functional and can be used for communication.
	 * By default, this event is sent on ID 0 after the application connect command is
	 * successfully received from the client.
	 * The event data is 4-byte and represents the stream ID of the stream that became functional.
	 */
	ctrl_msg_stream_begin = 0,

	/*!
	 * The server sends this event to notify the client that the playback of data is over
	 * as requested on this stream.
	 * No more data is sent without issuing additional commands.
	 * The client discards the messages received for the stream.
	 * The 4 bytes of event data represent the ID of the stream on which playback has ended.
	 */
	ctrl_msg_stream_eof = 1,

	/*!
	 * The server sends this event to notify the client that there is no more data on
	 * the stream.
	 * If the server does not detect any message for a time period, it can notify the
	 * subscribed clients that the stream is dry.
	 * The 4 bytes of event data represent the stream ID of the dry stream.
	 */
	ctrl_msg_stream_dry = 2,

	/*!
	 * The client sends this event to inform the server of the buffer size (in
	 * milliseconds) that is used to buffer any data coming over a stream.
	 * This event is sent before the server starts processing the stream.
	 * The first 4 bytes of the event data represent the stream ID and the next 4 bytes
	 * represent the buffer length, in milliseconds.
	 */
	ctrl_msg_set_buffer_length = 3,

	/*!
	 * The server sends this event to notify the client that the stream is a recorded stream.
	 * The 4 bytes event data represent the stream ID of the recorded stream.
	 */
	ctrl_msg_stream_is_recorded = 4,

	// 5 not in use

	/*!
	 * The server sends this event to test whether the client is reachable.
	 * Event data is a 4-byte timestamp, representing the local server time when the
	 * server dispatched the command.
	 * The client responds with PingResponse on receiving
	 */
	ctrl_msg_ping_request = 6,

	/*!
	 * The client sends this event to the server in response to the ping request.
	 * The event data is a 4-byte timestamp, which was received with the PingRequest request.
	 */
	ctrl_msg_ping_response = 7,
};

/*!
 * RTMP channels, each channel servers one purpose
 */
enum RTMPChannel
{
    RTMP_NETWORK_CHANNEL = 0x02,  /*< channel for network-related messages (bandwidth report, ping, etc) */
    RTMP_SYSTEM_CHANNEL  = 0x03,  /*< channel for sending server control messages */
    RTMP_SOURCE_CHANNEL  = 0x04,  /*< publish channel: the channel used to receive audio data*/
    RTMP_VIDEO_CHANNEL   = 0x08,  /*< channel for video data */
    RTMP_AUDIO_CHANNEL   = 0x09,  /*< play channel: the channel used to send audio data */
};

/*!
 * The state machine is very strict, see comments for detail
 */
enum session_state
{
	state_idle       = 0,        /*!< Initial state */
	state_connected  = (1 << 0), /*!< User is connected, must authenticate */
	state_registered = (1 << 1), /*!< user is connected and logged in, allows to place and receive calls */
	state_incoming   = (1 << 2), /*!< User is receiving an incoming call, allows to accept */
	state_outgoing   = (1 << 3), /*!< User is placing a call, allows to reject */
	state_talking    = (1 << 4), /*!< User is in an active call, allows to hangup */
};

/*!
 * An object of this type is dumped to the pvt pipe each time an
 * audio packet arrives. Contains information about the following
 * audio data like the number of samples and the codec.
 *
 * Written by \c handle_packet_audio
 * Read by \c rtmp_read
 */
struct audio_frame_marker
{
	uint8_t  marker;
	uint32_t framesize;
};

/*!
 */
enum pvt_flags
{
	pvt_flag_hold_sent    = (1 << 0),
	pvt_flag_rejected     = (1 << 1),
	pvt_flag_detached     = (1 << 2)
};

/*
 * The tech pvt attached to an \c ast_channel structure.
 */
struct rtmp_tech_pvt
{
	/*! The channel that owns us */
	struct ast_channel *owner;
	/*! The channel currently being on hold, this is mainly used for transfers, where the first channel
	 is put on hold while the peer is talking with the transfer recipient */
	struct ast_channel *hold;
	/*! A reference to the peer owning this call */
	struct rtmp_peer   *peer;
	/*! Flags stating some condition on this pvt, a combination of enum pvt_flags values*/
	struct ast_flags    flags;
	/*! Initial timestamp used to calculate the timestamp of each audio frame */
	struct timespec     epoch;
	/*! This pipe is filled by \c rtmp_peer_handle_packet_audio, and consumed by \c owner */
	int                 pipe[2];
};

/*!
 * A connected client
 */
struct rtmp_peer
{
	char                   name[80];
	int                    sockfd;
	unsigned int           state; // actually this is a combination of session_state values
	RTMP                   rtmp;
	ast_mutex_t            lock;
	struct rtmp_tech_pvt  *current_pvt;
	int                    micformat;
	int                    capability; // should be unsigned but asterisk 1.4 uses plain int
	char                   callingpres[80];
	char                   context[AST_MAX_CONTEXT];
	// stream handling
	unsigned int           next_stream_id; // doesn't need to be atomic
	unsigned int           play_stream_id;
	AVal                   play_stream_name;
	unsigned int           publish_stream_id;
	AVal                   publish_stream_name;
	struct sockaddr_in     sin_addr;
	// codec support will depend in the flash version (see _rtmp.m_fAudioCodecs)
	struct ast_codec_pref  codec_prefs;
};

static struct ast_flags global_flags = { 0 };
#define rtmp_debug            ast_test_flag(&global_flags, RTMP_FLAG_DEBUG)
#define option_debugaudioin   ast_test_flag(&global_flags, RTMP_FLAG_DEBUG_AIN)
#define option_debugaudioout  ast_test_flag(&global_flags, RTMP_FLAG_DEBUG_AOUT)

static unsigned int call_count = 1;

/*! Default context where to place calls */
static char default_context[AST_MAX_CONTEXT];
/*! Peers must connect to this application (configurable in rtmp.conf) */
static char default_application[80] = "asterisk";

/* sockets and networking */
static int                rtmpsock     = -1;
static int                ourport      = 0;
static struct sockaddr_in bindaddr     = { .sin_port = 0, };
static pthread_t          acceptthread = AST_PTHREADT_STOP;
#if defined(AST_MUTEX_INIT_W_CONSTRUCTORS)
static ast_mutex_t        netlock;
#else
AST_MUTEX_DEFINE_STATIC(netlock);
#endif

static struct ast_codec_pref default_prefs;
static int global_capability = RTMP_MAX_CAPS;

/*! List of connected peers, mapped by username */
static struct ao2_container *rtmp_peers;

/* PVT functions */
static struct rtmp_tech_pvt *pvt_alloc(struct ast_channel *owner, struct rtmp_peer *peer);
static void pvt_free(struct rtmp_tech_pvt *pvt);
static unsigned int pvt_timestamp(struct rtmp_tech_pvt *pvt);

/* RMTP helper functions */
static __thread char _state2str_buffer[40];
#define state2str(state) __state2str(state, _state2str_buffer, sizeof(_state2str_buffer))
static char *__state2str(unsigned int state, char *buff, unsigned int len);
#define rtmp_send_packet(r, p, q)  __rtmp_send_packet(r, p, q, __LINE__, __FUNCTION__)
static int __rtmp_send_packet(RTMP *rtmp_handle, struct RTMPPacket *p, int queue, int line, const char *fn);
static int rtmp_invoke(RTMP *rtmp_handle, AVal const *avmethod, AMFObjectProperty *args, unsigned int num_args);
static int rtmp_send_control(RTMP *rtmp_handle, enum rtmp_control_message ntype, unsigned int nobject, unsigned int ntime, int channel);
static int rtmp_send_on_status(RTMP *rtmp_handle, const AVal *avcode, const AVal *avdesc, unsigned int streamid, int channel);
static int rtmp_check_chunksize(RTMP *rtmp_handle, int newsize) __attribute__((unused));

static struct rtmp_peer *rtmp_peer_constructor();
static void rtmp_peer_destructor(void *p);
static void *rtmp_peer_handle_session(void *arg);
static int rtmp_peer_handle_packet        (struct rtmp_peer *peer, RTMPPacket *packet);
static int rtmp_peer_handle_packet_audio  (struct rtmp_peer *peer, RTMPPacket *packet);
static int rtmp_peer_handle_packet_control(struct rtmp_peer *peer, RTMPPacket *packet);
static int rtmp_peer_handle_packet_invoke (struct rtmp_peer *peer, RTMPPacket *packet, int offset);
static int rtmp_peer_handle_request_connect    (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_invite     (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_accept     (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_reject     (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_hangup     (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_dtmf       (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_hold       (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_play       (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_publish    (struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_handle_request_closeStream(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet);
static int rtmp_peer_send_connect_result (struct rtmp_peer *peer, double txn, int success);
static int rtmp_peer_send_error_result   (struct rtmp_peer *peer, double txn, const char *message);
static int rtmp_peer_send_null_result    (struct rtmp_peer *peer, double txn);
static int rtmp_peer_send_number_result  (struct rtmp_peer *peer, double txn, double number);
static struct ast_channel *rtmp_peer_get_channel_locked(struct rtmp_peer *peer);
static int rtmp_peer_change_state(struct rtmp_peer *peer, unsigned int newstate);
static int rtmp_peer_detach_call(struct rtmp_peer *peer);

// returns 0 on success, -1 on failure
static int rtmp_authenticate_peer(struct rtmp_peer *peer, const char *password);
static int rtmp_remove_peer(struct rtmp_peer *peer);

/*! Creates a new ast_channel with it's corresponding pvt. Launches the PBX thread if necessary */
static struct ast_channel *rtmp_new(struct rtmp_peer *s, const char *exten, int state);
/*! Called by Asterisk to request a new channel */
static struct ast_channel *rtmp_request(const char *type, int format, void *data, int *cause);
/*! Called by Asterisk when a DTMF digit is received */
static int rtmp_send_digit(struct ast_channel *ast, char digit, unsigned int duration);
/*! Called by Asterisk to call a RTMP peer, the channel shall be requested by calling \c rtmp_request first */
static int rtmp_call(struct ast_channel *ast, char *dest, int timeout);
/*! Called by Asterisk to hangup a channel, destroyes the pvt */
static int rtmp_hangup(struct ast_channel *ast);
/*! Called by Asterisk to perform answer on a channel, this one gets called when an RTMP peer performs an outbound call */
static int rtmp_answer(struct ast_channel *ast);
/*! Called by Asterisk to request a  frame from a rtmp channel, this function reads the audio from the pvt pipe */
static struct ast_frame *rtmp_read(struct ast_channel *ast);
/*! Called by Asterisk to write a frame to a channel */
static int rtmp_write(struct ast_channel *ast, struct ast_frame *frame);
/*! Called by Asterisk to indicate some condition in a channel, like the channel being on hold, or busy */
static int rtmp_indicate(struct ast_channel *c, int condition, const void *data, size_t datalen);
/*! Fix up a channel:  If a channel is consumed, this is called.  Basically update any ->owner links */
static int rtmp_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);

/* CLI handlers */
static int handle_cli_show_settings(int fd, int argc, char *argv[]);
static int handle_cli_show_peers   (int fd, int argc, char *argv[]);
static int handle_cli_show_channels(int fd, int argc, char *argv[]);
static int handle_cli_set_debug    (int fd, int argc, char *argv[]);


/*!
 * RTMP Tech definition
 */
static const struct ast_channel_tech rtmp_tech = {
	.type =  "RTMP",
	.description = "RTMP Channel Driver",
	.capabilities = RTMP_MAX_CAPS,
	.requester = rtmp_request,
	.send_digit_end = rtmp_send_digit,
	.call = rtmp_call,
	.hangup = rtmp_hangup,
	.answer = rtmp_answer,
	.read = rtmp_read,
	.write = rtmp_write,
	.indicate = rtmp_indicate,
	.fixup = rtmp_fixup
};

#if GCC_VERSION >= 46
 #pragma GCC diagnostic push
#endif
#pragma GCC diagnostic ignored "-Wwrite-strings"
static struct ast_cli_entry cli_commands[] = {
	{ .cmda = {"rtmp", "show", "settings", NULL }, .handler = &handle_cli_show_settings, .summary = "Shows global settings" },
	{ .cmda = {"rtmp", "show", "peers", NULL }, .handler = &handle_cli_show_peers, "Show RTMP users" },
	{ .cmda = {"rtmp", "show", "channels", NULL }, .handler = &handle_cli_show_channels, "Show currently active RTMP channels" },

	{ .cmda = {"rtmp", "set", "debug", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp debug on/off" },
	{ .cmda = {"rtmp", "set", "debug", "off", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp debug on/off" },
	{ .cmda = {"rtmp", "set", "debug", "audio", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp audio debug on/off" },
	{ .cmda = {"rtmp", "set", "debug", "audio", "off", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp audio debug on/off" },
	{ .cmda = {"rtmp", "set", "debug", "audio", "in", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp incoming audio debug on/off" },
	{ .cmda = {"rtmp", "set", "debug", "audio", "in", "off", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp incoming audio debug on/off" },
	{ .cmda = {"rtmp", "set", "debug", "audio", "out", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp outgoing audio debug on/off" },
	{ .cmda = {"rtmp", "set", "debug", "audio", "out", "off", NULL }, .handler = &handle_cli_set_debug, .summary = "Set rtmp outgoing audio debug on/off" }
};
#if GCC_VERSION >= 46
#pragma GCC diagnostic pop
#endif

static struct rtmp_tech_pvt *pvt_alloc(struct ast_channel *owner, struct rtmp_peer *peer)
{
	struct rtmp_tech_pvt *pvt = ast_calloc(1, sizeof(struct rtmp_tech_pvt));
	if ( pvt != NULL ) {
		pvt->owner = owner;
		pvt->peer = peer;
		ao2_ref(pvt->peer, +1);
		clock_gettime(CLOCK_MONOTONIC, &pvt->epoch);
		if ( pipe(pvt->pipe) < 0 ) {
			ast_log(LOG_ERROR, "Failed to create read pipe: %d (%s)\n", errno, strerror(errno));
			ao2_unref(pvt->peer);
			ast_free(pvt);
			pvt = NULL;
		}
	}
	return pvt;
}

static void pvt_free(struct rtmp_tech_pvt *pvt)
{
	if (pvt->pipe[0] > 0)
		close(pvt->pipe[0]);
	if (pvt->pipe[1] > 0)
		close(pvt->pipe[1]);
	ao2_unref(pvt->peer);
	ast_free(pvt);
}

static unsigned int pvt_timestamp(struct rtmp_tech_pvt *pvt)
{
	struct timespec tsnow;
	clock_gettime(CLOCK_MONOTONIC, &tsnow);
	tsnow.tv_sec  -= pvt->epoch.tv_sec; // tsnow.tv_sec is always >= epoch.tv_sec
	tsnow.tv_nsec -= pvt->epoch.tv_nsec;
	return tsnow.tv_sec*1000 + tsnow.tv_nsec/1000000;
}

static char *__state2str(unsigned int state, char *buff, unsigned int len)
{
	static struct {
		enum session_state state;
		AVal               name;
	} domain[] = {
		{ state_connected,  __AVC("connected")  },
		{ state_registered, __AVC("registered") },
		{ state_incoming,   __AVC("incoming")   },
		{ state_outgoing,   __AVC("outgoing")   },
		{ state_talking,    __AVC("talking")    }
	};
	unsigned int i;
	char *begin = buff, *end = buff + len - 1;

	if (state == state_idle) { // state == 0, no need to check with others
		snprintf(buff, end-buff, "idle");
		return buff;
	}
	for (i = 0; i < ARRAY_LEN(domain) && buff < end; ++i) {
		if ( !(state & domain[i].state) ) {
			continue;
		}
		if ( buff > begin ) { // not the first matching state
			*buff++ = '|';
		}
		if (buff + domain[i].name.av_len >= end) { // check for buffer overflow
			ast_log(LOG_WARNING, "No enough buffer to write state %u (len: %u, left: %u, needed (at least): %d)\n",
					state, len, (unsigned)(end-buff), domain[i].name.av_len+1);
			break;
		}
		// copy raw string to buff and move it to the end
		memcpy(buff, domain[i].name.av_val, domain[i].name.av_len);
		buff += domain[i].name.av_len;
	}
	*buff = 0; // finally, mark the end of the string
	return begin;
}

static inline int __rtmp_send_packet(struct RTMP *r, struct RTMPPacket *p, int queue, int line, const char *fn)
{ // used with a macro
	if (option_debugaudioout || (rtmp_debug && p->m_packetType != RTMP_PACKET_TYPE_AUDIO)) {
		ast_log(__LOG_DEBUG, "chan_rtmp.c", line, fn, "Sending packet type 0x%x:\n", p->m_packetType);
		if (p->m_packetType == RTMP_PACKET_TYPE_INVOKE || p->m_packetType == RTMP_PACKET_TYPE_FLEX_MESSAGE) {
			AMFObject amf;
			if (AMF_Decode(&amf, p->m_body, p->m_nBodySize, FALSE) >= 0) {
				AMF_Dump(&amf);
			}
			AMF_Reset(&amf);
		}
	}

	if (FALSE == RTMP_SendPacket(r, p, queue)) {
		return -1;
	}

	return 0;
}

static int rtmp_invoke(RTMP *rtmp_handle, AVal const *avmethod, AMFObjectProperty *args, unsigned int num_args)
{
	unsigned int i;
	char pbuf[384], *pend = pbuf+sizeof(pbuf), *enc;
	RTMPPacket packet = {
		.m_headerType = RTMP_PACKET_SIZE_MEDIUM,
		.m_packetType = RTMP_PACKET_TYPE_INVOKE,
		.m_nChannel   = RTMP_SYSTEM_CHANNEL,
		.m_body       = pbuf + RTMP_MAX_HEADER_SIZE
	};

	enc = packet.m_body;
	enc = AMF_EncodeString(enc, pend, avmethod);
	enc = AMF_EncodeNumber(enc, pend, 0);
	*enc++ = AMF_NULL;

	for ( i = 0; i < num_args; ++i ) {
		switch ( args[i].p_type ) {
			case AMF_NUMBER:
				enc = AMF_EncodeNumber(enc, pend, args[i].p_vu.p_number);
				break;
			case AMF_BOOLEAN:
				enc = AMF_EncodeBoolean(enc, pend, args[i].p_vu.p_number);
				break;
			case AMF_STRING:
				enc = AMF_EncodeString(enc, pend, &args[i].p_vu.p_aval);
				break;
			case AMF_NULL:
				*enc++ = AMF_NULL;
				break;
			default:
				RTMP_Log(RTMP_LOGWARNING, "%s: Unsupported parameter type %d", __FUNCTION__, args[i].p_type);
				break;
		}
	}

	packet.m_nBodySize = enc - packet.m_body;
	return rtmp_send_packet(rtmp_handle, &packet, FALSE);
}

static int rtmp_send_control(RTMP *rtmp_handle, enum rtmp_control_message ntype, unsigned int nobject, unsigned int ntime, int channel)
{
	char pbuf[256], *pend = pbuf+sizeof(pbuf), *enc;
	RTMPPacket packet = {
		.m_headerType = RTMP_PACKET_SIZE_LARGE,
		.m_packetType = RTMP_PACKET_TYPE_CONTROL,
		.m_nChannel   = channel,
		.m_body       = pbuf + RTMP_MAX_HEADER_SIZE,
		.m_nBodySize  = 6 // default size
	};

	enc = packet.m_body;
	enc = AMF_EncodeInt16(enc, pend, ntype);
	enc = AMF_EncodeInt32(enc, pend, nobject);
	if (ntype == ctrl_msg_set_buffer_length) {
		packet.m_nBodySize = 10;
		enc = AMF_EncodeInt32(enc, pend, ntime);
	}

	return rtmp_send_packet(rtmp_handle, &packet, FALSE);
}

static int rtmp_send_on_status(RTMP *rtmp_handle, const AVal *avcode, const AVal *avdesc, unsigned int streamid, int channel)
{
	char pbuf[384], *pend = pbuf+sizeof(pbuf), *enc;
	RTMPPacket packet = {
		.m_headerType  = RTMP_PACKET_SIZE_LARGE,
		.m_packetType  = RTMP_PACKET_TYPE_INVOKE,
		.m_nChannel    = channel,
		.m_body        = pbuf + RTMP_MAX_HEADER_SIZE,
		.m_nInfoField2 = streamid
	};

	enc = packet.m_body;
	enc = AMF_EncodeString(enc, pend, &av_onStatus); //Command Name | String | The command name "onStatus"
	enc = AMF_EncodeNumber(enc, pend, 0); // Transaction ID | Number | Transaction ID set to 0
	*enc++ = AMF_NULL; // Command Object| Null | There is no command object for onStatus messages.
	_AMF_OBJECT_BEGIN(enc); //  Info Object
		enc = AMF_EncodeNamedString(enc, pend, &av_level, &av_status);
		enc = AMF_EncodeNamedString(enc, pend, &av_code, avcode);
		enc = AMF_EncodeNamedString(enc, pend, &av_description, avdesc);
		// set 'details' to NULL
		enc = AMF_EncodeInt16(enc, pend, av_details.av_len);
		memcpy(enc, av_details.av_val, av_details.av_len);
		enc += av_details.av_len;
		*enc++ = AMF_NULL;
	_AMF_OBJECT_END(enc);

	packet.m_nBodySize = enc - packet.m_body;
	return rtmp_send_packet(rtmp_handle, &packet, FALSE);
}

static int rtmp_check_chunksize(RTMP *rtmp_handle, int newsize)
{
	if (newsize > rtmp_handle->m_outChunkSize) {
		char pbuf[100], *pend = pbuf +sizeof(pbuf);
		RTMPPacket packet = {
			.m_headerType = RTMP_PACKET_SIZE_LARGE,
			.m_packetType = RTMP_PACKET_TYPE_CHUNK_SIZE,
			.m_nChannel   = RTMP_NETWORK_CHANNEL,
			.m_body       = pbuf + RTMP_MAX_HEADER_SIZE,
			.m_nBodySize  = 4
		};

		rtmp_handle->m_outChunkSize = newsize;
		AMF_EncodeInt32(packet.m_body, pend, rtmp_handle->m_outChunkSize);

		return rtmp_send_packet(rtmp_handle, &packet,FALSE);
	}
	return 0;
}

static struct rtmp_peer *rtmp_peer_constructor()
{
	struct rtmp_peer *peer;

	if ( !(peer = ao2_alloc(sizeof(*peer), rtmp_peer_destructor)) ) {
		ast_log(LOG_ERROR, "Failed to allocate rtmp_peer object\n");
		return peer;
	}

	peer->sockfd = -1;
	peer->state = state_idle;
	peer->current_pvt = NULL;
	peer->micformat = 0;
	peer->capability = 0;
	peer->next_stream_id = 0;
	peer->play_stream_id = 0;
	peer->publish_stream_id = 0;
	peer->name[0] = peer->callingpres[0] = peer->context[0] = 0;
	ast_mutex_init(&peer->lock);

	return peer;
}

static void rtmp_peer_destructor(void *p)
{
	struct rtmp_peer *peer = p;
	ast_log_if(rtmp_debug, LOG_DEBUG, "destroying peer %s\n", S_OR(peer->name, "[unknown]"));
	ast_mutex_destroy(&peer->lock);
}

static void *rtmp_peer_handle_session(void *arg)
{
	int res;
	RTMPPacket packet;
	struct rtmp_peer *peer = arg;

	RTMP_Init(&peer->rtmp);
	peer->rtmp.m_sb.sb_socket = peer->sockfd;
	peer->rtmp.m_nClientBW = RTMP_WINDOW_SIZE;
	peer->rtmp.m_nServerBW = RTMP_WINDOW_SIZE;

	if ( !RTMP_Serve(&peer->rtmp) ) {
		ast_log(LOG_WARNING, "RTMP Handshake failed\n"); // TODO: Provide more data
		RTMP_Close(&peer->rtmp);
		ao2_unref(peer);
		return 0;
	}

	RTMP_SendServerBW(&peer->rtmp);
	RTMP_SendClientBW(&peer->rtmp);

	memset(&packet, 0, sizeof(packet));
	while (RTMP_IsConnected(&peer->rtmp) && RTMP_ReadPacket(&peer->rtmp, &packet))
    { // RTMP_ReadPacket blocks in recv
		if (peer->rtmp.m_nBytesIn > (peer->rtmp.m_nBytesInSent + (peer->rtmp.m_nClientBW/2)) ) {
			char pbuf[256], *pend = pbuf + sizeof(pbuf);
			RTMPPacket ackPacket = {
				.m_headerType = RTMP_PACKET_SIZE_MEDIUM,
				.m_packetType = RTMP_PACKET_TYPE_BYTES_READ_REPORT,
				.m_nChannel   = RTMP_NETWORK_CHANNEL,
				.m_body       = pbuf + RTMP_MAX_HEADER_SIZE,
				.m_nBodySize  = 4
			};

			AMF_EncodeInt32(ackPacket.m_body, pend, peer->rtmp.m_nBytesIn);
			peer->rtmp.m_nBytesInSent = peer->rtmp.m_nBytesIn;

			ast_mutex_lock(&peer->lock);
		    rtmp_send_packet(&peer->rtmp, &ackPacket, FALSE);
			ast_mutex_unlock(&peer->lock);
		}

		if (!RTMPPacket_IsReady(&packet)) {
			continue;
		}

		res = rtmp_peer_handle_packet(peer, &packet);
		RTMPPacket_Free(&packet);
		RTMPPacket_Reset(&packet);

		if ( res < 0 ) {
			ast_log(LOG_WARNING, "peer %s, failed to serve packet\n", peer->name);
			break;
		}
	}

	ast_log_if(option_debug, LOG_DEBUG, "peer %s disconnected\n", peer->name);

	rtmp_remove_peer(peer);

	ast_mutex_lock(&peer->lock);
	rtmp_peer_change_state(peer, state_idle);

	if ( peer->current_pvt ) {
		struct ast_channel *chan;
		if ((chan = rtmp_peer_get_channel_locked(peer))) {
			chan->hangupcause = AST_CAUSE_INTERWORKING;
			ast_queue_hangup(chan);
			rtmp_peer_detach_call(peer);
			ast_channel_unlock(chan);
		}
	}

	RTMP_Close(&peer->rtmp);
	close(peer->sockfd);
	peer->sockfd = -1;
	ast_mutex_unlock(&peer->lock);

	// and finally, remove the last reference to the peer
	ao2_unref(peer);

	return 0;
}

static int rtmp_peer_handle_packet(struct rtmp_peer *peer, RTMPPacket *packet)
{
	int res = 0;
	int offset = 0; // for invoke

	if (option_debugaudioin || (rtmp_debug && packet->m_packetType != RTMP_PACKET_TYPE_AUDIO)) {
		ast_log(LOG_DEBUG, "peer %s, received packet type %02X (%u), size %u bytes\n",
		        peer->name, packet->m_packetType, packet->m_packetType, packet->m_nBodySize);
	}

	switch( packet->m_packetType ) {
		case RTMP_PACKET_TYPE_CHUNK_SIZE:
			if (packet->m_nBodySize >= 4) {
				peer->rtmp.m_inChunkSize = AMF_DecodeInt32(packet->m_body);
				ast_log_if(rtmp_debug, LOG_DEBUG, "peer %s's chunk size change to %d\n",
				           peer->name, peer->rtmp.m_inChunkSize);
			}
			break;
		case RTMP_PACKET_TYPE_CONTROL:
			rtmp_peer_handle_packet_control(peer, packet);
			break;
		case RTMP_PACKET_TYPE_SERVER_BW:
			peer->rtmp.m_nServerBW = AMF_DecodeInt32(packet->m_body);
			ast_log_if(rtmp_debug, LOG_DEBUG, "peer %s's server bandwidth set to %d\n",
			           peer->name, peer->rtmp.m_nServerBW);
			break;
		case RTMP_PACKET_TYPE_CLIENT_BW:
			peer->rtmp.m_nClientBW = AMF_DecodeInt32(packet->m_body);
			peer->rtmp.m_nClientBW2 = packet->m_nBodySize > 4 ? packet->m_body[4] : -1;
			ast_log_if(rtmp_debug, LOG_DEBUG, "peer %s's client bandwidth set to %d-%d\n",
			           peer->name, peer->rtmp.m_nClientBW, peer->rtmp.m_nClientBW2);
			break;
		case RTMP_PACKET_TYPE_AUDIO:
			rtmp_peer_handle_packet_audio(peer, packet);
			break;
		case RTMP_PACKET_TYPE_VIDEO:
			//TODO: rxVideo(packet);
			break;
		case RTMP_PACKET_TYPE_FLEX_MESSAGE:
			offset = 1;
		case RTMP_PACKET_TYPE_INVOKE:
			res = rtmp_peer_handle_packet_invoke(peer, packet, offset);
			break;
		case RTMP_PACKET_TYPE_BYTES_READ_REPORT:
		case RTMP_PACKET_TYPE_FLEX_STREAM_SEND:
		case RTMP_PACKET_TYPE_FLEX_SHARED_OBJECT:
		case RTMP_PACKET_TYPE_INFO: // metadata (notify)
		case RTMP_PACKET_TYPE_SHARED_OBJECT:
		case RTMP_PACKET_TYPE_FLASH_VIDEO:
			break;
		default:
			RTMP_Log(RTMP_LOGWARNING, "Unknown packet type (0x%02x)", packet->m_packetType);
			break;
	}

	return res;
}

static int rtmp_peer_handle_packet_audio(struct rtmp_peer *peer, RTMPPacket *packet)
{
	struct ast_channel *chan;
	ast_mutex_lock(&peer->lock);

	if (peer->current_pvt && packet->m_nBodySize > 0 && (chan = rtmp_peer_get_channel_locked(peer))) {
		struct audio_frame_marker header = {
			(unsigned char) packet->m_body[0],
			packet->m_nBodySize - 1
		};

		ast_log_if(option_debugaudioin, LOG_DEBUG,
		           "peer %s queuing frame format: %#0x len: %u in read pipe\n",
	               peer->name, header.marker, header.framesize);

		write(peer->current_pvt->pipe[1], &header, sizeof(header));
		write(peer->current_pvt->pipe[1], packet->m_body+1, header.framesize);
		ast_channel_unlock(chan);
	} else {
		ast_log(LOG_NOTICE, "peer %s: ignoring audio packet, %s\n",
		        peer->name, peer->current_pvt ? "packet is empty" : "media session not set");
	}

	ast_mutex_unlock(&peer->lock);
	return 0;
}

static int rtmp_peer_handle_packet_control(struct rtmp_peer *peer, RTMPPacket *packet)
{
	unsigned short control_type = -1;

	if (packet->m_body && packet->m_nBodySize >= 2) {
		control_type = AMF_DecodeInt16(packet->m_body);
	}

	ast_log_if(rtmp_debug, LOG_DEBUG, "peer %s, received control type: %d\n", peer->name, control_type);

	ast_mutex_lock(&peer->lock);

	switch ( control_type ) {
		case ctrl_msg_ping_request: {
			unsigned int timestamp = AMF_DecodeInt32(packet->m_body + 2);
			rtmp_send_control(&peer->rtmp, ctrl_msg_ping_response, timestamp, 0, RTMP_NETWORK_CHANNEL);
			break;
		}
		case ctrl_msg_stream_begin:
		case ctrl_msg_stream_eof:
		case ctrl_msg_stream_dry:
		case ctrl_msg_set_buffer_length:
		case ctrl_msg_stream_is_recorded:
		case ctrl_msg_ping_response:
		default:
			break;
	}

	ast_mutex_unlock(&peer->lock);
	return 0;
}

static int rtmp_peer_handle_packet_invoke(struct rtmp_peer *peer, RTMPPacket *packet, int offset)
{
	int res;
	double txn;
	AVal method;
	AMFObject amfRequest;
	const char *body = packet->m_body + offset;
	unsigned int nBodySize = packet->m_nBodySize - offset;

	if (body[0] != 0x02) { // make sure it is a string method name we start with
		ast_log(LOG_WARNING, "peer %s, sanity failed. no string method in invoke packet\n", peer->name);
		return -1;
	}

	res = AMF_Decode(&amfRequest, body, nBodySize, FALSE);
	if ( res < 0 ) {
		ast_log(LOG_WARNING, "peer %s, error decoding invoke packet\n", peer->name);
		// in case the error wasn't at the beginning of the buffer
		AMF_Reset(&amfRequest); // AMF_Decode does not release previously allocated data
		return res;
	}

	if ( rtmp_debug ) {
		AMF_Dump(&amfRequest);
	}

	AMFProp_GetString(AMF_GetProp(&amfRequest, NULL, 0), &method);
	txn = AMFProp_GetNumber(AMF_GetProp(&amfRequest, NULL, 1));
	ast_log_if(rtmp_debug, LOG_DEBUG, "peer %s, client invoking <%s>\n", peer->name, method.av_val);

	ast_mutex_lock(&peer->lock);

	// TODO: Remove IFs, Make responses async?
	res = 0;
	if (AVMATCH(&method, &av_connect)) {
		res = rtmp_peer_handle_request_connect(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_invite)) {
		rtmp_peer_handle_request_invite(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_accept)) {
		rtmp_peer_handle_request_accept(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_reject)) {
		rtmp_peer_handle_request_reject(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_hangup)) {
		rtmp_peer_handle_request_hangup(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_sendDTMF)) {
		rtmp_peer_handle_request_dtmf(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_hold)) {
		rtmp_peer_handle_request_hold(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_createStream)) {
		if (peer->state < state_connected) {
			rtmp_peer_send_error_result(peer, txn, "inConcert.Error.InvalidState");
		} else {
			rtmp_peer_send_number_result(peer, txn, ++peer->next_stream_id);
		}
	} else if (AVMATCH(&method, &av_getStreamLength)) {
		rtmp_peer_send_number_result(peer, txn, 0.0); // TODO: find value for live streams (0?, -1?)
	} else if (AVMATCH(&method, &av_play)) {
		rtmp_peer_handle_request_play(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_publish)) {
		rtmp_peer_handle_request_publish(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_closeStream)) {
		rtmp_peer_handle_request_closeStream(peer, &amfRequest, txn, packet);
	} else if (AVMATCH(&method, &av_deleteStream)) {
		int32_t tmp = packet->m_nInfoField2;
		packet->m_nInfoField2 = (int32_t)AMFProp_GetNumber(AMF_GetProp(&amfRequest, NULL, 3));
		rtmp_peer_handle_request_closeStream(peer, &amfRequest, txn, packet);
		packet->m_nInfoField2 = tmp;
	} else if (!AVMATCH(&method, &av__result)) { // ignore response calls
		ast_log(LOG_NOTICE, "peer %s, unknown method %s\n", peer->name, method.av_val);
	}

	AMF_Reset(&amfRequest);
	ast_mutex_unlock(&peer->lock);
	return res;
}

static int rtmp_peer_handle_request_connect(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	static const AVal null_prop = { NULL, 0 };
	int i, have_auth = 0;
	AMFObject properties;
	UNUSED(packet);

	if (peer->state != state_idle) {
		ast_log(LOG_NOTICE, "peer %s, rejecting duplicate connect request\n", peer->name);
		rtmp_peer_send_connect_result(peer, txn, 0);
		return -1; // will close connection
	}

	AMFProp_GetObject(AMF_GetProp(amfRequest, NULL, 2), &properties);
	for ( i = 0; i < properties.o_num; ++i ) {
		const AMFObjectProperty *prop = &properties.o_props[i];
		const AVal *prop_val = prop->p_type == AMF_STRING ? &prop->p_vu.p_aval : &null_prop;
		if (AVMATCH(&prop->p_name, &av_app)) {
			peer->rtmp.Link.app = *prop_val;
		} else if (AVMATCH(&prop->p_name, &av_flashVer)) {
			peer->rtmp.Link.flashVer = *prop_val;
		} else if (AVMATCH(&prop->p_name, &av_swfUrl)) {
			peer->rtmp.Link.swfUrl = *prop_val;
		} else if (AVMATCH(&prop->p_name, &av_tcUrl)) {
			peer->rtmp.Link.tcUrl = *prop_val;
		} else if (AVMATCH(&prop->p_name, &av_pageUrl)) {
			peer->rtmp.Link.pageUrl = *prop_val;
		} else if (AVMATCH(&prop->p_name, &av_audioCodecs)) {
			peer->rtmp.m_fAudioCodecs = prop->p_vu.p_number;
		} else if (AVMATCH(&prop->p_name, &av_videoCodecs)) {
			peer->rtmp.m_fVideoCodecs = prop->p_vu.p_number;
		} else if (AVMATCH(&prop->p_name, &av_objectEncoding)) {
			peer->rtmp.m_fEncoding = prop->p_vu.p_number;
		}
	}

	do { // will break on error
		char tmpbuff[80]; // null terminated
		AMFObjectProperty *p_user, *p_password, *p_dispname;
		AVal av_user, av_password, av_dispname;

		AVAL2STR(tmpbuff, peer->rtmp.Link.app, sizeof(tmpbuff)); // copy app to temp buffer
		if ( ast_strlen_zero(tmpbuff) || strcasecmp(tmpbuff, default_application) ) {
			ast_log(LOG_WARNING, "Unknown application ('%s') from %s:%u\n", tmpbuff,
			        ast_inet_ntoa(peer->sin_addr.sin_addr), ntohs(peer->sin_addr.sin_port));
			break;
		}

		p_user     = AMF_GetProp(amfRequest, NULL, 3); // 1st parameter: name
		p_password = AMF_GetProp(amfRequest, NULL, 4); // 2nd parameter: password
		p_dispname = AMF_GetProp(amfRequest, NULL, 5); // 3rd parameter: display name (opt)
		if (p_user->p_type != AMF_STRING || p_password->p_type != AMF_STRING) {
			break; // invalid types
		}

		AMFProp_GetString(p_user, &av_user);
		AMFProp_GetString(p_password, &av_password);
		AMFProp_GetString(p_dispname, &av_dispname);
		if (av_user.av_len == 0) {
			break; // username cannot be empty
		}

		AVAL2STR(peer->name, av_user, sizeof(peer->name)); // fill peer->name
		AVAL2STR(tmpbuff, av_password, sizeof(tmpbuff)); // copy password to temp buffer
		if ( rtmp_authenticate_peer(peer, tmpbuff) < 0 ) {
			break; // auth failed
		}

		have_auth = 1;
		rtmp_peer_change_state(peer, state_registered);

		// fill peer->callingpres
		if (p_dispname->p_type == AMF_STRING && av_dispname.av_len > 0) {
			AVAL2STR(peer->callingpres, av_dispname, sizeof(peer->callingpres));
		} else {
			ast_copy_string(peer->callingpres, peer->name, sizeof(peer->callingpres));
		}

		if (option_debug)
			ast_verbose(VERBOSE_PREFIX_1 "Connected user %s (%s)\n", peer->name, peer->callingpres);

		peer->capability = 0; // just in case
		if ( peer->rtmp.m_fAudioCodecs ) {
			static struct {
				uint16_t    code;
				int         format;
			} formatmap[] = {
				{ SUPPORT_SND_G711U, AST_FORMAT_ULAW  },
				{ SUPPORT_SND_G711A, AST_FORMAT_ALAW  },
				{ SUPPORT_SND_SPEEX, AST_FORMAT_SPEEX }
			};
			unsigned int codecs = peer->rtmp.m_fAudioCodecs;

			if (option_debug)
				ast_verbose(VERBOSE_PREFIX_2 "Checking user %s's codecs (%f -> %u)\n", peer->name, peer->rtmp.m_fAudioCodecs, codecs);

			for (i = 0; i < (int)ARRAY_LEN(formatmap); ++i) {
				if ( (codecs & formatmap[i].code) && (global_capability & formatmap[i].format) ) {
					peer->capability |= formatmap[i].format;
					ast_codec_pref_append(&peer->codec_prefs, formatmap[i].format);
				}
			}
		}
		if ( !peer->capability ) {
			ast_log(LOG_WARNING, "Peer %s does not support audio\n", peer->name);
		}
	} while (0); // execute only once

	if ( have_auth ) {
		return rtmp_peer_send_connect_result(peer, txn, have_auth);
	}

	return -1; // no auth
}

static int rtmp_peer_handle_request_invite(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{ // this function does always return 0
	AVal av_dest;
	struct ast_channel *chan;
	const char *reject_reason;
	char exten[AST_MAX_EXTENSION];
	UNUSED(packet);

	if (peer->state != state_registered) {
		ast_log(LOG_NOTICE, "peer %s, state %s does not allow to place calls\n",
		        peer->name, state2str(peer->state));
		// TODO: Does this perform as expected? I don't think so
		rtmp_peer_send_error_result(peer, txn, "inConcert.Error.InvalidState");
		return 0;
	}

	// TODO: is this really needed?
	rtmp_peer_send_null_result(peer, txn);

	AMFProp_GetString(AMF_GetProp(amfRequest, NULL, 3), &av_dest);
	AVAL2STR(exten, av_dest, sizeof(exten));
	ast_log_if(rtmp_debug, LOG_DEBUG, "peer %s, calling extension %s\n", peer->name, exten);

	if (!ast_exists_extension(NULL, peer->context, exten, 1, peer->name)) {
		reject_reason = "Unknown extension";
		ast_log(LOG_NOTICE, "peer %s, call rejected because extension %s does not exist\n",
		        peer->name, exten);
	} else if ( !(chan = rtmp_new(peer, exten, AST_STATE_RING)) ) {
		reject_reason = "Internal server error";
	} else { // extension exist, channel was created
		peer->current_pvt = chan->tech_pvt;
		//notify_ringing(); TODO: Confirm this ringing does/(does not) belong here
		rtmp_peer_change_state(peer, (peer->state | state_outgoing));
		return 0;
	}

	if (reject_reason) { // reject_reason is always set at this point
		AMFObjectProperty rejectReason = { .p_type = AMF_STRING };
		STR2AVAL(rejectReason.p_vu.p_aval, reject_reason);
		rtmp_invoke(&peer->rtmp, &av_rejected, &rejectReason, 1);
	}

	return 0;
}

static int rtmp_peer_handle_request_accept(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	struct ast_channel *chan;
	UNUSED(packet);
	UNUSED(amfRequest);

	rtmp_peer_send_null_result(peer, txn);
	// Note: peer->state & state_incoming should "guarantee" a valid current_pvt
	if (peer->state == (state_registered|state_incoming) && (chan = rtmp_peer_get_channel_locked(peer))) {
		AMFObjectProperty props[] = {
			{ .p_type = AMF_STRING },
			{ .p_type = AMF_NULL   } // TODO: video codec
		};

		ast_queue_control(chan, AST_CONTROL_ANSWER);
		ast_setstate(chan, AST_STATE_UP);

		peer->micformat = (chan->readformat & peer->capability) ? chan->readformat : AST_FORMAT_SPEEX;
		STR2AVAL(props[0].p_vu.p_aval, ast_getformatname(peer->micformat));
		rtmp_invoke(&peer->rtmp, &av_accepted, props, 2);

		rtmp_peer_change_state(peer, (peer->state | state_talking)); // registered|incoming|talking
		ast_channel_unlock(chan);
	}
	return 0;
}

static int rtmp_peer_handle_request_reject(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	struct ast_channel *chan;
	UNUSED(packet);
	UNUSED(amfRequest);

	rtmp_peer_send_null_result(peer, txn);

	if (peer->state == (state_registered|state_incoming) && (chan = rtmp_peer_get_channel_locked(peer))) {
		chan->hangupcause = AST_CAUSE_CALL_REJECTED;
		ast_queue_control(chan, AST_CONTROL_BUSY);
		rtmp_peer_detach_call(peer);
		ast_channel_unlock(chan);
	}
	rtmp_peer_change_state(peer, (peer->state & ~state_incoming)); // will set back to registered
	return 0;
}

static int rtmp_peer_handle_request_hangup(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	struct ast_channel *chan;
	UNUSED(packet);
	UNUSED(amfRequest);

	rtmp_peer_send_null_result(peer, txn);

	if ((peer->state & (state_talking|state_outgoing)) && (chan = rtmp_peer_get_channel_locked(peer))) {
		ast_log_if(option_debug, LOG_DEBUG, "peer %s, hanging-up on channel %s\n",
		           peer->name, chan->name);
		ast_queue_hangup(chan);
		rtmp_peer_detach_call(peer);
		ast_channel_unlock(chan);
	}
	rtmp_peer_change_state(peer, peer->state & ~(state_talking|state_incoming|state_outgoing)); // registered
	return 0;
}

static int rtmp_peer_handle_request_dtmf(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	UNUSED(txn);
	UNUSED(packet);

	if ( peer->current_pvt ) {
		AVal av_digit;
		struct audio_frame_marker header = { FRAME_MARKER_DTMF, 0 };
		AMFProp_GetString(AMF_GetProp(amfRequest, NULL, 3), &av_digit);
		ast_log_if(option_debugaudioin, LOG_DEBUG, "peer %s, writing DTMF frame %s in read pipe\n",
		           peer->name, av_digit.av_val);
		write(peer->current_pvt->pipe[1], &header, sizeof(header));
		write(peer->current_pvt->pipe[1], av_digit.av_val, 1);
	} else {
		ast_log(LOG_NOTICE, "peer %s: got DTMF but no media session is set\n", peer->name);
	}
	return 0;
}

static int rtmp_peer_handle_request_hold(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	struct ast_channel *chan;
	UNUSED(txn);
	UNUSED(packet);

	if ((chan = rtmp_peer_get_channel_locked(peer))) {
		int do_hold = AMFProp_GetBoolean(AMF_GetProp(amfRequest, NULL, 3));
		if ( do_hold ) {
			ast_queue_control(peer->current_pvt->owner, AST_CONTROL_HOLD);
			peer->current_pvt->hold = peer->current_pvt->owner->_bridge;
		} else {
			ast_queue_control(peer->current_pvt->owner, AST_CONTROL_UNHOLD);
			peer->current_pvt->hold = NULL;
		}
		ast_channel_unlock(chan);
	}

	return 0;
}

static int rtmp_peer_handle_request_play(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	UNUSED(txn);

	peer->play_stream_id = packet->m_nInfoField2;
	rtmp_send_control(&peer->rtmp, ctrl_msg_stream_begin, 1, 0, RTMP_AUDIO_CHANNEL);
	AMFProp_GetString(AMF_GetProp(amfRequest, NULL, 3), &peer->play_stream_name);
	rtmp_send_on_status(&peer->rtmp, &av_NetStream_Play_Start, &peer->play_stream_name, peer->play_stream_id, RTMP_AUDIO_CHANNEL);
	return 0;
}

static int rtmp_peer_handle_request_publish(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	UNUSED(txn);

	peer->publish_stream_id = packet->m_nInfoField2;
	AMFProp_GetString(AMF_GetProp(amfRequest, NULL, 3), &peer->publish_stream_name);
	rtmp_send_on_status(&peer->rtmp, &av_NetStream_Publish_Start, &peer->publish_stream_name, peer->publish_stream_id, RTMP_SOURCE_CHANNEL);
	return 0;
}

static int rtmp_peer_handle_request_closeStream(struct rtmp_peer *peer, AMFObject *amfRequest, double txn, RTMPPacket *packet)
{
	UNUSED(txn);
	UNUSED(packet);
	UNUSED(amfRequest);

	if (peer->play_stream_id && peer->play_stream_id == (unsigned int)packet->m_nInfoField2) {
		rtmp_send_control(&peer->rtmp, ctrl_msg_stream_eof, 1, 0, RTMP_AUDIO_CHANNEL);
		rtmp_send_on_status(&peer->rtmp, &av_NetStream_Play_Stop, &peer->play_stream_name, peer->play_stream_id, RTMP_AUDIO_CHANNEL);
		peer->play_stream_id = 0;
	}
	else if (peer->publish_stream_id && peer->publish_stream_id == (unsigned int)packet->m_nInfoField2) {
		rtmp_send_on_status(&peer->rtmp, &av_NetStream_Publish_Stop, &peer->publish_stream_name, peer->publish_stream_id, RTMP_SOURCE_CHANNEL);
		peer->publish_stream_id = 0;
	}
	return 0;
}

static int rtmp_peer_send_connect_result(struct rtmp_peer *peer, double txn, int success)
{
	char pbuf[384], *pend = pbuf+sizeof(pbuf), *enc;
	RTMPPacket packet = {
		.m_headerType = RTMP_PACKET_SIZE_LARGE,
		.m_packetType = RTMP_PACKET_TYPE_INVOKE, // RTMP_PACKET_SIZE_LARGE works ok with both flash and red5 clients
		.m_nChannel   = RTMP_SYSTEM_CHANNEL,
		.m_body       = pbuf + RTMP_MAX_HEADER_SIZE
	};

	enc = packet.m_body;
	// Command Name|String| _result or _error; indicates whether the response is result or error.
	enc = AMF_EncodeString(enc, pend, success ? &av__result : &av__error);
	// Transaction ID|Number|Transaction ID is 1 for connect responses
	enc = AMF_EncodeNumber(enc, pend, txn);
	*enc++ = AMF_NULL;
	//Information| Object|Name-value pairs that describe the response from the server.
	// 'code', 'level', 'description' are names of few among such information.
	_AMF_OBJECT_BEGIN(enc);
		enc = AMF_EncodeNamedString(enc, pend, &av_level, &av_status);
		if ( success ) {
			enc = AMF_EncodeNamedString(enc, pend, &av_code, &av_NetConnection_Connect_Success);
			enc = AMF_EncodeNamedString(enc, pend, &av_description, &av_Connection_succeeded);
		} else {
			enc = AMF_EncodeNamedString(enc, pend, &av_code, &av_NetConnection_Connect_Rejected);
			enc = AMF_EncodeNamedString(enc, pend, &av_description, &av_Connection_rejected);
		}
		enc = AMF_EncodeNamedNumber(enc, pend, &av_objectEncoding, peer->rtmp.m_fEncoding);
		enc = AMF_EncodeNamedString(enc, pend, &av_fmsVer, &av_fmsVerVal);
		enc = AMF_EncodeNamedNumber(enc, pend, &av_capabilities, 31.0);
		enc = AMF_EncodeNamedNumber(enc, pend, &av_mode, 1.0);
	_AMF_OBJECT_END(enc);

	packet.m_nBodySize = enc - packet.m_body;
	return rtmp_send_packet(&peer->rtmp, &packet, FALSE);
}

static int rtmp_peer_send_error_result(struct rtmp_peer *peer, double txn, const char *message)
{
	AVal av;
	char pbuf[512], *pend = pbuf+sizeof(pbuf), *enc;
	RTMPPacket packet = {
		.m_headerType = RTMP_PACKET_SIZE_MEDIUM,
		.m_packetType = RTMP_PACKET_TYPE_INVOKE,
		.m_nChannel   = RTMP_SYSTEM_CHANNEL,
		.m_body       = pbuf + RTMP_MAX_HEADER_SIZE
	};

	enc = packet.m_body;
	enc = AMF_EncodeString(enc, pend, &av__error);
	enc = AMF_EncodeNumber(enc, pend, txn);
	*enc++ = AMF_NULL;
	_AMF_OBJECT_BEGIN(enc);
		STR2AVAL(av, message);
		enc = AMF_EncodeNamedString(enc, pend, &av_level, &av_error);
		enc = AMF_EncodeNamedString(enc, pend, &av_code, &av);
	_AMF_OBJECT_END(enc);

	packet.m_nBodySize = enc - packet.m_body;
	return rtmp_send_packet(&peer->rtmp, &packet, FALSE);
}

static int rtmp_peer_send_null_result(struct rtmp_peer *peer, double txn)
{
	char pbuf[256], *pend = pbuf+sizeof(pbuf), *enc;
	RTMPPacket packet = {
		.m_headerType = RTMP_PACKET_SIZE_MEDIUM,
		.m_packetType = RTMP_PACKET_TYPE_INVOKE,
		.m_nChannel   = RTMP_SYSTEM_CHANNEL,
		.m_body       = pbuf + RTMP_MAX_HEADER_SIZE
	};

	enc = packet.m_body;
	enc = AMF_EncodeString(enc, pend, &av__result);
	enc = AMF_EncodeNumber(enc, pend, txn);
	*enc++ = AMF_NULL;

	packet.m_nBodySize = enc - packet.m_body;
	return rtmp_send_packet(&peer->rtmp, &packet, FALSE);
}

static int rtmp_peer_send_number_result(struct rtmp_peer *peer, double txn, double number)
{
	char pbuf[256], *pend = pbuf+sizeof(pbuf), *enc;
	RTMPPacket packet = {
		.m_headerType = RTMP_PACKET_SIZE_MEDIUM,
		.m_packetType = RTMP_PACKET_TYPE_INVOKE,
		.m_nChannel   = RTMP_SYSTEM_CHANNEL,
		.m_body       = pbuf + RTMP_MAX_HEADER_SIZE
	};

	enc = packet.m_body;
	enc = AMF_EncodeString(enc, pend, &av__result);
	enc = AMF_EncodeNumber(enc, pend, txn);
	*enc++ = AMF_NULL;
	enc = AMF_EncodeNumber(enc, pend, number);

	packet.m_nBodySize = enc - packet.m_body;
	return rtmp_send_packet(&peer->rtmp, &packet, FALSE);
}


static struct ast_channel *rtmp_peer_get_channel_locked(struct rtmp_peer *peer)
{ // this function is called with the lock held
	while (peer->current_pvt && ast_channel_trylock(peer->current_pvt->owner)) {
		DEADLOCK_AVOIDANCE(&peer->lock);
	}
	return peer->current_pvt ? peer->current_pvt->owner : NULL;
}

static int rtmp_peer_change_state(struct rtmp_peer *peer, unsigned int newstate)
{
	char oldstate[40];
	ast_log_if(rtmp_debug, LOG_DEBUG, "Peer %s: Changing state from %s to %s\n",
	           peer->name, __state2str(peer->state, oldstate, 40), state2str(newstate));
	// TODO: Handle transitions between states?
	peer->state = newstate;
	return 0;
}

static int rtmp_peer_detach_call(struct rtmp_peer *peer)
{
	if ( peer->current_pvt ) {
		ast_set_flag(&peer->current_pvt->flags, pvt_flag_detached);
		ao2_unref(peer->current_pvt->peer);
		peer->current_pvt =  NULL;
	}
	return 0;
}

static int rtmp_authenticate_peer(struct rtmp_peer *peer, const char *password)
{ // FIXME: Use the ao2 container
	int authorized = 0;
	struct ast_variable *var, *tmp;
	struct rtmp_peer *oldpeer;

	if ( (oldpeer = ao2_find(rtmp_peers, peer->name, 0)) ) {
		ao2_unref(oldpeer);
		ast_log(LOG_NOTICE, "User %s already logged in\n", peer->name);
		return -1;
	}

	// TODO: There is a race condition here, we will not handle it rigth now

	var = ast_load_realtime("rtmppeers", "name", peer->name, NULL);
	if (var) {
		for (tmp = var; tmp; tmp = tmp->next) {
			if (!strcmp(tmp->name, "secret") && !strcmp(tmp->value, password)) {
				authorized = 1;
				break;
			} else if (!strcmp(tmp->name, "context")) { // keep the last context
				ast_copy_string(peer->context, tmp->value, sizeof(peer->context));
			}
		}
		ast_variables_destroy(var);
	}

	// TODO: Extra validation step: peer->rtmp.Link.swfUrl

	if ( authorized ) { // TODO: Set session data
		if (ast_strlen_zero(peer->context)) {
			ast_copy_string(peer->context, default_context, sizeof(peer->context));
		}

		ao2_link(rtmp_peers, peer); // increases reference count
	}

	return authorized ? 0 : -1;
}

static int rtmp_remove_peer(struct rtmp_peer *peer)
{
	ao2_unlink(rtmp_peers, peer);
	return 0;
}


static struct ast_channel *rtmp_new(struct rtmp_peer *peer, const char *exten, int state)
{
	struct ast_channel *chan;
	struct rtmp_tech_pvt *pvt;

	chan = ast_channel_alloc(1, state, peer->name, peer->callingpres, NULL, peer->name, peer->context, 0,
	                         "RTMP/%s-%08x", peer->name, ast_atomic_fetchadd_int((int *)&call_count, +1));
	if (chan == NULL) {
		ast_log(LOG_ERROR, "Unable to allocate AST channel structure for RTMP channel\n");
		return NULL;
	}

	if ( !(pvt = pvt_alloc(chan, peer)) ) {
		ast_log(LOG_ERROR, "Could not allocate pvt\n");
		ast_channel_free(chan);
		return NULL;
	}
	chan->tech = &rtmp_tech;
	chan->tech_pvt = pvt;
	chan->fds[0] = pvt->pipe[0];

	chan->nativeformats = peer->capability;  //ast_codec_choose(&s->codec_prefs, rtmp_tech.capabilities, 1);
	int fmt = ast_codec_choose(&peer->codec_prefs, peer->capability ?: global_capability, 1); //ast_best_codec(chan->nativeformats);
	chan->writeformat = fmt;
	chan->rawwriteformat = fmt;
	chan->readformat = fmt;
	chan->rawreadformat = fmt;
	if (state == AST_STATE_RING) {
		chan->rings = 1;
	}
	ast_string_field_set(chan, language, "");
	ast_string_field_set(chan, accountcode, "");
	ast_string_field_set(chan, call_forward, "");
	ast_copy_string(chan->context, peer->context, sizeof(chan->context));
	ast_copy_string(chan->exten, S_OR(exten, peer->name), sizeof(chan->exten));
	chan->amaflags = 0; // default

	// update use count
	ast_module_ref(ast_module_info->self);
	//chan->callgroup = s->callgroup();
	//chan->pickupgroup = s->pickupgroup();
	pbx_builtin_setvar_helper(chan, "RTMPUSER", peer->name);
	ast_set_flag(chan, AST_FLAG_END_DTMF_ONLY);

	// Avoid ast_set_callerid() here because it will generate a needless NewCallerID event
	chan->cid.cid_ani = ast_strdup(peer->name);
	chan->priority = 1;
	chan->adsicpe = AST_ADSI_UNAVAILABLE;

	if (state != AST_STATE_DOWN && ast_pbx_start(chan)) {
		ast_log(LOG_WARNING, "Unable to start PBX on %s\n", chan->name);
		chan->hangupcause = AST_CAUSE_SWITCH_CONGESTION;
		ast_hangup(chan);
		chan = NULL;
	}

	return chan;
}

static struct ast_channel *rtmp_request(const char *type, int format, void *data, int *cause)
{
	struct rtmp_peer *peer;
	struct ast_channel *chan;
	char *destaddr = data;
	UNUSED(type);
	UNUSED(format);
	UNUSED(data);

	if (ast_strlen_zero(destaddr)) {
		ast_log(LOG_ERROR, "Unable to create channel with empty destination.\n");
		*cause = AST_CAUSE_CHANNEL_UNACCEPTABLE;
		return NULL;
	}

	if ( !(peer = ao2_find(rtmp_peers, destaddr, 0)) ) {
		*cause = AST_CAUSE_UNREGISTERED;
		if (option_debug > 2)
			ast_log(LOG_DEBUG, "Can't create RTMP call - Peer %s not registered\n", destaddr);
		return NULL;
	}

	if (!(chan = rtmp_new(peer, NULL, AST_STATE_DOWN))) {
		*cause = AST_CAUSE_SWITCH_CONGESTION;
	}
	ao2_unref(peer); // reference was increased by ao2_find
	return chan;
}

static int rtmp_send_digit(struct ast_channel *ast, char digit, unsigned int duration)
{
	int res = -1;
	struct rtmp_peer *peer;
	struct rtmp_tech_pvt *pvt = ast->tech_pvt;
	UNUSED(duration);

	if ((peer = pvt->peer)) {
		AMFObjectProperty arg = { .p_type = AMF_STRING, .p_vu = { .p_aval={&digit,1} } };
		ast_mutex_lock(&peer->lock);
		res = rtmp_invoke(&peer->rtmp, &av_ondtmf, &arg, 1);
		ast_mutex_unlock(&peer->lock);
	}
	return res;
}

static int rtmp_call(struct ast_channel *ast, char *dest, int timeout)
{
	struct rtmp_peer *peer;
	struct rtmp_tech_pvt *pvt = ast->tech_pvt;
	AMFObjectProperty args[] = {
		{ .p_type = AMF_STRING },
		{ .p_type = AMF_NULL   } // TODO: Remove 'to' field?
	};
	UNUSED(timeout);

	if ((ast->_state != AST_STATE_DOWN) && (ast->_state != AST_STATE_RESERVED)) {
		ast_log(LOG_WARNING, "rtmp_call called on %s, neither down nor reserved\n", ast->name);
		return -1;
	}

	peer = pvt->peer;
	if ( !peer ) {
		ast_log(LOG_DEBUG, "Can't call %s: not registered\n", dest);
		return -1;
	}

	ast_mutex_lock(&peer->lock);

	if ( peer->state != state_registered ) {
		ast_log(LOG_NOTICE, "User %s: Cannot call peer due to state being %s\n",
		        pvt->peer->name, state2str(pvt->peer->state));
		if ( peer->state & (state_incoming|state_outgoing|state_talking) ) {
			ast_queue_control(ast, AST_CONTROL_BUSY);
		} else {
			ast_queue_control(ast, AST_CONTROL_CONGESTION);
		}
		ast_mutex_unlock(&peer->lock);
		return -1;
	}

	// invoke invited() at remote endpoint
	STR2AVAL(args[0].p_vu.p_aval, ast->cid.cid_num); // put caller id in first prop
	if (rtmp_invoke(&peer->rtmp, &av_invited, args, ARRAY_LEN(args)) < 0) {
		ast_mutex_unlock(&peer->lock);
		return -1;
	}

	rtmp_peer_change_state(peer, (peer->state | state_incoming));
	peer->current_pvt = pvt;
	ast_mutex_unlock(&peer->lock);

	ast_queue_control(ast, AST_CONTROL_RINGING);
	ast_setstate(ast, AST_STATE_RINGING);
	return 0;
}

static int rtmp_hangup(struct ast_channel *ast)
{
	struct rtmp_peer *peer;
	struct rtmp_tech_pvt *pvt = ast->tech_pvt;

	if ( (peer = pvt->peer) ) {
		ast_mutex_lock(&peer->lock);

		if ( peer->current_pvt ) {
			rtmp_peer_detach_call(peer);
		} else {
			ast_log(LOG_NOTICE, "peer %s: current pvt is not set\n", peer->name);
		}

		if (peer->state & state_talking) {
			rtmp_invoke(&peer->rtmp, &av_byed, NULL, 0);
		} else if ( !ast_test_flag(&pvt->flags, pvt_flag_rejected) ) {
			// TODO: Do not send cancelled in outgoing call
			rtmp_invoke(&peer->rtmp, &av_cancelled, NULL, 0);
		}

		rtmp_peer_change_state(peer, state_registered);
		ast_mutex_unlock(&peer->lock);
	}

	pvt_free(pvt);
	ast->tech_pvt = NULL;
	return 0;
}

static int rtmp_answer(struct ast_channel *ast)
{
	int res;
	struct rtmp_peer *peer;
	struct rtmp_tech_pvt *pvt = ast->tech_pvt;

	if ( !(peer = pvt->peer) ) {
		ast_log(LOG_WARNING, "Channel %s tried to answer with unknown peer\n", ast->name);
		return -1;
	}

	ast_mutex_lock(&peer->lock);

	if (peer->state == (state_registered|state_outgoing) || (peer->state & state_talking) ) {
	// some apps call ast_answer() 'just in case'
		res = 0;

		if ( !(peer->state & state_talking) ) {
		// not talking yet (note we support a single line per user :P)
			peer->micformat = (ast->readformat & peer->capability) ? ast->readformat : AST_FORMAT_SPEEX;
			AMFObjectProperty args[] = {
				{ .p_type = AMF_STRING },
				{ .p_type = AMF_NULL   } // TODO: video codec
			};
			STR2AVAL(args[0].p_vu.p_aval, ast_getformatname(peer->micformat));

			res = rtmp_invoke(&peer->rtmp, &av_accepted, args, ARRAY_LEN(args));
			if ( res == 0 ) {
				rtmp_peer_change_state(peer, peer->state | state_talking);
			}
		}
	} else { // current state does not allow answer
		res = -1;
		ast_log(LOG_WARNING, "peer %s: cannot answer while state is %s\n",
		        peer->name, state2str(peer->state));
	}

	ast_mutex_unlock(&peer->lock);
	return res;
}

static struct ast_frame *rtmp_read(struct ast_channel *ast)
{
	int format;
	struct ast_frame *f;
	struct audio_frame_marker header;
	struct rtmp_tech_pvt *pvt = ast->tech_pvt;

	size_t res = read(pvt->pipe[0], &header, sizeof(header));
	if (res != sizeof(header)) {
		ast_log(LOG_ERROR, "Failed to read frame header from channel pipe %s\n", ast->name);
		return &ast_null_frame;
	}

	switch (header.marker) {
		case FRAME_MARKER_SPEEX:
			format = AST_FORMAT_SPEEX; break;
		case FRAME_MARKER_ALAW:
			format = AST_FORMAT_ALAW; break;
		case FRAME_MARKER_ULAW:
			format = AST_FORMAT_ULAW; break;
		case FRAME_MARKER_SLINEAR:
			format = AST_FORMAT_SLINEAR; break;
		case FRAME_MARKER_DTMF:
			format = -1; break;
		default:
			ast_log(LOG_WARNING, "Dont know how to handle audio type %#x\n", header.marker);
			return &ast_null_frame;
	}

	if ( format > 0 )
	{ // audio frame
		char *buf = ast_malloc(header.framesize);
		res = read(pvt->pipe[0], buf, header.framesize);
		if (res != header.framesize) {
			ast_log(LOG_ERROR, "Failed to read frame from channel %s\n", ast->name);
			free(buf);
			return &ast_null_frame;
		}

		f = ast_calloc(1, sizeof(struct ast_frame));
		f->mallocd   = AST_MALLOCD_HDR | AST_MALLOCD_DATA;
		f->frametype = AST_FRAME_VOICE;
		f->subclass  = format;
		f->data      = buf;
		f->datalen   = header.framesize;
		f->samples   = ast_codec_get_samples(f);
	}
	else
	{ // DTMF frame
		char digit;
		res = read(pvt->pipe[0], &digit, 1);
		if (res != 1) {
			ast_log(LOG_ERROR, "Failed to read frame from channel %s\n", ast->name);
			return &ast_null_frame;
		}

		f = ast_calloc(1, sizeof(struct ast_frame));
		f->frametype = AST_FRAME_DTMF;
		f->mallocd   = AST_MALLOCD_HDR;
		f->subclass = digit;
	}

	return f;
}

static int rtmp_write(struct ast_channel *ast, struct ast_frame *frame)
{
	int res = 0;
	uint8_t *buf;
	char pbuf[512];
	struct rtmp_peer *peer;
	struct rtmp_tech_pvt *pvt = ast->tech_pvt;

	if (frame->frametype != AST_FRAME_VOICE) { // AST_FRAME_VIDEO
		ast_log(LOG_WARNING, "Don't know what to do with frame type '%d'\n", frame->frametype);
		return 0;
	}

	if (ast->_state != AST_STATE_UP) {
		return 0;
	}

	if ( pvt->hold == ast->_bridge ) {
	// peer put endpoint on hold, we must ignore all incoming frames from this channel
		return 0;
	}

	peer = pvt->peer;
	if ( !(peer && peer->current_pvt) ) {
		ast_log(LOG_WARNING, "channel %s cannot send audio, %s\n", ast->name,
		        peer ? "pvt is not set" : "peer gone");
		return -1;
	}

	ast_mutex_lock(&peer->lock);

	if ( !peer->play_stream_id ) {
		ast_log(LOG_NOTICE, "peer %s: cannot send audio, stream not open\n", peer->name);
		ast_mutex_unlock(&peer->lock);
		return 0;
	}

	if ( peer->current_pvt == pvt ) { // always true
		RTMPPacket packet = {
			.m_headerType      = RTMP_PACKET_SIZE_LARGE,
			.m_packetType      = RTMP_PACKET_TYPE_AUDIO,
			.m_nChannel        = RTMP_AUDIO_CHANNEL,
			.m_body            = pbuf + RTMP_MAX_HEADER_SIZE,
			.m_nInfoField2     = peer->play_stream_id,
			.m_nTimeStamp      = pvt_timestamp(pvt),
			.m_hasAbsTimestamp = 1
		};

		buf = (uint8_t *)packet.m_body;
		switch (frame->subclass) {
			case AST_FORMAT_SPEEX:
				*buf++ = FRAME_MARKER_SPEEX; break;
			case AST_FORMAT_ULAW:
				*buf++ = FRAME_MARKER_ULAW; break;
			case AST_FORMAT_ALAW:
				*buf++ = FRAME_MARKER_ALAW; break;
			case AST_FORMAT_SLINEAR:
				*buf++ = FRAME_MARKER_SLINEAR; break;
			default:
				ast_log(LOG_WARNING, "Unknown format %d (%s)\n", frame->subclass, ast_getformatname(frame->subclass));
				ast_mutex_unlock(&peer->lock);
				return -1;
		}

		ast_log_if(option_debugaudioout, LOG_DEBUG, "writing frame format: %s marker: %#0x len: %d timestamp: %u\n",
			ast_getformatname(frame->subclass), buf[-1], frame->datalen, packet.m_nTimeStamp);

		memcpy(buf, frame->data , frame->datalen);
		packet.m_nBodySize = frame->datalen + 1;

		//check_chunk_size(&peer->rtmp, packet.m_nBodySize + RTMP_MAX_HEADER_SIZE);

		if (peer->micformat != frame->subclass) {
			AMFObjectProperty arg = { .p_type = AMF_STRING };
			peer->micformat = frame->subclass;
			STR2AVAL(arg.p_vu.p_aval, ast_getformatname(peer->micformat));
			rtmp_invoke(&peer->rtmp, &av_codecChanged, &arg, 1);
		}

		rtmp_check_chunksize(&peer->rtmp, packet.m_nBodySize);

		res = rtmp_send_packet(&peer->rtmp, &packet, 0);
	}

	ast_mutex_unlock(&peer->lock);
	return res;
}

static int rtmp_indicate(struct ast_channel *ast, int condition, const void *data, size_t datalen)
{
	int res = 0;
	struct rtmp_peer *peer;
	struct rtmp_tech_pvt *pvt = ast->tech_pvt;
	UNUSED(data);
	UNUSED(datalen);

	if ( !(peer = pvt->peer) ) {
		ast_log(LOG_WARNING, "Cannot indicate, peer is null\n");
		return -1;
	}

	ast_mutex_lock(&peer->lock);
	switch (condition) {
		case AST_CONTROL_RINGING:
			rtmp_invoke(&peer->rtmp, &av_ringing, NULL, 0);
			break;
		case AST_CONTROL_BUSY:
		case AST_CONTROL_CONGESTION: {
				AMFObjectProperty rejectReason = { .p_type = AMF_STRING };
				STR2AVAL(rejectReason.p_vu.p_aval, condition == AST_CONTROL_BUSY ? "Busy" : "Congestion");
				rtmp_invoke(&peer->rtmp, &av_rejected, &rejectReason, 1);
				ast_set_flag(&pvt->flags, pvt_flag_rejected);
			}
			break;
		case AST_CONTROL_HOLD:
			if (!ast_test_flag(&pvt->flags, pvt_flag_hold_sent)) {
				AMFObjectProperty arg = { .p_type = AMF_BOOLEAN, .p_vu = {.p_number = 1} };
				rtmp_invoke(&peer->rtmp, &av_onhold, &arg, 1);
				ast_set_flag(&pvt->flags, pvt_flag_hold_sent);
			}
			ast_moh_start(ast, 0, 0);
			break;
		case AST_CONTROL_UNHOLD:
			if (ast_test_flag(&pvt->flags, pvt_flag_hold_sent)) {
				AMFObjectProperty arg = { .p_type = AMF_BOOLEAN, .p_vu = {.p_number = 0} };
				rtmp_invoke(&peer->rtmp, &av_onhold, &arg, 1);
				ast_clear_flag(&pvt->flags, pvt_flag_hold_sent);
			}
			ast_moh_stop(ast);
			break;
		case AST_CONTROL_PROGRESS:
		case AST_CONTROL_PROCEEDING:
		case AST_CONTROL_SRCUPDATE:
		case AST_CONTROL_SRCCHANGE:
			break;
		default:
			ast_log(LOG_WARNING, "Don't know how to indicate condition %d\n", condition);
			res = -1;
	}
	ast_mutex_unlock(&peer->lock);

	return res;
}

static int rtmp_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct rtmp_tech_pvt *pvt = newchan->tech_pvt;
	ast_log(LOG_NOTICE, "Fixing up channel %s -> %s\n", oldchan->name, newchan->name);
	if (pvt->owner != oldchan) {
		ast_log(LOG_WARNING, "old channel wasn't %p but was %p\n", oldchan, pvt->owner);
		return -1;
	}
	pvt->owner = newchan;
	return 0;
}


static __thread char rtmp_log_buffer[1024];
static void rtmp_log_callback(int level, const char *fmt, va_list va)
{
	vsnprintf(rtmp_log_buffer, sizeof(rtmp_log_buffer)-1, fmt, va);
	switch ( level ) {
		case RTMP_LOGCRIT:
		case RTMP_LOGERROR:
			ast_log(LOG_ERROR, "%s\n", rtmp_log_buffer);
			break;
		case RTMP_LOGWARNING:
			ast_log(LOG_WARNING, "%s\n", rtmp_log_buffer);
			break;
		case RTMP_LOGINFO:
			ast_log(LOG_NOTICE, "%s\n", rtmp_log_buffer);
			break;
		case RTMP_LOGDEBUG:
			ast_log_if(rtmp_debug, LOG_DEBUG, "%s\n", rtmp_log_buffer);
			break;
		case RTMP_LOGDEBUG2:
			ast_log_if(rtmp_debug && option_debug>3, LOG_DEBUG, "%s\n", rtmp_log_buffer);
			break;
	}
}

static int handle_cli_show_settings(int fd, int argc, char *argv[])
{
	char capsbuf[50];
	UNUSED(argc);
	UNUSED(argv);

	ast_getformatname_multiple(capsbuf, sizeof(capsbuf), global_capability);

	ast_cli2(fd, "%-30s -> %u\n", "RTMP port", ntohs(bindaddr.sin_port));
	ast_cli2(fd, "%-30s -> %s\n", "Bind Address", ast_inet_ntoa(bindaddr.sin_addr));
	ast_cli2(fd, "%-30s -> %s\n", "Default application", default_application);
	ast_cli2(fd, "%-30s -> %s\n", "Default Context", default_context);
	ast_cli2(fd, "%-30s -> %s\n", "Capabilities", capsbuf);
	return RESULT_SUCCESS;
}

static int handle_cli_show_peers(int fd, int argc, char *argv[])
{
	static const char *format = "%-10s%-20s%-28s%-25s%-22s\n";
	struct ao2_iterator i;
	struct rtmp_peer *peer = NULL;
	UNUSED(argc);
	UNUSED(argv);

	ast_cli2(fd, format, "Username", "Context", "State", "Caps", "Address");
	ast_cli2(fd, format, "--------", "-------", "-----", "----", "-------");

	i = ao2_iterator_init(rtmp_peers, 0);
	while ((peer = ao2_iterator_next(&i))) {
		char capsbuf[26], addrbuff[22];
		ast_getformatname_multiple(capsbuf, sizeof(capsbuf), peer->capability);
		snprintf(addrbuff, sizeof(addrbuff), "%s:%u",
		         ast_inet_ntoa(peer->sin_addr.sin_addr),
		         ntohs(peer->sin_addr.sin_port));
		ast_cli2(fd, format, peer->name, peer->context, state2str(peer->state), capsbuf, addrbuff);
		ao2_unref(peer);
	}
	ao2_iterator_destroy(&i);
	ast_cli2(fd, "Total users: %d\n", ao2_container_count(rtmp_peers));
	return RESULT_SUCCESS;
}

static int handle_cli_show_channels(int fd, int argc, char *argv[])
{
	static const char *format = "%-32s%-10s%-13s%-13s%-20s\n";
	struct ao2_iterator i;
	struct rtmp_peer *peer;
	unsigned int numchannels;
	UNUSED(argc);
	UNUSED(argv);

	ast_cli2(fd, format, "Channel", "Peer", "Write format", "Read format", "Native formats");
	ast_cli2(fd, format, "-------", "----", "------------", "-----------", "--------------");

	numchannels = 0;
	i = ao2_iterator_init(rtmp_peers, 0);
	while ((peer = ao2_iterator_next(&i))) {
		char capsbuf[26];
		struct ast_channel *chan;
		ast_mutex_lock(&peer->lock);
		if ( (chan = rtmp_peer_get_channel_locked(peer)) ) {
			++numchannels;
			char *channelname = ast_strdup(chan->name); // FIXME: Use strdupa
			if (strlen(channelname) > 31) {
				channelname[31] = 0;
				channelname[30] = channelname[29] = '.';
			}
			ast_cli2(fd, format, channelname, peer->name,
					ast_getformatname(chan->writeformat),
					ast_getformatname(chan->readformat),
					ast_getformatname_multiple(capsbuf, sizeof(capsbuf), chan->nativeformats)
				);
			ast_free(channelname); // FIXME: Remove when using strdupa
			ast_channel_unlock(chan);
		}
		ast_mutex_unlock(&peer->lock);
		ao2_unref(peer);
	}
	ao2_iterator_destroy(&i);
	ast_cli2(fd, "Total channels: %u\n", numchannels);
	return RESULT_SUCCESS;
}

static int handle_cli_set_debug(int fd, int argc, char *argv[])
{
	switch ( argc ) {
		case 3:
			ast_set_flag(&global_flags, RTMP_FLAG_DEBUG);
			ast_cli2(fd, "RTMP Debugging Enabled\n");
			break;
		case 4:
			if (!strcmp(argv[3], "audio")) {
				ast_set_flag(&global_flags, RTMP_FLAG_DEBUG_AIN|RTMP_FLAG_DEBUG_AOUT);
				ast_cli2(fd, "RTMP Audio Debugging Enabled\n");
			} else if (!strcmp(argv[3], "off")) { // turn off all debugging flags
				ast_clear_flag(&global_flags, RTMP_FLAG_DEBUG|RTMP_FLAG_DEBUG_AIN|RTMP_FLAG_DEBUG_AOUT);
				ast_cli2(fd, "RTMP Debugging Disabled\n");
			}
			break;
		case 5:
		case 6:
			if (!strcmp(argv[4], "in")) {
				if (argc > 5 && !strcmp(argv[5], "off")) {
					ast_clear_flag(&global_flags, RTMP_FLAG_DEBUG_AIN);
					ast_cli2(fd, "RTMP Incoming Audio Debugging Disabled\n");
				} else {
					ast_set_flag(&global_flags, RTMP_FLAG_DEBUG_AIN);
					ast_cli2(fd, "RTMP Incoming Audio Debugging Enabled\n");
				}
			} else if (!strcmp(argv[4], "out")) {
				if (argc > 5 && !strcmp(argv[5], "off")) {
					ast_clear_flag(&global_flags, RTMP_FLAG_DEBUG_AOUT);
					ast_cli2(fd, "RTMP Outgoing Audio Debugging Disabled\n");
				} else {
					ast_set_flag(&global_flags, RTMP_FLAG_DEBUG_AOUT);
					ast_cli2(fd, "RTMP Outgoing Audio Debugging Enabled\n");
				}
			} else if (!strcmp(argv[4], "off")) {
				ast_clear_flag(&global_flags, RTMP_FLAG_DEBUG_AIN|RTMP_FLAG_DEBUG_AOUT);
				ast_cli2(fd, "RTMP Audio Debugging Disabled\n");
			} else{
				return RESULT_SHOWUSAGE;
			}
			break;
		default:
			return RESULT_SHOWUSAGE;
	}
	return RESULT_SUCCESS;
}

static void *rtmp_accept_thread(void *ignore)
{
	int insock;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	for ( ; ; )
	{
		pthread_t th;
		struct rtmp_peer *peer;
		struct sockaddr_in sin_addr;
		socklen_t sin_addr_len = sizeof(sin_addr);

		insock = accept(rtmpsock, (struct sockaddr *)&sin_addr, &sin_addr_len);
		if (insock < 0) {
			ast_log(LOG_NOTICE, "Accept returned -1: %s\n", strerror(errno));
			continue;
		}

		peer = rtmp_peer_constructor();
		peer->sockfd = insock;
		memcpy(&peer->sin_addr, &sin_addr, sizeof(sin_addr));
		ast_pthread_create_stack(&th, &attr, &rtmp_peer_handle_session, peer, THREAD_STACKSIZE,
				__FILE__, __FUNCTION__, __LINE__, "rtmp_peer_handle_session"
			);
	}

	if (rtmp_debug)
		ast_verbose("killing accept thread\n");
	close(insock);
	pthread_attr_destroy(&attr);

	return ignore;
}

static int reload_config()
{
	struct ast_config *cfg;
	struct ast_variable *v;

	struct hostent *hp;
	struct ast_hostent ahp;
	struct sockaddr_in old_bindaddr = bindaddr;

	// reset default values
	ourport = RTMP_DEFAULT_PORT;
	memset(&bindaddr, 0, sizeof(bindaddr));
	ast_copy_string(default_context, RTMP_DEFULT_CONTEXT, sizeof(default_context));

	cfg = ast_config_load(configfile);
	if (!cfg) {
		ast_log(LOG_ERROR, "Could not load configuration file %s\n", configfile);
		return -1;
	}

	for (v = ast_variable_browse(cfg, "general"); v; v = v->next)
	{
		if (!strcasecmp(v->name, "context") && !ast_strlen_zero(v->value)) {
			ast_copy_string(default_context, v->value, sizeof(default_context));
		} else if (!strcasecmp(v->name, "bindaddr")) {
			if (!(hp = ast_gethostbyname(v->value, &ahp))) {
				ast_log(LOG_WARNING, "Invalid address: %s\n", v->value);
			} else {
				memcpy(&bindaddr.sin_addr, hp->h_addr, sizeof(bindaddr.sin_addr));
			}
		} else if (!strcasecmp(v->name, "bindport")) {
			if (sscanf(v->value, "%5d", &ourport) == 1) {
				bindaddr.sin_port = htons(ourport);
			} else {
				ast_log(LOG_WARNING, "Invalid port number '%s' at line %d of %s\n", v->value, v->lineno, configfile);
			}
		} else if (!strcasecmp(v->name, "allow")) {
			ast_parse_allow_disallow(&default_prefs, &global_capability, v->value, 1);
		} else if (!strcasecmp(v->name, "disallow")) {
			ast_parse_allow_disallow(&default_prefs, &global_capability, v->value, 0);
		} else if (!strcasecmp(v->name, "application")) {
			ast_copy_string(default_application, v->value, sizeof(default_application));
		}
	}

	if (!ntohs(bindaddr.sin_port))
		bindaddr.sin_port = ntohs(RTMP_DEFAULT_PORT);
	bindaddr.sin_family = AF_INET;

	ast_mutex_lock(&netlock);
	if ((rtmpsock > -1) && (memcmp(&old_bindaddr, &bindaddr, sizeof(struct sockaddr_in)))) {
		close(rtmpsock);
		rtmpsock = -1;
	}

	if (rtmpsock < 0)
	{
		rtmpsock = socket(AF_INET, SOCK_STREAM, 0);
		if ( rtmpsock < 0 ) {
			ast_log(LOG_WARNING, "Unable to create RTMP socket: %s\n", strerror(errno));
			ast_config_destroy(cfg);
			ast_mutex_unlock(&netlock);
			return -1;
		}

		const int reuse_flag = 1;
		setsockopt(rtmpsock, SOL_SOCKET, SO_REUSEADDR, &reuse_flag, sizeof reuse_flag);

		if (bind(rtmpsock, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) < 0) {
			ast_log(LOG_WARNING, "Failed to bind to %s:%d: %s\n",
					ast_inet_ntoa(bindaddr.sin_addr), ntohs(bindaddr.sin_port), strerror(errno));
			close(rtmpsock);
			rtmpsock = -1;
			ast_config_destroy(cfg);
			ast_mutex_unlock(&netlock);
		}

		if (listen(rtmpsock, SOMAXCONN) < 0) {
			ast_log(LOG_WARNING, "Failed to start listening to %s:%d: %s\n",
					ast_inet_ntoa(bindaddr.sin_addr), ntohs(bindaddr.sin_port), strerror(errno));
			close(rtmpsock);
			rtmpsock = -1;
			ast_config_destroy(cfg);
			ast_mutex_unlock(&netlock);
			return -1;
		}

		ast_verb(1, "RTMP Listening on %s:%d\n", ast_inet_ntoa(bindaddr.sin_addr), ntohs(bindaddr.sin_port));
		ast_pthread_create_background(&acceptthread, NULL, rtmp_accept_thread, NULL);
		ast_mutex_unlock(&netlock);
	}

	ast_config_destroy(cfg);
	return 0;
}


static void stop_accepting()
{
	ast_mutex_lock(&netlock);
	close(rtmpsock);
	rtmpsock = -1;
	if (acceptthread && (acceptthread != AST_PTHREADT_STOP)) {
		pthread_cancel(acceptthread);
		pthread_kill(acceptthread, SIGURG);
		pthread_join(acceptthread, NULL);
	}
	acceptthread = AST_PTHREADT_STOP;
	ast_mutex_unlock(&netlock);
}

static int rtmp_peer_hash(const void *obj, const int flags)
{
	const struct rtmp_peer *peer = obj;
	UNUSED(flags);
	return ast_str_case_hash(peer->name);
}

static int rtmp_peer_cmp(void *obj, void *arg, int flags)
{
	const struct rtmp_peer *peer = obj;
	const char *searchstr = arg;
	UNUSED(flags);
	return strcasecmp(peer->name, searchstr) ? 0 : CMP_MATCH | CMP_STOP;
}

static enum ast_module_load_result load_module()
{
	if (!(rtmp_peers = ao2_container_alloc(53, rtmp_peer_hash, rtmp_peer_cmp))) {
		return AST_MODULE_LOAD_DECLINE;
	}

#if defined(AST_MUTEX_INIT_W_CONSTRUCTORS)
	ast_mutex_init(&netlock);
#endif

	if (reload_config() == -1) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_channel_register(&rtmp_tech)) {
		ast_log(LOG_ERROR, "Unable to register RTMP channel driver\n");
		stop_accepting();
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_cli_register_multiple(cli_commands, ARRAY_LEN(cli_commands));

	RTMP_debuglevel = RTMP_LOGALL; // needed for RTMP_LogHex
	RTMP_LogSetCallback(&rtmp_log_callback);

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module()
{
	stop_accepting();
	ast_cli_unregister_multiple(cli_commands, ARRAY_LEN(cli_commands));
	ast_channel_unregister(&rtmp_tech);

	ao2_unref(rtmp_peers);
#if defined(AST_MUTEX_INIT_W_CONSTRUCTORS)
	ast_mutex_destroy(&netlock);
#endif
	return 0;
}

#if GCC_VERSION >= 46
 #pragma GCC diagnostic push
#endif
#pragma GCC diagnostic ignored "-Wold-style-declaration"
AST_MODULE_INFO (
		ASTERISK_GPL_KEY,
		AST_MODFLAG_DEFAULT,
		"Implements RTMP Protocol in order to be used by flash-based softphones",
		.load = load_module,
		.unload = unload_module,
	);
#if GCC_VERSION >= 46
 #pragma GCC diagnostic pop
#endif
