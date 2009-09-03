/* GStreamer
 * Copyright (C) <2006> Philippe Khalaf <philippe.kalaf@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:gstbasertpaudiopayload
 * @short_description: Base class for audio RTP payloader
 *
 * <refsect2>
 * <para>
 * Provides a base class for audio RTP payloaders for frame or sample based
 * audio codecs (constant bitrate)
 * </para>
 * <para>
 * This class derives from GstBaseRTPPayload. It can be used for payloading
 * audio codecs. It will only work with constant bitrate codecs. It supports
 * both frame based and sample based codecs. It takes care of packing up the
 * audio data into RTP packets and filling up the headers accordingly. The
 * payloading is done based on the maximum MTU (mtu) and the maximum time per
 * packet (max-ptime). The general idea is to divide large data buffers into
 * smaller RTP packets. The RTP packet size is the minimum of either the MTU,
 * max-ptime (if set) or available data. The RTP packet size is always larger or
 * equal to min-ptime (if set). If min-ptime is not set, any residual data is
 * sent in a last RTP packet. In the case of frame based codecs, the resulting
 * RTP packets always contain full frames.
 * </para>
 * <title>Usage</title>
 * <para>
 * To use this base class, your child element needs to call either
 * gst_base_rtp_audio_payload_set_frame_based() or
 * gst_base_rtp_audio_payload_set_sample_based(). This is usually done in the
 * element's _init() function. Then, the child element must call either
 * gst_base_rtp_audio_payload_set_frame_options(),
 * gst_base_rtp_audio_payload_set_sample_options() or
 * gst_base_rtp_audio_payload_set_samplebits_options. Since
 * GstBaseRTPAudioPayload derives from GstBaseRTPPayload, the child element
 * must set any variables or call/override any functions required by that base
 * class. The child element does not need to override any other functions
 * specific to GstBaseRTPAudioPayload.
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/base/gstadapter.h>

#include "gstbasertpaudiopayload.h"

GST_DEBUG_CATEGORY_STATIC (basertpaudiopayload_debug);
#define GST_CAT_DEFAULT (basertpaudiopayload_debug)

/* function to calculate the min/max length and alignment of a packet */
typedef gboolean (*GetLengthsFunc) (GstBaseRTPPayload * basepayload,
    guint * min_payload_len, guint * max_payload_len, guint * align);
/* function to convert bytes to a duration */
typedef GstClockTime (*GetDurationFunc) (GstBaseRTPAudioPayload * payload,
    guint64 bytes);
/* function to convert bytes to RTP timestamp */
typedef guint32 (*GetRTPTimeFunc) (GstBaseRTPAudioPayload * payload,
    guint64 bytes);

struct _GstBaseRTPAudioPayloadPrivate
{
  GetLengthsFunc get_lengths;
  GetDurationFunc get_duration;
  GetRTPTimeFunc get_rtptime;

  GstAdapter *adapter;
  guint fragment_size;
  gboolean discont;
  guint64 offset;
};


#define GST_BASE_RTP_AUDIO_PAYLOAD_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), GST_TYPE_BASE_RTP_AUDIO_PAYLOAD, \
                                GstBaseRTPAudioPayloadPrivate))

static void gst_base_rtp_audio_payload_finalize (GObject * object);

/* length functions */
static gboolean gst_base_rtp_audio_payload_get_frame_lengths (GstBaseRTPPayload
    * basepayload, guint * min_payload_len, guint * max_payload_len,
    guint * align);
static gboolean gst_base_rtp_audio_payload_get_sample_lengths (GstBaseRTPPayload
    * basepayload, guint * min_payload_len, guint * max_payload_len,
    guint * align);

/* duration functions */
static GstClockTime
gst_base_rtp_audio_payload_get_frame_duration (GstBaseRTPAudioPayload * payload,
    guint64 bytes);
static GstClockTime
gst_base_rtp_audio_payload_get_sample_duration (GstBaseRTPAudioPayload *
    payload, guint64 bytes);

/* rtptime functions */
static guint32
gst_base_rtp_audio_payload_get_frame_rtptime (GstBaseRTPAudioPayload * payload,
    guint64 bytes);
static guint32
gst_base_rtp_audio_payload_get_sample_rtptime (GstBaseRTPAudioPayload *
    payload, guint64 bytes);

static GstFlowReturn gst_base_rtp_audio_payload_handle_buffer (GstBaseRTPPayload
    * payload, GstBuffer * buffer);

static GstStateChangeReturn gst_base_rtp_payload_audio_change_state (GstElement
    * element, GstStateChange transition);

static gboolean gst_base_rtp_payload_audio_handle_event (GstPad * pad,
    GstEvent * event);

GST_BOILERPLATE (GstBaseRTPAudioPayload, gst_base_rtp_audio_payload,
    GstBaseRTPPayload, GST_TYPE_BASE_RTP_PAYLOAD);

static void
gst_base_rtp_audio_payload_base_init (gpointer klass)
{
}

static void
gst_base_rtp_audio_payload_class_init (GstBaseRTPAudioPayloadClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseRTPPayloadClass *gstbasertppayload_class;

  g_type_class_add_private (klass, sizeof (GstBaseRTPAudioPayloadPrivate));

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasertppayload_class = (GstBaseRTPPayloadClass *) klass;

  gobject_class->finalize =
      GST_DEBUG_FUNCPTR (gst_base_rtp_audio_payload_finalize);

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_base_rtp_payload_audio_change_state);

  gstbasertppayload_class->handle_buffer =
      GST_DEBUG_FUNCPTR (gst_base_rtp_audio_payload_handle_buffer);
  gstbasertppayload_class->handle_event =
      GST_DEBUG_FUNCPTR (gst_base_rtp_payload_audio_handle_event);

  GST_DEBUG_CATEGORY_INIT (basertpaudiopayload_debug, "basertpaudiopayload", 0,
      "base audio RTP payloader");
}

static void
gst_base_rtp_audio_payload_init (GstBaseRTPAudioPayload * basertpaudiopayload,
    GstBaseRTPAudioPayloadClass * klass)
{
  basertpaudiopayload->priv =
      GST_BASE_RTP_AUDIO_PAYLOAD_GET_PRIVATE (basertpaudiopayload);

  /* these need to be set by child object if frame based */
  basertpaudiopayload->frame_size = 0;
  basertpaudiopayload->frame_duration = 0;

  /* these need to be set by child object if sample based */
  basertpaudiopayload->sample_size = 0;

  basertpaudiopayload->priv->adapter = gst_adapter_new ();
}

static void
gst_base_rtp_audio_payload_finalize (GObject * object)
{
  GstBaseRTPAudioPayload *basertpaudiopayload;

  basertpaudiopayload = GST_BASE_RTP_AUDIO_PAYLOAD (object);

  g_object_unref (basertpaudiopayload->priv->adapter);

  GST_CALL_PARENT (G_OBJECT_CLASS, finalize, (object));
}

/**
 * gst_base_rtp_audio_payload_set_frame_based:
 * @basertpaudiopayload: a pointer to the element.
 *
 * Tells #GstBaseRTPAudioPayload that the child element is for a frame based
 * audio codec
 */
void
gst_base_rtp_audio_payload_set_frame_based (GstBaseRTPAudioPayload *
    basertpaudiopayload)
{
  g_return_if_fail (basertpaudiopayload != NULL);
  g_return_if_fail (basertpaudiopayload->priv->get_lengths == NULL);
  g_return_if_fail (basertpaudiopayload->priv->get_duration == NULL);

  basertpaudiopayload->priv->get_lengths =
      gst_base_rtp_audio_payload_get_frame_lengths;
  basertpaudiopayload->priv->get_duration =
      gst_base_rtp_audio_payload_get_frame_duration;
  basertpaudiopayload->priv->get_rtptime =
      gst_base_rtp_audio_payload_get_frame_rtptime;
}

/**
 * gst_base_rtp_audio_payload_set_sample_based:
 * @basertpaudiopayload: a pointer to the element.
 *
 * Tells #GstBaseRTPAudioPayload that the child element is for a sample based
 * audio codec
 */
void
gst_base_rtp_audio_payload_set_sample_based (GstBaseRTPAudioPayload *
    basertpaudiopayload)
{
  g_return_if_fail (basertpaudiopayload != NULL);
  g_return_if_fail (basertpaudiopayload->priv->get_lengths == NULL);
  g_return_if_fail (basertpaudiopayload->priv->get_duration == NULL);

  basertpaudiopayload->priv->get_lengths =
      gst_base_rtp_audio_payload_get_sample_lengths;
  basertpaudiopayload->priv->get_duration =
      gst_base_rtp_audio_payload_get_sample_duration;
  basertpaudiopayload->priv->get_rtptime =
      gst_base_rtp_audio_payload_get_sample_rtptime;
}

/**
 * gst_base_rtp_audio_payload_set_frame_options:
 * @basertpaudiopayload: a pointer to the element.
 * @frame_duration: The duraction of an audio frame in milliseconds.
 * @frame_size: The size of an audio frame in bytes.
 *
 * Sets the options for frame based audio codecs.
 *
 */
void
gst_base_rtp_audio_payload_set_frame_options (GstBaseRTPAudioPayload
    * basertpaudiopayload, gint frame_duration, gint frame_size)
{
  g_return_if_fail (basertpaudiopayload != NULL);

  basertpaudiopayload->frame_duration = frame_duration;
  basertpaudiopayload->frame_size = frame_size;

  gst_adapter_clear (basertpaudiopayload->priv->adapter);

  GST_DEBUG_OBJECT (basertpaudiopayload, "frame set to %d ms and size %d",
      frame_duration, frame_size);
}

/**
 * gst_base_rtp_audio_payload_set_sample_options:
 * @basertpaudiopayload: a pointer to the element.
 * @sample_size: Size per sample in bytes.
 *
 * Sets the options for sample based audio codecs.
 */
void
gst_base_rtp_audio_payload_set_sample_options (GstBaseRTPAudioPayload
    * basertpaudiopayload, gint sample_size)
{
  g_return_if_fail (basertpaudiopayload != NULL);

  /* sample_size is in bits internally */
  gst_base_rtp_audio_payload_set_samplebits_options (basertpaudiopayload,
      sample_size * 8);
}

/**
 * gst_base_rtp_audio_payload_set_samplebits_options:
 * @basertpaudiopayload: a pointer to the element.
 * @sample_size: Size per sample in bits.
 *
 * Sets the options for sample based audio codecs.
 *
 * Since: 0.10.18
 */
void
gst_base_rtp_audio_payload_set_samplebits_options (GstBaseRTPAudioPayload
    * basertpaudiopayload, gint sample_size)
{
  guint fragment_size;

  g_return_if_fail (basertpaudiopayload != NULL);

  basertpaudiopayload->sample_size = sample_size;

  /* sample_size is in bits and is converted into multiple bytes */
  fragment_size = sample_size;
  while ((fragment_size % 8) != 0)
    fragment_size += fragment_size;
  basertpaudiopayload->priv->fragment_size = fragment_size / 8;

  gst_adapter_clear (basertpaudiopayload->priv->adapter);

  GST_DEBUG_OBJECT (basertpaudiopayload,
      "Samplebits set to sample size %d bits", sample_size);
}

static void
gst_base_rtp_audio_payload_set_meta (GstBaseRTPAudioPayload * payload,
    GstBuffer * buffer, guint payload_len, GstClockTime timestamp)
{
  GstBaseRTPPayload *basepayload;

  basepayload = GST_BASE_RTP_PAYLOAD_CAST (payload);

  /* set payload type */
  gst_rtp_buffer_set_payload_type (buffer, basepayload->pt);
  /* set marker bit for disconts */
  if (payload->priv->discont) {
    GST_DEBUG_OBJECT (payload, "Setting marker and DISCONT");
    gst_rtp_buffer_set_marker (buffer, TRUE);
    GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DISCONT);
    payload->priv->discont = FALSE;
  }
  GST_BUFFER_TIMESTAMP (buffer) = timestamp;

  /* get the offset in bytes */
  GST_BUFFER_OFFSET (buffer) =
      payload->priv->get_rtptime (payload, payload->priv->offset);

  payload->priv->offset += payload_len;
}

/**
 * gst_base_rtp_audio_payload_push:
 * @baseaudiopayload: a #GstBaseRTPPayload
 * @data: data to set as payload
 * @payload_len: length of payload
 * @timestamp: a #GstClockTime
 *
 * Create an RTP buffer and store @payload_len bytes of @data as the
 * payload. Set the timestamp on the new buffer to @timestamp before pushing
 * the buffer downstream.
 *
 * Returns: a #GstFlowReturn
 *
 * Since: 0.10.13
 */
GstFlowReturn
gst_base_rtp_audio_payload_push (GstBaseRTPAudioPayload * baseaudiopayload,
    const guint8 * data, guint payload_len, GstClockTime timestamp)
{
  GstBaseRTPPayload *basepayload;
  GstBuffer *outbuf;
  guint8 *payload;
  GstFlowReturn ret;

  basepayload = GST_BASE_RTP_PAYLOAD (baseaudiopayload);

  GST_DEBUG_OBJECT (baseaudiopayload, "Pushing %d bytes ts %" GST_TIME_FORMAT,
      payload_len, GST_TIME_ARGS (timestamp));

  /* create buffer to hold the payload */
  outbuf = gst_rtp_buffer_new_allocate (payload_len, 0, 0);

  /* set metadata */
  gst_base_rtp_audio_payload_set_meta (baseaudiopayload, outbuf, payload_len,
      timestamp);

  /* copy payload */
  payload = gst_rtp_buffer_get_payload (outbuf);
  memcpy (payload, data, payload_len);

  ret = gst_basertppayload_push (basepayload, outbuf);

  return ret;
}

/**
 * gst_base_rtp_audio_payload_flush:
 * @baseaudiopayload: a #GstBaseRTPPayload
 * @payload_len: length of payload
 * @timestamp: a #GstClockTime
 *
 * Create an RTP buffer and store @payload_len bytes of the adapter as the
 * payload. Set the timestamp on the new buffer to @timestamp before pushing
 * the buffer downstream.
 *
 * If @payload_len is -1, all pending bytes will be flushed. If @timestamp is
 * -1, the timestamp will be calculated automatically.
 *
 * Returns: a #GstFlowReturn
 *
 * Since: 0.10.25
 */
GstFlowReturn
gst_base_rtp_audio_payload_flush (GstBaseRTPAudioPayload * baseaudiopayload,
    guint payload_len, GstClockTime timestamp)
{
  GstBaseRTPPayload *basepayload;
  GstBuffer *outbuf;
  guint8 *payload;
  GstFlowReturn ret;
  GstAdapter *adapter;
  guint64 distance;

  adapter = baseaudiopayload->priv->adapter;

  basepayload = GST_BASE_RTP_PAYLOAD (baseaudiopayload);

  if (payload_len == -1)
    payload_len = gst_adapter_available (adapter);

  /* nothing to do, just return */
  if (payload_len == 0)
    return GST_FLOW_OK;

  if (timestamp == -1) {
    /* calculate the timestamp */
    timestamp = gst_adapter_prev_timestamp (adapter, &distance);

    GST_LOG_OBJECT (baseaudiopayload,
        "last timestamp %" GST_TIME_FORMAT ", distance %" G_GUINT64_FORMAT,
        GST_TIME_ARGS (timestamp), distance);

    if (GST_CLOCK_TIME_IS_VALID (timestamp) && distance > 0) {
      /* convert the number of bytes since the last timestamp to time and add to
       * the last seen timestamp */
      timestamp +=
          baseaudiopayload->priv->get_duration (baseaudiopayload, distance);
    }
  }

  GST_DEBUG_OBJECT (baseaudiopayload, "Pushing %d bytes ts %" GST_TIME_FORMAT,
      payload_len, GST_TIME_ARGS (timestamp));

  /* create buffer to hold the payload */
  outbuf = gst_rtp_buffer_new_allocate (payload_len, 0, 0);

  /* set metadata */
  gst_base_rtp_audio_payload_set_meta (baseaudiopayload, outbuf, payload_len,
      timestamp);

  payload = gst_rtp_buffer_get_payload (outbuf);
  gst_adapter_copy (adapter, payload, 0, payload_len);
  gst_adapter_flush (adapter, payload_len);

  ret = gst_basertppayload_push (basepayload, outbuf);

  return ret;
}

#define ALIGN_DOWN(val,len) ((val) - ((val) % (len)))

/* this assumes all frames have a constant duration and a constant size */
static gboolean
gst_base_rtp_audio_payload_get_frame_lengths (GstBaseRTPPayload *
    basepayload, guint * min_payload_len, guint * max_payload_len,
    guint * align)
{
  GstBaseRTPAudioPayload *payload;
  guint frame_size;
  guint frame_duration;
  guint max_frames;
  guint maxptime_octets;
  guint minptime_octets;

  payload = GST_BASE_RTP_AUDIO_PAYLOAD_CAST (basepayload);

  if (payload->frame_size == 0 || payload->frame_duration == 0)
    return FALSE;

  *align = frame_size = payload->frame_size;
  frame_duration = payload->frame_duration * GST_MSECOND;

  if (basepayload->max_ptime != -1) {
    maxptime_octets =
        gst_util_uint64_scale (frame_size, basepayload->max_ptime,
        frame_duration);
    /* must be a multiple of the frame_size */
    maxptime_octets = MAX (frame_size, maxptime_octets);
  } else {
    maxptime_octets = G_MAXUINT;
  }

  /* MTU max */
  max_frames =
      gst_rtp_buffer_calc_payload_len (GST_BASE_RTP_PAYLOAD_MTU (payload), 0,
      0);
  /* round down to frame_size */
  max_frames = ALIGN_DOWN (max_frames, frame_size);
  /* max payload length */
  *max_payload_len = MIN (max_frames, maxptime_octets);

  /* min number of bytes based on a given ptime, has to be a multiple
     of frame duration */
  minptime_octets =
      gst_util_uint64_scale (frame_size, basepayload->min_ptime,
      frame_duration);
  *min_payload_len = MAX (minptime_octets, frame_size);

  if (*min_payload_len > *max_payload_len)
    *min_payload_len = *max_payload_len;

  return TRUE;
}

static GstClockTime
gst_base_rtp_audio_payload_get_frame_duration (GstBaseRTPAudioPayload *
    payload, guint64 bytes)
{
  return (bytes / payload->frame_size) * (payload->frame_duration *
      GST_MSECOND);
}

static guint32
gst_base_rtp_audio_payload_get_frame_rtptime (GstBaseRTPAudioPayload * payload,
    guint64 bytes)
{
  GstClockTime duration;

  duration =
      (bytes / payload->frame_size) * (payload->frame_duration * GST_MSECOND);

  return gst_util_uint64_scale_int (duration,
      GST_BASE_RTP_PAYLOAD_CAST (payload)->clock_rate, GST_SECOND);
}

static gboolean
gst_base_rtp_audio_payload_get_sample_lengths (GstBaseRTPPayload *
    basepayload, guint * min_payload_len, guint * max_payload_len,
    guint * align)
{
  GstBaseRTPAudioPayload *payload;
  guint maxptime_octets;
  guint minptime_octets;

  payload = GST_BASE_RTP_AUDIO_PAYLOAD_CAST (basepayload);

  if (payload->sample_size == 0)
    return FALSE;

  /* sample_size is in bits and is converted into multiple bytes */
  *align = payload->priv->fragment_size;

  /* max number of bytes based on given ptime */
  if (basepayload->max_ptime != -1) {
    maxptime_octets = gst_util_uint64_scale (basepayload->max_ptime * 8,
        basepayload->clock_rate, payload->sample_size * GST_SECOND);
  } else {
    maxptime_octets = G_MAXUINT;
  }

  *max_payload_len = MIN (
      /* MTU max */
      gst_rtp_buffer_calc_payload_len (GST_BASE_RTP_PAYLOAD_MTU
          (payload), 0, 0),
      /* ptime max */
      maxptime_octets);

  /* min number of bytes based on a given ptime, has to be a multiple
   * of sample rate */
  minptime_octets = gst_util_uint64_scale (basepayload->min_ptime * 8,
      basepayload->clock_rate, payload->sample_size * GST_SECOND);

  *min_payload_len = MAX (minptime_octets, *align);

  if (*min_payload_len > *max_payload_len)
    *min_payload_len = *max_payload_len;

  return TRUE;
}

static GstClockTime
gst_base_rtp_audio_payload_get_sample_duration (GstBaseRTPAudioPayload *
    payload, guint64 bytes)
{
  return gst_util_uint64_scale (bytes * 8, GST_SECOND,
      GST_BASE_RTP_PAYLOAD (payload)->clock_rate * payload->sample_size);
}

static guint32
gst_base_rtp_audio_payload_get_sample_rtptime (GstBaseRTPAudioPayload *
    payload, guint64 bytes)
{
  return (bytes * 8) / payload->sample_size;
}

static GstFlowReturn
gst_base_rtp_audio_payload_handle_buffer (GstBaseRTPPayload *
    basepayload, GstBuffer * buffer)
{
  GstBaseRTPAudioPayload *payload;
  guint payload_len;
  GstFlowReturn ret;
  guint available;
  guint min_payload_len;
  guint max_payload_len;
  guint align;
  guint size;
  gboolean discont;

  ret = GST_FLOW_OK;

  payload = GST_BASE_RTP_AUDIO_PAYLOAD_CAST (basepayload);

  if (payload->priv->get_lengths == NULL || payload->priv->get_duration == NULL)
    goto config_error;

  discont = GST_BUFFER_IS_DISCONT (buffer);
  if (discont) {
    GST_DEBUG_OBJECT (payload, "Got DISCONT");
    /* flush everything out of the adapter, mark DISCONT */
    ret = gst_base_rtp_audio_payload_flush (payload, -1, -1);
    payload->priv->discont = TRUE;
  }

  if (!payload->priv->get_lengths (basepayload, &min_payload_len,
          &max_payload_len, &align))
    goto config_error;

  GST_DEBUG_OBJECT (payload,
      "Calculated min_payload_len %u and max_payload_len %u",
      min_payload_len, max_payload_len);

  size = GST_BUFFER_SIZE (buffer);

  /* shortcut, we don't need to use the adapter when the packet can be pushed
   * through directly. */
  available = gst_adapter_available (payload->priv->adapter);

  GST_DEBUG_OBJECT (payload, "got buffer size %u, available %u",
      size, available);

  if (available == 0 && (size >= min_payload_len && size <= max_payload_len)) {
    /* If buffer fits on an RTP packet, let's just push it through
     * this will check against max_ptime and max_mtu */
    GST_DEBUG_OBJECT (payload, "Fast packet push");
    ret = gst_base_rtp_audio_payload_push (payload,
        GST_BUFFER_DATA (buffer), size, GST_BUFFER_TIMESTAMP (buffer));
    gst_buffer_unref (buffer);
  } else {
    /* push the buffer in the adapter */
    gst_adapter_push (payload->priv->adapter, buffer);
    available += size;

    GST_DEBUG_OBJECT (payload, "available now %u", available);

    /* as long as we have full frames */
    while (available >= min_payload_len) {
      payload_len = ALIGN_DOWN (available, align);
      payload_len = MIN (max_payload_len, payload_len);

      /* and flush out the bytes from the adapter, automatically set the
       * timestamp. */
      ret = gst_base_rtp_audio_payload_flush (payload, payload_len, -1);

      available -= payload_len;
      GST_DEBUG_OBJECT (payload, "available after push %u", available);
    }
  }
  return ret;

  /* ERRORS */
config_error:
  {
    GST_ELEMENT_ERROR (payload, STREAM, NOT_IMPLEMENTED, (NULL),
        ("subclass did not configure us properly"));
    gst_buffer_unref (buffer);
    return GST_FLOW_ERROR;
  }
}

static GstStateChangeReturn
gst_base_rtp_payload_audio_change_state (GstElement * element,
    GstStateChange transition)
{
  GstBaseRTPAudioPayload *basertppayload;
  GstStateChangeReturn ret;

  basertppayload = GST_BASE_RTP_AUDIO_PAYLOAD (element);

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_adapter_clear (basertppayload->priv->adapter);
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
gst_base_rtp_payload_audio_handle_event (GstPad * pad, GstEvent * event)
{
  GstBaseRTPAudioPayload *payload;
  gboolean res = FALSE;

  payload = GST_BASE_RTP_AUDIO_PAYLOAD (gst_pad_get_parent (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      /* flush remaining bytes in the adapter */
      gst_base_rtp_audio_payload_flush (payload, -1, -1);
      break;
    case GST_EVENT_FLUSH_STOP:
      gst_adapter_clear (payload->priv->adapter);
      break;
    default:
      break;
  }

  gst_object_unref (payload);

  /* return FALSE to let parent handle the remainder of the event */
  return res;
}

/**
 * gst_base_rtp_audio_payload_get_adapter:
 * @basertpaudiopayload: a #GstBaseRTPAudioPayload
 *
 * Gets the internal adapter used by the depayloader.
 *
 * Returns: a #GstAdapter.
 *
 * Since: 0.10.13
 */
GstAdapter *
gst_base_rtp_audio_payload_get_adapter (GstBaseRTPAudioPayload
    * basertpaudiopayload)
{
  GstAdapter *adapter;

  if ((adapter = basertpaudiopayload->priv->adapter))
    g_object_ref (adapter);

  return adapter;
}
