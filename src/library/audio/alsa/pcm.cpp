/*
    Copyright 2015-2018 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pcm.h"
#include "../../logging.h"
#include "../AudioContext.h"
#include "../AudioSource.h"
#include "../AudioBuffer.h"
#include <time.h> //nanosleep
#include "../../GlobalState.h"
#include "../../hook.h"
#include "../../DeterministicTimer.h"

namespace libtas {

static std::shared_ptr<AudioSource> sourceAlsa;
static int buffer_size = 4096; // in samples

DEFINE_ORIG_POINTER(snd_pcm_open);
DEFINE_ORIG_POINTER(snd_pcm_sw_params_current);
DEFINE_ORIG_POINTER(snd_pcm_sw_params);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_sizeof);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_any);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_access);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_format);

DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_rate);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_rate_near);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_rate_resample);

DEFINE_ORIG_POINTER(snd_pcm_hw_params_get_period_size);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_get_period_time_min);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_period_size_near);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_periods_near);

DEFINE_ORIG_POINTER(snd_pcm_hw_params_get_buffer_size);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_get_buffer_time_max);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_buffer_size_near);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_buffer_time_near);

DEFINE_ORIG_POINTER(snd_pcm_hw_params_get_channels);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_get_channels_max);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_set_channels);
DEFINE_ORIG_POINTER(snd_pcm_hw_params);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_malloc);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_free);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_copy);
DEFINE_ORIG_POINTER(snd_pcm_prepare);
DEFINE_ORIG_POINTER(snd_pcm_writei);
DEFINE_ORIG_POINTER(snd_pcm_readi);
DEFINE_ORIG_POINTER(snd_pcm_nonblock);
DEFINE_ORIG_POINTER(snd_pcm_close);

DEFINE_ORIG_POINTER(snd_pcm_mmap_begin);
DEFINE_ORIG_POINTER(snd_pcm_mmap_commit);

DEFINE_ORIG_POINTER(snd_pcm_start);
DEFINE_ORIG_POINTER(snd_pcm_resume);
DEFINE_ORIG_POINTER(snd_pcm_wait);
DEFINE_ORIG_POINTER(snd_pcm_delay);
DEFINE_ORIG_POINTER(snd_pcm_avail_update);
DEFINE_ORIG_POINTER(snd_pcm_hw_params_test_rate);
DEFINE_ORIG_POINTER(snd_pcm_sw_params_sizeof);
DEFINE_ORIG_POINTER(snd_pcm_sw_params_set_start_threshold);
DEFINE_ORIG_POINTER(snd_pcm_sw_params_set_avail_min);

DEFINE_ORIG_POINTER(snd_pcm_get_chmap);

static int get_latency();

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_open, nullptr);
        return orig::snd_pcm_open(pcm, name, stream, mode);
    }

    DEBUGLOGCALL(LCF_SOUND);

    if (stream != SND_PCM_STREAM_PLAYBACK) {
        debuglog(LCF_SOUND | LCF_ERROR, "    Unsupported stream direction ", stream);
        return -1;
    }
    game_info.audio |= GameInfo::ALSA;
    game_info.tosend = true;

    std::lock_guard<std::mutex> lock(audiocontext.mutex);

    /* We create an empty buffer that holds the audio parameters. That way,
     * we can guess the parameters from a previous buffer when adding a new one.
     */
    int bufferId = audiocontext.createBuffer();
    auto buffer = audiocontext.getBuffer(bufferId);

    /* Push buffer in the source */
    int sourceId = audiocontext.createSource();
    sourceAlsa = audiocontext.getSource(sourceId);

    sourceAlsa->buffer_queue.push_back(buffer);
    sourceAlsa->source = AudioSource::SOURCE_STREAMING;

    return 0;
}

int snd_pcm_close(snd_pcm_t *pcm)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_close, nullptr);
        return orig::snd_pcm_close(pcm);
    }

    DEBUGLOGCALL(LCF_SOUND);
    return 0;
}

int snd_pcm_nonblock(snd_pcm_t *pcm, int nonblock)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_nonblock, nullptr);
        return orig::snd_pcm_nonblock(pcm, nonblock);
    }

    debuglog(LCF_SOUND, __func__, " call with ", nonblock==0?"block":((nonblock==1)?"nonblock":"abort"), " mode");
    return 0;
}

int snd_pcm_start(snd_pcm_t *pcm)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_start, nullptr);
        return orig::snd_pcm_start(pcm);
    }

    DEBUGLOGCALL(LCF_SOUND);
    sourceAlsa->state = AudioSource::SOURCE_PLAYING;

    return 0;
}

int snd_pcm_resume(snd_pcm_t *pcm)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_resume, nullptr);
        return orig::snd_pcm_resume(pcm);
    }

    DEBUGLOGCALL(LCF_SOUND);
    sourceAlsa->state = AudioSource::SOURCE_PLAYING;

    return 0;
}

int snd_pcm_wait(snd_pcm_t *pcm, int timeout)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_wait, nullptr);
        return orig::snd_pcm_wait(pcm, timeout);
    }
    debuglog(LCF_SOUND, __func__, " called with timeout ", timeout);

    /* Check for no more available samples */
    int delta_ms = -1;
    if ((buffer_size - get_latency()) <= 0) {
        /* Wait for timeout or available samples */
        TimeHolder initial_time = detTimer.getTicks();
        do {
            struct timespec mssleep = {0, 1000*1000};
            NATIVECALL(nanosleep(&mssleep, NULL)); // Wait 1 ms before trying again
            TimeHolder delta_time = detTimer.getTicks();
            delta_time -= initial_time;
            delta_ms = delta_time.tv_sec * 1000 + delta_time.tv_nsec / 1000000;
        } while (!is_exiting && (get_latency() >= buffer_size) && ((timeout < 0) || (delta_ms < timeout)));
    }

    if ((buffer_size - get_latency()) > 0)
        return 1;

    /* Timeout */
    return 0;
}

int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_delay, nullptr);
        return orig::snd_pcm_delay(pcm, delayp);
    }

    DEBUGLOGCALL(LCF_SOUND);
    *delayp = get_latency();
    debuglog(LCF_SOUND, "   return ", *delayp);
    return 0;
}

snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_avail_update, nullptr);
        return orig::snd_pcm_avail_update(pcm);
    }

    DEBUGLOGCALL(LCF_SOUND);
    snd_pcm_sframes_t avail = buffer_size - get_latency();
    if (avail<0)
        avail = 0;
    debuglog(LCF_SOUND, "   return ", avail);
    return avail;
}

int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params, nullptr);
        return orig::snd_pcm_hw_params(pcm, params);
    }

    DEBUGLOGCALL(LCF_SOUND);

    /* Update internal buffer parameters */
    auto buffer = sourceAlsa->buffer_queue[0];
    buffer->size = 0;
    buffer->update();

    /* snd_pcm_hw_params calls snd_pcm_prepare, so we start playing here */
    sourceAlsa->state = AudioSource::SOURCE_PLAYING;

    return 0;
}

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_sw_params_current, nullptr);
        return orig::snd_pcm_sw_params_current(pcm, params);
    }

    DEBUGLOGCALL(LCF_SOUND);
    return 0;
}

int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_sw_params, nullptr);
        return orig::snd_pcm_sw_params(pcm, params);
    }

    DEBUGLOGCALL(LCF_SOUND);

    /* Update internal buffer parameters */
    auto buffer = sourceAlsa->buffer_queue[0];
    buffer->size = 0;
    buffer->update();

    /* snd_pcm_sw_params calls snd_pcm_prepare, so we start playing here */
    sourceAlsa->state = AudioSource::SOURCE_PLAYING;

    return 0;
}

int snd_pcm_prepare(snd_pcm_t *pcm)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_prepare, nullptr);
        return orig::snd_pcm_prepare(pcm);
    }

    DEBUGLOGCALL(LCF_SOUND);

    return 0;
}

static int get_latency()
{
    std::lock_guard<std::mutex> lock(audiocontext.mutex);
    return sourceAlsa->queueSize() - sourceAlsa->getPosition();
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_writei, nullptr);
        return orig::snd_pcm_writei(pcm, buffer, size);
    }

    debuglog(LCF_SOUND, __func__, " call with ", size, " frames");

    // return static_cast<snd_pcm_sframes_t>(size);

    /* Blocking if latency is too high */
    do {
        struct timespec mssleep = {0, 1000*1000};
        NATIVECALL(nanosleep(&mssleep, NULL)); // Wait 1 ms before trying again
    } while (!is_exiting && (get_latency() > buffer_size));

    if (is_exiting) return 0;

    std::lock_guard<std::mutex> lock(audiocontext.mutex);

    /* We try to reuse a buffer that has been processed from the source */
    std::shared_ptr<AudioBuffer> ab;
    if (sourceAlsa->nbQueueProcessed() > 0) {
        /* Removing first buffer */
        ab = sourceAlsa->buffer_queue[0];
        sourceAlsa->buffer_queue.erase(sourceAlsa->buffer_queue.begin());
        sourceAlsa->queue_index--;
    }
    else {
        /* Building a new buffer */
        int bufferId = audiocontext.createBuffer();
        ab = audiocontext.getBuffer(bufferId);

        /* Getting the parameters of the buffer from one of the queue */
        if (sourceAlsa->buffer_queue.empty()) {
            debuglog(LCF_SOUND | LCF_ERROR, "Empty queue, cannot guess buffer parameters");
            return -1;
        }

        auto ref = sourceAlsa->buffer_queue[0];
        ab->format = ref->format;
        ab->nbChannels = ref->nbChannels;
        ab->frequency = ref->frequency;
    }

    /* Filling buffer */
    ab->update(); // Compute alignSize
    ab->sampleSize = size;
    ab->size = size * ab->alignSize;
    ab->samples.clear();
    ab->samples.insert(ab->samples.end(), static_cast<const uint8_t*>(buffer), &(static_cast<const uint8_t*>(buffer))[ab->size]);

    sourceAlsa->buffer_queue.push_back(ab);

    return static_cast<snd_pcm_sframes_t>(size);
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_readi, nullptr);
        return orig::snd_pcm_readi(pcm, buffer, size);
    }

    debuglog(LCF_SOUND, __func__, " call with ", size, " bytes");
    return static_cast<snd_pcm_sframes_t>(size);
}

int snd_pcm_mmap_begin(snd_pcm_t *pcm, const snd_pcm_channel_area_t **areas, snd_pcm_uframes_t *offset, snd_pcm_uframes_t *frames)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_mmap_begin, nullptr);
        return orig::snd_pcm_mmap_begin(pcm, areas, offset, frames);
    }

    debuglog(LCF_SOUND, __func__, " call with ", *frames, " frames");

    /* Getting the available samples, don't return more frames than that */
    snd_pcm_sframes_t avail = buffer_size - get_latency();
    if (avail < 0)
        avail = 0;

    if (*frames > avail)
        *frames = avail;

    debuglog(LCF_SOUND, "  returning ", *frames, " frames");

    /* We should lock the audio mutex until snd_pcm_mmap_commit() is called,
     * but for some reason, FTL doesn't call snd_pcm_mmap_commit() the first
     * time, resulting in a deadlock. So for now we only lock inside this
     * function.
     */
    // audiocontext.mutex.lock();
    std::lock_guard<std::mutex> lock(audiocontext.mutex);

    /* We try to reuse a buffer that has been processed from the source */
    std::shared_ptr<AudioBuffer> ab;
    if (sourceAlsa->nbQueueProcessed() > 0) {
        /* Removing first buffer */
        ab = sourceAlsa->buffer_queue[0];
        sourceAlsa->buffer_queue.erase(sourceAlsa->buffer_queue.begin());
        sourceAlsa->queue_index--;
    }
    else {
        /* Building a new buffer */
        int bufferId = audiocontext.createBuffer();
        ab = audiocontext.getBuffer(bufferId);

        /* Getting the parameters of the buffer from one of the queue */
        if (sourceAlsa->buffer_queue.empty()) {
            debuglog(LCF_SOUND | LCF_ERROR, "Empty queue, cannot guess buffer parameters");
            return -1;
        }

        auto ref = sourceAlsa->buffer_queue[0];
        ab->format = ref->format;
        ab->nbChannels = ref->nbChannels;
        ab->frequency = ref->frequency;
    }

    /* Configuring the buffer */
    ab->update(); // Compute alignSize
    ab->sampleSize = *frames;
    ab->size = *frames * ab->alignSize;
    ab->samples.resize(ab->size);
    sourceAlsa->buffer_queue.push_back(ab);

    /* Fill the area info */
    static snd_pcm_channel_area_t my_areas[2];
    my_areas[0].addr = ab->samples.data();
    my_areas[0].first = 0;
    my_areas[0].step = ab->alignSize * 8; // in bits

    my_areas[1].addr = ab->samples.data();
    my_areas[1].first = ab->bitDepth; // in bits
    my_areas[1].step = ab->alignSize * 8; // in bits

    *areas = my_areas;
    *offset = 0;
    return 0;
}

snd_pcm_sframes_t snd_pcm_mmap_commit(snd_pcm_t *pcm, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_mmap_commit, nullptr);
        return orig::snd_pcm_mmap_commit(pcm, offset, frames);
    }

    /* We should unlock the audio mutex here, but we don't (see above comment) */
    // audiocontext.mutex.unlock();

    debuglog(LCF_SOUND, __func__, " call with frames ", frames);
    return frames;
}


int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_any, nullptr);
        return orig::snd_pcm_hw_params_any(pcm, params);
    }

    DEBUGLOGCALL(LCF_SOUND);
    return 0;
}

size_t snd_pcm_hw_params_sizeof(void)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_sizeof, nullptr);
        return orig::snd_pcm_hw_params_sizeof();
    }

    DEBUGLOGCALL(LCF_SOUND);
    return 8;
}

int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **ptr)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_malloc, nullptr);
        return orig::snd_pcm_hw_params_malloc(ptr);
    }

    DEBUGLOGCALL(LCF_SOUND);
    *ptr = reinterpret_cast<snd_pcm_hw_params_t*>(1); // Set a non-null value
    return 0;
}

void snd_pcm_hw_params_free(snd_pcm_hw_params_t *obj)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_free, nullptr);
        return orig::snd_pcm_hw_params_free(obj);
    }

    DEBUGLOGCALL(LCF_SOUND);
}

void snd_pcm_hw_params_copy(snd_pcm_hw_params_t *dst, const snd_pcm_hw_params_t *src)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_copy, nullptr);
        return orig::snd_pcm_hw_params_copy(dst, src);
    }

    DEBUGLOGCALL(LCF_SOUND);
}

int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t access)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_access, nullptr);
        return orig::snd_pcm_hw_params_set_access(pcm, params, access);
    }

    debuglog(LCF_SOUND, __func__, " call with access ", access);
    if ((access != SND_PCM_ACCESS_RW_INTERLEAVED) && (access != SND_PCM_ACCESS_MMAP_INTERLEAVED)) {
        debuglog(LCF_SOUND | LCF_ERROR, "    Unsupported access ", access);
    }
    return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_format, nullptr);
        return orig::snd_pcm_hw_params_set_format(pcm, params, val);
    }

    debuglog(LCF_SOUND, __func__, " call with format ", val);

    auto buffer = sourceAlsa->buffer_queue[0];

    switch(val) {
        case SND_PCM_FORMAT_U8:
            buffer->format = AudioBuffer::SAMPLE_FMT_U8;
            break;
        case SND_PCM_FORMAT_S16_LE:
            buffer->format = AudioBuffer::SAMPLE_FMT_S16;
            break;
        case SND_PCM_FORMAT_S32_LE:
            buffer->format = AudioBuffer::SAMPLE_FMT_S32;
            break;
        case SND_PCM_FORMAT_FLOAT_LE:
            buffer->format = AudioBuffer::SAMPLE_FMT_FLT;
            break;
        default:
            debuglog(LCF_SOUND | LCF_ERROR, "    Unsupported audio format");
            return -1;
    }

    return 0;
}

int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *params, unsigned int *val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_get_channels, nullptr);
        return orig::snd_pcm_hw_params_get_channels(params, val);
    }

    DEBUGLOGCALL(LCF_SOUND);

    auto buffer = sourceAlsa->buffer_queue[0];
    *val = buffer->nbChannels;

    return 0;
}

int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *params, unsigned int *val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_get_channels_max, nullptr);
        return orig::snd_pcm_hw_params_get_channels_max(params, val);
    }

    DEBUGLOGCALL(LCF_SOUND);

    *val = 2;
    return 0;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_channels, nullptr);
        return orig::snd_pcm_hw_params_set_channels(pcm, params, val);
    }

    debuglog(LCF_SOUND, __func__, " call with channels ", val);

    auto buffer = sourceAlsa->buffer_queue[0];
    buffer->nbChannels = val;

    return 0;
}

int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_rate, nullptr);
        return orig::snd_pcm_hw_params_set_rate(pcm, params, val, dir);
    }

    debuglog(LCF_SOUND, __func__, " call with rate ", val, " and dir ", dir);

    auto buffer = sourceAlsa->buffer_queue[0];
    buffer->frequency = val;

    return 0;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_rate_near, nullptr);
        return orig::snd_pcm_hw_params_set_rate_near(pcm, params, val, dir);
    }

    debuglog(LCF_SOUND, __func__, " call with rate ", *val);

    auto buffer = sourceAlsa->buffer_queue[0];
    buffer->frequency = *val;

    return 0;
}

int snd_pcm_hw_params_set_rate_resample(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_rate_resample, nullptr);
        return orig::snd_pcm_hw_params_set_rate_resample(pcm, params, val);
    }

    debuglog(LCF_SOUND, __func__, " call with val ", val);

    /* Not sure what should we do here */
    return 0;
}

static int periods = 2;

int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *frames, int *dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_get_period_size, nullptr);
        return orig::snd_pcm_hw_params_get_period_size(params, frames, dir);
    }

    DEBUGLOGCALL(LCF_SOUND);
    *frames = buffer_size / periods;

    return 0;
}

int snd_pcm_hw_params_get_period_time_min(const snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_get_period_time_min, nullptr);
        return orig::snd_pcm_hw_params_get_period_time_min(params, val, dir);
    }

    DEBUGLOGCALL(LCF_SOUND);
    *val = 0;

    return 0;
}

int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_period_size_near, nullptr);
        return orig::snd_pcm_hw_params_set_period_size_near(pcm, params, val, dir);
    }

    debuglog(LCF_SOUND, __func__, " call with period size ", *val, " and dir ", dir?*dir:-2);
    return 0;
}

int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_periods_near, nullptr);
        return orig::snd_pcm_hw_params_set_periods_near(pcm, params, val, dir);
    }

    debuglog(LCF_SOUND, __func__, " call with period ", *val, " and dir ", dir?*dir:-2);
    periods = *val;
    return 0;
}

int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_get_buffer_size, nullptr);
        return orig::snd_pcm_hw_params_get_buffer_size(params, val);
    }

    DEBUGLOGCALL(LCF_SOUND);
    *val = buffer_size;
    return 0;
}

int snd_pcm_hw_params_get_buffer_time_max(const snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_get_buffer_time_max, nullptr);
        return orig::snd_pcm_hw_params_get_buffer_time_max(params, val, dir);
    }

    DEBUGLOGCALL(LCF_SOUND);

    auto buffer = sourceAlsa->buffer_queue[0];

    *val = buffer_size * 1000000 / buffer->frequency;
    return 0;
}

int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_buffer_size_near, nullptr);
        return orig::snd_pcm_hw_params_set_buffer_size_near(pcm, params, val);
    }

    debuglog(LCF_SOUND, __func__, " call with buffer size ", *val);
    buffer_size = *val;
    return 0;
}

int snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_set_buffer_time_near, nullptr);
        return orig::snd_pcm_hw_params_set_buffer_time_near(pcm, params, val, dir);
    }

    debuglog(LCF_SOUND, __func__, " call with buffer time ", *val);

    auto buffer = sourceAlsa->buffer_queue[0];

    /* Special case for 0, return the default value */
    if (*val == 0) {
        *val = buffer_size * 1000000 / buffer->frequency;
        return 0;
    }

    if (buffer->frequency != 0) {
        buffer_size = *val * buffer->frequency / 1000000;
    }
    return 0;
}

int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val, int dir)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_hw_params_test_rate, nullptr);
        return orig::snd_pcm_hw_params_test_rate(pcm, params, val, dir);
    }

    debuglog(LCF_SOUND, __func__, " call with val ", val);
    return 0;
}

size_t snd_pcm_sw_params_sizeof(void)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_sw_params_sizeof, nullptr);
        return orig::snd_pcm_sw_params_sizeof();
    }

    DEBUGLOGCALL(LCF_SOUND);
    return 8;
}


int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_sw_params_set_start_threshold, nullptr);
        return orig::snd_pcm_sw_params_set_start_threshold(pcm, params, val);
    }

    debuglog(LCF_SOUND, __func__, " call with start threshold ", val);
    return 0;
}

int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *params, snd_pcm_uframes_t val)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_sw_params_set_avail_min, nullptr);
        return orig::snd_pcm_sw_params_set_avail_min(pcm, params, val);
    }

    debuglog(LCF_SOUND, __func__, " call with val ", val);
    return 0;
}

snd_pcm_chmap_t *snd_pcm_get_chmap(snd_pcm_t *pcm)
{
    if (GlobalState::isNative()) {
        LINK_NAMESPACE(snd_pcm_get_chmap, nullptr);
        return orig::snd_pcm_get_chmap(pcm);
    }

    DEBUGLOGCALL(LCF_SOUND);
    int channels = 2; // TODO!!
    snd_pcm_chmap_t *map = static_cast<snd_pcm_chmap_t*>(malloc(sizeof(int) * (channels + 1)));
    map->channels = channels;
    map->pos[0] = SND_CHMAP_FL;
    map->pos[1] = SND_CHMAP_FR;

    return map;
}

}
