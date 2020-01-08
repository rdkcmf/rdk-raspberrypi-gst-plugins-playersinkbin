/*
 * Copyright 2018 RDK Management
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation, version 2
 * of the license.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#ifndef __GST_PLAYERSINKBIN_H__
#define __GST_PLAYERSINKBIN_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_PLAYER_SINKBIN \
  (gst_player_sinkbin_get_type())
#define GST_PLAYER_SINKBIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PLAYER_SINKBIN,GstPlayerSinkBin))
#define GST_PLAYER_SINKBIN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PLAYER_SINKBIN,GstPlayerSinkBinClass))
#define GST_IS_PLAYER_SINKBIN(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PLAYER_SINKBIN))
#define GST_IS_PLAYER_SINKBIN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PLAYER_SINKBIN))

#define DEFAULT_RAW_CAPS \
    "video/x-raw; " \
    "audio/x-raw; " \
    "text/x-raw; " \
    "subpicture/x-dvd; " \
    "subpicture/x-pgs"

#define ENABLE_AUDIO_REMOVAL_FOR_TRICK_MODES 1

typedef struct _GstPlayerSinkBin      GstPlayerSinkBin;
typedef struct _GstPlayerSinkBinClass GstPlayerSinkBinClass;

typedef struct _esinfo {
	guint es_pid;
	guint es_type;
	char lang[12];
} esinfo_t;


struct _GstPlayerSinkBin
{
	GstBin parent_instance; /* Extend GstBin*/

	/* SOC CHANGES: Add/Remove Bin Elements based on SOC specific media pipeline*/
	GstElement* demux;
	GstElement* mpegdemux;
	GstElement* video_decoder;
        GstElement* video_parser;
        GstElement* videoParse;
        GstElement* cc_dec;
	GstElement* video_sink;
	GstElement* audio_sink;
	GstElement* m_vqueue;
	GstElement* m_aqueue;
	GstElement* m_cqueue;
	GstElement* m_aconvert;
	GstElement* m_adec;
	GstElement* m_audio_parser;
	GstElement* m_resample;
	GstElement* vconvert;
	GstElement* m_vscalar;
        GstElement* m_vfilter;
	GstCaps* m_vcaps;
	/* Generic Properties */
    	gint video_native_width;/* Readable */
    	gint video_native_height;/* Readable */
	gint prog_no; /* ReadWriteable */
	char available_languages[256]; /* Readable */
	char preffered_language[8];/* Writeable */
	gpointer video_decode_handle;/* Readable */
	gboolean video_mute; /* ReadWriteable */
	gboolean show_last_frame; /* ReadWriteable */
	char video_rectangle[16];/* ReadWriteable */
	gint plane;/* ReadWriteable */
	gfloat play_speed; /* ReadWriteable */
	gdouble current_position; /* ReadWriteable */
	gint resolution;/* ReadWriteable */
	gboolean audio_mute; /* ReadWriteable */
	gdouble volume;/* ReadWriteable */

	GstCaps *caps;

	/* Audio Stream Information */
	esinfo_t *m_esinfo;
	guint num_streams;
	guint num_audio_streams;
	guint preffered_audio_pid;
	gchar prefferd_pid_instring[8];
	gchar linkedaudiopadname[16];

	gboolean m_bUseIsmdDemux;
};

struct _GstPlayerSinkBinClass
{
	GstBinClass parent_class;
	void (*playersinkbinstatuscb)( GstPlayerSinkBin* gstplayersinkbinsrc, gint status, gpointer userdata);
	void (*videonativesizecb)(gint* width, gint* height);  
};

GType gst_player_sinkbin_get_type (void);
/* Enums for Plane and Resolution */
#define GST_TYPE_PLANE \
  (playersink_gst_plane_get_type())
#define GST_TYPE_RESOLUTION \
  (playersink_gst_resolution_get_type())

GType playersink_gst_plane_get_type (void);
GType playersink_gst_resolution_get_type (void);

G_END_DECLS

#endif /* __GST_PLAYERSINKBIN_H__ */
