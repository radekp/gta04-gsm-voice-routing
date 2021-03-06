/*
 * GTA04 gsm voice routing utility
 * Copyright (c) 2012 Radek Polak
 *
 * gta04-gsm-voice-routing is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * gta04-gsm-voice-routing is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with gta04-gsm-voice-routing; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*

This program routes sound between GTA04 internal sound card ("default") and
and UMTS modem sound card ("hw:1,0")

The function can be written with following shell script:

arecord -fS16_LE | aplay -Dhw:1,0 &
arecord -Dhw:1,0 -fS16_LE | aplay

However the program is written in C for better control over the process. E.g.
we want to wait with routing until sound is available from UMTS.

We have 4 streams called r0, p1, r1, p0

r0 - record from hw:0,0 (default) internal sound card
r1 - record from hw:1,0 umts sound card
p0 - play on hw:0,0 (default) internal sound card
p1 - play on hw:1,0 umts sound card

All streams have rate 8000 (rate of umts sound card), 1 channel and 16bit per
sample (SND_PCM_FORMAT_S16_LE).

We set buffer_size of sound card to 1024.
The sound card buffer consists of 4 periods.
Period has 256 frames
Frame has just one sample (one channel) and both sample and frame are 2 bytes

We always play/record one period (256 samples, 512 bytes). At rate 8000Hz if
we had 8000 samples it would be 1s, with 256 samples it makes 31.25ms long
period - this is our latency.

*/

/* Use the newer ALSA API */
#define ALSA_PCM_NEW_HW_PARAMS_API

/* Define this to enable speex acoustic echo cancellation */
#define USE_SPEEX_AEC

/* Define this to use walkie talkie echo reduction */
//#define USE_WALKIE_TALKIE_AEC

#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <alsa/asoundlib.h>

#ifdef USE_SPEEX_AEC
#include <speex/speex_echo.h>
#endif

#define ERR_PCM_OPEN_FAILED -1
#define ERR_HW_PARAMS_ANY -2
#define ERR_HW_PARAMS_SET_ACCESS -3
#define ERR_HW_PARAMS_SET_FORMAT -4
#define ERR_HW_PARAMS_SET_CHANNELS -5
#define ERR_HW_PARAMS_SET_RATE -6
#define ERR_SW_PARAMS_CURRENT -7
#define ERR_HW_PARAMS_SET_PERIOD_SIZE -8
#define ERR_HW_PARAMS_SET_BUFFER_SIZE -9
#define ERR_HW_PARAMS -10
#define ERR_BUFFER_ALLOC_FAILED -11
#define ERR_SW_PARAMS_SET_START_THRESHOLD -12
#define ERR_SW_PARAMS_SET_STOP_THRESHOLD -13
#define ERR_SW_PARAMS -14
#define ERR_READ_OVERRUN -15
#define ERR_READ -16
#define ERR_SHORT_READ -17
#define ERR_WRITE_UNDERRUN -18
#define ERR_WRITE -19
#define ERR_SHORT_WRITE -20
#define ERR_TERMINATING -21

#define s16 short
#define u16 unsigned short

FILE *logfile;
int terminating = 0;

struct route_stream
{
    const char *id;             // in: one of r0, r1, p0, p1
    char *pcm_name;             // in: "default" or "hw:1,0"
    snd_pcm_stream_t stream;    // in: SND_PCM_STREAM_PLAYBACK / SND_PCM_STREAM_CAPTURE
    snd_pcm_uframes_t start_threshold;  // in: start treshold or 0 to keep default
    snd_pcm_uframes_t stop_threshold;   // in: stop treshold or 0 to keep default
    snd_pcm_uframes_t buffer_size;  // in/out: hw buffer size, e.g. 1024 frames
    snd_pcm_uframes_t period_size;  // in/out: period size, e.g. 256 frames

    snd_pcm_t *handle;          // out: pcm handle
    snd_pcm_hw_params_t *hwparams;  // out:
    snd_pcm_sw_params_t *swparams;  // out:
    int period_buffer_size;     // out: size 2000 (256 frames=256 samples, one sample=2bytes)
    char *period_buffer;        // out: allocated buffer for playing/recording
};

/* Dump error on stderr with stream and error description, and return given
   return_code */
static int err(const char *msg, int snd_err, struct route_stream *s,
               int return_code)
{
    if(terminating) {
        return ERR_TERMINATING;
    }
    
    fprintf(logfile, "%s (%s): %s", s->id, s->pcm_name, msg);
    if (snd_err < 0) {
        fprintf(logfile, ": %s", snd_strerror(snd_err));
    }
    fprintf(logfile, "\n");
    return return_code;
}

static int open_route_stream(struct route_stream *s)
{
    int rc;

    /* Open PCM device for playback. */
    rc = snd_pcm_open(&(s->handle), s->pcm_name, s->stream, 0);
    if (rc < 0) {
        return err("unable to open pcm device", rc, s, ERR_PCM_OPEN_FAILED);
    }

    /* Allocate a hardware parameters object. */
    snd_pcm_hw_params_alloca(&(s->hwparams));

    /* Fill it in with default values. */
    rc = snd_pcm_hw_params_any(s->handle, s->hwparams);
    if (rc < 0) {
        return err("snd_pcm_hw_params_any failed", rc, s, ERR_HW_PARAMS_ANY);
    }

    /* Interleaved mode */
    rc = snd_pcm_hw_params_set_access(s->handle, s->hwparams,
                                      SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        return err("snd_pcm_hw_params_set_access failed", rc, s,
                   ERR_HW_PARAMS_SET_ACCESS);
    }

    /* Signed 16-bit little-endian format */
    rc = snd_pcm_hw_params_set_format(s->handle, s->hwparams,
                                      SND_PCM_FORMAT_S16_LE);
    if (rc < 0) {
        return err("snd_pcm_hw_params_set_format failed", rc, s,
                   ERR_HW_PARAMS_SET_FORMAT);
    }

    /* One channel (mono) */
    rc = snd_pcm_hw_params_set_channels(s->handle, s->hwparams, 1);
    if (rc < 0) {
        return err("snd_pcm_hw_params_set_channels failed", rc, s,
                   ERR_HW_PARAMS_SET_CHANNELS);
    }

    /* 8000 bits/second sampling rate (umts modem quality) */
    rc = snd_pcm_hw_params_set_rate(s->handle, s->hwparams, 8000, 0);
    if (rc < 0) {
        return err("snd_pcm_hw_params_set_rate_near failed", rc, s,
                   ERR_HW_PARAMS_SET_RATE);
    }

    /* Period size in frames (e.g. 256) */
    rc = snd_pcm_hw_params_set_period_size(s->handle, s->hwparams,
                                           s->period_size, 0);
    if (rc < 0) {
        return err("snd_pcm_hw_params_set_period_size failed", rc, s,
                   ERR_HW_PARAMS_SET_PERIOD_SIZE);
    }

    /* Buffer size in frames (e.g. 1024) */
    rc = snd_pcm_hw_params_set_buffer_size(s->handle, s->hwparams,
                                           s->buffer_size);
    if (rc < 0) {
        return err("snd_pcm_hw_params_set_buffer_size failed", rc, s,
                   ERR_HW_PARAMS_SET_BUFFER_SIZE);
    }

    /* Write the parameters to the driver */
    rc = snd_pcm_hw_params(s->handle, s->hwparams);
    if (rc < 0) {
        return err("snd_pcm_hw_params failed", rc, s, ERR_HW_PARAMS);
    }

    /* Allocate buffer for one period twice as big as period_size because:
       1 frame = 1 sample = 2 bytes because of S16_LE and 1 channel */
    s->period_buffer_size = 2 * s->period_size;
    s->period_buffer = (char *)malloc(s->period_buffer_size);
    if (s->period_buffer == 0) {
        return err("period_buffer alloc failed", 0, s, ERR_BUFFER_ALLOC_FAILED);
    }

    /* Setup software params */
    if (s->start_threshold > 0 || s->stop_threshold > 0) {
        snd_pcm_sw_params_alloca(&(s->swparams));

        rc = snd_pcm_sw_params_current(s->handle, s->swparams);
        if (rc < 0) {
            return err("snd_pcm_sw_params_current failed", rc, s,
                       ERR_SW_PARAMS_CURRENT);
        }

        /* start_threshold */
        if (s->start_threshold > 0) {
            rc = snd_pcm_sw_params_set_start_threshold(s->handle,
                                                       s->swparams,
                                                       s->start_threshold);
            if (rc < 0) {
                return err("snd_pcm_sw_params_set_start_threshold failed", rc,
                           s, ERR_SW_PARAMS_SET_START_THRESHOLD);
            }
        }

        /* stop_threshold */
        if (s->stop_threshold > 0) {
            rc = snd_pcm_sw_params_set_stop_threshold(s->handle,
                                                      s->swparams,
                                                      s->stop_threshold);
            if (rc < 0) {
                return err("snd_pcm_sw_params_set_start_threshold failed", rc,
                           s, ERR_SW_PARAMS_SET_STOP_THRESHOLD);
            }
        }

        rc = snd_pcm_sw_params(s->handle, s->swparams);
        if (rc < 0) {
            return err("snd_pcm_sw_params failed", rc, s, ERR_SW_PARAMS);
        }
    }

    /* Uncomment to dump hw setup */
    /* static snd_output_t *log;
       snd_output_stdio_attach(&log, logfile, 0);
       snd_pcm_dump(s->handle, log);
       snd_output_close(log); */

    return 0;
}

static int close_route_stream(struct route_stream *s)
{
    if (s->handle == 0) {
        return 0;
    }
    snd_pcm_close(s->handle);
    s->handle = 0;
    if (s->period_buffer == 0) {
        return 0;
    }
    free(s->period_buffer);
    s->period_buffer = 0;
    return 0;
}

static void open_route_stream_repeated(struct route_stream *s)
{
    int rc;
    for (;;) {
        rc = open_route_stream(s);
        if (rc == 0) {
            return;
        }
        close_route_stream(s);
        fprintf(logfile, "retrying in 100 ms\n");
        usleep(1000 * 100);
    }
}

static int route_stream_read(struct route_stream *s)
{
    int rc;

    if (terminating) {
        return ERR_TERMINATING;
    }

    rc = snd_pcm_readi(s->handle, s->period_buffer, s->period_size);
    if (rc == s->period_size) {
        return 0;
    }

    /* EPIPE means overrun */
    if (rc == -EPIPE) {
        err("overrun occured", rc, s, ERR_READ_OVERRUN);
        snd_pcm_prepare(s->handle);
        return ERR_READ_OVERRUN;
    }

    if (rc < 0) {
        return err("snd_pcm_readi failed", rc, s, ERR_READ);
    }

    return err("short read", rc, s, ERR_SHORT_READ);
}

static int route_stream_write(struct route_stream *s)
{
    int rc;

    if (terminating) {
        return ERR_TERMINATING;
    }

    rc = snd_pcm_writei(s->handle, s->period_buffer, s->period_size);
    if (rc == s->period_size) {
        return 0;
    }

    /* EPIPE means underrun */
    if (rc == -EPIPE) {
        err("underrun occured", rc, s, ERR_WRITE_UNDERRUN);
        snd_pcm_prepare(s->handle);
        return ERR_WRITE_UNDERRUN;
    }

    if (rc < 0) {
        return err("snd_pcm_writei failed", rc, s, ERR_WRITE);
    }

    return err("short write", rc, s, ERR_SHORT_WRITE);
}

/*static void log_with_timestamp(const char *msg)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    fprintf(logfile, "%ld %ld: %s\n", tp.tv_sec, tp.tv_nsec, msg);
}*/

// Write string count bytes long to file
static void write_file(const char *path, const char *value, size_t count)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return;
    }
    write(fd, value, count);
    close(fd);
}

int aux_red_state = 0;
int aux_green_state = 0;

static void set_aux_leds(int red, int green)
{
    if (aux_red_state == red && aux_green_state == green) {
        return;
    }
    aux_red_state = red;
    aux_green_state = green;

    if (red) {
        write_file("/sys/class/leds/gta04:red:aux/brightness", "255", 3);
    } else {
        write_file("/sys/class/leds/gta04:red:aux/brightness", "0", 1);
    }
    if (green) {
        write_file("/sys/class/leds/gta04:green:aux/brightness", "255", 3);
    } else {
        write_file("/sys/class/leds/gta04:green:aux/brightness", "0", 1);
    }
}

static void blink_aux()
{
    static int last_blink_secs;

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);

    if (tp.tv_sec == last_blink_secs) {
        return;
    }
    last_blink_secs = tp.tv_sec;
    set_aux_leds(!aux_red_state, aux_red_state);
}

static void show_progress()
{
/*   static int counter = 0;
    char ch = "|\\-/"[(counter++) % 4];
    fputc(ch, logfile);
    fputc('\b', logfile);
    fflush(logfile);*/
}

#ifdef USE_WALKIE_TALKIE_AEC

static void vol_up(char *buf, snd_pcm_uframes_t period_size)
{
    int i;
    s16 *val = (s16 *) (buf);
    for (i = 0; i < period_size; i++) {
        *val = *val * 2;
        val++;
    }
}

static void vol_down(char *buf, snd_pcm_uframes_t period_size)
{
    int i;
    s16 *val = (s16 *) (buf);
    for (i = 0; i < period_size; i++) {
        *val = 0;               // *val / 2;
        val++;
    }
}

/* Reduce echo by adjusting volumes in record and playback buffer with simple
   walkie-talkie like algorithm.

   First we decide which buffer has higher volume (i.e. if you are speaking or
   listening). This buffer is kept as is and we made the other buffer silent or
   very low volume.
   
   We use integer for computing volumes, this should be fine if the buffer size
   is not too big (65535 is max).
*/
static void reduce_echo(char *p0, char *p1, snd_pcm_uframes_t period_size)
{
    int i;
    int sum_p0 = 0;
    int sum_p1 = 0;
    s16 *v0;
    s16 *v1;

    v0 = (s16 *) (p0);
    v1 = (s16 *) (p1);
    for (i = 0; i < period_size; i++) {
        sum_p0 += abs(*v0);
        sum_p1 += abs(*v1);
        v0++;
        v1++;
    }

    int diff = sum_p0 - sum_p1;

    /* 10000 seems to be good limit value. Silence is ~2000 and speech is
       ~80000. It would be nice to somehow compute this value on the fly, but
       i have no idea how to do it now */
    if (diff > 10000) {
        /* fprintf(stderr, "listening p0=%*d    p1=%*d    diff=%*d\n", -6, sum_p0,
           -6, sum_p1, -6, diff); */
        vol_up(p0, period_size);
        vol_down(p1, period_size);
        set_aux_leds(0, 1);
        return;

    } else if (diff < -10000) {
        /* fprintf(stderr, "talking   p0=%*d    p1=%*d    diff=%*d\n", -6, sum_p0,
           -6, sum_p1, -6, diff); */
        vol_up(p1, period_size);
        vol_down(p0, period_size);
        set_aux_leds(1, 0);
    } else {
        set_aux_leds(0, 0);
    }
    //printf("%d, %d, %d\n", round++, sum_a, sum_b);
}
#endif

struct route_stream p0 = {
    .id = "p0",
    .pcm_name = "default",
    .stream = SND_PCM_STREAM_PLAYBACK,
    .start_threshold = 1024,
    .stop_threshold = 1024,
    .buffer_size = 1024,
    .period_size = 256,
    .handle = 0,
    .period_buffer = 0
};

struct route_stream r0 = {
    .id = "r0",
    .pcm_name = "default",
    .stream = SND_PCM_STREAM_CAPTURE,
    .start_threshold = 0,
    .stop_threshold = 0,
    .buffer_size = 1024,
    .period_size = 256,
    .handle = 0,
    .period_buffer = 0
};

struct route_stream p1 = {
    .id = "p1",
    .pcm_name = "hw:1,0",
    .stream = SND_PCM_STREAM_PLAYBACK,
    .start_threshold = 1024,
    .stop_threshold = 1024,
    .buffer_size = 1024,
    .period_size = 256,
    .handle = 0,
    .period_buffer = 0
};

struct route_stream r1 = {
    .id = "r1",
    .pcm_name = "hw:1,0",
    .stream = SND_PCM_STREAM_CAPTURE,
    .start_threshold = 0,
    .stop_threshold = 0,
    .buffer_size = 1024,
    .period_size = 256,
    .handle = 0,
    .period_buffer = 0
};

static void cleanup()
{
    close_route_stream(&p0);
    close_route_stream(&p1);
    close_route_stream(&r0);
    close_route_stream(&r1);
    
    set_aux_leds(0, 0);
    fclose(logfile);
}

static void sighandler(int signum)
{
    if (terminating) {
        return;
    }
    terminating = 1;
    fprintf(logfile, "gsm-voice-routing ending - signal %d\n", signum);
    cleanup();
    exit(0);
}

int main()
{
    int rc;
    int started = 0;
    char *logfilename;
#ifdef USE_SPEEX_AEC
    SpeexEchoState *echo_state;
#endif

    // Register for TERM and interrupt signals
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    blink_aux();                // turn red led on so that we know we started

    logfile = stderr;
    logfilename = getenv("GSM_VOICE_ROUTING_LOGFILE");
    if (logfilename) {
        FILE *f = fopen(logfilename, "w");
        if (f) {
            logfile = f;
        } else {
            fprintf(stderr, "failed to open logfile %s\n", logfilename);
        }
    }
    fprintf(logfile, "gsm-voice-routing started\n");

    /* We want realtime process priority */
    rc = nice(-20);
    if (rc != -20) {
        fprintf(logfile, "nice() failed\n");
    }
#ifdef USE_SPEEX_AEC
    /* 256=frame (period size), 4096 is filter length (recommended is 1/3 of
       reverbation time - for 1s it's 8000 / 3 */
    echo_state = speex_echo_state_init(256, 8192);
#endif

    /* Open streams - umts first */
    open_route_stream_repeated(&p1);
    open_route_stream_repeated(&r1);
    open_route_stream_repeated(&p0);
    open_route_stream_repeated(&r0);

    /* Route sound */
    while (!terminating) {

        /* Recording  - first from internal card (so that we always clean the
           recording buffer), then UMTS, which can fail */
        if (route_stream_read(&r0)) {
            blink_aux();
            continue;
        }

        rc = route_stream_read(&r1);
        if (rc == ERR_READ && started) {
            fprintf(logfile,
                    "read error after some succesful routing (hangup)\n");
            break;
        }
        if (rc != 0) {
            continue;
        }

        if (started) {
            show_progress();
        } else {
            fprintf(logfile, "voice routing started\n");
            started = 1;
        }

#ifdef USE_SPEEX_AEC
        speex_echo_cancellation(echo_state, (spx_int16_t *) r0.period_buffer,
                                (spx_int16_t *) p0.period_buffer,
                                (spx_int16_t *) p1.period_buffer);

        memmove(p0.period_buffer, r1.period_buffer, r1.period_buffer_size);
#endif

#ifdef USE_WALKIE_TALKIE_AEC
        memmove(p0.period_buffer, r1.period_buffer, r1.period_buffer_size);
        memmove(p1.period_buffer, r0.period_buffer, r0.period_buffer_size);
        reduce_echo(p0.period_buffer, p1.period_buffer, p0.period_size);
#endif

        route_stream_write(&p0);
        route_stream_write(&p1);
    }

#ifdef USE_SPEEX_AEC
    speex_echo_state_destroy(echo_state);
#endif

    fprintf(logfile, "gsm-voice-routing ending\n");
    cleanup();
    return 0;
}
