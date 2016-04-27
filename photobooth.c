/*
 * photobooth.c
 * Copyright 2016 Andreas Frisch <fraxinas@opendreambox.org>
 *
 * This program is licensed under the Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 Unported
 * License. To view a copy of this license, visit
 * http://creativecommons.org/licenses/by-nc-sa/3.0/ or send a letter to
 * Creative Commons,559 Nathan Abbott Way,Stanford,California 94305,USA.
 *
 * This program is NOT free software. It is open source, you are allowed
 * to modify it (if you keep the license), but it may not be commercially
 * distributed other than under the conditions noted above.
 */

// gcc -Wall -g `pkg-config gstreamer-1.0 gstreamer-video-1.0 libgphoto2 gtk+-3.0 gtk+-x11-3.0 --cflags --libs` photobooth.c focus.c -o photobooth

#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <gst/video/videooverlay.h>
#include <gst/video/gstvideosink.h>
#include "photobooth.h"
#include "photoboothwin.h"

#define photo_booth_parent_class parent_class

typedef struct _PhotoBoothPrivate PhotoBoothPrivate;

struct _PhotoBoothPrivate
{
	PhotoBoothWindow *win;
	GSettings *settings;
	guint countdown;
	GstElement *audio_playbin;
	GstVideoRectangle video_size;
};

#define DEFAULT_AUDIOFILE_COUNTDOWN "/net/home/fraxinas/microcontroller/photobooth/beep.m4a"
#define DEFAULT_COUNTDOWN 5
#define PRINT_WIDTH 2076
#define PRINT_HEIGHT 1384
#define PREVIEW_WIDTH 640
#define PREVIEW_HEIGHT 424
#define MOVIEPIPE "moviepipe.mjpg"

enum
{
	ARG_0,
	ARG_COUNTDOWN,
};

G_DEFINE_TYPE_WITH_PRIVATE (PhotoBooth, photo_booth, GTK_TYPE_APPLICATION);

GST_DEBUG_CATEGORY_STATIC (photo_booth_debug);
#define GST_CAT_DEFAULT photo_booth_debug

/* GObject / GApplication */
static void photo_booth_activate (GApplication *app);
static void photo_booth_open (GApplication  *app, GFile **files, gint n_files, const gchar *hint);
static void photo_booth_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void photo_booth_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void photo_booth_dispose (GObject *object);
static void photo_booth_finalize (GObject *object);
PhotoBooth *photo_booth_new (void);
void photo_booth_background_clicked (GtkWidget *widget, GdkEventButton *event, PhotoBoothWindow *win);
void photo_booth_button_yes_clicked (GtkButton *button, PhotoBoothWindow *win);

/* general private functions */
static void photo_booth_quit_signal (PhotoBooth *pb);
static void photo_booth_window_destroyed_signal (PhotoBoothWindow *win, PhotoBooth *pb);
static void photo_booth_setup_window (PhotoBooth *pb);
static void photo_booth_video_widget_ready (PhotoBooth *pb);
static gboolean photo_booth_preview (PhotoBooth *pb);
static void photo_booth_snapshot_start (PhotoBooth *pb);
static gboolean photo_booth_snapshot_prepare (PhotoBooth *pb);
static gboolean photo_booth_snapshot_taken (PhotoBooth *pb);
static void photo_booth_print (PhotoBooth *pb);

/* libgphoto2 */
static gboolean photo_booth_cam_init (CameraInfo **cam_info);
static gboolean photo_booth_cam_close (CameraInfo **cam_info);
static gboolean photo_booth_take_photo (CameraInfo *cam_info);
static void photo_booth_flush_pipe (int fd);
static void photo_booth_capture_thread_func (PhotoBooth *pb);
static void _gphoto_err(GPLogLevel level, const char *domain, const char *str, void *data);

/* gstreamer functions */
static GstElement *build_video_bin (PhotoBooth *pb);
static GstElement *build_photo_bin (PhotoBooth *pb);
static gboolean photo_booth_setup_gstreamer (PhotoBooth *pb);
static gboolean photo_booth_bus_callback (GstBus *bus, GstMessage *message, PhotoBooth *pb);
static GstPadProbeReturn photo_booth_catch_photo_buffer (GstPad * pad, GstPadProbeInfo * info, gpointer user_data);

static void photo_booth_class_init (PhotoBoothClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GApplicationClass *gapplication_class = G_APPLICATION_CLASS (klass);

	GST_DEBUG_CATEGORY_INIT (photo_booth_debug, "photobooth", GST_DEBUG_BOLD | GST_DEBUG_FG_YELLOW | GST_DEBUG_BG_BLUE, "PhotoBooth");
	GST_DEBUG ("photo_booth_class_init");
	gp_log_add_func(GP_LOG_ERROR, _gphoto_err, NULL);

	gobject_class->finalize = photo_booth_finalize;
	gobject_class->set_property = photo_booth_set_property;
	gobject_class->get_property = photo_booth_get_property;
	gapplication_class->activate = photo_booth_activate;
	gapplication_class->open = photo_booth_open;
	g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_COUNTDOWN,
	  g_param_spec_uint ("countdown", "Shutter delay (s)",
	    "Shutter actuation delay countdown in seconds", 0, 60, DEFAULT_COUNTDOWN,
	    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void photo_booth_init (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);

	GST_DEBUG_OBJECT (pb, "photo_booth_init init object!");

	int control_sock[2];
	if (socketpair (PF_UNIX, SOCK_STREAM, 0, control_sock) < 0)
	{
		GST_ERROR_OBJECT (pb, "cannot create control sockets: %s (%i)", strerror(errno), errno);
		g_application_quit (G_APPLICATION (pb));
	}
	READ_SOCKET (pb) = control_sock[0];
	WRITE_SOCKET (pb) = control_sock[1];
	fcntl (READ_SOCKET (pb), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (pb), F_SETFL, O_NONBLOCK);

	pb->cam_info = NULL;

	pb->pipeline = NULL;
	pb->state = PB_STATE_NONE;
	pb->video_block_id = 0;
	pb->photo_block_id = 0;

	if (mkfifo(MOVIEPIPE, 0666) == -1 && errno != EEXIST)
	{
		GST_ERROR_OBJECT (pb, "cannot create moviepipe file %s: %s (%i)", MOVIEPIPE, strerror(errno), errno);
		g_application_quit (G_APPLICATION (pb));
	}

	pb->video_fd = open(MOVIEPIPE, O_RDWR);
	if (pb->video_fd == -1)
	{
		GST_ERROR_OBJECT (pb, "cannot open moviepipe file %s: %s (%i)", MOVIEPIPE, strerror(errno), errno);
		g_application_quit (G_APPLICATION (pb));
	}

	pb->capture_thread = NULL;
// 	pb->capture_thread = g_thread_try_new ("gphoto-capture", (GThreadFunc) photo_booth_capture_thread_func, pb, NULL); //!!!TODO FIXME

	priv->settings = NULL;
}

static void photo_booth_setup_window (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (pb);
	priv->win = photo_booth_window_new (pb);
	gtk_window_present (GTK_WINDOW (priv->win));
	g_signal_connect (G_OBJECT (priv->win), "destroy", G_CALLBACK (photo_booth_window_destroyed_signal), pb);
	photo_booth_setup_gstreamer (pb);
}

static void photo_booth_activate (GApplication *app)
{
	GST_DEBUG_OBJECT (app, "photo_booth_activate");
	photo_booth_setup_window (PHOTO_BOOTH (app));
}

static void photo_booth_open (GApplication  *app, GFile **files, gint n_files, const gchar *hint)
{
	GST_DEBUG_OBJECT (app, "photo_booth_open");
	photo_booth_setup_window (PHOTO_BOOTH (app));
}

static void photo_booth_finalize (GObject *object)
{
	PhotoBooth *pb = PHOTO_BOOTH (object);
	GST_INFO_OBJECT (pb, "finalize");
	SEND_COMMAND (pb, CONTROL_STOP);
	photo_booth_flush_pipe (pb->video_fd);
	g_thread_join (pb->capture_thread);
	if (pb->cam_info)
		photo_booth_cam_close (&pb->cam_info);
	if (pb->video_fd)
	{
		close (pb->video_fd);
		unlink (MOVIEPIPE);
	}
}

static void photo_booth_dispose (GObject *object)
{
	PhotoBoothPrivate *priv;
	priv = photo_booth_get_instance_private (PHOTO_BOOTH (object));
	g_clear_object (&priv->settings);
	G_OBJECT_CLASS (photo_booth_parent_class)->dispose (object);
}

static void _gphoto_err(GPLogLevel level, const char *domain, const char *str, void *data)
{
	GST_DEBUG ("GPhoto %d, %s:%s", (int) level, domain, str);
}

static GstPadProbeReturn _gst_photo_probecb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	GST_DEBUG_OBJECT (pad, "drop photo");
	return GST_PAD_PROBE_DROP;
}

static GstPadProbeReturn _gst_video_probecb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	GST_DEBUG_OBJECT (pad, "drop video");
	return GST_PAD_PROBE_DROP;
}

static gboolean photo_booth_cam_init (CameraInfo **cam_info)
{
	int retval;
	*cam_info = (CameraInfo*)malloc(sizeof(struct _CameraInfo));
	if (!cam_info)
		return FALSE;

	g_mutex_init (&(*cam_info)->mutex);
	g_mutex_lock (&(*cam_info)->mutex);
	(*cam_info)->preview_capture_count = 0;
	(*cam_info)->size = 0;
	(*cam_info)->data = NULL;
	(*cam_info)->context = gp_context_new();

	gp_camera_new(&(*cam_info)->camera);
	retval = gp_camera_init((*cam_info)->camera, (*cam_info)->context);
	GST_DEBUG ("gp_camera_init returned %d cam_info@%p camera@%p", retval, *cam_info, (*cam_info)->camera);
	g_mutex_unlock (&(*cam_info)->mutex);
	if (retval != GP_OK) {
		g_mutex_clear (&(*cam_info)->mutex);
		free (*cam_info);
		*cam_info = NULL;
		return FALSE;
	}
	return TRUE;
}

static gboolean photo_booth_cam_close (CameraInfo **cam_info)
{
	int retval;
	g_mutex_lock (&(*cam_info)->mutex);
	retval = gp_camera_exit((*cam_info)->camera, (*cam_info)->context);
	GST_DEBUG ("gp_camera_exit returned %i", retval);
	g_mutex_unlock (&(*cam_info)->mutex);
	g_mutex_clear (&(*cam_info)->mutex);
	free (*cam_info);
	*cam_info = NULL;
	return GP_OK ? TRUE : FALSE;
}

static void photo_booth_flush_pipe (int fd)
{
	int rlen = 0;
	unsigned char buf[1024];
	fcntl (fd, F_SETFL, O_NONBLOCK);
	while (rlen != -1)
		rlen = read (fd, buf, sizeof(buf));
}

static void photo_booth_quit_signal (PhotoBooth *pb)
{
	GST_INFO_OBJECT (pb, "caught SIGINT! exit...");
	g_application_quit (G_APPLICATION (pb));
}

static void photo_booth_window_destroyed_signal (PhotoBoothWindow *win, PhotoBooth *pb)
{
	GST_INFO_OBJECT (pb, "main window closed! exit...");
	g_application_quit (G_APPLICATION (pb));
}

static void photo_booth_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
	PhotoBooth *pb = PHOTO_BOOTH (object);
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);

	switch (prop_id) {
		case ARG_COUNTDOWN:
			priv->countdown = g_value_get_uint (value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void photo_booth_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
	PhotoBooth *pb = PHOTO_BOOTH (object);
	PhotoBoothPrivate *priv = photo_booth_get_instance_private (pb);

	switch (prop_id) {
		case ARG_COUNTDOWN:
			g_value_set_int (value, priv->countdown);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void photo_booth_capture_thread_func (PhotoBooth *pb)
{
	PhotoboothCaptureThreadState state = CAPTURE_INIT;

	GST_DEBUG_OBJECT (pb, "enter capture thread fd = %d", pb->video_fd);

	CameraFile *gp_file = NULL;
	int gpret, captured_frames = 0;

	if (gp_file_new_from_fd (&gp_file, pb->video_fd) != GP_OK)
	{
		GST_ERROR_OBJECT (pb, "couldn't start capture thread because gp_file_new_from_fd (%d) failed!", pb->video_fd);
		goto stop_running;
	}

	while (TRUE) {
		if (state == CAPTURE_STOP)
			goto stop_running;

		struct pollfd rfd[2];
		int timeout = 0;
		rfd[0].fd = READ_SOCKET (pb);
		rfd[0].events = POLLIN | POLLERR | POLLHUP | POLLPRI;

		if (state == CAPTURE_INIT && !pb->cam_info)
		{
			if (photo_booth_cam_init (&pb->cam_info))
			{
				GST_INFO_OBJECT (pb, "photo_booth_cam_inited @ %p", pb->cam_info);
				state = CAPTURE_VIDEO;
				g_main_context_invoke (NULL, (GSourceFunc) photo_booth_preview, pb);
			}
			else
				GST_INFO_OBJECT (pb, "no camera info.");
			timeout = 5000;
		}
		else if (state == CAPTURE_PAUSED)
			timeout = 1000;
		else
			timeout = 1000 / PREVIEW_FPS;

		int ret = poll(rfd, 1, timeout);

		if (G_UNLIKELY (ret == -1))
		{
			GST_ERROR_OBJECT (pb, "SELECT ERROR!");
			goto stop_running;
		}
		else if (ret == 0 && state == CAPTURE_VIDEO)
		{
			const char *mime;
			if (pb->cam_info)
			{
				g_mutex_lock (&pb->cam_info->mutex);
				gpret = gp_camera_capture_preview (pb->cam_info->camera, gp_file, pb->cam_info->context);
				g_mutex_unlock (&pb->cam_info->mutex);
				if (gpret < 0) {
					GST_ERROR_OBJECT (pb, "Movie capture error %d", gpret);
					if (gpret == -7)
					{
						state = CAPTURE_INIT;
						pb->state = PB_STATE_NONE;
						photo_booth_cam_close (&pb->cam_info);
					}
					continue;
				}
				else {
					g_mutex_lock (&pb->cam_info->mutex);
					gp_file_get_mime_type (gp_file, &mime);
					g_mutex_unlock (&pb->cam_info->mutex);
					if (strcmp (mime, GP_MIME_JPEG)) {
						GST_ERROR_OBJECT ("Movie capture error... Unhandled MIME type '%s'.", mime);
						continue;
					}
					captured_frames++;
					GST_LOG_OBJECT (pb, "captured frame (%d frames total)", captured_frames);
				}
			}
		}
		else if (ret == 0 && state == CAPTURE_PHOTO)
		{
			if (pb->cam_info)
			{
				ret = photo_booth_take_photo (pb->cam_info);
				if (ret)
					g_main_context_invoke (NULL, (GSourceFunc) photo_booth_snapshot_taken, pb);
				else
				{
					GST_ERROR_OBJECT (pb, "taking photo failed!");
					state = CAPTURE_INIT;
				}
			}
		}
		else if (rfd[0].revents)
		{
			char command;
			READ_COMMAND (pb, command, ret);
			switch (command) {
				case CONTROL_STOP:
					GST_DEBUG_OBJECT (pb, "CONTROL_STOP!");
					state = CAPTURE_STOP;
					break;
				case CONTROL_PAUSE:
					GST_DEBUG_OBJECT (pb, "CONTROL_PAUSE!");
					state = CAPTURE_PAUSED;
					break;
				case CONTROL_VIDEO:
					GST_DEBUG_OBJECT (pb, "CONTROL_VIDEO");
					state = CAPTURE_VIDEO;
					break;
				case CONTROL_PHOTO:
					GST_DEBUG_OBJECT (pb, "CONTROL_PHOTO");
					state = CAPTURE_PHOTO;
					break;
				default:
					GST_ERROR_OBJECT (pb, "illegal control socket command %c received!", command);
			}
			continue;
		}
		else if (state == CAPTURE_PAUSED)
		{
			GST_DEBUG_OBJECT (pb, "captured thread paused... timeout");
		}
	}

	g_assert_not_reached ();
	return;

	stop_running:
	{
		if (gp_file)
			gp_file_unref (gp_file);
		GST_DEBUG ("stop running, exit thread, %d frames captured", captured_frames);
		return;
	}
}

static GstElement *build_video_bin (PhotoBooth *pb)
{
	GstElement *video_bin;
	GstElement *mjpeg_source, *mjpeg_decoder, *mjpeg_filter, *video_filter, *video_scale, *video_convert, *video_overlay;
	GstCaps *caps;
	GstPad *ghost, *pad;

	video_bin = gst_element_factory_make ("bin", "video-bin");
	mjpeg_source = gst_element_factory_make ("fdsrc", "mjpeg-fdsrc");
	g_object_set (mjpeg_source, "fd", pb->video_fd, NULL);
	g_object_set (mjpeg_source, "do-timestamp", TRUE, NULL);
	g_object_set (mjpeg_source, "blocksize", 65536, NULL);

	mjpeg_filter = gst_element_factory_make ("capsfilter", "mjpeg-capsfilter");
	caps = gst_caps_new_simple ("image/jpeg", "width", G_TYPE_INT, PREVIEW_WIDTH, "height", G_TYPE_INT, PREVIEW_HEIGHT, "framerate", GST_TYPE_FRACTION, PREVIEW_FPS, 1, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
	g_object_set (G_OBJECT (mjpeg_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	mjpeg_decoder = gst_element_factory_make ("jpegdec", "mjpeg-decoder");
	video_scale = gst_element_factory_make ("videoscale", "mjpeg-videoscale");
	video_convert = gst_element_factory_make ("videoconvert", "mjpeg-videoconvert");
	video_filter = gst_element_factory_make ("capsfilter", "video-capsfilter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, PREVIEW_WIDTH, "height", G_TYPE_INT, PREVIEW_HEIGHT, NULL);
	g_object_set (G_OBJECT (video_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	video_overlay = gst_element_factory_make ("gdkpixbufoverlay", "video-overlay");
	g_object_set (video_overlay, "location", "overlay_print.png", NULL);

	if (!(mjpeg_source && mjpeg_filter && mjpeg_decoder && video_scale && video_convert && video_filter && video_overlay))
	{
		GST_ERROR_OBJECT (video_bin, "Failed to make videobin pipeline element(s):%s%s%s%s%s%s%s", mjpeg_source?"":" fdsrc", mjpeg_filter?"":" capsfilter", mjpeg_decoder?"":" jpegdec",
			video_scale?"":" videoscale", video_convert?"":" videoconvert", video_filter?"":" capsfilter", video_overlay?"":" gdkpixbufoverlay");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (video_bin), mjpeg_source, mjpeg_filter, mjpeg_decoder, video_scale, video_convert, video_filter, video_overlay, NULL);

	if (!gst_element_link_many (mjpeg_source, mjpeg_filter, mjpeg_decoder, video_scale, video_convert, video_filter, video_overlay, NULL))
	{
		GST_ERROR_OBJECT (video_bin, "couldn't link videobin elements!");
		return FALSE;
	}

	pad = gst_element_get_static_pad (video_overlay, "src");
	ghost = gst_ghost_pad_new ("src", pad);
	gst_object_unref (pad);
	gst_pad_set_active (ghost, TRUE);
	gst_element_add_pad (video_bin, ghost);
	return video_bin;
}

static GstElement *build_photo_bin (PhotoBooth *pb)
{
	GstElement *photo_bin;
	GstElement *photo_source, *photo_decoder, *photo_freeze, *photo_scale, *photo_filter, *photo_overlay, *photo_tee;
	GstCaps *caps;
	GstPad *ghost, *pad;

	photo_bin = gst_element_factory_make ("bin", "photo-bin");
	photo_source = gst_element_factory_make ("appsrc", "photo-appsrc");
	photo_decoder = gst_element_factory_make ("jpegdec", "photo-decoder");
	photo_freeze = gst_element_factory_make ("imagefreeze", "photo-freeze");
	photo_scale = gst_element_factory_make ("videoscale", "photo-scale");

	photo_filter = gst_element_factory_make ("capsfilter", "photo-capsfilter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, PRINT_WIDTH, "height", G_TYPE_INT, PRINT_HEIGHT, "framerate", GST_TYPE_FRACTION, 1, 5, NULL);
	g_object_set (G_OBJECT (photo_filter), "caps", caps, NULL);
	gst_caps_unref (caps);

	photo_overlay = gst_element_factory_make ("gdkpixbufoverlay", "photo-overlay");
	g_object_set (photo_overlay, "location", "overlay_print.png", NULL);
	g_object_set (photo_overlay, "overlay-width", PRINT_WIDTH, NULL);
	g_object_set (photo_overlay, "overlay-height", PRINT_HEIGHT, NULL);

	photo_tee = gst_element_factory_make ("tee", "photo-tee");

	if (!(photo_bin && photo_source && photo_decoder && photo_freeze && photo_scale && photo_filter, photo_overlay, photo_tee))
	{
		GST_ERROR_OBJECT (photo_bin, "Failed to make photobin pipeline element(s)");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (photo_bin), photo_source, photo_decoder, photo_freeze, photo_scale, photo_filter, photo_overlay, photo_tee, NULL);

	if (!gst_element_link_many (photo_source, photo_decoder, photo_freeze, photo_scale, photo_filter, photo_overlay, photo_tee, NULL))
	{
		GST_ERROR_OBJECT (photo_bin, "couldn't link photobin elements!");
		return FALSE;
	}

	pad = gst_element_get_request_pad (photo_tee, "src_%u");
	ghost = gst_ghost_pad_new ("src", pad);
	gst_object_unref (pad);
	gst_pad_set_active (ghost, TRUE);
	gst_element_add_pad (photo_bin, ghost);
	return photo_bin;
}

static gboolean photo_booth_setup_gstreamer (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstBus *bus;
	GstElement *video_convert, *video_sink;
	GtkWidget *gtkgstwidget;
	GstPad *ghost, *pad;

	priv = photo_booth_get_instance_private (pb);

	pb->video_bin  = build_video_bin (pb);
	pb->photo_bin  = build_photo_bin (pb);
	pb->output_bin = gst_element_factory_make ("bin", "output-bin");

	pb->pipeline = gst_pipeline_new ("photobooth-pipeline");

	video_convert = gst_element_factory_make ("videoconvert", "output-videoconvert");

	video_sink = gst_element_factory_make ("gtksink", NULL);
// 	g_object_set (pb->video_sink, "sync", FALSE, NULL);

	if (!(video_convert && video_sink))
	{
		GST_ERROR_OBJECT (pb, "Failed to create pipeline element(s):%s%s", video_convert?"":" videoconvert", video_sink?"":" gtksink");
		return FALSE;
	}

	gst_bin_add_many (GST_BIN (pb->output_bin), video_convert, video_sink, NULL);

	if (!gst_element_link (video_convert, video_sink))
	{
		GST_ERROR_OBJECT (pb, "couldn't link elements!");
		return FALSE;
	}

	g_object_get (video_sink, "widget", &gtkgstwidget, NULL);
	photo_booth_window_add_gtkgstwidget (priv->win, gtkgstwidget);
	g_object_unref (gtkgstwidget);

	gst_element_set_state (pb->pipeline, GST_STATE_PLAYING);
	gst_element_set_state (pb->output_bin, GST_STATE_PLAYING);

	pad = gst_element_get_static_pad (video_convert, "sink");
	ghost = gst_ghost_pad_new ("sink", pad);
	gst_object_unref (pad);
	gst_pad_set_active (ghost, TRUE);
	gst_element_add_pad (pb->output_bin, ghost);
	gst_bin_add_many (GST_BIN (pb->pipeline), pb->video_bin, pb->photo_bin, pb->output_bin, NULL);

	/* add watch for messages */
	bus = gst_pipeline_get_bus (GST_PIPELINE (pb->pipeline));
	gst_bus_add_watch (bus, (GstBusFunc) photo_booth_bus_callback, pb);

	GstElement* audio_pipeline;
	audio_pipeline = gst_pipeline_new ("audio-pipeline");
	priv->audio_playbin = gst_element_factory_make ("playbin", "audio-playbin");
	gst_bin_add (GST_BIN (audio_pipeline), priv->audio_playbin);
	GST_LOG ("added %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT "", priv->audio_playbin, audio_pipeline);

	return TRUE;
}

static gboolean photo_booth_bus_callback (GstBus *bus, GstMessage *message, PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	switch (GST_MESSAGE_TYPE (message))
	{
		case GST_MESSAGE_WARNING:
		{
			GError *err = NULL;
			gchar *debug = NULL;

			gst_message_parse_warning (message, &err, &debug);
			GST_WARNING ("Warning: %s\n", err->message);
			g_error_free (err);
			g_free (debug);
			break;
		}
		case GST_MESSAGE_ERROR:
		{
			GError *err = NULL;
			gchar *debug = NULL;

			gst_message_parse_error (message, &err, &debug);
			GST_ERROR ("Error: %s : %s", err->message, debug);
			g_error_free (err);
			g_free (debug);

			gtk_main_quit ();
			break;
		}
		case GST_MESSAGE_EOS:
		{
			GST_INFO ("EOS");
			gtk_main_quit ();
			break;
		}
		case GST_MESSAGE_STATE_CHANGED:
		{
			GstState old_state, new_state;
			gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
			GstStateChange transition = (GstStateChange)GST_STATE_TRANSITION (old_state, new_state);
			GstElement *src = GST_ELEMENT (GST_MESSAGE_SRC (message));
			GST_LOG ("%" GST_PTR_FORMAT " state transition %s -> %s", src, gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT(transition)), gst_element_state_get_name(GST_STATE_TRANSITION_NEXT(transition)));
			if (src == pb->video_bin && transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING)
			{
				GST_DEBUG ("video_bin GST_STATE_CHANGE_READY_TO_PAUSED -> CAPTURE VIDEO!");
				SEND_COMMAND (pb, CONTROL_VIDEO);
			}
			if (src == pb->output_bin && transition == GST_STATE_CHANGE_READY_TO_PAUSED)
			{
				priv = photo_booth_get_instance_private (pb);
				photo_booth_video_widget_ready (pb);
			}
			if (src == pb->output_bin && transition == GST_STATE_CHANGE_PAUSED_TO_PLAYING)
			{
				GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_video_start.dot");
				GST_DEBUG ("video_bin GST_STATE_CHANGE_PAUSED_TO_PLAYING -> hide spinner!");
				priv = photo_booth_get_instance_private (pb);
				photo_booth_window_set_spinner (priv->win, FALSE);
			}
			break;
		}
		case GST_MESSAGE_STREAM_START:
		{
			GST_DEBUG ("GST_MESSAGE_STREAM_START! state=%i", pb->state);
// 			if (pb->state == PB_STATE_ASKING)
// 			{
// 			}
		}
		default:
		{
// 			GST_DEBUG ("gst_message from %" GST_PTR_FORMAT ": %" GST_PTR_FORMAT "", GST_MESSAGE_SRC(message), message);
		}
	}
	return TRUE;
}

static void photo_booth_video_widget_ready (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GtkRequisition size;
	GtkAllocation size2;
	GstVideoRectangle s1, s2, rect;
	GstElement *element;
	GstCaps *caps;

	priv = photo_booth_get_instance_private (pb);
	gtk_widget_get_preferred_size (priv->win->gtkgstwidget, NULL, &size);
	gtk_widget_get_allocated_size (priv->win->gtkgstwidget, &size2, NULL);
	s1.w = size.width;
	s1.h = size.height;
	s2.w = size2.width;
	s2.h = size2.height;
	gst_video_sink_center_rect (s1, s2, &rect, TRUE);

	GST_INFO_OBJECT (pb, "gtksink widget is ready. preferred dimensions: %dx%d allocated %dx%d", size.width, size.height, size2.width, size2.height);

	element = gst_bin_get_by_name (GST_BIN (pb->video_bin), "video-capsfilter");
	caps = gst_caps_new_simple ("video/x-raw", "width", G_TYPE_INT, rect.w, "height", G_TYPE_INT, rect.h, NULL);
	g_object_set (G_OBJECT (element), "caps", caps, NULL);
	gst_caps_unref (caps);
	gst_object_unref (element);

	element = gst_bin_get_by_name (GST_BIN (pb->video_bin), "video-overlay");
	g_object_set (element, "overlay-width", rect.w, NULL);
	g_object_set (element, "overlay-height", rect.h, NULL);
	gst_object_unref (element);

	GST_INFO_OBJECT (pb, "gtksink widget is ready. output dimensions: %dx%d", rect.w, rect.h);
	priv->video_size = rect;
}

static gboolean photo_booth_preview (PhotoBooth *pb)
{
	GstPad *pad;
	if (pb->video_block_id)
	{
		GST_DEBUG_OBJECT (pb, "photo_booth_preview! halt photo_bin...");
		gst_element_set_state (pb->photo_bin, GST_STATE_READY);
		pad = gst_element_get_static_pad (pb->photo_bin, "src");
		pb->photo_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, _gst_photo_probecb, pb, NULL);
		gst_object_unref (pad);
		gst_element_unlink (pb->photo_bin, pb->output_bin);

		GST_DEBUG_OBJECT (pb, "photo_booth_preview! unblock video_bin...");
		pad = gst_element_get_static_pad (pb->video_bin, "src");
		gst_pad_remove_probe (pad, pb->video_block_id);
		gst_object_unref (pad);
	}
	int ret = gst_element_link (pb->video_bin, pb->output_bin);
	GST_DEBUG_OBJECT (pb, "linking %" GST_PTR_FORMAT " ! %" GST_PTR_FORMAT " ret=%i", pb->video_bin, pb->output_bin, ret);
	gst_element_set_state (pb->video_bin, GST_STATE_PLAYING);
	GST_DEBUG_OBJECT (pb, "photo_booth_preview done");
	pb->state = PB_STATE_PREVIEW;
	return FALSE;
}

static GtkPrintSettings *settings = NULL;

static void
begin_print (GObject *obj, gpointer user_data)
{
	GST_DEBUG_OBJECT (obj, "begin_print");
}

static void
draw_page (GtkPrintOperation *operation, GtkPrintContext *context, int page_nr)
{
	GST_DEBUG_OBJECT (context, "draw_page no. %p", page_nr);
	cairo_t *cr = gtk_print_context_get_cairo_context (context);
	/* Draw a red rectangle, as wide as the paper (inside the margins) */
	cairo_set_source_rgb (cr, 1.0, 0, 0);
	cairo_rectangle (cr, 0, 0, gtk_print_context_get_width (context), 50);
	cairo_fill (cr);
	/* Draw path */
	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_move_to (cr, 90, 75);
	cairo_line_to (cr, 60, 80);
	cairo_curve_to (cr, 40, 70, 65, 65, 70, 60);
	cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width (cr, 5);
	cairo_stroke (cr);
}

static void printing_error_dialog(GtkWindow *window, GError *print_error)
{
	GtkWidget *error_dialog;
	printf("printing_error_dialog called\n");
	printf("error message = %s\n", print_error->message);
	error_dialog = gtk_message_dialog_new(GTK_WINDOW(window),
			GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
			GTK_BUTTONS_CLOSE, "Print error:\n%s",
			print_error->message);
	g_signal_connect(error_dialog, "response",
			G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_widget_show(error_dialog);
}

static void print_done(GtkPrintOperation *operation,
			GtkPrintOperationResult result, gpointer user_data)
{
	GError *print_error;
	if (result != GTK_PRINT_OPERATION_RESULT_ERROR)
		return;
	gtk_print_operation_get_error(operation, &print_error);
	GST_DEBUG_OBJECT (user_data, "print_done");
	printing_error_dialog(GTK_WINDOW (user_data), print_error);
	g_error_free(print_error);
}


static void
do_print (gpointer user_data)
{
  GtkPrintOperation *print;
  GtkPrintOperationResult res;
  GError *print_error;


  GST_DEBUG_OBJECT (user_data, "do_print");
  
  print = gtk_print_operation_new ();

  if (settings != NULL)
    gtk_print_operation_set_print_settings (print, settings);

  g_signal_connect (print, "begin_print", G_CALLBACK (begin_print), NULL);
  g_signal_connect (print, "draw_page", G_CALLBACK (draw_page), NULL);
  g_signal_connect (print, "done", G_CALLBACK(print_done), NULL);

  res = gtk_print_operation_run (print, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                                 GTK_WINDOW (user_data), &print_error);

  GST_DEBUG_OBJECT (user_data, "print res=%i", (int)res);
  if (res == GTK_PRINT_OPERATION_RESULT_ERROR) {
		printing_error_dialog(user_data, print_error);
		g_error_free(print_error);
	} else {
		if (res == GTK_PRINT_OPERATION_RESULT_APPLY) {
			if (settings != NULL)
				g_object_unref(settings);
			settings = g_object_ref(gtk_print_operation_get_print_settings(print));
		}
	}
	
  if (res == GTK_PRINT_OPERATION_RESULT_CANCEL)
	  GST_DEBUG_OBJECT (user_data, "print cancelled");

  g_object_unref (print);

}

void photo_booth_background_clicked (GtkWidget *widget, GdkEventButton *event, PhotoBoothWindow *win)
{
	PhotoBoothPrivate *priv;
	PhotoBooth *pb = PHOTO_BOOTH_FROM_WINDOW (win);
	do_print (win);
	return;
	GST_DEBUG_OBJECT (widget, "photo_booth_background_clicked state=%d", pb->state);

	switch (pb->state) {
		case PB_STATE_PREVIEW:
		{
			photo_booth_snapshot_start (pb);
			break;
		}
		case PB_STATE_TAKING_PHOTO:
			GST_WARNING_OBJECT (pb, "BUSY TAKING A PHOTO, IGNORE CLICK");
			break;
		case PB_STATE_WAITING_FOR_ANSWER:
		{
			priv = photo_booth_get_instance_private (pb);
			gtk_widget_hide  (GTK_WIDGET (priv->win->button_yes));
			photo_booth_preview (pb);
			break;
		}
		default:
			break;
	}
}

static void photo_booth_snapshot_start (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	gchar* uri;
	guint delay = 1;

	priv = photo_booth_get_instance_private (pb);
	photo_booth_window_start_countdown (priv->win, priv->countdown);
	if (priv->countdown > 1)
		delay = (priv->countdown*1000)-100;
	GST_INFO_OBJECT (pb, "started countdown of %d seconds, start taking photo in %d ms", priv->countdown, delay);
	g_timeout_add (delay, (GSourceFunc) photo_booth_snapshot_prepare, pb);
	uri = g_filename_to_uri (DEFAULT_AUDIOFILE_COUNTDOWN, NULL, NULL);
	GST_INFO_OBJECT (pb, "audio uri: %s", uri);
	g_object_set (priv->audio_playbin, "uri", uri, NULL);
	g_free (uri);
	gst_element_set_state (GST_ELEMENT_PARENT (priv->audio_playbin), GST_STATE_PLAYING);
}

static gboolean photo_booth_snapshot_prepare (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GstPad *pad;
	gboolean ret;

	GST_INFO_OBJECT (pb, "SNAPSHOT!");
	GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_pre_snapshot.dot");

	if (!pb->cam_info)
		return FALSE;
	pb->state = PB_STATE_TAKING_PHOTO;

	priv = photo_booth_get_instance_private (pb);
	photo_booth_window_set_spinner (priv->win, TRUE);

	gst_element_set_state (priv->audio_playbin, GST_STATE_READY);

	SEND_COMMAND (pb, CONTROL_PHOTO);

	gst_element_set_state (pb->video_bin, GST_STATE_READY);
	GST_DEBUG_OBJECT (pb, "photo_booth_preview! halt video_bin...");
	pad = gst_element_get_static_pad (pb->video_bin, "src");
	pb->video_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, _gst_video_probecb, pb, NULL);
	gst_object_unref (pad);
	gst_element_unlink (pb->video_bin, pb->output_bin);

	if (pb->photo_block_id)
	{
		GST_DEBUG_OBJECT (pb, "photo_booth_preview! unblock photo_bin...");
		pad = gst_element_get_static_pad (pb->photo_bin, "src");
		gst_pad_remove_probe (pad, pb->photo_block_id);
		gst_object_unref (pad);
	}

	ret = gst_element_link (pb->photo_bin, pb->output_bin);
	GST_DEBUG_OBJECT (pb, "linking %" GST_PTR_FORMAT " ! %" GST_PTR_FORMAT " ret=%i", pb->photo_bin, pb->output_bin, ret);
	gst_element_set_state (pb->photo_bin, GST_STATE_PLAYING);

	return FALSE;
}

extern int camera_auto_focus (Camera *list, GPContext *context, int onoff);

static gboolean photo_booth_focus (CameraInfo *cam_info)
{
	int gpret;
// 	CameraEventType evttype;
// 	void *evtdata;


// 	do {
// 		g_mutex_lock (&cam_info->mutex);
// 		gpret = gp_camera_wait_for_event (cam_info->camera, 10, &evttype, &evtdata, cam_info->context);
// 		g_mutex_unlock (&cam_info->mutex);
// 		GST_DEBUG ("gp_camera_wait_for_event gpret=%i", gpret);
// 	} while ((gpret == GP_OK) && (evttype != GP_EVENT_TIMEOUT));
// 
// 	g_mutex_lock (&cam_info->mutex);
// 	gpret = camera_auto_focus (cam_info->camera, cam_info->context, 1);
// 	g_mutex_unlock (&cam_info->mutex);
// 	if (gpret != GP_OK) {
// 		GST_WARNING ("gphoto error: %s\n", gp_result_as_string(gpret));
// 		return FALSE;
// 	}
// 
// 	do {
// 		GST_DEBUG ("gp_camera_wait_for_event gpret=%i", gpret);
// 		g_mutex_lock (&cam_info->mutex);
// 		gpret = gp_camera_wait_for_event (cam_info->camera, 10, &evttype, &evtdata, cam_info->context);
// 		g_mutex_unlock (&cam_info->mutex);
// 	} while ((gpret == GP_OK) && (evttype != GP_EVENT_TIMEOUT));
// 
	g_mutex_lock (&cam_info->mutex);
	gpret = camera_auto_focus (cam_info->camera, cam_info->context, 0);
	g_mutex_unlock (&cam_info->mutex);
	if (gpret != GP_OK) {
		GST_WARNING ("gphoto error: %s\n", gp_result_as_string(gpret));
	}
	return TRUE;
}

static gboolean photo_booth_take_photo (CameraInfo *cam_info)
{
	int gpret;
	CameraFile *file;
	CameraFilePath camera_file_path;

	g_mutex_lock (&cam_info->mutex);
	strcpy (camera_file_path.folder, "/");
	strcpy (camera_file_path.name, "foo.jpg");
// 	snprintf (camera_file_path.name, 128, "pb_capt_%04d", cam_info->preview_capture_count);
	gpret = gp_camera_capture (cam_info->camera, GP_CAPTURE_IMAGE, &camera_file_path, cam_info->context);
	GST_DEBUG ("gp_camera_capture gpret=%i Pathname on the camera: %s/%s", gpret, camera_file_path.folder, camera_file_path.name);
	if (gpret < 0)
		return FALSE;

	gpret = gp_file_new (&file);
	GST_DEBUG ("gp_file_new gpret=%i", gpret);

	gpret = gp_camera_file_get (cam_info->camera, camera_file_path.folder, camera_file_path.name, GP_FILE_TYPE_NORMAL, file, cam_info->context);
	GST_DEBUG ("gp_camera_file_get gpret=%i", gpret);
	if (gpret < 0)
		return FALSE;
	gp_file_get_data_and_size (file, (const char**)&(cam_info->data), &(cam_info->size));
	if (gpret < 0)
		return FALSE;

	gpret = gp_camera_file_delete (cam_info->camera, camera_file_path.folder, camera_file_path.name, cam_info->context);
	GST_DEBUG ("gp_camera_file_delete gpret=%i", gpret);
// 	gp_file_free(file);
	g_mutex_unlock (&cam_info->mutex);

	return TRUE;
}

static gboolean photo_booth_snapshot_taken (PhotoBooth *pb)
{
	GstElement *appsrc;
	GstBuffer *buffer;
	GstFlowReturn flowret;
	GstPad *pad;

	GST_INFO_OBJECT (pb, "photo_booth_snapshot_taken size=%lu", pb->cam_info->size);

	appsrc = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-appsrc");
	buffer = gst_buffer_new_wrapped (pb->cam_info->data, pb->cam_info->size);
	g_signal_emit_by_name (appsrc, "push-buffer", buffer, &flowret);

	if (flowret != GST_FLOW_OK)
		GST_ERROR_OBJECT (appsrc, "couldn't push %" GST_PTR_FORMAT " to appsrc", buffer);
	gst_object_unref (appsrc);
	GST_INFO_OBJECT (pb, "photo_booth_snapshot now waiting for user input... PB_STATE_ASKING");

	SEND_COMMAND (pb, CONTROL_PAUSE);

	gst_element_set_state (pb->photo_bin, GST_STATE_PLAYING);
	pad = gst_element_get_static_pad (pb->photo_bin, "src");
	pb->photo_block_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, photo_booth_catch_photo_buffer, pb, NULL);
	GST_DEBUG_OBJECT (pb, "probe id = %lu", pb->photo_block_id);

	return FALSE;
}

static GstPadProbeReturn photo_booth_catch_photo_buffer (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	PhotoBooth *pb = PHOTO_BOOTH (user_data);
	PhotoBoothPrivate *priv;
	GstElement *tee, *encoder, *filesink;
	priv = photo_booth_get_instance_private (pb);

	if (pb->state == PB_STATE_TAKING_PHOTO)
	{
		pb->state = PB_STATE_PROCESS_PHOTO;
		photo_booth_window_set_spinner (priv->win, FALSE);
		GST_INFO_OBJECT (priv->win->button_yes, "PB_STATE_TAKING_PHOTO -> PB_STATE_PROCESS_PHOTO. hide spinner, show button");
		gtk_widget_show (GTK_WIDGET (priv->win->button_yes));

		return GST_PAD_PROBE_PASS;
	}
	if (pb->state == PB_STATE_PROCESS_PHOTO)
	{
		encoder = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-encoder");
		if (GST_IS_ELEMENT (encoder))
		{
			pb->state = PB_STATE_WAITING_FOR_ANSWER;
			return GST_PAD_PROBE_PASS;
		}
		tee = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-tee");
		GST_INFO_OBJECT (priv->win->button_yes, "PB_STATE_PROCESS_PHOTO -> PB_STATE_WAITING_FOR_ANSWER. insert output file encoder and writer elements");
		encoder = gst_element_factory_make ("jpegenc", "photo-encoder");
		filesink = gst_element_factory_make ("filesink", "photo-filesink");
		if (!encoder || !filesink)
			GST_ERROR_OBJECT (pb->photo_bin, "Failed to make photo encoder");
		g_object_set (filesink, "location", "PHOTOBOOTH-PRINT.JPG", NULL);
		gst_bin_add_many (GST_BIN (pb->photo_bin), encoder, filesink, NULL);
		tee = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-tee");
		GST_INFO_OBJECT (pb->photo_bin, "linking elements %" GST_PTR_FORMAT " ! %" GST_PTR_FORMAT " ! %" GST_PTR_FORMAT "", tee, encoder, filesink);
		if (!gst_element_link_many (tee, encoder, filesink, NULL))
			GST_ERROR_OBJECT (pb->photo_bin, "couldn't link photobin filewrite elements!");
		gst_object_unref (tee);
		gst_element_set_state (pb->photo_bin, GST_STATE_PLAYING);
		GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_video_snapshot_taken.dot");
		return GST_PAD_PROBE_PASS;
	}
	if (pb->state == PB_STATE_WAITING_FOR_ANSWER)
	{
		GST_INFO_OBJECT (pb, "PB_STATE_WAITING_FOR_ANSWER -> PB_STATE_WAITING_FOR_ANSWER. remove output file encoder and writer elements and PAUSE");
		gst_element_set_state (pb->photo_bin, GST_STATE_PAUSED);
		tee = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-tee");
		encoder = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-encoder");
		filesink = gst_bin_get_by_name (GST_BIN (pb->photo_bin), "photo-filesink");
		gst_element_unlink_many (tee, encoder, filesink, NULL);
		gst_bin_remove_many (GST_BIN (pb->photo_bin), encoder, filesink, NULL);
		gst_element_set_state (filesink, GST_STATE_NULL);
		gst_element_set_state (encoder, GST_STATE_NULL);
		gst_object_unref (tee);
		gst_object_unref (filesink);
		gst_object_unref (encoder);
		GST_INFO_OBJECT (pb, "PB_STATE_WAITING_FOR_ANSWER -> unreffed encoder and file writer.");
		pb->photo_block_id = 0;
		return GST_PAD_PROBE_REMOVE;
	}
}

void photo_booth_button_yes_clicked (GtkButton *button, PhotoBoothWindow *win)
{
	PhotoBooth *pb = PHOTO_BOOTH_FROM_WINDOW (win);
	GST_DEBUG_OBJECT (pb, "on_button_yes_clicked");
	if (pb->state == PB_STATE_WAITING_FOR_ANSWER)
	{
		photo_booth_print (pb);
	}
}

static void photo_booth_print (PhotoBooth *pb)
{
	PhotoBoothPrivate *priv;
	GST_DEBUG_OBJECT (pb, "!!!PRINT!!!");
	priv = photo_booth_get_instance_private (pb);
	gtk_widget_hide  (GTK_WIDGET (priv->win->button_yes));
	GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (pb->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "photo_booth_photo_print.dot");
}

PhotoBooth *photo_booth_new (void)
{
	return g_object_new (PHOTO_BOOTH_TYPE,
			"application-id", "org.schaffenburg.photobooth",
			"flags", G_APPLICATION_HANDLES_OPEN,
			NULL);
}

int main (int argc, char *argv[])
{
	PhotoBooth *pb;
	int ret;

	gst_init (&argc, &argv);

	pb = photo_booth_new ();

	g_unix_signal_add (SIGINT, (GSourceFunc) photo_booth_quit_signal, pb);
	ret = g_application_run (G_APPLICATION (pb), argc, argv);
	GST_INFO_OBJECT (pb, "g_application_run returned %i", ret);
	g_object_unref (pb);
	return ret;
}
