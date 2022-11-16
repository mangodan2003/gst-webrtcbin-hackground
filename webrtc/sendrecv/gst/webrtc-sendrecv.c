/*
 * Demo gstreamer app for negotiating and streaming a sendrecv webrtc stream
 * with a browser JS app.
 *
 * Build gstreamer using the following options:
 *   `cd /path/to/gstreamer`
 *   `meson setup --wipe builddir -Dgst-plugins-ugly:x264=enabled -Dgstreamer:tools=enabled -Dugly=enabled -Dgpl=enabled -Dbad=enabled -Dgst-plugins-bad:webrtc=enabled -Dgst-plugins-bad:srtp=enabled -Dgst-plugins-bad:dtls=enabled`
 *   `ninja -C builddir`
 *
 * Enter devenv
 *   `ninja -C builddir devenv`
 *
 * Build by running:
 *   `cd /pat/to/this/repo`
 *
 *   `cd webrtc/sendrecv/gst/webrtc-sendrecv`
 *   `make webrtc-sendrecv`.
 *   or with meson
 *   `meson _builddir`
     `ninja -C _builddir`
 *
 * Usage:
 * Start the signalling server :
 *   `webrtc/signalling`
 *   `python3 simple_server.py`
 *
 * Serve the webapp :
 *   `cd webrtc/sendrecv/js`
 *   `http-server -c`
 *
 * Visit 127.0.0.1:8080 in browser
 *
 * Start the server:
 *   `cd webrtc/sendrecv/gst/webrtc-sendrecv`
 *   `./webrtc-sendrecv`
 *   or if built using meson
 *   `./_builddir/webrtc/sendrecv/gst/webrtc-sendrecv`
 *
 * Toggle streams on and off using the Browser UI
 *
 * Author: Dan Squires <mangodan2003@gmail.com>, Nirbheek Chauhan <nirbheek@centricular.com>
 *
 */
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/rtp/rtp.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

/* For signalling */
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>

enum AppState
{
  APP_STATE_UNKNOWN = 0,
  APP_STATE_ERROR = 1,          /* generic error */
  SERVER_CONNECTING = 1000,
  SERVER_CONNECTION_ERROR,
  SERVER_CONNECTED,             /* Ready to register */
  SERVER_REGISTERING = 2000,
  SERVER_REGISTRATION_ERROR,
  SERVER_REGISTERED,            /* Ready to call a peer */
  SERVER_CLOSED,                /* server connection closed by us or the server */
  PEER_CONNECTING = 3000,
  PEER_CONNECTION_ERROR,
  PEER_CONNECTED,
  PEER_CALL_NEGOTIATING = 4000,
  PEER_CALL_STARTED,
  PEER_CALL_STOPPING,
  PEER_CALL_STOPPED,
  PEER_CALL_ERROR,
};

enum AppVideoSource
{
  VIDEO_SOURCE_INVALID,
  VIDEO_SOURCE_TEST_PATTERN,
  VIDEO_SOURCE_LOOPBACK
};

#define VIDEO_H264_CAPS "video/x-h264, profile=constrained-baseline"
#define INPUT_CAPS "video/x-raw, width=640, height=480, framerate=25/1"
#define RTP_VIDEO_H264_CAPS "application/x-rtp,media=video,encoding-name=H264,payload=96"
#define RTP_AUDIO_OPUS_CAPS "application/x-rtp,media=audio,encoding-name=OPUS,payload=97"

#define GST_CAT_DEFAULT webrtc_sendrecv_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

static GMainLoop *loop;
static GstElement *pipe1, *webrtc1 = NULL, *video_bin = NULL, *audio_bin = NULL;
static GstPad *video_sink = NULL, *audio_sink = NULL;

static SoupWebsocketConnection *ws_conn = NULL;
static enum AppState app_state = 0;
static gchar *peer_id = NULL; //"test";
static gchar *our_id = "gst-peer";
// Changed this so that it defaults to localhost as requires changes to the js
static const gchar *server_url = "wss://127.0.0.1:8443";
static gboolean disable_ssl = FALSE;
static gboolean making_offer = FALSE;

static unsigned int ping_count = 0;

static GOptionEntry entries[] = {
  {"server", 0, 0, G_OPTION_ARG_STRING, &server_url,
      "Signalling server to connect to", "URL"},
  {"disable-ssl", 0, 0, G_OPTION_ARG_NONE, &disable_ssl, "Disable ssl", NULL},
  {NULL},
};


static const char* video_source_to_string(enum AppVideoSource source) {
  switch(source) {
  case VIDEO_SOURCE_TEST_PATTERN:
    return "test pattern";
  case VIDEO_SOURCE_LOOPBACK:
    return "loopback";
  default:
    return "Invalid";
  }
}

const char* signaling_state_name(int state) {
  switch (state) {
    case GST_WEBRTC_SIGNALING_STATE_STABLE:
      return "stable";
    case GST_WEBRTC_SIGNALING_STATE_CLOSED:
      return "closed";
    case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_OFFER:
      return "have-local-offer";
    case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_OFFER:
      return "have-remote-offer";
    case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_PRANSWER:
      return "have-local-pranswer";
    case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_PRANSWER:
      return "have-remote-pranswer";
    default:
      return "bogus";
  }
}

static gboolean dump_graph() {
  gst_debug_bin_to_dot_file(GST_BIN(pipe1), GST_DEBUG_GRAPH_SHOW_VERBOSE, "pipeline");
  return G_SOURCE_REMOVE;
}

static gboolean
cleanup_and_quit_loop (const gchar * msg, enum AppState state)
{
  if (msg)
    gst_printerr ("%s\n", msg);
  if (state > 0)
    app_state = state;

  if (ws_conn) {
    if (soup_websocket_connection_get_state (ws_conn) ==
        SOUP_WEBSOCKET_STATE_OPEN)
      /* This will call us again */
      soup_websocket_connection_close (ws_conn, 1000, "");
    else
      g_clear_object (&ws_conn);
  }

  if (loop) {
    g_main_loop_quit (loop);
    g_clear_pointer (&loop, g_main_loop_unref);
  }

  /* To allow usage as a GSourceFunc */
  return G_SOURCE_REMOVE;
}

static gchar *
get_string_from_json_object (JsonObject * object)
{
  JsonNode *root;
  JsonGenerator *generator;
  gchar *text;

  /* Make it the root node */
  root = json_node_init_object (json_node_alloc (), object);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);

  /* Release everything */
  g_object_unref (generator);
  json_node_free (root);
  return text;
}

static void
handle_media_stream (GstPad * pad, GstElement * pipe, const char *convert_name,
    const char *sink_name)
{
  GstPad *qpad, *tee_pad1, *tee_pad2, *q1_pad, *q2_pad;
  GstElement *q1, *q2, *t, *conv, *resample, *sink1, *sink2;
  GstPadLinkReturn ret;

  gst_println ("Trying to handle stream with %s ! %s", convert_name, sink_name);

  q1 = gst_element_factory_make ("queue", NULL);
  g_assert_nonnull (q1);
  conv = gst_element_factory_make (convert_name, NULL);
  g_assert_nonnull (conv);
  sink1 = gst_element_factory_make (sink_name, NULL);
  g_assert_nonnull (sink1);

  if (g_strcmp0 (convert_name, "audioconvert") == 0) {
    /* Might also need to resample, so add it just in case.
     * Will be a no-op if it's not required. */
    resample = gst_element_factory_make ("audioresample", NULL);
    g_assert_nonnull (resample);
    gst_bin_add_many (GST_BIN (pipe), q1, conv, resample, sink1, NULL);
    gst_element_sync_state_with_parent (q1);
    gst_element_sync_state_with_parent (conv);
    gst_element_sync_state_with_parent (resample);
    gst_element_sync_state_with_parent (sink1);
    gst_element_link_many (q1, conv, resample, sink1, NULL);
    qpad = gst_element_get_static_pad (q1, "sink");
  } else {
    t = gst_element_factory_make ("tee", NULL);
    g_assert_nonnull (t);
    q2 = gst_element_factory_make ("queue", NULL);
    g_object_set(q2, "leaky", 1, NULL);
    g_assert_nonnull (q2);
    sink2 = gst_element_factory_make ("shmsink", NULL);
    g_assert_nonnull (sink2);
    g_object_set(sink2, "socket-path", "/tmp/gst-send-recv", "shm-size", 2000000, NULL);
    gst_bin_add_many (GST_BIN (pipe), t, q1, conv, sink1, q2, sink2, NULL);
    gst_element_sync_state_with_parent (t);
    gst_element_sync_state_with_parent (q1);
    gst_element_sync_state_with_parent (q2);
    gst_element_sync_state_with_parent (conv);
    gst_element_sync_state_with_parent (sink1);
    gst_element_sync_state_with_parent (sink2);

    gst_element_link_many (q1, conv, sink1, NULL);
    gst_element_link_many (q2, sink2, NULL);

    tee_pad1 = gst_element_request_pad_simple (t, "src_%u");
    g_print ("Obtained request pad %s for autovideosink branch.\n", gst_pad_get_name (tee_pad1));
    q1_pad = gst_element_get_static_pad(q1, "sink");
    tee_pad2 = gst_element_request_pad_simple (t, "src_%u");
    g_print ("Obtained request pad %s for shmsink branch.\n", gst_pad_get_name (tee_pad2));
    q2_pad = gst_element_get_static_pad(q2, "sink");

    ret = gst_pad_link (tee_pad1, q1_pad);
    g_assert_cmphex (ret, ==, GST_PAD_LINK_OK);
    ret = gst_pad_link (tee_pad2, q2_pad);
    g_assert_cmphex (ret, ==, GST_PAD_LINK_OK);



    qpad = gst_element_get_static_pad (t, "sink");
  }


  ret = gst_pad_link (pad, qpad);
  g_assert_cmphex (ret, ==, GST_PAD_LINK_OK);
}

static void
on_incoming_decodebin_stream (GstElement * decodebin, GstPad * pad,
    GstElement * pipe)
{
  GstCaps *caps;
  const gchar *name;

  if (!gst_pad_has_current_caps (pad)) {
    gst_printerr ("Pad '%s' has no caps, can't do anything, ignoring\n",
        GST_PAD_NAME (pad));
    return;
  }

  caps = gst_pad_get_current_caps (pad);
  name = gst_structure_get_name (gst_caps_get_structure (caps, 0));

  if (g_str_has_prefix (name, "video")) {
    handle_media_stream (pad, pipe, "videoconvert", "autovideosink");
  } else if (g_str_has_prefix (name, "audio")) {
    handle_media_stream (pad, pipe, "audioconvert", "autoaudiosink");
  } else {
    gst_printerr ("Unknown pad %s, ignoring", GST_PAD_NAME (pad));
  }
}

static void
on_incoming_stream (GstElement * webrtc, GstPad * pad, GstElement * pipe)
{
  gst_println ("on_incoming_stream() pad name: %s\n", GST_PAD_NAME (pad));

  GstElement *decodebin;
  GstPad *sinkpad;

  if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC)
    return;

  decodebin = gst_element_factory_make ("decodebin", NULL);
  g_signal_connect (decodebin, "pad-added",
      G_CALLBACK (on_incoming_decodebin_stream), pipe1);
  gst_bin_add (GST_BIN (pipe), decodebin);
  gst_element_sync_state_with_parent (decodebin);

  sinkpad = gst_element_get_static_pad (decodebin, "sink");
  gst_pad_link (pad, sinkpad);
  gst_object_unref (sinkpad);

  // Wait 2 seconds to allow rest of pipeline to be setup, then dump graph file
  g_timeout_add (2000, (GSourceFunc) dump_graph, NULL);

}



static void
on_stream_removed (GstElement * webrtc, GstPad * pad, GstElement * pipe)
{

  gchar* name = GST_PAD_NAME (pad);
  GstPadDirection direction = gst_pad_get_direction(pad);
  switch (direction) {
    case GST_PAD_SRC:
      // ToDo tear down stream handling part of pipeline when browser stops sending
      // however currently this is not getting called
      gst_println("WEBRTC PAD REMOVED %s (src)", name);
      break;
    case GST_PAD_SINK:
      gst_println("WEBRTC PAD REMOVED %s (sink)", name);
      break;
    case GST_PAD_UNKNOWN:
      // ?
      gst_println("WEBRTC PAD REMOVED %s (unknown direction)", name);
      break;
    default:
      gst_println("WEBRTC PAD REMOVED %s (undefined condition!)", name);
      break;
  }
}

static void
send_ice_candidate_message (GstElement * webrtc G_GNUC_UNUSED, guint mlineindex,
    gchar * candidate, gpointer user_data G_GNUC_UNUSED)
{
  gchar *text;
  JsonObject *ice, *msg;

  if (app_state < PEER_CALL_NEGOTIATING) {
    cleanup_and_quit_loop ("Can't send ICE, not in call", APP_STATE_ERROR);
    return;
  }

  ice = json_object_new ();
  json_object_set_string_member (ice, "candidate", candidate);
  json_object_set_int_member (ice, "sdpMLineIndex", mlineindex);
  msg = json_object_new ();
  json_object_set_object_member (msg, "ice", ice);
  text = get_string_from_json_object (msg);
  json_object_unref (msg);

  soup_websocket_connection_send_text (ws_conn, text);
  g_free (text);
}

static void
send_sdp_to_peer (GstWebRTCSessionDescription * desc)
{
  gchar *text;
  JsonObject *msg, *sdp;

  if (app_state < PEER_CALL_NEGOTIATING) {
    cleanup_and_quit_loop ("Can't send SDP to peer, not in call",
        APP_STATE_ERROR);
    return;
  }

  text = gst_sdp_message_as_text (desc->sdp);
  sdp = json_object_new ();

  if (desc->type == GST_WEBRTC_SDP_TYPE_OFFER) {
    gst_print ("Sending offer:\n%s\n", text);
    json_object_set_string_member (sdp, "type", "offer");
  } else if (desc->type == GST_WEBRTC_SDP_TYPE_ANSWER) {
    gst_print ("Sending answer:\n%s\n", text);
    json_object_set_string_member (sdp, "type", "answer");
  } else {
    g_assert_not_reached ();
  }

  json_object_set_string_member (sdp, "sdp", text);
  g_free (text);

  msg = json_object_new ();
  json_object_set_object_member (msg, "sdp", sdp);
  text = get_string_from_json_object (msg);
  json_object_unref (msg);

  soup_websocket_connection_send_text (ws_conn, text);
  g_free (text);
}

/* Offer created by our pipeline, to be sent to the peer */
static void
on_offer_created (GstPromise * promise, gpointer user_data)
{
  GstWebRTCSessionDescription *offer = NULL;
  const GstStructure *reply;

  g_assert_cmphex (app_state, ==, PEER_CALL_NEGOTIATING);

  g_assert_cmphex (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "offer",
      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
  gst_promise_unref (promise);

  GstWebRTCSignalingState signalling_state;
  g_object_get(webrtc1, "signaling-state", &signalling_state, NULL);
  if(signalling_state != GST_WEBRTC_SIGNALING_STATE_STABLE) {
    making_offer = FALSE;
    return;
  }


  promise = gst_promise_new ();
  g_signal_emit_by_name (webrtc1, "set-local-description", offer, promise);
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

  /* Send offer to peer */
  send_sdp_to_peer (offer);
  gst_webrtc_session_description_free (offer);
  making_offer = FALSE;
}

static void create_offer() {
  app_state = PEER_CALL_NEGOTIATING;

  making_offer = TRUE;

  GstPromise *promise = gst_promise_new_with_change_func (on_offer_created, NULL, NULL);
  g_signal_emit_by_name (webrtc1, "create-offer", NULL, promise);
}

static void
on_negotiation_needed (GstElement * element, gpointer user_data)
{
  gst_print ("on_negotiation_needed()\n");
  //soup_websocket_connection_send_text (ws_conn, "OFFER_REQUEST");
  create_offer();
}

static void
data_channel_on_error (GObject * dc, gpointer user_data)
{
  cleanup_and_quit_loop ("Data channel error", 0);
}


static gboolean data_channel_send_hello(GObject * dc) {
  GBytes *bytes = g_bytes_new ("data", strlen ("data"));
  gchar* ping = g_strdup_printf ("PING %i", ping_count++);
  gst_print ("Sending ping to browser\n");
  g_signal_emit_by_name (dc, "send-string", ping);
  g_signal_emit_by_name (dc, "send-data", bytes);
  g_bytes_unref (bytes);
  g_free(ping);
  return G_SOURCE_CONTINUE;
}


/*
 * Changed this so that is regularly sends a message such that it is obvious when
 * pipeline has stalled.
 *
 * QUESTION: Why is this called twice?
 */
static void
data_channel_on_open (GObject * dc, gpointer user_data)
{
  gst_print ("data channel opened\n");
  ping_count = 0;
  static int done = 0;
  if(done == 0) {
    g_timeout_add (2000, (GSourceFunc) data_channel_send_hello, dc);
    done = 1;
  }

  g_idle_add((GSourceFunc)dump_graph, NULL);
}

static void
data_channel_on_close (GObject * dc, gpointer user_data)
{
  cleanup_and_quit_loop ("Data channel closed", 0);
}

void add_ghost_src(GstElement* bin, GstElement* el) {
  GstPad* src = gst_element_get_static_pad(el, "src");
  GstPad* ghostSrc = gst_ghost_pad_new("src", src);
  gst_element_add_pad(bin, ghostSrc);
  gst_object_unref(src);
}

static GstPad* send_media_to_browser(GstElement* bin) {
  gst_element_set_locked_state(bin, TRUE);
  gst_bin_add(GST_BIN(pipe1), bin);

  // explicitly link pads to get reference to sink
  GstPad* src = gst_element_get_static_pad(bin, "src");
  GstPad* sink = gst_element_request_pad_simple(webrtc1, "sink_%u");

  gchar *sink_name;
  g_object_get(sink, "name", &sink_name, NULL);
  gst_print ("send_media_to_browser() new sink named: %s\n", sink_name);
  g_free(sink_name);

  gst_pad_link(src, sink);

  gst_object_unref(src);

  gst_element_set_locked_state(bin, FALSE);
  gst_element_sync_state_with_parent(bin);

  return sink;
}

static void stop_media_to_browser(GstElement* element, GstPad *sink) {
  GstPad *src;
  GstWebRTCRTPTransceiver* transceiver;

  src = gst_element_get_static_pad(element, "src");

  gst_element_send_event(element, gst_event_new_eos());

  g_object_get(sink, "transceiver", &transceiver, NULL);

  GstWebRTCRTPTransceiverDirection dir;
  g_object_get(transceiver, "direction", &dir, NULL);
  if(dir == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV) {
    gst_print ("stop_media_to_browser() Setting transceiver direction to recvonly\n");
    g_object_set(transceiver, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, NULL);
  } else {
    gst_print ("stop_media_to_browser() Setting transceiver direction to inactive\n");
    g_object_set(transceiver, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE, NULL);
  }

  gst_element_set_locked_state(element, TRUE);
  gst_element_set_state(element, GST_STATE_NULL);
  gst_pad_unlink(src, sink);
  gst_element_release_request_pad(webrtc1, sink);

  gst_object_unref(transceiver);
  gst_object_unref(sink);
  gst_object_unref(src);

  gst_bin_remove(GST_BIN(pipe1), element);
}


static gboolean send_video_to_browser(enum AppVideoSource source) {
  gst_print ("send_video_to_browser() source: %s\n", video_source_to_string(source));

  GstElement *videosrc = NULL, *shmsrc=NULL;

  if(source == VIDEO_SOURCE_TEST_PATTERN) {
    videosrc = gst_element_factory_make("videotestsrc", NULL);
    g_object_set(videosrc, "pattern", 18, NULL);
    g_object_set(videosrc, "is-live", 1, NULL);
  }

  if(source == VIDEO_SOURCE_LOOPBACK) {
    shmsrc = gst_element_factory_make("shmsrc", NULL);
    g_object_set(shmsrc, "socket-path", "/tmp/gst-send-recv", "do-timestamp", 1, NULL);
    videosrc = gst_element_factory_make("videoparse", NULL);
    g_object_set(videosrc, "width", 640, "height", 480, "format", 2, NULL);
  }


  GstElement* videorate = gst_element_factory_make("videorate", NULL);
  GstElement* videoscale = gst_element_factory_make("videoscale", NULL);
  GstElement* videoconvert = gst_element_factory_make("videoconvert", NULL);

  GstElement* queue1 = gst_element_factory_make("queue", NULL);
  g_object_set(queue1, "max-size-buffers", 1, NULL);

  GstElement* x264enc = gst_element_factory_make("x264enc", NULL);
  g_object_set(x264enc, "bitrate", 800, NULL);
  g_object_set(x264enc, "speed-preset", 1 /* ultrafast */, NULL);
  g_object_set(x264enc, "tune", 4 /* zerolatency */, NULL);

  // chrome seems happy with threads=1 or 2, but not 3+ (freeze on first keyframe)
  // doesn't seem to affect behaviour, so just 1 thread for safety
  g_object_set(x264enc, "threads", 1, NULL);

  GstElement* queue2 = gst_element_factory_make("queue", NULL);

  GstElement* h264parse = gst_element_factory_make("h264parse", NULL);
  GstElement* rtph264pay = gst_element_factory_make("rtph264pay", NULL);
  g_object_set(rtph264pay, "config-interval", -1, NULL);
  g_object_set(rtph264pay, "aggregate-mode", 1 /* "zero-latency" */, NULL);
  guint mtu;

  g_object_set(rtph264pay, "mtu", 1300, NULL);
  g_object_get(rtph264pay, "mtu", &mtu, NULL);


  GstElement* queue3 = gst_element_factory_make("queue", NULL);

  GstCaps* inputCaps = gst_caps_from_string(INPUT_CAPS);
  GstCaps* encodeCaps = gst_caps_from_string(VIDEO_H264_CAPS);

  GstElement* bin = gst_bin_new("video-to-browser");

  gst_bin_add_many(GST_BIN(bin), videosrc, videorate, videoscale, videoconvert, queue1, x264enc, queue2, h264parse,
                   rtph264pay, queue3, NULL);

  if(shmsrc) {
    gst_bin_add(GST_BIN(bin), shmsrc);
    gst_element_link(shmsrc, videosrc);
  }

  gst_element_link_many(videosrc, videorate, videoscale, NULL);
  gst_element_link_filtered(videoscale, videoconvert, inputCaps);
  gst_element_link_many(videoconvert, queue1, x264enc, NULL);
  gst_element_link_filtered(x264enc, queue2, encodeCaps);
  gst_element_link_many(queue2, h264parse, rtph264pay, NULL);

  GstCaps* caps = gst_caps_from_string(RTP_VIDEO_H264_CAPS);
  gst_element_link_filtered(rtph264pay, queue3, caps);

  // expose queue3 src pad as the bin src
  add_ghost_src(bin, queue3);

  video_sink = send_media_to_browser(bin);

  video_bin = bin;
  return G_SOURCE_REMOVE;
}


static gboolean stop_video_to_browser() {
  gst_print ("stop_video_to_browser()\n");
  stop_media_to_browser(video_bin, video_sink);
  video_sink = NULL;
  video_bin = NULL;
  return G_SOURCE_REMOVE;
}

static gboolean send_audio_to_browser() {
  gst_print ("send_audio_to_browser()\n");

  GstElement* testaudiosrc = gst_element_factory_make("audiotestsrc", NULL);
  g_object_set(testaudiosrc, "wave", 10, NULL); // Red noise

  GstElement* opusenc = gst_element_factory_make("opusenc", NULL);
  GstElement* rtpopuspay = gst_element_factory_make("rtpopuspay", NULL);
  GstElement* queue = gst_element_factory_make("queue", NULL);

  GstElement* bin = gst_bin_new("audio-to-browser");

  gst_bin_add_many(GST_BIN(bin), testaudiosrc, opusenc, rtpopuspay, queue, NULL);
  gst_element_link_many(testaudiosrc, opusenc, rtpopuspay, NULL);

  GstCaps* caps = gst_caps_from_string(RTP_AUDIO_OPUS_CAPS);
  gst_element_link_filtered(rtpopuspay, queue, caps);

  // expose queue3 src pad as the bin src
  add_ghost_src(bin, queue);

  audio_sink = send_media_to_browser(bin);
  audio_bin = bin;
  return G_SOURCE_REMOVE;
}

static gboolean stop_audio_to_browser() {
  gst_print ("stop_audio_to_browser()\n");
  stop_media_to_browser(audio_bin, audio_sink);
  audio_bin = NULL;
  audio_sink = NULL;
  return G_SOURCE_REMOVE;
}

static void
data_channel_on_message_string (GObject * dc, gchar * str, gpointer user_data)
{
  gst_print ("Received data channel message: %s\n", str);

  if(g_strcmp0(str, "RECV VIDEO START TESTPATTERN") == 0) {
    // Just calling send_video_to_browser directly from this context doesn't work
    // so schedule an event to do it for us.
    g_idle_add((GSourceFunc) send_video_to_browser, (void*)VIDEO_SOURCE_TEST_PATTERN);
  }
  if(g_strcmp0(str, "RECV VIDEO START LOOPBACK") == 0) {
    // Just calling send_video_to_browser directly from this context doesn't work
    // so schedule an event to do it for us.
    g_idle_add((GSourceFunc) send_video_to_browser, (void*)VIDEO_SOURCE_LOOPBACK);
  }
  if(g_strcmp0(str, "RECV VIDEO STOP") == 0) {
    g_idle_add ((GSourceFunc) stop_video_to_browser, NULL);
  }
  if(g_strcmp0(str, "RECV AUDIO START") == 0) {
    g_idle_add((GSourceFunc) send_audio_to_browser, NULL);
  }
  if(g_strcmp0(str, "RECV AUDIO STOP") == 0) {
    g_idle_add ((GSourceFunc) stop_audio_to_browser, NULL);
  }
}



static void
connect_data_channel_signals (GObject * data_channel)
{
  g_signal_connect (data_channel, "on-error",
      G_CALLBACK (data_channel_on_error), NULL);
  g_signal_connect (data_channel, "on-open", G_CALLBACK (data_channel_on_open),
      NULL);
  g_signal_connect (data_channel, "on-close",
      G_CALLBACK (data_channel_on_close), NULL);
  g_signal_connect (data_channel, "on-message-string",
      G_CALLBACK (data_channel_on_message_string), NULL);
}

static void
on_data_channel (GstElement * webrtc, GObject * data_channel,
    gpointer user_data)
{
  gst_print ("on_data_channel\n");
  connect_data_channel_signals (data_channel);
}

static void
on_ice_gathering_state_notify (GstElement * webrtcbin, GParamSpec * pspec,
    gpointer user_data)
{
  GstWebRTCICEGatheringState ice_gather_state;
  const gchar *new_state = "unknown";

  g_object_get (webrtcbin, "ice-gathering-state", &ice_gather_state, NULL);
  switch (ice_gather_state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
      new_state = "new";
      break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
      new_state = "gathering";
      break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
      new_state = "complete";
      break;
  }
  gst_print ("ICE gathering state changed to %s\n", new_state);
}

static gboolean webrtcbin_get_stats (GstElement * webrtcbin);

static gboolean
on_webrtcbin_stat (GQuark field_id, const GValue * value, gpointer unused)
{
  if (GST_VALUE_HOLDS_STRUCTURE (value)) {
    GST_DEBUG ("stat: \'%s\': %" GST_PTR_FORMAT, g_quark_to_string (field_id),
        gst_value_get_structure (value));
  } else {
    GST_FIXME ("unknown field \'%s\' value type: \'%s\'",
        g_quark_to_string (field_id), g_type_name (G_VALUE_TYPE (value)));
  }

  return TRUE;
}

static void
on_webrtcbin_get_stats (GstPromise * promise, GstElement * webrtcbin)
{
  const GstStructure *stats;

  g_return_if_fail (gst_promise_wait (promise) == GST_PROMISE_RESULT_REPLIED);

  stats = gst_promise_get_reply (promise);
  gst_structure_foreach (stats, on_webrtcbin_stat, NULL);

  g_timeout_add (100, (GSourceFunc) webrtcbin_get_stats, webrtcbin);
}

static gboolean
webrtcbin_get_stats (GstElement * webrtcbin)
{
  GstPromise *promise;

  promise =
      gst_promise_new_with_change_func (
      (GstPromiseChangeFunc) on_webrtcbin_get_stats, webrtcbin, NULL);

  GST_TRACE ("emitting get-stats on %" GST_PTR_FORMAT, webrtcbin);
  g_signal_emit_by_name (webrtcbin, "get-stats", NULL, promise);
  gst_promise_unref (promise);

  return G_SOURCE_REMOVE;
}


static void on_local_description_set(GstPromise * promise, gpointer user_data) {

  GstWebRTCSessionDescription *answer = user_data;

  /* Send answer to peer */
  send_sdp_to_peer (answer);
  gst_webrtc_session_description_free (answer);
}

/* Answer created by our pipeline, to be sent to the peer */
static void
on_answer_created (GstPromise * promise, gpointer user_data)
{
  GstWebRTCSessionDescription *answer = NULL;
  const GstStructure *reply;

  g_assert_cmphex (app_state, ==, PEER_CALL_NEGOTIATING);

  g_assert_cmphex (gst_promise_wait (promise), ==, GST_PROMISE_RESULT_REPLIED);
  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
  gst_promise_unref (promise);

  promise = gst_promise_new_with_change_func((GstPromiseChangeFunc)on_local_description_set, answer, NULL);
  g_signal_emit_by_name (webrtc1, "set-local-description", answer, promise);
  gst_promise_interrupt (promise);
  gst_promise_unref (promise);

}


static void create_answer() {
  GstPromise *promise = gst_promise_new_with_change_func (on_answer_created, NULL, NULL);
  g_signal_emit_by_name (webrtc1, "create-answer", NULL, promise);
}

static void
on_signaling_state_changed(GstElement* object, GParamSpec* pspec, gpointer user_data) {
  int state;
  g_object_get(webrtc1, "signaling-state", &state, NULL);
  gst_println("on_signaling_state_changed() SIGNALLING STATE CHANGED to %s", signaling_state_name(state));
  switch (state) {
    case GST_WEBRTC_SIGNALING_STATE_STABLE:

      break;

    case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_OFFER:
      //create_answer();
      break;

    case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_OFFER:
    case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_PRANSWER:
    case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_PRANSWER:
    case GST_WEBRTC_SIGNALING_STATE_CLOSED:
      break;
  }
}


#define STUN_SERVER "stun://stun.l.google.com:19302 "
#define RTP_TWCC_URI "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
#define RTP_PAYLOAD_TYPE "96"

static gboolean
start_pipeline ()
{
//  char *pipeline;
  GstStateChangeReturn ret;

  //  replaced gst_parse_launch with manual pipeline and webrtcbin
  // as don't want audio and video by default,

  pipe1 = gst_pipeline_new("pipeline");
  g_assert_nonnull (pipe1);
  webrtc1 = gst_element_factory_make("webrtcbin", NULL);
  g_assert_nonnull (webrtc1);

  //g_object_set(webrtc1, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_COMPAT, NULL);
  g_object_set(webrtc1, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);
  g_object_set(webrtc1, "stun-server", STUN_SERVER, NULL, "name", "sendrecv", NULL);

  gst_bin_add(GST_BIN(pipe1), webrtc1);


  /* This is the gstwebrtc entry point where we create the offer and so on. It
   * will be called when the pipeline goes to PLAYING. */
  g_signal_connect (webrtc1, "on-negotiation-needed",
      G_CALLBACK (on_negotiation_needed), NULL);
  /* We need to transmit this ICE candidate to the browser via the websockets
   * signalling server. Incoming ice candidates from the browser need to be
   * added by us too, see on_server_message() */
  g_signal_connect (webrtc1, "on-ice-candidate",
      G_CALLBACK (send_ice_candidate_message), NULL);
  g_signal_connect (webrtc1, "notify::ice-gathering-state",
      G_CALLBACK (on_ice_gathering_state_notify), NULL);

  gst_element_set_state (pipe1, GST_STATE_READY);

  g_signal_connect (webrtc1, "on-data-channel", G_CALLBACK (on_data_channel), NULL);
  /* Incoming streams will be exposed via this signal */
  g_signal_connect (webrtc1, "pad-added", G_CALLBACK (on_incoming_stream), pipe1);
  /* Removed streams via this one */
  g_signal_connect(webrtc1, "pad-removed", G_CALLBACK(on_stream_removed), pipe1);


//  GstCaps *video_caps =
//      gst_caps_from_string
//      ("application/x-rtp,media=video,encoding-name=H264,payload="
//      RTP_PAYLOAD_TYPE
//      ",clock-rate=90000,packetization-mode=(string)1, profile-level-id=(string)42c016");
//  GstWebRTCRTPTransceiver *trans;
//  g_signal_emit_by_name (webrtc1, "add-transceiver",
//      GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, video_caps, &trans);
//  gst_caps_unref (video_caps);
//  gst_object_unref (trans);


  g_signal_connect(webrtc1, "notify::signaling-state", G_CALLBACK(on_signaling_state_changed), NULL);


  /* Lifetime is the same as the pipeline itself */
  gst_object_unref (webrtc1);

  g_timeout_add (100, (GSourceFunc) webrtcbin_get_stats, webrtc1);

  gst_print ("Starting pipeline\n");
  ret = gst_element_set_state (GST_ELEMENT (pipe1), GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto err;

  return TRUE;

err:
  if (pipe1)
    g_clear_object (&pipe1);
  if (webrtc1)
    webrtc1 = NULL;
  return FALSE;
}

static gboolean
setup_call (void)
{
  gchar *msg;

  if (soup_websocket_connection_get_state (ws_conn) !=
      SOUP_WEBSOCKET_STATE_OPEN)
    return FALSE;

  if (!peer_id)
    return FALSE;

  gst_print ("Setting up signalling server call with %s\n", peer_id);
  app_state = PEER_CONNECTING;
  msg = g_strdup_printf ("SESSION %s", peer_id);
  soup_websocket_connection_send_text (ws_conn, msg);
  g_free (msg);
  return TRUE;
}

static gboolean
register_with_server (void)
{
  gchar *hello;

  if (soup_websocket_connection_get_state (ws_conn) !=
      SOUP_WEBSOCKET_STATE_OPEN)
    return FALSE;

  if (!our_id) {
    gint32 id;

    id = g_random_int_range (10, 10000);
    gst_print ("Registering id %i with server\n", id);

    hello = g_strdup_printf ("HELLO %i", id);
  } else {
    gst_print ("Registering id %s with server\n", our_id);

    hello = g_strdup_printf ("HELLO %s", our_id);
  }

  app_state = SERVER_REGISTERING;

  /* Register with the server with a random integer id. Reply will be received
   * by on_server_message() */
  soup_websocket_connection_send_text (ws_conn, hello);
  g_free (hello);

  return TRUE;
}

static void
on_server_closed (SoupWebsocketConnection * conn G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
  app_state = SERVER_CLOSED;
  cleanup_and_quit_loop ("Server connection closed", 0);
}





static void
on_offer_set (GstPromise * promise, gpointer user_data)
{
  gst_promise_unref (promise);
  create_answer();
}

static void
on_offer_received (GstSDPMessage * sdp)
{
  GstWebRTCSessionDescription *offer = NULL;
  GstPromise *promise;

  GstWebRTCSignalingState signalling_state;
  g_object_get(webrtc1, "signaling-state", &signalling_state, NULL);
  if(signalling_state != GST_WEBRTC_SIGNALING_STATE_STABLE) {
    gst_println ("on_offer_received() not in stable state. state: %d", signalling_state);
    return;
  }

  offer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_OFFER, sdp);
  g_assert_nonnull (offer);

  /* Set remote description on our pipeline */
  {
    promise = gst_promise_new_with_change_func (on_offer_set, NULL, NULL);
    g_signal_emit_by_name (webrtc1, "set-remote-description", offer, promise);
    //g_signal_emit_by_name (webrtc1, "set-remote-description", offer, NULL);
  }
  gst_webrtc_session_description_free (offer);
}




/* One mega message handler for our asynchronous calling mechanism */
static void
on_server_message (SoupWebsocketConnection * conn, SoupWebsocketDataType type,
    GBytes * message, gpointer user_data)
{
  gchar *text;

  switch (type) {
    case SOUP_WEBSOCKET_DATA_BINARY:
      gst_printerr ("Received unknown binary message, ignoring\n");
      return;
    case SOUP_WEBSOCKET_DATA_TEXT:{
      gsize size;
      const gchar *data = g_bytes_get_data (message, &size);
      /* Convert to NULL-terminated string */
      text = g_strndup (data, size);
      break;
    }
    default:
      g_assert_not_reached ();
  }

  if (g_strcmp0 (text, "HELLO") == 0) {
    /* Server has accepted our registration, we are ready to send commands */
    if (app_state != SERVER_REGISTERING) {
      cleanup_and_quit_loop ("ERROR: Received HELLO when not registering",
          APP_STATE_ERROR);
      goto out;
    }
    app_state = SERVER_REGISTERED;
    gst_print ("Registered with server\n");
    if (!our_id) {
      /* Ask signalling server to connect us with a specific peer */
      if (!setup_call ()) {
        cleanup_and_quit_loop ("ERROR: Failed to setup call", PEER_CALL_ERROR);
        goto out;
      }
    } else {
      gst_println ("Waiting for connection from peer (our-id: %s)", our_id);
    }
  } else if (g_strcmp0 (text, "SESSION_OK") == 0) {
    /* The call initiated by us has been setup by the server; now we can start
     * negotiation */
    if (app_state != PEER_CONNECTING) {
      cleanup_and_quit_loop ("ERROR: Received SESSION_OK when not calling",
          PEER_CONNECTION_ERROR);
      goto out;
    }

    app_state = PEER_CONNECTED;
    /* Start negotiation (exchange SDP and ICE candidates) */
    if (!start_pipeline ())
      cleanup_and_quit_loop ("ERROR: failed to start pipeline",
          PEER_CALL_ERROR);
  } else if (g_strcmp0 (text, "OFFER_REQUEST") == 0) {
   // if (app_state != SERVER_REGISTERED) {
   //   gst_printerr ("Received OFFER_REQUEST at a strange time, ignoring\n");
   //  goto out;
   // }
    gst_print ("Received OFFER_REQUEST, sending offer\n");
    /* Peer wants us to start negotiation (exchange SDP and ICE candidates) */
    if (!start_pipeline ())
      cleanup_and_quit_loop ("ERROR: failed to start pipeline",
          PEER_CALL_ERROR);



  } else if (g_str_has_prefix (text, "ERROR")) {
    /* Handle errors */
    switch (app_state) {
      case SERVER_CONNECTING:
        app_state = SERVER_CONNECTION_ERROR;
        break;
      case SERVER_REGISTERING:
        app_state = SERVER_REGISTRATION_ERROR;
        break;
      case PEER_CONNECTING:
        app_state = PEER_CONNECTION_ERROR;
        break;
      case PEER_CONNECTED:
      case PEER_CALL_NEGOTIATING:
        app_state = PEER_CALL_ERROR;
        break;
      default:
        app_state = APP_STATE_ERROR;
    }
    cleanup_and_quit_loop (text, 0);
  } else {
    /* Look for JSON messages containing SDP and ICE candidates */
    JsonNode *root;
    JsonObject *object, *child;
    JsonParser *parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, text, -1, NULL)) {
      gst_printerr ("Unknown message '%s', ignoring\n", text);
      g_object_unref (parser);
      goto out;
    }

    root = json_parser_get_root (parser);
    if (!JSON_NODE_HOLDS_OBJECT (root)) {
      gst_printerr ("Unknown json message '%s', ignoring\n", text);
      g_object_unref (parser);
      goto out;
    }

    object = json_node_get_object (root);
    /* Check type of JSON message */
    if (json_object_has_member (object, "sdp")) {
      int ret;
      GstSDPMessage *sdp;
      const gchar *text, *sdptype;
      GstWebRTCSessionDescription *answer;

      app_state = PEER_CALL_NEGOTIATING;

      child = json_object_get_object_member (object, "sdp");

      if (!json_object_has_member (child, "type")) {
        cleanup_and_quit_loop ("ERROR: received SDP without 'type'",
            PEER_CALL_ERROR);
        goto out;
      }

      sdptype = json_object_get_string_member (child, "type");
      /* In this example, we create the offer and receive one answer by default,
       * but it's possible to comment out the offer creation and wait for an offer
       * instead, so we handle either here.
       *
       * See tests/examples/webrtcbidirectional.c in gst-plugins-bad for another
       * example how to handle offers from peers and reply with answers using webrtcbin. */
      text = json_object_get_string_member (child, "sdp");
      ret = gst_sdp_message_new (&sdp);
      g_assert_cmphex (ret, ==, GST_SDP_OK);
      ret = gst_sdp_message_parse_buffer ((guint8 *) text, strlen (text), sdp);
      g_assert_cmphex (ret, ==, GST_SDP_OK);

      if (g_str_equal (sdptype, "answer")) {
        gst_print ("Received answer:\n%s\n", text);
        answer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER,
            sdp);
        g_assert_nonnull (answer);

        /* Set remote description on our pipeline */
        {
          GstPromise *promise = gst_promise_new ();
          g_signal_emit_by_name (webrtc1, "set-remote-description", answer,
              promise);
          gst_promise_interrupt (promise);
          gst_promise_unref (promise);
        }
        app_state = PEER_CALL_STARTED;
      } else {
        gst_print ("Received offer:\n%s\n", text);
        on_offer_received (sdp);
      }

    } else if (json_object_has_member (object, "ice")) {
      const gchar *candidate;
      gint sdpmlineindex;

      child = json_object_get_object_member (object, "ice");
      candidate = json_object_get_string_member (child, "candidate");
      sdpmlineindex = json_object_get_int_member (child, "sdpMLineIndex");

      /* Add ice candidate sent by remote peer */
      g_signal_emit_by_name (webrtc1, "add-ice-candidate", sdpmlineindex,
          candidate);
    } else {
      gst_printerr ("Ignoring unknown JSON message:\n%s\n", text);
    }
    g_object_unref (parser);
  }

out:
  g_free (text);
}

static void
on_server_connected (SoupSession * session, GAsyncResult * res,
    SoupMessage * msg)
{
  GError *error = NULL;

  ws_conn = soup_session_websocket_connect_finish (session, res, &error);
  if (error) {
    cleanup_and_quit_loop (error->message, SERVER_CONNECTION_ERROR);
    g_error_free (error);
    return;
  }

  g_assert_nonnull (ws_conn);

  app_state = SERVER_CONNECTED;
  gst_print ("Connected to signalling server\n");

  g_signal_connect (ws_conn, "closed", G_CALLBACK (on_server_closed), NULL);
  g_signal_connect (ws_conn, "message", G_CALLBACK (on_server_message), NULL);

  /* Register with the server so it knows about us and can accept commands */
  register_with_server ();
}

/*
 * Connect to the signalling server. This is the entrypoint for everything else.
 */
static void
connect_to_websocket_server_async (void)
{
  SoupLogger *logger;
  SoupMessage *message;
  SoupSession *session;
  const char *https_aliases[] = { "wss", NULL };

  session =
      soup_session_new_with_options (SOUP_SESSION_SSL_STRICT, !disable_ssl,
      SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
      //SOUP_SESSION_SSL_CA_FILE, "/etc/ssl/certs/ca-bundle.crt",
      SOUP_SESSION_HTTPS_ALIASES, https_aliases, NULL);

  logger = soup_logger_new (SOUP_LOGGER_LOG_BODY, -1);
  soup_session_add_feature (session, SOUP_SESSION_FEATURE (logger));
  g_object_unref (logger);

  message = soup_message_new (SOUP_METHOD_GET, server_url);

  gst_print ("Connecting to server...\n");

  /* Once connected, we will register */
  soup_session_websocket_connect_async (session, message, NULL, NULL, NULL,
      (GAsyncReadyCallback) on_server_connected, message);
  app_state = SERVER_CONNECTING;
}

static gboolean
check_plugins (void)
{
  int i;
  gboolean ret;
  GstPlugin *plugin;
  GstRegistry *registry;
  const gchar *needed[] = { "opus", "vpx", "nice", "webrtc", "dtls", "srtp",
    "rtpmanager", "videotestsrc", "audiotestsrc", NULL
  };

  registry = gst_registry_get ();
  ret = TRUE;
  for (i = 0; i < g_strv_length ((gchar **) needed); i++) {
    plugin = gst_registry_find_plugin (registry, needed[i]);
    if (!plugin) {
      gst_print ("Required gstreamer plugin '%s' not found\n", needed[i]);
      ret = FALSE;
      continue;
    }
    gst_object_unref (plugin);
  }
  return ret;
}

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;
  int ret_code = -1;

  context = g_option_context_new ("- gstreamer webrtc sendrecv demo");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    gst_printerr ("Error initializing: %s\n", error->message);
    return -1;
  }

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "webrtc-sendrecv", 0,
      "WebRTC Sending and Receiving example");

  if (!check_plugins ()) {
    goto out;
  }

  ret_code = 0;

  /* Disable ssl when running a localhost server, because
   * it's probably a test server with a self-signed certificate */
  {
    GstUri *uri = gst_uri_from_string (server_url);
    if (g_strcmp0 ("localhost", gst_uri_get_host (uri)) == 0 ||
        g_strcmp0 ("127.0.0.1", gst_uri_get_host (uri)) == 0)
      disable_ssl = TRUE;
    gst_uri_unref (uri);
  }

  loop = g_main_loop_new (NULL, FALSE);

  connect_to_websocket_server_async ();

  g_main_loop_run (loop);

  if (loop)
    g_main_loop_unref (loop);

  if (pipe1) {
    gst_element_set_state (GST_ELEMENT (pipe1), GST_STATE_NULL);
    gst_print ("Pipeline stopped\n");
    gst_object_unref (pipe1);
  }

out:
  g_free (our_id);

  return ret_code;
}
