/**
 * Copyright (C) 2011-2017 Aratelia Limited - Juan A. Rubio
 *
 * This file is part of Tizonia
 *
 * Tizonia is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Tizonia is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Tizonia.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file   mp4dmuxfltprc.c
 * @author Juan A. Rubio <juan.rubio@aratelia.com>
 *
 * @brief  Tizonia - MP4 demuxer filter processor
 *
 * TODO: Seek support.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <stdarg.h>
#include <alloca.h>

#include <OMX_TizoniaExt.h>

#include <tizplatform.h>

#include <tizkernel.h>
#include <tizscheduler.h>

#include "mp4dmux.h"
#include "mp4dmuxfltprc.h"
#include "mp4dmuxfltprc_decls.h"

#ifdef TIZ_LOG_CATEGORY_NAME
#undef TIZ_LOG_CATEGORY_NAME
#define TIZ_LOG_CATEGORY_NAME "tiz.mp4_demuxer.filter.prc"
#endif

static mp4dmuxflt_prc_t * gp_prc = NULL;

#define FILE_SIZE 7747480
#define MP4V2_INT_MAX_FAILED_ATTEMPTS 20

/* Forward declarations */
static OMX_ERRORTYPE
mp4dmuxflt_prc_deallocate_resources (void *);
static OMX_ERRORTYPE
store_data (mp4dmuxflt_prc_t * ap_prc);
static void
reset_mp4v2_members (mp4dmuxflt_prc_t * ap_prc);
static OMX_ERRORTYPE
send_port_auto_detect_events (mp4dmuxflt_prc_t * ap_prc);

#define on_nestegg_error_ret_omx_oom(expr)                            \
  do                                                                  \
    {                                                                 \
      int nestegg_error = 0;                                          \
      if (0 != (nestegg_error = (expr)))                              \
        {                                                             \
          TIZ_ERROR (handleOf (ap_prc),                               \
                     "[OMX_ErrorInsufficientResources] : while using" \
                     " libnestegg");                                  \
          return OMX_ErrorInsufficientResources;                      \
        }                                                             \
    }                                                                 \
  while (0)

static inline OMX_BUFFERHEADERTYPE *
get_mp4_hdr (mp4dmuxflt_prc_t * ap_prc)
{
  assert (ap_prc);
  return tiz_filter_prc_get_header (ap_prc,
                                    ARATELIA_MP4_DEMUXER_FILTER_PORT_0_INDEX);
}

static void
mp4_log_cback (MP4LogLevel loglevel, const char * fmt, va_list ap)
{
  #define MAX_ALLOCA_BUF 1024
  char * buffer = alloca (MAX_ALLOCA_BUF);
  vsnprintf (buffer, MAX_ALLOCA_BUF, fmt, ap);

  /* typedef enum {
     MP4_LOG_NONE = 0,
     MP4_LOG_ERROR = 1,
     MP4_LOG_WARNING = 2,
     MP4_LOG_INFO = 3,
     MP4_LOG_VERBOSE1 = 4,
     MP4_LOG_VERBOSE2 = 5,
     MP4_LOG_VERBOSE3 = 6,
     MP4_LOG_VERBOSE4 = 7
     }  MP4LogLevel;
  */

  switch(loglevel)
    {
    case MP4_LOG_ERROR:
      {
        TIZ_ERROR(handleOf(gp_prc), "%s", buffer);
      }
      break;
    case MP4_LOG_INFO:
      {
        TIZ_NOTICE(handleOf(gp_prc), "%s", buffer);
      }
      break;
    case MP4_LOG_WARNING:
      {
        TIZ_DEBUG(handleOf(gp_prc), "%s", buffer);
      }
      break;
    default:
      {
        TIZ_TRACE(handleOf(gp_prc), "%s", buffer);
      }
      break;
    };
}

static void *
mp4_open_cback (const char * name, MP4FileMode mode)
{
  TIZ_TRACE(handleOf(gp_prc), "file name [%s]", name);
  return gp_prc;
}

static int
mp4_seek_cback (void * ap_handle, int64_t pos)
{
  mp4dmuxflt_prc_t * p_prc = ap_handle;
  assert (gp_prc == ap_handle);
  assert (p_prc);
  TIZ_TRACE(handleOf(gp_prc), "pos [%lld]", pos);
  (void)tiz_buffer_seek (p_prc->p_mp4_store_, pos, TIZ_BUFFER_SEEK_SET);
  return 0;
}

static int
mp4_read_cback (void * ap_handle, void * ap_buffer, int64_t a_size,
                int64_t * ap_nin, int64_t a_maxChunkSize)
{
  mp4dmuxflt_prc_t * p_prc = ap_handle;
  int retval = -1;

  assert (gp_prc == ap_handle);
  assert (p_prc);
  assert (ap_buffer);
  assert (ap_nin);

  *ap_nin = 0;

  if (tiz_filter_prc_is_eos (p_prc)
      && tiz_buffer_available (p_prc->p_mp4_store_) == 0)
    {
      TIZ_DEBUG (handleOf (p_prc), "out of compressed data");
      return 1;
    }

  (void) store_data (p_prc);

  if (ap_buffer && a_size > 0)
    {
      if (tiz_buffer_available (p_prc->p_mp4_store_) >= a_size)
        {
          memcpy (ap_buffer, tiz_buffer_get (p_prc->p_mp4_store_), a_size);
          tiz_buffer_advance (p_prc->p_mp4_store_, a_size);
          *ap_nin = a_size;
          retval = 0;
        }
      else
        {
          TIZ_DEBUG (handleOf (p_prc), "out of compressed data");
          retval = 1;
        }
    }

  TIZ_DEBUG (handleOf (p_prc), "a_size [%lld] nin [%lld]", a_size, *ap_nin);

  return retval;
}

static int
mp4_write_cback (void * handle, const void * buffer, int64_t size, int64_t * nout,
           int64_t maxChunkSize)
{
  TIZ_TRACE(handleOf(gp_prc), "");
  return 0;
}

static int
mp4_close_cback (void * handle)
{
  TIZ_TRACE(handleOf(gp_prc), "");
  return 0;
}

/* static int64_t */
/* mp4_get_size (void * handle) */
/* { */
/*   TIZ_TRACE(handleOf(gp_prc), "FILE_SIZE"); */
/*   return FILE_SIZE; */
/* } */

static void
propagate_eos_if_required (mp4dmuxflt_prc_t * ap_prc,
                           OMX_BUFFERHEADERTYPE * ap_out_hdr)
{
  assert (ap_prc);
  assert (ap_out_hdr);

  /* If EOS, propagate the flag to the next component */
  if (tiz_filter_prc_is_eos (ap_prc)
      && tiz_buffer_available (ap_prc->p_mp4_store_) == 0)
    {
      ap_out_hdr->nFlags |= OMX_BUFFERFLAG_EOS;
      tiz_filter_prc_update_eos_flag (ap_prc, false);
    }
}

/* TODO: move this functionality to tiz_filter_prc_t */
static OMX_ERRORTYPE
release_input_header (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE * p_hdr = get_mp4_hdr (ap_prc);

  assert (ap_prc);

  if (p_hdr)
    {
      TIZ_DEBUG (handleOf (ap_prc), "[%p] nFlags [%d]", p_hdr, p_hdr->nFlags);
      if ((p_hdr->nFlags & OMX_BUFFERFLAG_EOS) > 0)
        {
          tiz_filter_prc_update_eos_flag (ap_prc, true);
          p_hdr->nFlags &= ~(1 << OMX_BUFFERFLAG_EOS);
        }
      rc = tiz_filter_prc_release_header (
        ap_prc, ARATELIA_MP4_DEMUXER_FILTER_PORT_0_INDEX);
    }
  return rc;
}

static OMX_ERRORTYPE
release_output_header (mp4dmuxflt_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  if (tiz_filter_prc_is_port_enabled (ap_prc, a_pid))
    {
      OMX_BUFFERHEADERTYPE * p_hdr = tiz_filter_prc_get_header (ap_prc, a_pid);
      assert (ap_prc);
      if (p_hdr)
        {
          TIZ_DEBUG (handleOf (ap_prc), "p_hdr [%p] nFilledLen [%u]", p_hdr,
                     p_hdr->nFilledLen);
          propagate_eos_if_required (ap_prc, p_hdr);
          rc = tiz_filter_prc_release_header (ap_prc, a_pid);
        }
    }
  return rc;
}

OMX_ERRORTYPE
deliver_codec_metadata (mp4dmuxflt_prc_t * ap_prc, const OMX_U32 a_pid)
{
  OMX_BUFFERHEADERTYPE * p_hdr = NULL;
  bool * p_metadata_delivered = NULL;
  tiz_buffer_t * p_out_store = NULL;
  tiz_vector_t * p_header_lengths = NULL;

  assert (ap_prc);

  p_metadata_delivered = ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX == a_pid
                           ? &(ap_prc->audio_metadata_delivered_)
                           : &(ap_prc->video_metadata_delivered_);
  assert (p_metadata_delivered);

  p_out_store = ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX == a_pid
                  ? ap_prc->p_aud_store_
                  : ap_prc->p_vid_store_;
  assert (p_out_store);

  p_header_lengths = ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX == a_pid
                       ? ap_prc->p_aud_header_lengths_
                       : ap_prc->p_vid_header_lengths_;
  assert (p_header_lengths);

  if (tiz_vector_length (p_header_lengths) > 0)
    {
      if (tiz_buffer_available (p_out_store)
          && (p_hdr = tiz_filter_prc_get_header (ap_prc, a_pid)))
        {
          size_t headerlen = 0;
          size_t nbytes_to_copy = 0;
          size_t * p_headerlen = tiz_vector_at (p_header_lengths, (OMX_S32) 0);
          assert (p_headerlen);
          headerlen = *p_headerlen;

          /* Copy the data into the omx buffer */
          nbytes_to_copy = MIN (TIZ_OMX_BUF_AVAIL (p_hdr), headerlen);
          memcpy (TIZ_OMX_BUF_PTR (p_hdr) + p_hdr->nFilledLen,
                  tiz_buffer_get (p_out_store), nbytes_to_copy);
          tiz_buffer_advance (p_out_store, nbytes_to_copy);
          p_hdr->nFilledLen += nbytes_to_copy;
          tiz_vector_erase (p_header_lengths, (OMX_S32) 0, (OMX_S32) 1);
          tiz_check_omx (release_output_header (ap_prc, a_pid));
        }
    }

  if (0 == tiz_vector_length (p_header_lengths))
    {
      *p_metadata_delivered = true;
    }

  return OMX_ErrorNone;
}

/* static OMX_ERRORTYPE */
/* store_audio_codec_metadata (mp4dmuxflt_prc_t * ap_prc, */
/*                             unsigned char * ap_codec_data, size_t a_length) */
/* { */
/*   int pushed = 0; */
/*   assert (ap_prc); */
/*   assert (ap_codec_data); */
/*   assert (a_length); */

/*   pushed = tiz_buffer_push (ap_prc->p_aud_store_, ap_codec_data, a_length); */
/*   tiz_check_true_ret_val ((pushed == a_length), OMX_ErrorInsufficientResources); */
/*   tiz_check_omx ( */
/*     tiz_vector_push_back (ap_prc->p_aud_header_lengths_, &a_length)); */

/*   return OMX_ErrorNone; */
/* } */

/* static OMX_ERRORTYPE */
/* store_video_codec_metadata (mp4dmuxflt_prc_t * ap_prc, */
/*                             unsigned char * ap_codec_data, size_t a_length) */
/* { */
/*   int pushed = 0; */
/*   assert (ap_prc); */
/*   assert (ap_codec_data); */
/*   assert (a_length); */

/*   pushed = tiz_buffer_push (ap_prc->p_vid_store_, ap_codec_data, a_length); */
/*   tiz_check_true_ret_val ((pushed == a_length), OMX_ErrorInsufficientResources); */
/*   tiz_check_omx ( */
/*     tiz_vector_push_back (ap_prc->p_vid_header_lengths_, &a_length)); */

/*   return OMX_ErrorNone; */
/* } */

static mp4_track_type_t
get_track_type (mp4dmuxflt_prc_t * ap_prc, const MP4FileHandle a_mp4hdl,
                const MP4TrackId tid)
{
  char * p_track_type = NULL;
  assert (ap_prc);
  assert (a_mp4hdl != MP4_INVALID_FILE_HANDLE);
  assert (MP4_INVALID_TRACK_ID != tid);

  {
    p_track_type = (char *) MP4GetTrackType (a_mp4hdl, tid);
    if (p_track_type)
      {
        if (!strcmp (p_track_type, MP4_AUDIO_TRACK_TYPE))
          {
            ap_prc->track_type_ = mp4_track_audio;
          }
        else if (!strcmp (p_track_type, MP4_VIDEO_TRACK_TYPE))
          {
            ap_prc->track_type_ = mp4_track_video;
          }
        else if (!strcmp (p_track_type, MP4_HINT_TRACK_TYPE))
          {
            ap_prc->track_type_ = mp4_track_hint;
          }
        else if (strcmp (p_track_type, MP4_CNTL_TRACK_TYPE) == 0)
          {
            ap_prc->track_type_ = mp4_track_cntl;
          }
        else if (!strcmp (p_track_type, MP4_OD_TRACK_TYPE))
          {
            ap_prc->track_type_ = mp4_track_od;
          }
        else if (!strcmp (p_track_type, MP4_SCENE_TRACK_TYPE))
          {
            ap_prc->track_type_ = mp4_track_scene;
          }
        else
          {
            ap_prc->track_type_ = mp4_track_unknown;
          }
      }
  }
  return ap_prc->track_type_;
}

#ifndef NDEBUG
static void
print_track_info (mp4dmuxflt_prc_t * ap_prc, const MP4TrackId tid)
{
  assert (ap_prc);
  assert (ap_prc->mp4v2_hdl_ != MP4_INVALID_FILE_HANDLE);
  if (MP4_INVALID_TRACK_ID != tid)
    {
      char * p_track_nfo = MP4Info (ap_prc->mp4v2_hdl_, tid);
      TIZ_DEBUG (handleOf (ap_prc), "track info: ", (p_track_nfo ? p_track_nfo : ""));
      tiz_mem_free (p_track_nfo);
    }
}
#endif

static OMX_ERRORTYPE
prepare_port_auto_detection (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  /* Prepare audio port */
  TIZ_INIT_OMX_PORT_STRUCT (port_def, ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX);
  tiz_check_omx (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &port_def));
  ap_prc->audio_coding_type_ = port_def.format.audio.eEncoding;
  ap_prc->audio_auto_detect_on_
    = (OMX_AUDIO_CodingAutoDetect == ap_prc->audio_coding_type_) ? true : false;

  /* Prepare video port */
  TIZ_INIT_OMX_PORT_STRUCT (port_def, ARATELIA_MP4_DEMUXER_FILTER_PORT_2_INDEX);
  tiz_check_omx (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &port_def));
  ap_prc->video_coding_type_ = port_def.format.video.eCompressionFormat;
  ap_prc->video_auto_detect_on_
    = (OMX_VIDEO_CodingAutoDetect == ap_prc->video_coding_type_) ? true : false;

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
get_temp_file(mp4dmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert(ap_prc);

  static char template[] = "/tmp/tizonia-mp4dmux-XXXXXX";
  if (!ap_prc->tmp_fd_1_)
    {
      char fname[PATH_MAX];
      strcpy(fname, template);
      if ((ap_prc->tmp_fd_1_ = mkstemp (fname)) == -1)
        {
          TIZ_ERROR (handleOf (ap_prc), "Error creating temp file (%s)",
                     strerror (errno));
          rc = OMX_ErrorInsufficientResources;
        }
    }
  return rc;
}

static OMX_ERRORTYPE
store_data (mp4dmuxflt_prc_t * ap_prc)
{
  bool rc = OMX_ErrorNone;
  assert (ap_prc);

  OMX_BUFFERHEADERTYPE * p_in = get_mp4_hdr (ap_prc);
  if (p_in)
    {
      const void *p_buf = p_in->pBuffer + p_in->nOffset;
      const size_t count = p_in->nFilledLen;
      tiz_check_omx (get_temp_file (ap_prc));
      if (write (ap_prc->tmp_fd_1_, p_buf, count) != count)
        {
          TIZ_ERROR (handleOf (ap_prc),
                     "Error writing to temp file (%s)",
                     strerror (errno));
        }
      rc = release_input_header (ap_prc);
    }
  return rc;
}

/* static OMX_ERRORTYPE */
/* extract_track_data (mp4dmuxflt_prc_t * ap_prc, const unsigned int a_track, */
/*                     const OMX_U32 a_pid) */
/* { */
/*   OMX_ERRORTYPE rc = OMX_ErrorNotReady; */
/*   OMX_BUFFERHEADERTYPE * p_hdr = NULL; */

/*   if (!tiz_filter_prc_is_port_disabled (ap_prc, a_pid)) */
/*     { */
/*       p_hdr = tiz_filter_prc_get_header (ap_prc, a_pid); */
/*       if (p_hdr) */
/*         { */
/*           assert (ap_prc); */

/*           /\* Extract a packet *\/ */

/*           /\* Release the OMX buffer *\/ */
/*           if (TIZ_OMX_BUF_FILL_LEN (p_hdr) > 0) */
/*             { */
/*               tiz_check_omx (release_output_header (ap_prc, a_pid)); */
/*             } */
/*         } /\* if (p_hdr) *\/ */
/*     }     /\* if (!tiz_filter_prc_is_port_disabled (ap_prc, a_pid)) *\/ */

/*   /\* Release the ne packet if all chunks have been processed *\/ */

/*   return rc; */
/* } */

/* static OMX_ERRORTYPE */
/* extract_track_audio_data (mp4dmuxflt_prc_t * ap_prc, */
/*                           const unsigned int a_track) */
/* { */
/*   OMX_ERRORTYPE rc = OMX_ErrorNone; */
/*   assert (ap_prc); */
/*   rc = extract_track_data (ap_prc, a_track, */
/*                            ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX); */
/*   return rc; */
/* } */

/* static OMX_ERRORTYPE */
/* extract_track_video_data (mp4dmuxflt_prc_t * ap_prc, */
/*                           const unsigned int a_track) */
/* { */
/*   OMX_ERRORTYPE rc = OMX_ErrorNone; */
/*   assert (ap_prc); */
/*   rc = extract_track_data (ap_prc, a_track, */
/*                            ARATELIA_MP4_DEMUXER_FILTER_PORT_2_INDEX); */
/*   return rc; */
/* } */

/* static bool */
/* able_to_demux (mp4dmuxflt_prc_t * ap_prc) */
/* { */
/*   bool rc = true; */
/*   bool compressed_data_avail */
/*     = (tiz_buffer_available (ap_prc->p_mp4_store_) > 0); */
/*   bool enough_compressed_data_avail */
/*     = compressed_data_avail */
/*       && !(tiz_buffer_available (ap_prc->p_mp4_store_)); */

/*   if (!compressed_data_avail && tiz_filter_prc_is_eos (ap_prc)) */
/*     { */
/*       release_output_header (ap_prc, ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX); */
/*       release_output_header (ap_prc, ARATELIA_MP4_DEMUXER_FILTER_PORT_2_INDEX); */
/*     } */

/*   if (!compressed_data_avail || !enough_compressed_data_avail */
/*       || (!tiz_filter_prc_output_headers_available (ap_prc))) */
/*     { */
/*       rc = false; */
/*     } */

/*   TIZ_DEBUG (handleOf (ap_prc), "able to demux - %s", rc ? "YES" : "NO"); */

/*   return rc; */
/* } */

/* static OMX_ERRORTYPE */
/* read_packet (mp4dmuxflt_prc_t * ap_prc) */
/* { */
/*   OMX_ERRORTYPE rc = OMX_ErrorNotReady; */

/*   assert (ap_prc); */

/*   return rc; */
/* } */

/* static OMX_ERRORTYPE */
/* demux_stream (mp4dmuxflt_prc_t * ap_prc) */
/* { */
/*   OMX_ERRORTYPE rc = OMX_ErrorNotReady; */

/*   assert (ap_prc); */

/*   while (able_to_demux (ap_prc)) */
/*     { */
/*       rc = read_packet (ap_prc); */
/*     } */

/*   return rc; */
/* } */

static OMX_ERRORTYPE
alloc_input_store (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  TIZ_INIT_OMX_PORT_STRUCT (port_def, ARATELIA_MP4_DEMUXER_FILTER_PORT_0_INDEX);
  tiz_check_omx (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &port_def));

  assert (ap_prc->p_mp4_store_ == NULL);
  tiz_check_omx (
    tiz_buffer_init (&(ap_prc->p_mp4_store_), port_def.nBufferSize * 4));

  /* Will need to seek on this buffer  */
  return tiz_buffer_seek_mode (ap_prc->p_mp4_store_, TIZ_BUFFER_SEEKABLE);
}

static OMX_ERRORTYPE
alloc_output_stores (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE aud_port_def;
  OMX_PARAM_PORTDEFINITIONTYPE vid_port_def;
  assert (ap_prc);

  TIZ_INIT_OMX_PORT_STRUCT (aud_port_def,
                            ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX);
  tiz_check_omx (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &aud_port_def));

  TIZ_INIT_OMX_PORT_STRUCT (vid_port_def,
                            ARATELIA_MP4_DEMUXER_FILTER_PORT_2_INDEX);
  tiz_check_omx (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &vid_port_def));

  assert (!ap_prc->p_aud_store_);
  tiz_check_omx (
    tiz_buffer_init (&(ap_prc->p_aud_store_), aud_port_def.nBufferSize));

  assert (!ap_prc->p_vid_store_);
  tiz_check_omx (
    tiz_buffer_init (&(ap_prc->p_vid_store_), vid_port_def.nBufferSize));

  assert (!ap_prc->p_aud_header_lengths_);
  tiz_check_omx (
    tiz_vector_init (&(ap_prc->p_aud_header_lengths_), sizeof (size_t)));

  assert (!ap_prc->p_vid_header_lengths_);
  tiz_check_omx (
    tiz_vector_init (&(ap_prc->p_vid_header_lengths_), sizeof (size_t)));

  return OMX_ErrorNone;
}

static inline void
dealloc_mp4v2 (
  /*@special@ */ mp4dmuxflt_prc_t * ap_prc)
/*@releases ap_prc->p_ne_@ */
/*@ensures isnull ap_prc->p_ne_@ */
{
  assert (ap_prc);
  if (MP4_IS_VALID_FILE_HANDLE (ap_prc->mp4v2_hdl_))
    {
      MP4Close (ap_prc->mp4v2_hdl_, 0);
      ap_prc->mp4v2_hdl_ = MP4_INVALID_FILE_HANDLE;
    }
}

static OMX_ERRORTYPE
alloc_mp4v2 (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (ap_prc);
  TIZ_TRACE(handleOf(ap_prc), "");
  if (!MP4_IS_VALID_FILE_HANDLE (ap_prc->mp4v2_hdl_))
    {
/*       const MP4FileProvider provider */
/*         = {mp4_open_cback, mp4_seek_cback, mp4_read_cback, mp4_write_cback, */
/*            mp4_close_cback, mp4_get_size}; */
      const MP4FileProvider provider
        = {mp4_open_cback, mp4_seek_cback, mp4_read_cback, mp4_write_cback,
           mp4_close_cback};
      ap_prc->mp4v2_hdl_ = MP4ReadProvider ("Tizonia", &provider);
      TIZ_TRACE(handleOf(ap_prc), "MP4ReadProvider");
      if (!MP4_IS_VALID_FILE_HANDLE (ap_prc->mp4v2_hdl_))
        {
          /* We'll assume mp4v2 has not initialised correctly because there is
             not enough input data yet... we'll wait for more data to arrive, but
             we'll also give up after the max number of failed attempts. */
          dealloc_mp4v2 (ap_prc);
          reset_mp4v2_members (ap_prc);
          tiz_buffer_seek (ap_prc->p_mp4_store_, 0, TIZ_BUFFER_SEEK_SET);
          ap_prc->mp4v2_failed_init_count_ += 1;
          rc = OMX_ErrorNotReady;
          TIZ_ERROR (handleOf (ap_prc),
                     "[OMX_ErrorNotReady] on MP4ReadProvider");
        }
      else
        {
          rc = send_port_auto_detect_events (ap_prc);
          ap_prc->mp4v2_inited_ = true;
        }
      TIZ_DEBUG (handleOf (ap_prc), "mp4v2 inited = %s",
                 (ap_prc->mp4v2_inited_ ? "TRUE" : "FALSE"));
    }
  return rc;
}

static void
reset_mp4v2_members (mp4dmuxflt_prc_t * ap_prc)
{
  assert (ap_prc);
  assert (!MP4_IS_VALID_FILE_HANDLE (ap_prc->mp4v2_hdl_));
  ap_prc->mp4v2_inited_ = false;
  ap_prc->mp4v2_duration_ = 0;
  ap_prc->track_type_ = mp4_track_unknown;
  ap_prc->audio_type_ = mp4_audio_unknown;
  ap_prc->video_type_ = mp4_video_unknown;
}

static void
reset_stream_parameters (mp4dmuxflt_prc_t * ap_prc)
{
  assert (ap_prc);
  TIZ_DEBUG (handleOf (ap_prc), "Resetting stream parameters");
  ap_prc->audio_metadata_delivered_ = false;
  ap_prc->video_metadata_delivered_ = false;
  ap_prc->audio_auto_detect_on_ = false;
  ap_prc->audio_coding_type_ = OMX_AUDIO_CodingUnused;
  ap_prc->video_auto_detect_on_ = false;
  ap_prc->video_coding_type_ = OMX_VIDEO_CodingUnused;

  dealloc_mp4v2 (ap_prc);
  reset_mp4v2_members (ap_prc);
  ap_prc->mp4v2_failed_init_count_ = 0;

  tiz_buffer_clear (ap_prc->p_mp4_store_);
  tiz_buffer_clear (ap_prc->p_aud_store_);
  tiz_buffer_clear (ap_prc->p_vid_store_);
  tiz_vector_clear (ap_prc->p_aud_header_lengths_);
  tiz_vector_clear (ap_prc->p_vid_header_lengths_);

  tiz_filter_prc_update_eos_flag (ap_prc, false);
}

static inline void
dealloc_input_store (
  /*@special@ */ mp4dmuxflt_prc_t * ap_prc)
/*@releases ap_prc->p_mp4_store_@ */
/*@ensures isnull ap_prc->p_mp4_store_@ */
{
  assert (ap_prc);
  tiz_buffer_destroy (ap_prc->p_mp4_store_);
  ap_prc->p_mp4_store_ = NULL;
}

static inline void
dealloc_output_stores (
  /*@special@ */ mp4dmuxflt_prc_t * ap_prc)
/*@releases ap_prc->p_mp4_store_@ */
/*@ensures isnull ap_prc->p_mp4_store_@ */
{
  assert (ap_prc);
  tiz_buffer_destroy (ap_prc->p_aud_store_);
  ap_prc->p_aud_store_ = NULL;
  tiz_buffer_destroy (ap_prc->p_vid_store_);
  ap_prc->p_vid_store_ = NULL;
  tiz_vector_destroy (ap_prc->p_aud_header_lengths_);
  ap_prc->p_aud_header_lengths_ = NULL;
  tiz_vector_destroy (ap_prc->p_vid_header_lengths_);
  ap_prc->p_vid_header_lengths_ = NULL;
}

static OMX_ERRORTYPE
set_audio_coding_on_port (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  TIZ_INIT_OMX_PORT_STRUCT (port_def,
                            ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX);
  tiz_check_omx (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &port_def));

  /* Set the new value */
  port_def.format.audio.eEncoding = ap_prc->audio_coding_type_;

  tiz_check_omx (tiz_krn_SetParameter_internal (
    tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
    OMX_IndexParamPortDefinition, &port_def));

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
set_video_coding_on_port (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  assert (ap_prc);

  TIZ_INIT_OMX_PORT_STRUCT (port_def,
                            ARATELIA_MP4_DEMUXER_FILTER_PORT_2_INDEX);
  tiz_check_omx (
    tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &port_def));

  /* Set the new value */
  port_def.format.video.eCompressionFormat = ap_prc->video_coding_type_;

  tiz_check_omx (tiz_krn_SetParameter_internal (
    tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
    OMX_IndexParamPortDefinition, &port_def));

  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
read_audio_codec_metadata (mp4dmuxflt_prc_t * ap_prc,
                           const MP4TrackId a_track_id,
                           const int a_track_type)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (ap_prc);

  if (mp4_track_audio == a_track_type)
    {
      mp4_audio_type_t audio_type = mp4_audio_unknown;
      uint32_t time_scale = 0;
      MP4Duration track_duration = 0;
      double ms_duration = .0;
      uint32_t avg_bitrate = 0;
      char * p_track_nfo = mp4_get_audio_info (
        ap_prc->mp4v2_hdl_, a_track_id, &audio_type, &time_scale,
        &track_duration, &ms_duration, &avg_bitrate);
      tiz_mem_free(p_track_nfo);
      p_track_nfo = NULL;

      switch(audio_type)
        {
        case mp4_audio_mp3:
          {
            ap_prc->audio_coding_type_ = OMX_AUDIO_CodingMP3;
          }
          break;
        case mp4_audio_aac:
        case mp4_audio_aac_from_mov:
          {
            ap_prc->audio_coding_type_ = OMX_AUDIO_CodingAAC;
          }
          break;
        case mp4_audio_amr:
        case mp4_audio_amrwb:
          {
            ap_prc->audio_coding_type_ = OMX_AUDIO_CodingAMR;
          }
          break;
        default:
          ap_prc->audio_coding_type_ = OMX_AUDIO_CodingUnused;
          break;
        };

      /* Do nothing if track type is not audio */
      tiz_check_omx (set_audio_coding_on_port (ap_prc));
    }
  return rc;
}

static OMX_ERRORTYPE
read_video_codec_metadata (mp4dmuxflt_prc_t * ap_prc,
                           const unsigned int a_track_id,
                           const int a_track_type)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  assert (ap_prc);

  /* Do nothing if track type is not video */
  tiz_check_omx (set_video_coding_on_port (ap_prc));
  return rc;
}

static OMX_ERRORTYPE
obtain_track_info (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  MP4TrackId track_id = MP4_INVALID_TRACK_ID;
  uint32_t i = 0;
  uint32_t ntracks = 0;

  assert (ap_prc);
  assert (ap_prc->mp4v2_hdl_ != MP4_INVALID_FILE_HANDLE);

  /* Obtain the number of tracks */
  ntracks = MP4GetNumberOfTracks (ap_prc->mp4v2_hdl_, NULL, 0);

  for (i = 0; i < ntracks; ++i)
    {
      mp4_track_type_t track_type = mp4_track_unknown;

      track_id = MP4FindTrackId (ap_prc->mp4v2_hdl_, i, NULL, 0);
      track_type = get_track_type (ap_prc, ap_prc->mp4v2_hdl_, track_id);

#ifndef NDEBUG
      print_track_info (ap_prc, track_id);
#endif

      tiz_check_omx (
        read_audio_codec_metadata (ap_prc, track_id, track_type));
      tiz_check_omx (
        read_video_codec_metadata (ap_prc, track_id, track_type));
    }
  return rc;
}

static void
send_auto_detect_event (mp4dmuxflt_prc_t * ap_prc, OMX_S32 * ap_coding_type,
                        const OMX_S32 a_coding_type1,
                        const OMX_S32 a_coding_type2, const OMX_U32 a_pid)
{
  assert (ap_prc);
  assert (ap_coding_type);
  if (*ap_coding_type != a_coding_type1 && *ap_coding_type != a_coding_type2)
    {
      TIZ_DEBUG (
        handleOf (ap_prc),
        "Issuing OMX_EventPortFormatDetected - audio_coding_type_ [%X]",
        *ap_coding_type);

      /* TODO: update the output port with the corresponding audio or video settings detected */
      tiz_srv_issue_event ((OMX_PTR) ap_prc, OMX_EventPortFormatDetected, 0, 0,
                           NULL);
      tiz_srv_issue_event ((OMX_PTR) ap_prc, OMX_EventPortSettingsChanged,
                           a_pid,
                           OMX_IndexParamPortDefinition, /* the index of the
                                                      struct that has
                                                      been modififed */
                           NULL);
    }
  else
    {
      /* Oops... could not detect the stream format */
      tiz_srv_issue_err_event ((OMX_PTR) ap_prc, OMX_ErrorFormatNotDetected);
    }
}

static OMX_ERRORTYPE
send_port_auto_detect_events (mp4dmuxflt_prc_t * ap_prc)
{
  OMX_ERRORTYPE rc = obtain_track_info (ap_prc);
  if (OMX_ErrorNone == rc)
    {
      if (mp4_audio_unknown != ap_prc->audio_type_)
        {
          send_auto_detect_event (ap_prc, &(ap_prc->audio_coding_type_),
                                  OMX_AUDIO_CodingUnused,
                                  OMX_AUDIO_CodingAutoDetect,
                                  ARATELIA_MP4_DEMUXER_FILTER_PORT_1_INDEX);
        }
      if (mp4_video_unknown != ap_prc->video_type_)
        {
          send_auto_detect_event (ap_prc, &(ap_prc->video_coding_type_),
                                  OMX_VIDEO_CodingUnused,
                                  OMX_VIDEO_CodingAutoDetect,
                                  ARATELIA_MP4_DEMUXER_FILTER_PORT_2_INDEX);
        }
    }
  return rc;
}

static inline OMX_ERRORTYPE
do_flush (mp4dmuxflt_prc_t * ap_prc, OMX_U32 a_pid)
{
  assert (ap_prc);
  if (OMX_ALL == a_pid || ARATELIA_MP4_DEMUXER_FILTER_PORT_0_INDEX == a_pid)
    {
      reset_stream_parameters (ap_prc);
    }
  /* Release any buffers held  */
  return tiz_filter_prc_release_header (ap_prc, a_pid);
}

/*
 * mp4dmuxfltprc
 */

static void *
mp4dmuxflt_prc_ctor (void * ap_prc, va_list * app)
{
  mp4dmuxflt_prc_t * p_prc
    = super_ctor (typeOf (ap_prc, "mp4dmuxfltprc"), ap_prc, app);
  assert (p_prc);
  p_prc->tmp_fd_1_ = -1;
  p_prc->tmp_fd_2_ = -1;
  p_prc->mp4v2_hdl_ = MP4_INVALID_FILE_HANDLE;
  p_prc->mp4v2_inited_ = false;
  p_prc->mp4v2_duration_ = 0;
  p_prc->p_mp4_store_ = NULL;
  p_prc->p_aud_store_ = NULL;
  p_prc->p_vid_store_ = NULL;
  p_prc->p_aud_header_lengths_ = NULL;
  p_prc->p_vid_header_lengths_ = NULL;
  reset_stream_parameters (p_prc);
  MP4SetLogCallback(mp4_log_cback);
  gp_prc = ap_prc;
  return p_prc;
}

static void *
mp4dmuxflt_prc_dtor (void * ap_obj)
{
  (void) mp4dmuxflt_prc_deallocate_resources (ap_obj);
  gp_prc = NULL;
  return super_dtor (typeOf (ap_obj, "mp4dmuxfltprc"), ap_obj);
}

/*
 * from tizsrv class
 */

static OMX_ERRORTYPE
mp4dmuxflt_prc_allocate_resources (void * ap_prc, OMX_U32 a_pid)
{
  OMX_ERRORTYPE rc = OMX_ErrorNone;
  mp4dmuxflt_prc_t * p_prc = ap_prc;
  assert (p_prc);
  tiz_check_omx (alloc_input_store (p_prc));
  tiz_check_omx (alloc_output_stores (p_prc));
  return rc;
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_deallocate_resources (void * ap_prc)
{
  mp4dmuxflt_prc_t * p_prc = ap_prc;
  assert (p_prc);
  dealloc_output_stores (p_prc);
  dealloc_input_store (p_prc);
  dealloc_mp4v2 (p_prc);
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_prepare_to_transfer (void * ap_prc, OMX_U32 a_pid)
{
  mp4dmuxflt_prc_t * p_prc = ap_prc;
  assert (ap_prc);
  return prepare_port_auto_detection (p_prc);
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_transfer_and_process (void * ap_prc, OMX_U32 a_pid)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_stop_and_return (void * ap_prc)
{
  /* Do flush on all ports; this will reset the stream parameters and release
     any buffers held */
  return do_flush (ap_prc, OMX_ALL);
}

/*
 * from tizprc class
 */

static OMX_ERRORTYPE
mp4dmuxflt_prc_buffers_ready (const void * ap_prc)
{
  mp4dmuxflt_prc_t * p_prc = (mp4dmuxflt_prc_t *) ap_prc;
  OMX_ERRORTYPE rc = OMX_ErrorNone;

  assert (p_prc);

  TIZ_TRACE(handleOf(ap_prc), "buffer ready");

  tiz_check_omx (store_data (p_prc));

  if (!p_prc->mp4v2_inited_)
    {
      rc = alloc_mp4v2 (p_prc);
      if (OMX_ErrorNotReady == rc)
        {
          if (MP4V2_INT_MAX_FAILED_ATTEMPTS > p_prc->mp4v2_failed_init_count_)
            {
              /* Need to wait for more stream data to be able to initialise the
                 mp4v2 object */
              rc = OMX_ErrorNone;
            }
          else
            {
              /* It's time to give up */
              rc = OMX_ErrorStreamCorruptFatal;
            }
        }
    }

  return rc;
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_pause (const void * ap_obj)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_resume (const void * ap_obj)
{
  return OMX_ErrorNone;
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_port_flush (const void * ap_prc, OMX_U32 a_pid)
{
  mp4dmuxflt_prc_t * p_prc = (mp4dmuxflt_prc_t *) ap_prc;
  return do_flush (p_prc, a_pid);
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_port_disable (const void * ap_prc, OMX_U32 a_pid)
{
  mp4dmuxflt_prc_t * p_prc = (mp4dmuxflt_prc_t *) ap_prc;
  OMX_ERRORTYPE rc = tiz_filter_prc_release_header (p_prc, a_pid);
  if (OMX_ALL == a_pid || ARATELIA_MP4_DEMUXER_FILTER_PORT_0_INDEX == a_pid)
    {
      reset_stream_parameters (p_prc);
    }
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, true);
  return rc;
}

static OMX_ERRORTYPE
mp4dmuxflt_prc_port_enable (const void * ap_prc, OMX_U32 a_pid)
{
  mp4dmuxflt_prc_t * p_prc = (mp4dmuxflt_prc_t *) ap_prc;
  tiz_filter_prc_update_port_disabled_flag (p_prc, a_pid, false);
  return OMX_ErrorNone;
}

/*
 * mp4dmuxflt_prc_class
 */

static void *
mp4dmuxflt_prc_class_ctor (void * ap_prc, va_list * app)
{
  /* NOTE: Class methods might be added in the future. None for now. */
  return super_ctor (typeOf (ap_prc, "mp4dmuxfltprc_class"), ap_prc, app);
}

/*
 * initialization
 */

void *
mp4dmuxflt_prc_class_init (void * ap_tos, void * ap_hdl)
{
  void * tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void * mp4dmuxfltprc_class = factory_new
    /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
    (classOf (tizfilterprc), "mp4dmuxfltprc_class", classOf (tizfilterprc),
     sizeof (mp4dmuxflt_prc_class_t),
     /* TIZ_CLASS_COMMENT: */
     ap_tos, ap_hdl,
     /* TIZ_CLASS_COMMENT: class constructor */
     ctor, mp4dmuxflt_prc_class_ctor,
     /* TIZ_CLASS_COMMENT: stop value*/
     0);
  return mp4dmuxfltprc_class;
}

void *
mp4dmuxflt_prc_init (void * ap_tos, void * ap_hdl)
{
  void * tizfilterprc = tiz_get_type (ap_hdl, "tizfilterprc");
  void * mp4dmuxfltprc_class = tiz_get_type (ap_hdl, "mp4dmuxfltprc_class");
  TIZ_LOG_CLASS (mp4dmuxfltprc_class);
  void * mp4dmuxfltprc = factory_new
    /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
    (mp4dmuxfltprc_class, "mp4dmuxfltprc", tizfilterprc,
     sizeof (mp4dmuxflt_prc_t),
     /* TIZ_CLASS_COMMENT: */
     ap_tos, ap_hdl,
     /* TIZ_CLASS_COMMENT: class constructor */
     ctor, mp4dmuxflt_prc_ctor,
     /* TIZ_CLASS_COMMENT: class destructor */
     dtor, mp4dmuxflt_prc_dtor,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_allocate_resources, mp4dmuxflt_prc_allocate_resources,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_deallocate_resources, mp4dmuxflt_prc_deallocate_resources,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_prepare_to_transfer, mp4dmuxflt_prc_prepare_to_transfer,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_transfer_and_process, mp4dmuxflt_prc_transfer_and_process,
     /* TIZ_CLASS_COMMENT: */
     tiz_srv_stop_and_return, mp4dmuxflt_prc_stop_and_return,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_buffers_ready, mp4dmuxflt_prc_buffers_ready,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_pause, mp4dmuxflt_prc_pause,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_resume, mp4dmuxflt_prc_resume,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_flush, mp4dmuxflt_prc_port_flush,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_disable, mp4dmuxflt_prc_port_disable,
     /* TIZ_CLASS_COMMENT: */
     tiz_prc_port_enable, mp4dmuxflt_prc_port_enable,
     /* TIZ_CLASS_COMMENT: stop value */
     0);

  return mp4dmuxfltprc;
}
