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
/**
 * SECTION:element-playersinkbin
 *
 * #GstBin that constructs a demux and decode pipeline for TS source using RaspberryPi gstreamer
 * demuxers and decoders for use by RMF MediaPlayerSink
 *  export XDG_RUNTIME_DIR="/run/user/0"
 *  export WAYLAND_DISPLAY= "wayland displayname"
 * <title>Example launch line</title>
 * |[
 * gst-launch filesrc location="input.ts" ! playersinkbin
 * ]|
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>

#include "gstplayersinkbin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MULTIPLE_AUDIO_LANG_SELECTION
#define MEDIA_CONF "/etc/media.conf"
#define AV_STATUS "/opt/AVstatus.dat"
#define GST_CAPS "/opt/gstcaps.txt"
/* GST Enums for Plane and Resolution */
static int OMX_Enable=1;
static int Audio_Enable=1;
static int audio_flag=0;
static int avstatus=0;

#define AUDIO_ENABLE 1

enum VideoParser {
    MPEG_VPARSE,
    H264_VPARSE,
    MAX_VPARSER
};

const char V_PARSER[MAX_VPARSER][16] = {"mpegvideoparse",\
                             "h264parse",\
                            };

GType
playersink_gst_plane_get_type (void)
{
        static GType playersink_gst_plane_type = 0;
        static const GEnumValue plane_types[] = {
                {1, "Plane 1", "Pixel Plane 1"},
                {2, "Plane 2", "Pixel Plane 2"},
                {3, "Plane 3", "Pixel Plane 3"},
                {4, "Plane 4", "Pixel Plane 4"},
                {10, NULL, NULL}
        };

        if (!playersink_gst_plane_type) {
                playersink_gst_plane_type =
                    g_enum_register_static ("GstPlane", plane_types);
        }
        return playersink_gst_plane_type;
}

GType
playersink_gst_resolution_get_type (void)
{
        static GType playersink_gst_resolution_type = 0;
        static const GEnumValue resolution_types[] = {
                {-1, "None to be configured", "none"},
                {0, "480 interlaced at 60 Hz", "480i"},
                {1, "480 progressive at 60 Hz", "480p"},
                {2, "576 interlaced at 50 Hz", "576i"},
                {3, "576 progressive at 50 Hz", "576p"},
                {4, "720 progressive at 60 Hz", "720p"},
                {5, "720 progressive at 50 Hz", "720p50"},
                {6, "1080 interlaced at 60 Hz", "1080i"},
                {7, "1080 interlaced at 50 Hz", "1080i50"},
                {8, "1080 progressive at 60 Hz", "1080p"},
                {9, "1080 progressive at 50 Hz", "1080p50"},
                {10, NULL, NULL}
        };

        if (!playersink_gst_resolution_type) {
                playersink_gst_resolution_type =
                    g_enum_register_static ("GstResolution", resolution_types);
        }
        return playersink_gst_resolution_type;
}


#define DEFAULT_PROGRAM_NUM 1
#define DEFAULT_AVAILABLE_LANGUAGES "eng"
#define DEFAULT_PREFERRED_LANGUAGE "eng"
#define DEFAULT_SHOW_LAST_FRAME 1
#define DEFAULT_VIDEO_RECTANGLE "0,0,0,0"
#define DEFAULT_VIDEO_DECODER_HANDLE -1
#define DEFAULT_PLANE 1
#define DEFAULT_PLAY_SPEED 1
#define DEFAULT_CURRENT_POSITION 0
#define DEFAULT_RESOLUTION -1
#define DEFAULT_VIDEO_MUTE 0 
#define DEFAULT_AUDIO_MUTE 0 
#define DEFAULT_VOLUME 1.0
#define DEFAULT_LIVE 1
#define DEFAULT_CC_DESC "0"

/* SOC CHANGES: Names of gstreamer Elements to be contained in the bin,add/remove appropriately based on platform */
#define VIDEO_PLANE	7

/* Plane Values for Intel platform. Reference ismd-vidsink gstreamer element */
#define UPPA 5
#define UPPB 6
#define UPPC 7
#define UPPD 8

#define PLANE_1 1
#define PLANE_2 2
#define PLANE_3 3
#define PLANE_4 4

#ifdef USE_GST1
const char DEMUX[] = "tsdemux";
const char VDEC[] = "avdec_mpeg2video";
const char AC3_DEC[] = "avdec_ac3";
const char MP3_DEC[] = "avdec_mp3";
#else
const char DEMUX[] = "flutsdemux";
const char VDEC[] = "ffdec_mpeg2video";
const char AC3_DEC[] = "ffdec_ac3";
const char MP3_DEC[] = "ffdec_mp3";
#endif
const char ASINK[] = "autoaudiosink";
const char VSINK[] = "westerossink";	// Rpi westeros sink
/* End SOC CHANGES */

#define GSTPLAYERSINKBIN_EVENT_HAVE_VIDEO 0x01
#define GSTPLAYERSINKBIN_EVENT_HAVE_AUDIO 0x02
#define GSTPLAYERSINKBIN_EVENT_FIRST_VIDEO_FRAME 0x03	
#define GSTPLAYERSINKBIN_EVENT_FIRST_AUDIO_FRAME 0x04

static void gst_decode_bin_dispose (GObject * object);

#if defined(ENABLE_AUDIO_REMOVAL_FOR_TRICK_MODES)
static void createLinkAudioChain (GstPlayerSinkBin *playersinkbin);
static void deleteUnlinkAudioChain (GstPlayerSinkBin *playersinkbin);
#endif

GST_DEBUG_CATEGORY_STATIC (gst_player_sinkbin_debug);
#define GST_CAT_DEFAULT gst_player_sinkbin_debug

static GstStaticPadTemplate  playersink_bin_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
                             GST_PAD_SINK,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS("ANY"));

/* Properties */
enum
{
	PROP_0,
	PROP_PROGRAM_NUM,
	PROP_PMT_INFO,
	PROP_AVAILABLE_LANGUAGES,
	PROP_PREFERRED_LANGUAGE,
	PROP_VIDEO_DECODE_HANDLE,
	PROP_SHOW_LAST_FRAME,
	PROP_VIDEO_RECTANGLE,
	PROP_VIDEO_MUTE,
	PROP_PLANE,
	PROP_PLAY_SPEED,
	PROP_CURRENT_POSITION,
	PROP_RESOLUTION,
	PROP_AUDIO_MUTE,
	PROP_VOLUME,
	PROP_LAST,
	PROP_IS_LIVE,
        PROP_CC_DESC
};

enum
{
	SIGNAL_PLAYERSINKBIN,
	LAST_SIGNAL
};

static gchar getVideoParserIndex(const gchar* padname) {
        gchar index = -1;
        if(g_strrstr(padname, "x-h264"))
                 index = H264_VPARSE;
        else if(g_strrstr(padname, "mpeg"))
                 index = MPEG_VPARSE;

        return index;
}

static guint gst_player_sinkbin_signals[LAST_SIGNAL] = { 0 };
static GstBinClass *parent_class;

static void gst_player_sinkbin_set_property (GObject * object, guint prop_id,
        const GValue * value, GParamSpec * pspec);
static void gst_player_sinkbin_get_property (GObject * object, guint prop_id,
        GValue * value, GParamSpec * pspec);

/*static void gst_player_sinkbin_handle_message (GstBin * bin, GstMessage * message);*/
static GstStateChangeReturn gst_playersinkbin_change_state (GstElement * element,
    GstStateChange transition);

static gboolean printField (GQuark field, const GValue * val, gpointer fp) {
  gchar *str = gst_value_serialize (val);
  
  fprintf((FILE*)fp,"%s:%s", g_quark_to_string (field), str);
  fwrite("#",1,1,(FILE*)fp); 
  g_free (str);
  return TRUE;
}

static void printCapabilities (const GstCaps * capabilities, const gchar * ptr, FILE *fp) {
  guint i;
   
  g_return_if_fail (capabilities != NULL);
   
  if (gst_caps_is_any (capabilities)) {
    g_print ("ANY\n");
    return;
  }
  if (gst_caps_is_empty (capabilities)) {
    g_print ("EMPTY\n");
    return;
  }
   
  for (i = 0; i < gst_caps_get_size (capabilities); i++) {
    GstStructure *caps_Struct = gst_caps_get_structure (capabilities, i);

     fprintf(fp, "%s", gst_structure_get_name (caps_Struct));
    fwrite("#",1,1,fp);
    gst_structure_foreach (caps_Struct, printField, (gpointer) fp);
  }
}
static void print_pad_caps (GstElement *element, gchar *padName, FILE *fp) {
  GstPad *pad = NULL;
  GstCaps *capabilities = NULL;
   
  pad = gst_element_get_static_pad (element, padName);
  if (!pad) {
    g_printerr ("Pad cannot be retreived '%s'\n", padName);
    return;
  }
   
  capabilities = gst_pad_get_current_caps (pad);
   
  printCapabilities (capabilities, "      ", fp);
  fwrite("\n",1,1,fp);
  gst_caps_unref (capabilities);
  gst_object_unref (pad);
}


static GstStateChangeReturn
gst_playersinkbin_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;


  GstPlayerSinkBin *playersinkbin = GST_PLAYER_SINKBIN (element);
  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
       GST_INFO_OBJECT(playersinkbin,"Pipeline in playing state .Emitting GSTPLAYERSINKBIN_EVENT_FIRST_VIDEO_FRAME signal\n");
        g_signal_emit (G_OBJECT (playersinkbin), gst_player_sinkbin_signals[SIGNAL_PLAYERSINKBIN], 0,
                                       GSTPLAYERSINKBIN_EVENT_FIRST_VIDEO_FRAME);
	 FILE *fp;
        fp = fopen(GST_CAPS, "w");
        if(fp == NULL)
        {
                printf("File Not found\n");
        }
	else{
	print_pad_caps (playersinkbin->m_aconvert, "sink", fp);
	print_pad_caps (playersinkbin->m_adec, "sink", fp);	
	fclose(fp);	
	}
          if(avstatus)
              {
                remove(AV_STATUS);
                FILE* fq= fopen(AV_STATUS,"w");
                fwrite("Video:SUCCESS",1,13,fq);
                fwrite("\n",1,1,fq);
                fwrite("Audio:SUCCESS",1,13,fq);
                if(fq)
                fclose(fq);
                avstatus=1;
              } 
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
    switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
                remove(AV_STATUS);
                FILE* fp= fopen(AV_STATUS,"w");
                fwrite("Video:PAUSED",1,13,fp);
                fwrite("\n",1,1,fp);
                fwrite("Audio:PAUSED",1,13,fp );
                if(fp)
                fclose(fp);
                avstatus=1;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
                remove(AV_STATUS);
                fp= fopen(AV_STATUS,"w");
                fwrite("Video:STOPPED",1,13,fp);
                fwrite("\n",1,1,fp);
                fwrite("Audio:STOPPED",1,13,fp );
                if(fp)
                fclose(fp);
                avstatus=0;
      break;
    default:
      break;
  }

  return ret;

}
/* GObject vmethod implementations */

/* initialize the plugin's class */
static void
gst_player_sinkbin_class_init (GstPlayerSinkBinClass * klass)
{
	GObjectClass *gobject_klass;
	GstElementClass *gstelement_klass;
	/* GstBinClass *gstbin_klass = (GstBinClass *) klass; */

	gobject_klass = (GObjectClass *) klass;
	gstelement_klass = (GstElementClass *) klass;

	parent_class = g_type_class_peek_parent (klass);

	gobject_klass->dispose = gst_decode_bin_dispose;
	gobject_klass->set_property = gst_player_sinkbin_set_property;
	gobject_klass->get_property = gst_player_sinkbin_get_property;

	/* Install Properties for the bin */
	g_object_class_install_property (gobject_klass, PROP_PROGRAM_NUM,
	                                 g_param_spec_int("program-num", "program-num",
	                                         "Program number to set",
	                                         G_MININT, G_MAXINT, DEFAULT_PROGRAM_NUM,
	                                         G_PARAM_READWRITE));

	g_object_class_install_property (gobject_klass, PROP_AVAILABLE_LANGUAGES,
	                                 g_param_spec_string("available-languages", "available-languages",
	                                         "Available languages in the program as a comma separated list",
	                                         DEFAULT_AVAILABLE_LANGUAGES,
	                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_klass, PROP_PREFERRED_LANGUAGE,
	                                 g_param_spec_string("preferred-language", "preferred-language",
	                                         "Preferred language to be set",
	                                         DEFAULT_PREFERRED_LANGUAGE,
	                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


	g_object_class_install_property (gobject_klass, PROP_VIDEO_DECODE_HANDLE,
	                                 g_param_spec_pointer ("video-decode-handle","video-decode-handle",
	                                         "Video decoder handle in use",
	                                         G_PARAM_READABLE));


	g_object_class_install_property (gobject_klass, PROP_SHOW_LAST_FRAME,
	                                 g_param_spec_boolean ("show-last-frame", "show-last-frame",
	                                         "Keep displaying the last frame rather than a black one",
	                                         DEFAULT_SHOW_LAST_FRAME,
	                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_klass, PROP_VIDEO_RECTANGLE,
	                                 g_param_spec_string ("rectangle", "Destination rectangle",
	                                         "The destination rectangle, (0,0,0,0) full screen",
	                                         DEFAULT_VIDEO_RECTANGLE,
	                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_klass, PROP_CC_DESC,
	                                 g_param_spec_string ("cc-descriptor", "cc-descriptor",
	                                         "Get Closed captioning descriptor",
	                                         DEFAULT_CC_DESC,
	                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (gobject_klass, PROP_IS_LIVE,
	                                 g_param_spec_boolean ("is-live", "is live",
	                                         "Check whether Live stream or recorded content",
	                                         DEFAULT_LIVE,
						 G_PARAM_WRITABLE));

	g_object_class_install_property (gobject_klass, PROP_PLANE,
	                                 g_param_spec_enum ("plane",
	                                         "Plane used for rendering",
	                                         "Define the Plane to be used by the platform ",
	                                         GST_TYPE_PLANE,
	                                         DEFAULT_PLANE, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_klass, PROP_PLAY_SPEED,
	                                 g_param_spec_float("play-speed", "play speed",
	                                         "Play Speed to be set or current play speed",
	                                         -G_MAXFLOAT, G_MAXFLOAT, DEFAULT_PLAY_SPEED,
	                                         G_PARAM_READWRITE));

	g_object_class_install_property (gobject_klass, PROP_CURRENT_POSITION,
	                                 g_param_spec_double("current-position", "current position",
	                                         "Current play position in the stream",
	                                         G_MINDOUBLE, G_MAXDOUBLE, DEFAULT_PROGRAM_NUM,
	                                         G_PARAM_READWRITE));

	g_object_class_install_property (gobject_klass, PROP_RESOLUTION,
	                                 g_param_spec_enum ("resolution",
	                                         "Resolution to be set",
	                                         "Define the resolution to be used",
	                                         GST_TYPE_RESOLUTION,
	                                         DEFAULT_RESOLUTION, G_PARAM_READWRITE));

	g_object_class_install_property (gobject_klass, PROP_VIDEO_MUTE,
	                                 g_param_spec_boolean ("video-mute", "video-mute",
	                                         "mute/unmute video",
	                                         DEFAULT_VIDEO_MUTE,
	                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_klass, PROP_AUDIO_MUTE,
	                                 g_param_spec_boolean ("audio-mute", "audio-mute",
	                                         "mute/unmute audio",
	                                         DEFAULT_AUDIO_MUTE,
	                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (gobject_klass, PROP_VOLUME,
	                                 g_param_spec_double("volume", "volume",
	                                         "Audio volume to be set",
	                                         G_MINDOUBLE, G_MAXDOUBLE, DEFAULT_VOLUME,
	                                         G_PARAM_READWRITE));


	gst_element_class_add_pad_template (gstelement_klass,
	                                    gst_static_pad_template_get (&playersink_bin_sink_template));

	gst_element_class_set_details_simple (gstelement_klass,
	                                      "Player Sink Bin", "Demux/Decoder",
	                                      "Demux and decode mpegts",
	                                      "RDK-M");
	 gstelement_klass->change_state =
	     GST_DEBUG_FUNCPTR (gst_playersinkbin_change_state);

	/* gstbin_klass->handle_message =
	     GST_DEBUG_FUNCPTR (gst_player_sinkbin_handle_message);
	*/
	/**
	 * Signal that the video/audio has been reached. This signal is emited from
	 * the onDemuxSrcPadAdded  thread.
	 */
	gst_player_sinkbin_signals[SIGNAL_PLAYERSINKBIN] =
	    g_signal_new ("event-callback", G_TYPE_FROM_CLASS (gstelement_klass),
	                  (GSignalFlags)(G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
	                  G_STRUCT_OFFSET (GstPlayerSinkBinClass, playersinkbinstatuscb), NULL, NULL,
	                  g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
}


#if defined(ENABLE_AUDIO_REMOVAL_FOR_TRICK_MODES)
static void
createLinkAudioChain (
	GstPlayerSinkBin *playersinkbin
) {
    GstState gst_current;
    GstState gst_pending;
    float timeout = 1.0; 

	if (playersinkbin == NULL) {
		GST_ERROR_OBJECT(playersinkbin,"playersinkbin : createLinkAudioChain - Invalid playersinkbin pointer\n");
		return;
	}

	/* Create audio decoding pipeline factories */
	playersinkbin->audio_sink = gst_element_factory_make(ASINK, "player_asink");

	if (!playersinkbin->audio_sink) {
		GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to instantiate audio decoder (%s)\n", ASINK);
		return;
	}


	gst_bin_add_many (GST_BIN(playersinkbin), playersinkbin->audio_sink, NULL);


	if ((GST_STATE_CHANGE_FAILURE  == gst_element_get_state (GST_ELEMENT (playersinkbin), &gst_current, &gst_pending, timeout * GST_SECOND)) || (gst_current == GST_STATE_NULL) ) {
		GST_WARNING_OBJECT(playersinkbin,"playersinkbin : createLinkAudioChain - gst_element_get_state - State = %d, Pending = %d\n", gst_current, gst_pending);
		return;
	}
                gst_element_link(playersinkbin->m_resample,playersinkbin->audio_sink);
                //send audioCallback event
                gst_element_sync_state_with_parent(playersinkbin->audio_sink);
                gst_element_sync_state_with_parent(GST_ELEMENT (playersinkbin));
                g_signal_emit (G_OBJECT (playersinkbin), gst_player_sinkbin_signals[SIGNAL_PLAYERSINKBIN], 0,
                                           GSTPLAYERSINKBIN_EVENT_HAVE_AUDIO);

}


static void
deleteUnlinkAudioChain (
	GstPlayerSinkBin *playersinkbin
) {
        GstState gst_current;
        GstState gst_pending;
        float timeout = 1.0;
	if (playersinkbin == NULL) {
		GST_ERROR_OBJECT(playersinkbin,"playersinkbin : deleteUnlinkAudioChain - Invalid playersinkbin pointer\n");
		return;
	}
        gst_element_get_state (GST_ELEMENT (playersinkbin), &gst_current, &gst_pending, timeout * GST_SECOND);
         printf(" playersinkbin : deleteLinkAudioChain - gst_element_get_state - State = %d, Pending = %d\n", gst_current, gst_pending);

	// Unlink pads after setting the downstream elements to NULL
	GST_DEBUG_OBJECT(playersinkbin,"playersinkbin : Speed Change - Trying to Unlink on Speed Change\n");

	if (gst_element_set_state(playersinkbin->audio_sink, GST_STATE_NULL) == GST_STATE_CHANGE_FAILURE) {
		GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Speed Change - Set State to NULL Failed\n");
	}

	GST_DEBUG_OBJECT(playersinkbin,"Ref count Sink=%d", GST_OBJECT_REFCOUNT_VALUE(GST_OBJECT(playersinkbin->audio_sink)));

	gst_bin_remove(GST_BIN(playersinkbin), playersinkbin->audio_sink);
        gst_element_unlink(playersinkbin->m_resample,playersinkbin->audio_sink);
        gst_element_sync_state_with_parent(GST_ELEMENT (playersinkbin));
	playersinkbin->audio_sink = NULL;
}
#endif


/* SOC CHANGES: Dynamic call back from demux element to link audio and video source pads to downstream decode elements
 * For video, identify the video encoder type(h264/mpeg) and link appropriate decoder element if soc has differenti
 * elements for h264 and mpeg.
 * For audio, link the audio pad containing the preferred language if multiple audio language streams are present.
 */
static void
plug_pad (GstPlayerSinkBin * playersinkbin, GstPad * demux_src_pad)
{
	GstCaps* caps;
	const gchar *padname;
	gchar *pad1;
         unsigned char index = -1;
#ifdef USE_GST1
	caps = gst_pad_get_current_caps(demux_src_pad);
#else
	caps =  gst_pad_get_caps(demux_src_pad);
#endif
	padname = gst_structure_get_name(gst_caps_get_structure (caps, 0));
	pad1 = gst_pad_get_name (demux_src_pad);
          
	GST_DEBUG_OBJECT(playersinkbin,"playersinkbin : plugging: padname: %s, pad1: %s\n",padname,pad1);
	g_free (pad1);
	if(g_strrstr(padname,"video"))
	{
                index = getVideoParserIndex(padname); 
               if(index == -1 || index > MAX_VPARSER) {
                        GST_ERROR_OBJECT(playersinkbin, "%s parser not supported in playersinkbin", padname);
                        return;
                } 
                playersinkbin->video_parser = gst_element_factory_make(V_PARSER[index], "video-parser");
                if(playersinkbin->video_parser==NULL)
		{
			GST_ERROR_OBJECT(playersinkbin,"Cannot get %s parser from factory\n", padname);
			return;
		}
               /******Creating the video decoding elements******/
                playersinkbin->video_sink = gst_element_factory_make(VSINK, "player_vsink");
                playersinkbin->m_vqueue = gst_element_factory_make("queue", "vqueue");
                if(playersinkbin->video_sink==NULL)
                {
                        GST_ERROR_OBJECT(playersinkbin,"Cannot get %s sink from factory\n", padname);
                        return;
                }

        if (!playersinkbin->m_vqueue || !playersinkbin->video_sink)
        {
            if (!playersinkbin->video_sink) GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to instantiate video sink (%s)\n", VSINK);
            if (!playersinkbin->m_vqueue) GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to instantiate vqueue \n");
                return;
        }
               /*********linking the video elements********/
                gst_bin_add_many(GST_BIN(playersinkbin), playersinkbin->demux,playersinkbin->m_vqueue,NULL);

                gst_bin_add_many(GST_BIN(playersinkbin),playersinkbin->video_parser,playersinkbin->video_sink,NULL);	


		if(!gst_element_link(playersinkbin->video_parser, playersinkbin->video_sink))
		{
			GST_ERROR_OBJECT(playersinkbin,"Failed to link video parser & sink\n");
			return;
		}

                if(!gst_element_link(playersinkbin->m_vqueue,playersinkbin->video_parser))
                {
                        GST_ERROR_OBJECT(playersinkbin,"Failed to link video parse\n");
                        return;
                }

		GstPad* sink_pad = gst_element_get_static_pad(playersinkbin->m_vqueue, "sink");
		if (!GST_PAD_LINK_SUCCESSFUL(gst_pad_link(demux_src_pad, sink_pad)))
		{
			GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to connect video pad");
			gst_caps_unref (caps);
			return;
		}
		else
		{
			// Send the VideoCallback event
			g_signal_emit (G_OBJECT (playersinkbin), gst_player_sinkbin_signals[SIGNAL_PLAYERSINKBIN], 0,
			               GSTPLAYERSINKBIN_EVENT_HAVE_VIDEO);
		}
                gst_element_sync_state_with_parent(playersinkbin->m_vqueue);
		gst_element_sync_state_with_parent(playersinkbin->video_parser);
		gst_element_sync_state_with_parent(playersinkbin->video_sink);
                gst_element_sync_state_with_parent(playersinkbin->demux);
                gst_object_unref(sink_pad);
                if(avstatus)
		{
			FILE* fp= fopen(AV_STATUS,"a");
	                fwrite(" Video:SUCCESS",1,13,fp);
			fwrite("\n",1,1,fp);
        	        fclose(fp);
		}
		else
		{
			FILE* fp= fopen(AV_STATUS,"w");
			fwrite("Video:SUCCESS",1,13,fp);
			fwrite("\n",1,1,fp);
			avstatus=1;
			fclose(fp);
		}

	}

	else if (g_strrstr(padname,"audio"))
	{
	if(Audio_Enable){
          if(!audio_flag) {

		if(g_strrstr(padname,"ac3"))
		{
			playersinkbin->m_audio_parser = gst_element_factory_make("ac3parse", NULL);

			if(OMX_Enable){
				playersinkbin->m_adec = gst_element_factory_make("avdec_ac3", "adec");
			}
			else{

				playersinkbin->m_adec = gst_element_factory_make(AC3_DEC, "adec");
			}
	        }
	        else if(g_strrstr(padname, "mpeg")) 
		{
                   const GValue* value = gst_structure_get_value(gst_caps_get_structure (caps, 0), "mpegversion");
                   GST_INFO("mpeg version %d", g_value_get_int(value));
                   switch ( g_value_get_int ( value ) ) 
		   {
                   case 1:
                         printf("mp3 playback=============\n");
                        playersinkbin->m_audio_parser = gst_element_factory_make("mpegaudioparse", NULL);

                        if(OMX_Enable)
                        {
                                playersinkbin->m_adec = gst_element_factory_make("mpg123audiodec", "adec");
                        }
                        else {
                                playersinkbin->m_adec = gst_element_factory_make(MP3_DEC, "adec");
                        }
                        break;
                   default:
                        printf("aac playback...........\n");
                        playersinkbin->m_audio_parser = gst_element_factory_make("aacparse", NULL);

                        if(OMX_Enable){
                                playersinkbin->m_adec = gst_element_factory_make("faad", "adec");
                        }
                        else{

                                playersinkbin->m_adec = gst_element_factory_make(AC3_DEC, "adec");
                        }
                        break;

                   }
                }
                else
            	{
 		GST_DEBUG_OBJECT(playersinkbin,"playersinkbin unsupported audio type\n");	
		}
		if(!playersinkbin->m_adec)
		{
			GST_ERROR_OBJECT(playersinkbin,"Audio decoder is failed...........\n");
		}
            /* Create audio decoding pipeline factories */
        playersinkbin->audio_sink = gst_element_factory_make(ASINK, "player_asink");
        playersinkbin->m_aqueue = gst_element_factory_make("queue", "aqueue");
        playersinkbin->m_aconvert = gst_element_factory_make("audioconvert", "convert");
        playersinkbin->m_resample = gst_element_factory_make("audioresample", "resample");
        if (!playersinkbin->audio_sink || !playersinkbin->m_aqueue || !playersinkbin->m_aconvert || !playersinkbin->m_resample)
        {
                if (!playersinkbin->audio_sink) GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to instantiate audio decoder (%s)\n", ASINK);
                if (!playersinkbin->m_aqueue) GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to instantiate audio m_aqueue \n");
            if (!playersinkbin->m_aconvert) GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to instantiate audio m_aconvert \n");
                if (!playersinkbin->m_resample) GST_ERROR_OBJECT(playersinkbin,"playersinkbin : Failed to instantiate audio m_resample \n");
                return;
        }

        if (Audio_Enable) {
        gst_bin_add_many(GST_BIN(playersinkbin), playersinkbin->m_aqueue,playersinkbin->m_aconvert,playersinkbin->m_resample,playersinkbin->audio_sink,NULL);
}

       	gst_bin_add(GST_BIN(playersinkbin), playersinkbin->m_audio_parser);
	gst_bin_add(GST_BIN(playersinkbin), playersinkbin->m_adec);
        gst_util_set_object_arg(G_OBJECT(playersinkbin->audio_sink), "sync", "false");

           /******** Linking audio elements**********/
	if(!gst_element_link(playersinkbin->m_aqueue, playersinkbin->m_audio_parser))
                 {
                        GST_ERROR_OBJECT(playersinkbin,"Failed to link m_aqueue to audio_parser\n");
                        return;
                }
		if(!gst_element_link(playersinkbin->m_audio_parser, playersinkbin->m_adec))
                {
                        GST_ERROR_OBJECT(playersinkbin,"Failed to link m_audio_parser to m_adec\n");
                        return;
                }
		if(!gst_element_link(playersinkbin->m_adec,playersinkbin->m_aconvert))
                {
                        GST_ERROR_OBJECT(playersinkbin,"Failed to link m_adec to m_aconvert\n");
                        return;
                }
                if(!gst_element_link(playersinkbin->m_aconvert,playersinkbin->m_resample))
                {
                     GST_ERROR_OBJECT(playersinkbin,"Failed to link m_aconvert to m_resample\n");
                        return;
                }
                 if(!gst_element_link(playersinkbin->m_resample,playersinkbin->audio_sink))
                {
                       GST_ERROR_OBJECT(playersinkbin,"Failed to link m_resample to audio_sink\n");
                        return;
                }
		 GstPad* sink_pad = gst_element_get_static_pad(playersinkbin->m_aqueue, "sink");
		if (!GST_PAD_LINK_SUCCESSFUL(gst_pad_link(demux_src_pad, sink_pad)))
			GST_ERROR_OBJECT(playersinkbin, "Failed to connect audio pad");

	 gst_element_sync_state_with_parent(playersinkbin->m_aqueue);
	 gst_element_sync_state_with_parent(playersinkbin->m_audio_parser);
	 gst_element_sync_state_with_parent(playersinkbin->m_adec);
         gst_element_sync_state_with_parent(playersinkbin->m_aconvert);
         gst_element_sync_state_with_parent(playersinkbin->m_resample);
	 gst_element_sync_state_with_parent(playersinkbin->audio_sink);	 
         gst_object_unref(sink_pad);
  		audio_flag=1;
		if(avstatus)
		{
			FILE* fp= fopen(AV_STATUS,"a");
			fwrite("Audio:SUCCESS",1,13,fp);
			fclose(fp);
		}
		else
		{
			FILE* fp= fopen(AV_STATUS,"w");
			fwrite("Audio:SUCCESS",1,13,fp);
			avstatus=1;
			fclose(fp);
		}
         }
        }
	}

	gst_caps_unref (caps);
}

#ifdef USE_GST1
GstPadProbeReturn demuxSrcPadProbe (GstPad * demux_src_pad, GstPadProbeInfo *info, gpointer data)
{
	GstPlayerSinkBin *playersinkbin = GST_PLAYER_SINKBIN (data);
	gboolean plug = FALSE;

	/* We pass everything, but first plug all pads except UNSELECT ones */
	if (GST_IS_EVENT (info->data)) {
		GstEvent *event = GST_EVENT (info->data);
		GST_DEBUG_OBJECT (demux_src_pad, "Probe got an event: %s", GST_EVENT_TYPE_NAME (event));
		if (GST_EVENT_TYPE (event) == GST_EVENT_SEGMENT) {
			GstStreamFlags flags;
			plug = TRUE;
		}
	}

	if (!plug)
		return GST_PAD_PROBE_OK;
	plug_pad (playersinkbin, demux_src_pad);

	return GST_PAD_PROBE_REMOVE;
}
#endif

void onDemuxSrcPadAdded(GstElement* element, GstPad *demux_src_pad, gpointer data)
{
	GstPlayerSinkBin *playersinkbin = GST_PLAYER_SINKBIN (data);

	gchar *padname = gst_pad_get_name(demux_src_pad);

#ifdef ENABLE_SUBTEC
	if (g_strrstr( padname, "subpicture"))
	{
		// create SUBTEC filter:
		GstElement *subtecfilter = gst_element_factory_make("subtecfilter", "player_subtecfilter");
		if (subtecfilter == NULL)
		{
			g_print("[%s :: %s] ERROR while creating SUBTECFILTER\n", __FILE__, __FUNCTION__);
			goto done;
		}

		// create SUBTEC sink:
		GstElement *subtec_sink = gst_element_factory_make("subtecsink", "player_subtec_sink");
		if (!subtec_sink) {
			gst_object_unref(subtecfilter);
			subtecfilter = NULL;
			g_print("[%s] ERROR while creating sink element for subtecfilter\n", __FUNCTION__);
			goto done;
		}
		gst_element_set_locked_state(subtec_sink, TRUE);
		gst_element_set_locked_state(subtecfilter, TRUE);

		// add SUBTEC sink to pipeline:
		gst_bin_add_many(GST_BIN(playersinkbin), subtecfilter, subtec_sink, NULL);
		g_object_set(subtec_sink, "location", "/var/run/subttx/pes_data_main", NULL);
		if (!gst_element_link(subtecfilter, subtec_sink)) {
			g_print("[%s] Could not link SUBTECFILTER with SUBTECSINK\n", __FUNCTION__);
			goto done;
		}
		if (!gst_element_link_pads(playersinkbin->demux, padname, subtecfilter, NULL)) {
			g_print("[%s] Could not link DEMUX with SUBTECFILTER\n", __FUNCTION__);
			goto done;
		}

		if (playersinkbin->video_decoder) {
			if (!gst_element_link_pads(playersinkbin->video_decoder, "user-data-pad", subtecfilter, "sink-user-data")) {
				g_print("[%s] Could not link VDEC with SUBTECFILTER\n", __FUNCTION__);
			}
		}

		gst_element_set_state(subtecfilter, GST_STATE_PLAYING);
		gst_element_set_state(subtec_sink, GST_STATE_PLAYING);
		gst_element_set_locked_state(subtec_sink, FALSE);
		gst_element_set_locked_state(subtecfilter, FALSE);

		gst_element_sync_state_with_parent(subtec_sink);
		gst_element_sync_state_with_parent(subtecfilter);
		goto done;
	}
#endif

#ifdef USE_GST1
	gst_pad_add_probe (demux_src_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
		demuxSrcPadProbe, playersinkbin, NULL);
#else
	plug_pad (playersinkbin, demux_src_pad);
#endif
done:
	g_free(padname);
}


/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_player_sinkbin_init (GstPlayerSinkBin* sinkbin)
{
	GstPad *pad;
	GstPad *gpad;
	GstPadTemplate *pad_tmpl;

	/* Initialize properties */
	audio_flag = 0;
	sinkbin->prog_no = DEFAULT_PROGRAM_NUM;
	strncpy(sinkbin->available_languages,DEFAULT_AVAILABLE_LANGUAGES,sizeof(DEFAULT_AVAILABLE_LANGUAGES));
	strncpy(sinkbin->preffered_language,DEFAULT_PREFERRED_LANGUAGE,sizeof(DEFAULT_PREFERRED_LANGUAGE));
	sinkbin->show_last_frame = DEFAULT_SHOW_LAST_FRAME;
	strncpy(sinkbin->video_rectangle,DEFAULT_VIDEO_RECTANGLE,sizeof(DEFAULT_VIDEO_RECTANGLE));
	sinkbin->plane = DEFAULT_PLANE;
	sinkbin->video_mute = DEFAULT_VIDEO_MUTE;
	sinkbin->play_speed = DEFAULT_PLAY_SPEED;
	sinkbin->current_position = DEFAULT_CURRENT_POSITION;
	sinkbin->resolution = DEFAULT_RESOLUTION;
	sinkbin->audio_mute = DEFAULT_AUDIO_MUTE;
	sinkbin->volume = DEFAULT_VOLUME;

        printf("gst_player_sinkbin_init===\n");
        {
 	   FILE*fp=NULL;
	   char data[50];
               fp= fopen(MEDIA_CONF,"r");
              if(fp)
 	      {
		fgets(data,30,fp);
                if(strstr(data,"OMX"))
                {
		   OMX_Enable=1;
		}
                if(strstr(data,"Audio_Disable"))
		{
			Audio_Enable=0;
		}
	      }
	}

	sinkbin->demux = gst_element_factory_make(DEMUX, "player_demux");

	if (!sinkbin->demux)
	{
		GST_ERROR_OBJECT(sinkbin,"playersinkbin : Failed to instantiate demuxer (%s)", DEMUX);
		return;
	}
	g_signal_connect(sinkbin->demux, "pad-added", G_CALLBACK (onDemuxSrcPadAdded), sinkbin);


	/* Install Resolution Change Signal callback */
	GST_INFO_OBJECT(sinkbin,"playersinkbin::Installing signal callback for resolution change on video decoder\n");

        gst_bin_add(GST_BIN(sinkbin), sinkbin->demux);
	/* get the sinkpad of demux and ghost it */
        pad = gst_element_get_static_pad (sinkbin->demux, "sink");

	/* get the pad template */
	pad_tmpl = gst_static_pad_template_get (&playersink_bin_sink_template);

	/* ghost the sink pad to ourself */
	gpad = gst_ghost_pad_new_from_template ("sink", pad, pad_tmpl);
	gst_pad_set_active (gpad, TRUE);
	gst_element_add_pad (GST_ELEMENT (sinkbin), gpad);

	gst_object_unref (pad_tmpl);
    	gst_object_unref(GST_OBJECT(pad));

}

static void gst_decode_bin_dispose (GObject * object)
{
	GstPlayerSinkBin* playersinkbin;

	playersinkbin=GST_PLAYER_SINKBIN(object);

	if (playersinkbin->caps)
		gst_caps_unref (playersinkbin->caps);

	G_OBJECT_CLASS (parent_class)->dispose (object);
	system("cat /dev/null > /opt/gstcaps.txt");
                remove(AV_STATUS);
               FILE* fp= fopen(AV_STATUS,"w");
                fwrite("Video:STOPPED",1,13,fp);
		fwrite("\n",1,1,fp);
		fwrite("Audio:STOPPED",1,13,fp );
                fclose(fp);
		avstatus=0;
}

static void gst_player_sinkbin_get_pmtinfo(GstPlayerSinkBin* playersinkbin,GValue * value)
{
#ifndef USE_HW_DEMUX
#endif
}


/**
* Map the plane to ISMD plane
*playersinkbin has the following values which are maped to ismd_vidsink value
*                           (1): Pixel Plane 1    - Plane 1
*                           (2): Pixel Plane 2    - Plane 2
*                           (3): Pixel Plane 3    - Plane 3
*                           (4): Pixel Plane 4    - Plane 4
*ismd_vidsink has the following values
*                           (5): UPPA             - Universal Pixel Plane A
*                           (6): UPPB             - Universal Pixel Plane B
*                           (7): UPPC             - Universal Pixel Plane C
*                           (8): UPPD             - Universal Pixel Plane D
**/
static gint mapToISMDplane(gint playersinkbin_plane)
{
	gint plane = VIDEO_PLANE;
	switch(playersinkbin_plane)
	{
	case PLANE_1:
		plane = UPPA;
		break;
	case PLANE_2:
		plane = UPPB;
		break;
	case PLANE_3:
		plane = UPPC;
		break;
	case PLANE_4:
		plane = UPPD;
		break;
	}
	return plane;
}

/**
* Map the plane From ISMD plane
*playersinkbin has the following values which are maped from ismd_vidsink value
*                           (1): Pixel Plane 1    - Plane 1
*                           (2): Pixel Plane 2    - Plane 2
*                           (3): Pixel Plane 3    - Plane 3
*                           (4): Pixel Plane 4    - Plane 4
*ismd_vidsink has the following values
*                           (5): UPPA             - Universal Pixel Plane A
*                           (6): UPPB             - Universal Pixel Plane B
*                           (7): UPPC             - Universal Pixel Plane C
*                           (8): UPPD             - Universal Pixel Plane D
**/
static gint mapFromISMDplane(gint playersinkbin_plane)
{
	gint plane = DEFAULT_PLANE;
	switch(playersinkbin_plane)
	{
	case UPPA:
		plane = PLANE_1;
		break;
	case UPPB:
		plane = PLANE_2;
		break;
	case UPPC:
		plane = PLANE_3;
		break;
	case UPPD:
		plane = PLANE_4;
		break;
	}
	return plane;
}

gint video_decoder_handle=-1;
static void
gst_player_sinkbin_get_property (GObject * object, guint prop_id,
                                 GValue * value, GParamSpec * pspec)
{
	GstPlayerSinkBin *playersinkbin = GST_PLAYER_SINKBIN (object);

	/* SOC CHANGES: Get appropriate properties from soc gstreamer elements */
	switch (prop_id) {
	case PROP_PROGRAM_NUM:
		/* Get the current program number from demux element
		 */
		//g_object_get(playersinkbin->demux,"program-number",&(playersinkbin->prog_no),NULL);
		g_value_set_int(value,playersinkbin->prog_no);
		break;
	case PROP_PMT_INFO:
		/* Get the pmt information from demux element. Assumes the pmt information from the soc element
		 * is of the form defined in playersinkbinpmtinfo.h.Change appropriately if not so.
		 */
		gst_player_sinkbin_get_pmtinfo(playersinkbin,value);
		break;
	case PROP_AVAILABLE_LANGUAGES:
		/* Get the available languages filled after demux src pad callback
		 */
		g_value_set_string(value,playersinkbin->available_languages);
		break;
	case PROP_PREFERRED_LANGUAGE:
		/* Send back the user set preffered language.No SOC specific change required here. Use this value on demux src pad callback to link appropriate language pid
		*/
		g_value_set_string(value,playersinkbin->preffered_language);
		break;
	case PROP_VIDEO_DECODE_HANDLE:
		/* Get current decode handle from soc video decoder element
		*/
	{
		g_object_get(playersinkbin->video_decoder,"decode-handle",&video_decoder_handle,NULL);
		GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_get_property::video_decoder_handle %d\n",video_decoder_handle);
		playersinkbin->video_decode_handle = (gpointer) &video_decoder_handle;
		g_value_set_pointer(value, playersinkbin->video_decode_handle);
	}
	break;
	case PROP_SHOW_LAST_FRAME:
		/* Get current value of show last frame from soc gstreamer element
		*/
		break;
	case PROP_VIDEO_MUTE:
		/* Get current value of video mute from soc gstreamer element
		*/
		g_object_get(playersinkbin->video_sink,"mute",&(playersinkbin->video_mute),NULL);
		g_value_set_boolean( value,playersinkbin->video_mute);
		break;
	case PROP_VIDEO_RECTANGLE:
		/* Get current value of the rectange from soc gstreamer element
		 */
		g_object_get(playersinkbin->video_sink,"rectangle",&(playersinkbin->video_rectangle),NULL);
		g_value_set_string(value,playersinkbin->video_rectangle);
		break;
	case PROP_CC_DESC:
		g_value_set_string(value,"0");
		break;
	case PROP_PLANE:
	{
		gint gdl_plane = VIDEO_PLANE;
		g_object_get(playersinkbin->video_sink,"gdl-plane",&(playersinkbin->plane),NULL);
		/* Get current plane used from soc element
		 * Map the soc plane value to generic property of this bin if types are different
		*/
		gdl_plane = mapFromISMDplane(playersinkbin->plane);
		g_value_set_enum(value, gdl_plane);
	}
	break;
	case PROP_PLAY_SPEED:
		/* Get play speed from the soc specific gstreamer element if available. May not be needed at SOC level.
		*/
		g_value_set_float(value,playersinkbin->play_speed);
		break;
	case PROP_CURRENT_POSITION:
		/* Get appropriate value from soc gstreamer element
		 * Map,if types/units are different,before setting on the generic property of this bin
		 * playersinkbin->current_position = currentpts/90000L;
		*/
	{
		unsigned long currentPTS = 0;
		g_object_get(playersinkbin->video_sink, "currentPTS", &currentPTS,     NULL);
		playersinkbin->current_position = currentPTS/90000L;
		g_value_set_double(value,playersinkbin->current_position);
		break;
	}
	case PROP_RESOLUTION:
		/* Get appropriate value from soc gstreamer element
		 * Format,if necessary,before setting on the generic property of the bin
		 * playersinkbin->resolution = resolution;
		*/
		// TODO: Get tvmode enum values and map
		g_value_set_enum(value,playersinkbin->resolution);
		break;
	case PROP_AUDIO_MUTE:
		/* Get current value of audio mute from soc gstreamer element
		*/
		g_object_get(playersinkbin->audio_sink,"mute",&(playersinkbin->audio_mute),NULL);
		g_value_set_boolean( value,playersinkbin->audio_mute);
		break;
	case PROP_VOLUME:
		/* Get current value of volume from soc gstreamer element
		 * Example: g_object_get(playersinkbin->audio_sink,"volume",&(playersinkbin->volume),NULL);
		*/
		g_object_get(playersinkbin->audio_sink,"volume",&(playersinkbin->volume),NULL);
		g_value_set_double( value,playersinkbin->volume);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
	/* END SOC CHANGES */
}
static  char * getResolution( const char *filename, unsigned int *pFileSize )
{
        int rc;
        int lengthDidRead;
        char *pData = NULL;

        FILE* pFile= fopen( filename, "r" );
        if ( pFile )
        {
                rc = fseek( pFile, 0, SEEK_END );
                if ( rc )
                {
                        goto exit;
                }

                *pFileSize = ftell( pFile );
                if ( -1 == *pFileSize )
                {
                        goto exit;
                }

                rc = fseek( pFile, 0, SEEK_SET );
                if ( rc )
                {
                        goto exit;
                }
                pData = (char*)malloc( (*pFileSize +1) * sizeof(char) );
                if ( !pData )
                {
                        goto exit;
                }
                lengthDidRead = fread( pData, sizeof(char), *pFileSize, pFile );

   		 if ( lengthDidRead != *pFileSize )
                {
                        free( pData );
                        goto exit;
                }
                pData[*pFileSize]= '\0';
        }
        else
        {
                return NULL;
        }
exit:
  fclose( pFile );

    return pData;
}
static void
gst_player_sinkbin_set_property (GObject * object, guint prop_id,
                                 const GValue * value, GParamSpec * pspec)
{
         char *data[4];
	GstPlayerSinkBin *playersinkbin = GST_PLAYER_SINKBIN (object);
	/* SOC CHANGES: Set appropriate properties on soc gstreamer elements*/
	switch (prop_id) {
	case PROP_PROGRAM_NUM:
		playersinkbin->prog_no = g_value_get_int(value);
		/*
		* Set the program number on soc demux gstreamer element
		 */
		g_object_set(playersinkbin->demux, "program-number", playersinkbin->prog_no, NULL);
		break;
	case PROP_PREFERRED_LANGUAGE:
	{
		int i = 0;
		if(strcmp(playersinkbin->preffered_language,g_value_get_string (value))) /* Change preffered language only if different */
		{
			memset(playersinkbin->preffered_language, '\0', sizeof(playersinkbin->preffered_language));
			/* Use this setting to select the preferred audio language while dynamically linking source
			* pads from demux,no SOC specific change required here */
			strncpy(playersinkbin->preffered_language, g_value_get_string (value),sizeof(playersinkbin->preffered_language));
			playersinkbin->preffered_language[sizeof(playersinkbin->preffered_language)-1] = 0;
			/*Get the audio pid corresponding to preferred language and link it*/
			GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::playersinkbin->num_streams is %d\n",playersinkbin->num_streams);
			for (i = 0 ; i < playersinkbin->num_streams; i++)
			{
				GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::playersinkbin->preffered_language is %s playersinkbin->m_esinfo[%d].lang is %s\n",playersinkbin->preffered_language,i,playersinkbin->m_esinfo[i].lang);
				if(!strcmp(playersinkbin->preffered_language,playersinkbin->m_esinfo[i].lang))
				{
					playersinkbin->preffered_audio_pid = playersinkbin->m_esinfo[i].es_pid;
					sprintf(playersinkbin->prefferd_pid_instring,"%04x",playersinkbin->preffered_audio_pid);
					GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::playersinkbin->prefferd_pid_instring is %s\n",playersinkbin->prefferd_pid_instring);
					break;
				}
			}
			GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::unlink pad %s preferred pid to set %s playersinkbin->num_audio_streams %d\n",playersinkbin->linkedaudiopadname,playersinkbin->prefferd_pid_instring,playersinkbin->num_audio_streams);
			if(playersinkbin->num_audio_streams > 1) /* Change pads only if more than one audio */
			{
				GstPad *linkedpad = gst_element_get_static_pad(playersinkbin->demux,playersinkbin->linkedaudiopadname);
				GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::linked pad is %x\n", (unsigned int)linkedpad);
				GstPad *sink_pad = gst_element_get_static_pad(playersinkbin->audio_sink, "sink");
				if(gst_pad_unlink(linkedpad,sink_pad))
				{
					GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::Pad Unlink successful\n");
					gchar padnametolink[16] = "audio_";
					strcat(padnametolink,playersinkbin->prefferd_pid_instring);
					GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::Pad name to link is %s\n",padnametolink);
					GstPad *pad_to_link = gst_element_get_static_pad(playersinkbin->demux,padnametolink);
					if (!GST_PAD_LINK_SUCCESSFUL(gst_pad_link(pad_to_link, sink_pad)))
					{
						GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::pad link not successful\n");
					}
					else
					{
						GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::linked new audio pad successfully\n");
						strcpy(playersinkbin->linkedaudiopadname,padnametolink);
					}
				}
				else
				{
					GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::Error in unlinking pad\n");
				}
			}
		}
	}
	break;
	case PROP_SHOW_LAST_FRAME:
		/* This property keeps the last frame on screen while stop rather than a black frame.
		* Set the appropriate property on soc gstreamer element.
		 */
		break;
	case PROP_VIDEO_MUTE:
		playersinkbin->video_mute = g_value_get_boolean(value);
		/* This property sets the video to mute
		* Set the appropriate property on soc gstreamer element.
		 */
		g_object_set(playersinkbin->video_sink, "mute",playersinkbin->video_mute, NULL);
		break;
	case PROP_VIDEO_RECTANGLE:
		memset(playersinkbin->video_rectangle, '\0', sizeof(playersinkbin->video_rectangle));
		strncpy(playersinkbin->video_rectangle, g_value_get_string (value),sizeof(playersinkbin->video_rectangle));
		playersinkbin->video_rectangle[sizeof(playersinkbin->video_rectangle)-1] = 0;
                g_object_set(playersinkbin->video_sink,"rectangle", playersinkbin->video_rectangle, NULL);
		printf("setting rectangle = %s \n",playersinkbin->video_rectangle);
		break;
	case PROP_PLANE:
	{
		gint gdl_plane = VIDEO_PLANE;
		playersinkbin->plane = g_value_get_enum(value);
		/* Map plane value to soc specific value and set on appropriate gstreamer element
		*/
		gdl_plane = mapToISMDplane(playersinkbin->plane);
		//g_object_set(playersinkbin->video_sink,"gdl-plane", gdl_plane,NULL);
	}
	break;
	case PROP_IS_LIVE:
		/*Currently is-live property is installed to remove warning messages
		*/
		printf("Setting is-live prop is not supported\n");
	break;
	case PROP_PLAY_SPEED:
		playersinkbin->play_speed = g_value_get_float(value);
		GST_DEBUG_OBJECT(playersinkbin,"gst_player_sinkbin_set_property::setting play_speed %f\n",playersinkbin->play_speed);

#if defined(ENABLE_AUDIO_REMOVAL_FOR_TRICK_MODES)
		if ((playersinkbin->play_speed == DEFAULT_PLAY_SPEED) && (playersinkbin->audio_sink == NULL)) {
			// Link pads after bringing the state of the downstream elements to the state of the bin
			createLinkAudioChain(playersinkbin);

		}
		else if ((playersinkbin->play_speed != DEFAULT_PLAY_SPEED) && (playersinkbin->audio_sink != NULL)) {
			deleteUnlinkAudioChain(playersinkbin);
		}
#endif

		break;
	case PROP_RESOLUTION:
		playersinkbin->resolution = g_value_get_enum(value);
		/* Map resolution value to soc specific value and set on appropriate gstreamer element
		 * Example:g_object_set(playersinkbin->video_sink,"resolution",playersinkbin->resolution,NULL);
		*/
		// TODO: map to tv mode enums and set resolution
		break;
	case PROP_AUDIO_MUTE:
#if defined(ENABLE_AUDIO_REMOVAL_FOR_TRICK_MODES)
		if ((playersinkbin->play_speed == DEFAULT_PLAY_SPEED) && (playersinkbin->audio_sink != NULL)) {
			playersinkbin->audio_mute = g_value_get_boolean(value);
			g_object_set(playersinkbin->audio_sink, "mute",playersinkbin->audio_mute, NULL);
		}
#else
		playersinkbin->audio_mute = g_value_get_boolean(value);
		/* This property sets the audio to mute
		* Set the appropriate property on soc gstreamer element.
		 */
		g_object_set(playersinkbin->audio_sink, "mute",playersinkbin->audio_mute, NULL);
#endif
		break;
	case PROP_VOLUME:
		playersinkbin->volume = g_value_get_double(value);
		/* This property sets the audio volume level to requested volume
		* Set the appropriate property on soc gstreamer element.
		 */
		g_object_set(playersinkbin->audio_sink, "volume",playersinkbin->volume, NULL);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
	/* END SOC CHANGES */
}

GType
gst_player_sinkbin_get_type (void)
{
	static GType gst_player_sinkbin_type = 0;

	if (!gst_player_sinkbin_type) {
		static const GTypeInfo gst_player_sinkbin_info = {
			sizeof (GstPlayerSinkBinClass),
			NULL,
			NULL,
			(GClassInitFunc) gst_player_sinkbin_class_init,
			NULL,
			NULL,
			sizeof (GstPlayerSinkBin),
			0,
			(GInstanceInitFunc) gst_player_sinkbin_init,
			NULL
		};

		gst_player_sinkbin_type =
		    g_type_register_static (GST_TYPE_BIN, "GstPlayerSinkBin",
		                            &gst_player_sinkbin_info, 0);
	}

	return gst_player_sinkbin_type;
}

/* entry point to initialize the plug-in
 */
static gboolean
playersinkbin_init (GstPlugin * plugin)
{
	/* debug category for fltering log messages
	 *
	 */
	GST_DEBUG_CATEGORY_INIT (gst_player_sinkbin_debug, "playersinkbin",
	                         0, "playersinkbin");

	return gst_element_register (plugin, "playersinkbin", GST_RANK_NONE,
	                             GST_TYPE_PLAYER_SINKBIN);
}


/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "playersinkbin"
#endif

/* gstreamer looks for this structure to register plugins
 *
 */
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
#ifdef USE_GST1
    playersinkbin,
#else
    "playersinkbin",
#endif
    "Demux and Decode Transport stream",
    playersinkbin_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
