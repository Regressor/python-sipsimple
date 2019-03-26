#include "pjmedia.h"
#include "pjlib.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <sys/stat.h> 
#include <fcntl.h>

#define THIS_FILE "pjs_pipe_port.c"
#define USEC_IN_SEC (pj_uint64_t)1000000

/* Struct attached to pipe port */
typedef struct
{
   pj_uint8_t *buffer;
   pj_size_t fsize;
   void *self;
   int alaw;
   pj_pool_t *pool;
   FILE *pipefd;   /* pipe fd to read */
   pj_oshandle_t wpipefd;
   pj_status_t (*cb)(pjmedia_port*, void*);
} port_data;

/*
 * Register a callback to be called when the file reading has reached the
 * end of file.
 */
PJ_DEF(pj_status_t) pjmedia_pipe_player_set_eof_cb( pjmedia_port *port,
			       void *user_data,
			       pj_status_t (*cb)(pjmedia_port *port,
  				   void *usr_data))
{
    port_data *data = NULL;
    PJ_ASSERT_RETURN(port, -PJ_EINVAL);
    data = port->port_data.pdata;
    data->self = user_data;
    data->cb = cb;
    return PJ_SUCCESS;
}

static int pipe_read_data(void *buffer, unsigned count, port_data *data)
{
    pj_size_t rcount = 0;
    pj_size_t rscount = 0;
    
    rcount = fread(buffer, 1, count, data->pipefd);
    while (rcount < count) {
        rscount = fread(buffer + rcount, 1, count - rcount, data->pipefd);
        if (rscount <= 0) return 0;
        rcount += rscount;
    }
    
    return rcount;
}


/* Get new frame from pipe */
static pj_status_t pipe_get_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    port_data *data = port->port_data.pdata;
    pj_int16_t *samples = frame->buf;
    pj_size_t count = 0;
    pj_size_t rcount = 0;
    count = frame->size; /* bytes in frame */

    if (data->buffer == NULL && data->alaw) {
       data->buffer = pj_pool_zalloc(data->pool, sizeof(frame->size) + 16);
    }

    PJ_ASSERT_RETURN(data != NULL && data->pipefd != NULL, PJ_EINVAL);

    /* Trying to read full audio frame from pipe */
    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    if (data->alaw) count = count / 2;
    
    rcount = pipe_read_data(data->buffer, count, data);
    if (rcount <= 0) {
        frame->type = PJMEDIA_FRAME_TYPE_NONE;
        frame->size = 0;

        if (data->cb)
	        (*data->cb)(port, data->self);
        
        return PJ_EEOF; /* pipe was closed and we got incomplete frame */
    }

    if (!data->alaw) {
        /* If PCM just move data to frame buffer */
	    pj_memcpy(samples, data->buffer, count);
    } 
    else 
    {
        /* If alaw convert data to frame buffer */
        for (int i=0; i<count; i++) {
            samples[i] = pjmedia_alaw2linear(data->buffer[i]);
        }
    }

    return PJ_SUCCESS;
}


/* EXPORTED: Creates new media port - read frames from pipe */
PJ_DEF(pj_status_t) pjmedia_pipe_player_port_create(pj_pool_t *pool, const char *filename, 
    int clock_rate, int channel_count, pjmedia_port **p_port, int alaw)
{
    pj_str_t name;
    unsigned bits_per_sample = 0;
    unsigned samples_per_frame = 0;
    unsigned frame_time_usec = 0;
    pj_uint32_t fmt_id = PJMEDIA_FORMAT_L16;
    unsigned avg_bps = 0;
    
    pjmedia_port_info *info;
    port_data *data = NULL;
    pjmedia_port *port = NULL;
    PJ_ASSERT_RETURN(channel_count > 0 && channel_count <= 2, 0);

    port = pj_pool_zalloc(pool, sizeof(pjmedia_port));
    PJ_ASSERT_RETURN(port, 0);

    info = &port->info;

    name = pj_str("pipe source");
    pj_bzero(info, sizeof(*info));

    info->signature = PJMEDIA_SIG_CLASS_PORT_AUD('s', 'i');
    info->dir = PJMEDIA_DIR_ENCODING_DECODING;
    info->name = name;

    bits_per_sample = 16;
    samples_per_frame = clock_rate * 20 / 1000 * channel_count;
    frame_time_usec = (unsigned)(samples_per_frame * USEC_IN_SEC / channel_count / clock_rate);
    avg_bps = clock_rate * channel_count * bits_per_sample;

    pjmedia_format_init_audio(&info->fmt, fmt_id, clock_rate,
			      channel_count, bits_per_sample, frame_time_usec,
			      avg_bps, avg_bps);

    port->get_frame = &pipe_get_frame;
    port->port_data.pdata = data = pj_pool_zalloc(pool, sizeof(port_data));
    data->pipefd = fopen(filename, "rb");
    data->alaw = alaw;
    data->pool = pool;
    data->buffer = NULL;
    if (data->pipefd == NULL) 
        return PJ_ENOTFOUND;

    *p_port = port;

    return PJ_SUCCESS;
}


/* EXPORTED: Deletes media port */
PJ_DEF(pj_status_t) pjmedia_pipe_player_port_destroy(pjmedia_port *p_port) 
{
    port_data *data = NULL; 
    FILE *fd = NULL;
    if (p_port == NULL) return PJ_EINVAL;
    data = p_port->port_data.pdata;
    if (data == NULL) return PJ_EINVAL;

    fd = data->pipefd;
    pjmedia_port_destroy(p_port);
    pj_thread_sleep(100);
    fclose(fd);

    return PJ_SUCCESS;
}


/* Put a frame into pipe (Can block. WILL block!) */
/* TODO: add a LARGE buffer to prevent block */
static pj_status_t wpipe_put_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    port_data *data = port->port_data.pdata;
    pj_int16_t *samples = frame->buf;
    pj_ssize_t fsize = 0;
    pj_ssize_t scount = 0;
    pj_status_t status = 0;

    PJ_ASSERT_RETURN(data != NULL && data->wpipefd != NULL, PJ_EINVAL);

    fsize = frame->size; /* bytes in frame */
    scount = fsize / 2;  /* 16 bit samples in frame */

    if (data->buffer == NULL) {
       data->buffer = pj_pool_zalloc(data->pool, data->fsize+16);
    }
    PJ_ASSERT_RETURN(data->buffer != NULL, PJ_ENOMEM);

    if (frame->type != PJMEDIA_FRAME_TYPE_AUDIO) return PJ_SUCCESS;
    if (!data->alaw) {
        /* If PCM just copy data to temp buffer */
	    pj_memcpy(data->buffer, frame->buf, fsize);
    }
    else 
    {
        /* If ALAW convert PCM data to temp buffer sample by sample */
        fsize = scount;
        for (int i=0; i<scount; i++)
            data->buffer[i] = pjmedia_linear2alaw(samples[i]);
    }

    /* Write temp buffer to pipe */
    status = pj_file_write(data->wpipefd, data->buffer, &fsize);
    printf("Wrote %d bytes to pipe\n", (int)fsize);
    return status;
}


/* Get frame, basicy is a no-op operation */
static pj_status_t wpipe_get_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    PJ_UNUSED_ARG(port);
    PJ_UNUSED_ARG(frame);
    return PJ_EINVALIDOP;
}

/* Close the port, modify file header with updated file length */
static pj_status_t wpipe_on_destroy(pjmedia_port *port)
{
    pj_status_t status;
    port_data *data = port->port_data.pdata;
    PJ_ASSERT_RETURN(data != NULL && data->wpipefd != NULL, PJ_EINVAL);
    printf("pipe_writer_port destroy called\n");
    status = pj_file_close(data->wpipefd);
	return status;
}


PJ_DEF(pj_status_t) pjmedia_pipe_writer_port_create( pj_pool_t *pool,
						     const char *filename,
						     unsigned sampling_rate,
						     unsigned channel_count,
						     unsigned samples_per_frame,
						     unsigned bits_per_sample,
						     unsigned alaw,
						     pj_ssize_t buff_size,
						     pjmedia_port **p_port )
{
    pj_str_t name;
    pj_status_t status;
    pjmedia_port *port;
    port_data *data = NULL;

    PJ_ASSERT_RETURN(pool && filename && p_port, PJ_EINVAL);
    PJ_ASSERT_RETURN(bits_per_sample == 16, PJ_EINVAL);

    /* Allocate memory for create media port instance */
    port = pj_pool_zalloc(pool, sizeof(pjmedia_port));
    PJ_ASSERT_RETURN(port != NULL, PJ_ENOMEM);

    /* Initialize media port info */
    pj_strdup2(pool, &name, filename);
    pjmedia_port_info_init(&port->info, &name, PJMEDIA_SIG_CLASS_PORT_AUD('s', 'i'),
			   sampling_rate, channel_count, bits_per_sample,
			   samples_per_frame);

    port->get_frame = &wpipe_get_frame;
    port->put_frame = &wpipe_put_frame;
    port->on_destroy = &wpipe_on_destroy;
    port->port_data.pdata = data = pj_pool_zalloc(pool, sizeof(port_data));
    PJ_ASSERT_RETURN(data != NULL, PJ_ENOMEM);

    /* Open pipe for writing */
    status = pj_file_open(pool, filename, PJ_O_WRONLY, &data->wpipefd);
    if (status != PJ_SUCCESS)
	return status;

    /* Init private data chunk */
    data->alaw = alaw;
    data->pool = pool;
    data->buffer = NULL;
    data->fsize = bits_per_sample * samples_per_frame;

    *p_port = port;
    PJ_LOG(4,(THIS_FILE, 
	      "Pipe writer '%.*s' opened pipe: samp.rate=%d, alaw?: %d",
	      (int)port->info.name.slen,
	      port->info.name.ptr,
	      PJMEDIA_PIA_SRATE(&port->info),
	      alaw));

    return PJ_SUCCESS;
}


