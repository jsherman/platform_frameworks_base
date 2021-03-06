/* //device/extlibs/pv/android/AudioTrack.cpp
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/


//#define LOG_NDEBUG 0
#define LOG_TAG "AudioTrack"

#include <stdint.h>
#include <sys/types.h>
#include <limits.h>

#include <sched.h>
#include <sys/resource.h>

#include <private/media/AudioTrackShared.h>

#include <media/AudioSystem.h>
#include <media/AudioTrack.h>

#include <utils/Log.h>
#include <utils/MemoryDealer.h>
#include <utils/Parcel.h>
#include <utils/IPCThreadState.h>
#include <utils/Timers.h>
#include <cutils/atomic.h>

#define LIKELY( exp )       (__builtin_expect( (exp) != 0, true  ))
#define UNLIKELY( exp )     (__builtin_expect( (exp) != 0, false ))

namespace android {

// ---------------------------------------------------------------------------

AudioTrack::AudioTrack()
    : mStatus(NO_INIT)
{
}

AudioTrack::AudioTrack(
        int streamType,
        uint32_t sampleRate,
        int format,
        int channelCount,
        int frameCount,
        uint32_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames)
    : mStatus(NO_INIT)
{
    mStatus = set(streamType, sampleRate, format, channelCount,
            frameCount, flags, cbf, user, notificationFrames, 0);
}

AudioTrack::AudioTrack(
        int streamType,
        uint32_t sampleRate,
        int format,
        int channelCount,
        const sp<IMemory>& sharedBuffer,
        uint32_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames)
    : mStatus(NO_INIT)
{
    mStatus = set(streamType, sampleRate, format, channelCount,
            0, flags, cbf, user, notificationFrames, sharedBuffer);
}

AudioTrack::~AudioTrack()
{
    LOGV_IF(mSharedBuffer != 0, "Destructor sharedBuffer: %p", mSharedBuffer->pointer());

    if (mStatus == NO_ERROR) {
        // Make sure that callback function exits in the case where
        // it is looping on buffer full condition in obtainBuffer().
        // Otherwise the callback thread will never exit.
        stop();
        if (mAudioTrackThread != 0) {
            mCblk->cv.signal();
            mAudioTrackThread->requestExitAndWait();
            mAudioTrackThread.clear();
        }
        mAudioTrack.clear();
        IPCThreadState::self()->flushCommands();
    }
}

status_t AudioTrack::set(
        int streamType,
        uint32_t sampleRate,
        int format,
        int channelCount,
        int frameCount,
        uint32_t flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        const sp<IMemory>& sharedBuffer,
        bool threadCanCallJava)
{

    LOGV_IF(sharedBuffer != 0, "sharedBuffer: %p, size: %d", sharedBuffer->pointer(), sharedBuffer->size());

    if (mAudioFlinger != 0) {
        LOGE("Track already in use");
        return INVALID_OPERATION;
    }

    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
       LOGE("Could not get audioflinger");
       return NO_INIT;
    }
    int afSampleRate;
    if (AudioSystem::getOutputSamplingRate(&afSampleRate) != NO_ERROR) {
        return NO_INIT;
    }
    int afFrameCount;
    if (AudioSystem::getOutputFrameCount(&afFrameCount) != NO_ERROR) {
        return NO_INIT;
    }
    uint32_t afLatency;
    if (AudioSystem::getOutputLatency(&afLatency) != NO_ERROR) {
        return NO_INIT;
    }


    // handle default values first.
    if (streamType == DEFAULT) {
        streamType = MUSIC;
    }
    if (sampleRate == 0) {
        sampleRate = afSampleRate;
    }
    // these below should probably come from the audioFlinger too...
    if (format == 0) {
        format = AudioSystem::PCM_16_BIT;
    }
    if (channelCount == 0) {
        channelCount = 2;
    }

    // validate parameters
    if (((format != AudioSystem::PCM_8_BIT) || mSharedBuffer != 0) &&
        (format != AudioSystem::PCM_16_BIT)) {
        LOGE("Invalid format");
        return BAD_VALUE;
    }
    if (channelCount != 1 && channelCount != 2) {
        LOGE("Invalid channel number");
        return BAD_VALUE;
    }

    // Ensure that buffer depth covers at least audio hardware latency
    uint32_t minBufCount = afLatency / ((1000 * afFrameCount)/afSampleRate);
    // When playing from shared buffer, playback will start even if last audioflinger
    // block is partly filled.
    if (sharedBuffer != 0 && minBufCount > 1) {
        minBufCount--;
    }

    int minFrameCount = (afFrameCount*sampleRate*minBufCount)/afSampleRate;

    if (sharedBuffer == 0) {
        if (frameCount == 0) {
            frameCount = minFrameCount;
        }
        if (notificationFrames == 0) {
            notificationFrames = frameCount/2;
        }
        // Make sure that application is notified with sufficient margin
        // before underrun
        if (notificationFrames > frameCount/2) {
            notificationFrames = frameCount/2;
        }
    } else {
        // Ensure that buffer alignment matches channelcount
        if (((uint32_t)sharedBuffer->pointer() & (channelCount | 1)) != 0) {
            LOGE("Invalid buffer alignement: address %p, channelCount %d", sharedBuffer->pointer(), channelCount);
            return BAD_VALUE;
        }
        frameCount = sharedBuffer->size()/channelCount/sizeof(int16_t);
    }

    if (frameCount < minFrameCount) {
      LOGE("Invalid buffer size: minFrameCount %d, frameCount %d", minFrameCount, frameCount);
      return BAD_VALUE;
    }

    // create the track
    status_t status;
    sp<IAudioTrack> track = audioFlinger->createTrack(getpid(),
                streamType, sampleRate, format, channelCount, frameCount, flags, sharedBuffer, &status);

    if (track == 0) {
        LOGE("AudioFlinger could not create track, status: %d", status);
        return status;
    }
    sp<IMemory> cblk = track->getCblk();
    if (cblk == 0) {
        LOGE("Could not get control block");
        return NO_INIT;
    }
    if (cbf != 0) {
        mAudioTrackThread = new AudioTrackThread(*this, threadCanCallJava);
        if (mAudioTrackThread == 0) {
          LOGE("Could not create callback thread");
          return NO_INIT;
        }
    }

    mStatus = NO_ERROR;

    mAudioFlinger = audioFlinger;
    mAudioTrack = track;
    mCblkMemory = cblk;
    mCblk = static_cast<audio_track_cblk_t*>(cblk->pointer());
    if (sharedBuffer == 0) {
        mCblk->buffers = (char*)mCblk + sizeof(audio_track_cblk_t);
    } else {
        mCblk->buffers = sharedBuffer->pointer();
    }
    mCblk->out = 1;
    mCblk->volume[0] = mCblk->volume[1] = 0x1000;
    mVolume[LEFT] = 1.0f;
    mVolume[RIGHT] = 1.0f;
    mSampleRate = sampleRate;
    mStreamType = streamType;
    mFormat = format;
    // Update buffer size in case it has been limited by AudioFlinger during track creation
    mFrameCount = mCblk->frameCount;
    mChannelCount = channelCount;
    mSharedBuffer = sharedBuffer;
    mMuted = false;
    mActive = 0;
    mCbf = cbf;
    mNotificationFrames = notificationFrames;
    mRemainingFrames = notificationFrames;
    mUserData = user;
    mLatency = afLatency + (1000*mFrameCount) / mSampleRate;
    mLoopCount = 0;
    mMarkerPosition = 0;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    
    return NO_ERROR;
}

status_t AudioTrack::initCheck() const
{
    return mStatus;
}

// -------------------------------------------------------------------------

uint32_t AudioTrack::latency() const
{
    return mLatency;
}

int AudioTrack::streamType() const
{
    return mStreamType;
}

uint32_t AudioTrack::sampleRate() const
{
    return mSampleRate;
}

int AudioTrack::format() const
{
    return mFormat;
}

int AudioTrack::channelCount() const
{
    return mChannelCount;
}

uint32_t AudioTrack::frameCount() const
{
    return mFrameCount;
}

int AudioTrack::frameSize() const
{
    return channelCount()*((format() == AudioSystem::PCM_8_BIT) ? sizeof(uint8_t) : sizeof(int16_t));
}

sp<IMemory>& AudioTrack::sharedBuffer()
{
    return mSharedBuffer;
}

// -------------------------------------------------------------------------

void AudioTrack::start()
{
    sp<AudioTrackThread> t = mAudioTrackThread;

    LOGV("start");
    if (t != 0) {
        if (t->exitPending()) {
            if (t->requestExitAndWait() == WOULD_BLOCK) {
                LOGE("AudioTrack::start called from thread");
                return;
            }
        }
        t->mLock.lock();
     }

    if (android_atomic_or(1, &mActive) == 0) {
        if (mSharedBuffer != 0) {
             // Force buffer full condition as data is already present in shared memory
            mCblk->user = mFrameCount;
            mCblk->flowControlFlag = 0;
        }
        mNewPosition = mCblk->server + mUpdatePeriod;
        if (t != 0) {
           t->run("AudioTrackThread", THREAD_PRIORITY_AUDIO_CLIENT);
        } else {
            setpriority(PRIO_PROCESS, 0, THREAD_PRIORITY_AUDIO_CLIENT);
        }
        mAudioTrack->start();
    }

    if (t != 0) {
        t->mLock.unlock();
    }
}

void AudioTrack::stop()
{
    sp<AudioTrackThread> t = mAudioTrackThread;

    LOGV("stop");
    if (t != 0) {
        t->mLock.lock();
    }

    if (android_atomic_and(~1, &mActive) == 1) {
        mAudioTrack->stop();
        // Cancel loops (If we are in the middle of a loop, playback
        // would not stop until loopCount reaches 0).
        setLoop(0, 0, 0);
        // Force flush if a shared buffer is used otherwise audioflinger
        // will not stop before end of buffer is reached.
        if (mSharedBuffer != 0) {
            flush();
        }
        if (t != 0) {
            t->requestExit();
        } else {
            setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_NORMAL);
        }
    }

    if (t != 0) {
        t->mLock.unlock();
    }
}

bool AudioTrack::stopped() const
{
    return !mActive;
}

void AudioTrack::flush()
{
    LOGV("flush");

    if (!mActive) {
        mCblk->lock.lock();
        mAudioTrack->flush();
        // Release AudioTrack callback thread in case it was waiting for new buffers
        // in AudioTrack::obtainBuffer()
        mCblk->cv.signal();
        mCblk->lock.unlock();
    }
}

void AudioTrack::pause()
{
    LOGV("pause");
    if (android_atomic_and(~1, &mActive) == 1) {
        mActive = 0;
        mAudioTrack->pause();
    }
}

void AudioTrack::mute(bool e)
{
    mAudioTrack->mute(e);
    mMuted = e;
}

bool AudioTrack::muted() const
{
    return mMuted;
}

void AudioTrack::setVolume(float left, float right)
{
    mVolume[LEFT] = left;
    mVolume[RIGHT] = right;

    // write must be atomic
    mCblk->volumeLR = (int32_t(int16_t(left * 0x1000)) << 16) | int16_t(right * 0x1000);
}

void AudioTrack::getVolume(float* left, float* right)
{
    *left  = mVolume[LEFT];
    *right = mVolume[RIGHT];
}

void AudioTrack::setSampleRate(int rate)
{
    int afSamplingRate;

    if (AudioSystem::getOutputSamplingRate(&afSamplingRate) != NO_ERROR) {
        return;
    }
    // Resampler implementation limits input sampling rate to 2 x output sampling rate.
    if (rate > afSamplingRate*2) rate = afSamplingRate*2;

    if (rate > MAX_SAMPLE_RATE) rate = MAX_SAMPLE_RATE;

    mCblk->sampleRate = rate;
}

uint32_t AudioTrack::getSampleRate()
{
    return uint32_t(mCblk->sampleRate);
}

status_t AudioTrack::setLoop(uint32_t loopStart, uint32_t loopEnd, int loopCount)
{
    audio_track_cblk_t* cblk = mCblk;


    Mutex::Autolock _l(cblk->lock);

    if (loopCount == 0) {
        cblk->loopStart = UINT_MAX;
        cblk->loopEnd = UINT_MAX;
        cblk->loopCount = 0;
        mLoopCount = 0;
        return NO_ERROR;
    }

    if (loopStart >= loopEnd ||
        loopStart < cblk->user ||
        loopEnd - loopStart > mFrameCount) {
        LOGW("setLoop invalid value: loopStart %d, loopEnd %d, loopCount %d, framecount %d, user %d", loopStart, loopEnd, loopCount, mFrameCount, cblk->user);
        return BAD_VALUE;
    }
    // TODO handle shared buffer here: limit loop end to framecount

    cblk->loopStart = loopStart;
    cblk->loopEnd = loopEnd;
    cblk->loopCount = loopCount;
    mLoopCount = loopCount;

    return NO_ERROR;
}

status_t AudioTrack::getLoop(uint32_t *loopStart, uint32_t *loopEnd, int *loopCount)
{
    if (loopStart != 0) {
        *loopStart = mCblk->loopStart;
    }
    if (loopEnd != 0) {
        *loopEnd = mCblk->loopEnd;
    }
    if (loopCount != 0) {
        if (mCblk->loopCount < 0) {
            *loopCount = -1;
        } else {
            *loopCount = mCblk->loopCount;
        }
    }

    return NO_ERROR;
}

status_t AudioTrack::setMarkerPosition(uint32_t marker)
{
    if (mCbf == 0) return INVALID_OPERATION;

    mMarkerPosition = marker;

    return NO_ERROR;
}

status_t AudioTrack::getMarkerPosition(uint32_t *marker)
{
    if (marker == 0) return BAD_VALUE;

    *marker = mMarkerPosition;

    return NO_ERROR;
}

status_t AudioTrack::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    if (mCbf == 0) return INVALID_OPERATION;

    uint32_t curPosition;
    getPosition(&curPosition);
    mNewPosition = curPosition + updatePeriod;
    mUpdatePeriod = updatePeriod;

    return NO_ERROR;
}

status_t AudioTrack::getPositionUpdatePeriod(uint32_t *updatePeriod)
{
    if (updatePeriod == 0) return BAD_VALUE;

    *updatePeriod = mUpdatePeriod;

    return NO_ERROR;
}

status_t AudioTrack::setPosition(uint32_t position)
{
    Mutex::Autolock _l(mCblk->lock);

    if (!stopped()) return INVALID_OPERATION;

    if (position > mCblk->user) return BAD_VALUE;

    mCblk->server = position;
    mCblk->forceReady = 1;
    
    return NO_ERROR;
}

status_t AudioTrack::getPosition(uint32_t *position)
{
    if (position == 0) return BAD_VALUE;

    *position = mCblk->server;

    return NO_ERROR;
}

status_t AudioTrack::reload()
{
    if (!stopped()) return INVALID_OPERATION;
    
    flush();

    mCblk->stepUser(mFrameCount);

    return NO_ERROR;
}

// -------------------------------------------------------------------------

status_t AudioTrack::obtainBuffer(Buffer* audioBuffer, bool blocking)
{
    int active;
    int timeout = 0;
    status_t result;
    audio_track_cblk_t* cblk = mCblk;
    uint32_t framesReq = audioBuffer->frameCount;

    audioBuffer->frameCount  = 0;
    audioBuffer->size = 0;

    uint32_t framesAvail = cblk->framesAvailable();

    if (framesAvail == 0) {
        Mutex::Autolock _l(cblk->lock);
        goto start_loop_here;
        while (framesAvail == 0) {
            active = mActive;
            if (UNLIKELY(!active)) {
                LOGV("Not active and NO_MORE_BUFFERS");
                return NO_MORE_BUFFERS;
            }
            if (UNLIKELY(!blocking))
                return WOULD_BLOCK;
            timeout = 0;
            result = cblk->cv.waitRelative(cblk->lock, seconds(1));
            if (__builtin_expect(result!=NO_ERROR, false)) {
                LOGW(   "obtainBuffer timed out (is the CPU pegged?) "
                        "user=%08x, server=%08x", cblk->user, cblk->server);
                mAudioTrack->start(); // FIXME: Wake up audioflinger
                timeout = 1;
            }
            // read the server count again
        start_loop_here:
            framesAvail = cblk->framesAvailable_l();
        }
    }

    if (framesReq > framesAvail) {
        framesReq = framesAvail;
    }

    uint32_t u = cblk->user;
    uint32_t bufferEnd = cblk->userBase + cblk->frameCount;

    if (u + framesReq > bufferEnd) {
        framesReq = bufferEnd - u;
    }

    LOGW_IF(timeout,
        "*** SERIOUS WARNING *** obtainBuffer() timed out "
        "but didn't need to be locked. We recovered, but "
        "this shouldn't happen (user=%08x, server=%08x)", cblk->user, cblk->server);

    audioBuffer->flags       = mMuted ? Buffer::MUTE : 0;
    audioBuffer->channelCount= mChannelCount;
    audioBuffer->format      = AudioSystem::PCM_16_BIT;
    audioBuffer->frameCount  = framesReq;
    audioBuffer->size = framesReq*mChannelCount*sizeof(int16_t);
    audioBuffer->raw         = (int8_t *)cblk->buffer(u);
    active = mActive;
    return active ? status_t(NO_ERROR) : status_t(STOPPED);
}

void AudioTrack::releaseBuffer(Buffer* audioBuffer)
{
    audio_track_cblk_t* cblk = mCblk;
    cblk->stepUser(audioBuffer->frameCount);
}

// -------------------------------------------------------------------------

ssize_t AudioTrack::write(const void* buffer, size_t userSize)
{

    if (mSharedBuffer != 0) return INVALID_OPERATION;

    if (ssize_t(userSize) < 0) {
        // sanity-check. user is most-likely passing an error code.
        LOGE("AudioTrack::write(buffer=%p, size=%u (%d)",
                buffer, userSize, userSize);
        return BAD_VALUE;
    }

    LOGV("write %d bytes, mActive=%d", userSize, mActive);

    ssize_t written = 0;
    const int8_t *src = (const int8_t *)buffer;
    Buffer audioBuffer;

    do {
        audioBuffer.frameCount = userSize/mChannelCount;
        if (mFormat == AudioSystem::PCM_16_BIT) {
            audioBuffer.frameCount >>= 1;
        }

        status_t err = obtainBuffer(&audioBuffer, true);
        if (err < 0) {
            // out of buffers, return #bytes written
            if (err == status_t(NO_MORE_BUFFERS))
                break;
            return ssize_t(err);
        }

        size_t toWrite;
        if (mFormat == AudioSystem::PCM_8_BIT) {
            // Divide capacity by 2 to take expansion into account
            toWrite = audioBuffer.size>>1;
            // 8 to 16 bit conversion
            int count = toWrite;
            int16_t *dst = (int16_t *)(audioBuffer.i8);
            while(count--) {
                *dst++ = (int16_t)(*src++^0x80) << 8;
            }
        }else {
            toWrite = audioBuffer.size;
            memcpy(audioBuffer.i8, src, toWrite);
            src += toWrite;
        }
        userSize -= toWrite;
        written += toWrite;

        releaseBuffer(&audioBuffer);
    } while (userSize);

    return written;
}

// -------------------------------------------------------------------------

bool AudioTrack::processAudioBuffer(const sp<AudioTrackThread>& thread)
{
    Buffer audioBuffer;
    uint32_t frames;
    size_t writtenSize = 0;

    // Manage underrun callback
    if (mActive && (mCblk->framesReady() == 0)) {
        LOGV("Underrun user: %x, server: %x, flowControlFlag %d", mCblk->user, mCblk->server, mCblk->flowControlFlag);
        if (mCblk->flowControlFlag == 0) {
            mCbf(EVENT_UNDERRUN, mUserData, 0);
            if (mCblk->server == mCblk->frameCount) {
                mCbf(EVENT_BUFFER_END, mUserData, 0);                
            }
            mCblk->flowControlFlag = 1;
            if (mSharedBuffer != 0) return false;
        }
    }
    
    // Manage loop end callback
    while (mLoopCount > mCblk->loopCount) {
        int loopCount = -1;
        mLoopCount--;
        if (mLoopCount >= 0) loopCount = mLoopCount;

        mCbf(EVENT_LOOP_END, mUserData, (void *)&loopCount);
    }

    // Manage marker callback
    if(mMarkerPosition > 0) {
        if (mCblk->server >= mMarkerPosition) {
            mCbf(EVENT_MARKER, mUserData, (void *)&mMarkerPosition);
            mMarkerPosition = 0;
        }
    }

    // Manage new position callback
    if(mUpdatePeriod > 0) {
        while (mCblk->server >= mNewPosition) {
            mCbf(EVENT_NEW_POS, mUserData, (void *)&mNewPosition);
            mNewPosition += mUpdatePeriod;
        }
    }

    // If Shared buffer is used, no data is requested from client.
    if (mSharedBuffer != 0) {
        frames = 0;
    } else {
        frames = mRemainingFrames;
    }

    do {

        audioBuffer.frameCount = frames;

        status_t err = obtainBuffer(&audioBuffer, false);
        if (err < NO_ERROR) {
            if (err != WOULD_BLOCK) {
                LOGE("Error obtaining an audio buffer, giving up.");
                return false;
            }
        }
        if (err == status_t(STOPPED)) return false;

        if (audioBuffer.size == 0) break;

        // Divide buffer size by 2 to take into account the expansion
        // due to 8 to 16 bit conversion: the callback must fill only half
        // of the destination buffer
        if (mFormat == AudioSystem::PCM_8_BIT) {
            audioBuffer.size >>= 1;
        }

        size_t reqSize = audioBuffer.size;
        mCbf(EVENT_MORE_DATA, mUserData, &audioBuffer);
        writtenSize = audioBuffer.size;

        // Sanity check on returned size
        if (ssize_t(writtenSize) <= 0) break;
        if (writtenSize > reqSize) writtenSize = reqSize;

        if (mFormat == AudioSystem::PCM_8_BIT) {
            // 8 to 16 bit conversion
            const int8_t *src = audioBuffer.i8 + writtenSize-1;
            int count = writtenSize;
            int16_t *dst = audioBuffer.i16 + writtenSize-1;
            while(count--) {
                *dst-- = (int16_t)(*src--^0x80) << 8;
            }
            writtenSize <<= 1;
        }

        audioBuffer.size = writtenSize;
        audioBuffer.frameCount = writtenSize/mChannelCount/sizeof(int16_t);
        frames -= audioBuffer.frameCount;

        releaseBuffer(&audioBuffer);
    }
    while (frames);

    // If no data was written, it is likely that obtainBuffer() did
    // not find room in PCM buffer: we release the processor for
    // a few millisecond before polling again for available room.
    if (writtenSize == 0) {
        usleep(5000);
    }

    if (frames == 0) {
        mRemainingFrames = mNotificationFrames;
    } else {
        mRemainingFrames = frames;
    }
    return true;
}

status_t AudioTrack::dump(int fd, const Vector<String16>& args) const
{

    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    result.append(" AudioTrack::dump\n");
    snprintf(buffer, 255, "  stream type(%d), left - right volume(%f, %f)\n", mStreamType, mVolume[0], mVolume[1]);
    result.append(buffer);
    snprintf(buffer, 255, "  format(%d), channel count(%d), frame count(%d)\n", mFormat, mChannelCount, mFrameCount);
    result.append(buffer);
    snprintf(buffer, 255, "  sample rate(%d), status(%d), muted(%d)\n", mSampleRate, mStatus, mMuted);
    result.append(buffer);
    snprintf(buffer, 255, "  active(%d), latency (%d)\n", mActive, mLatency);
    result.append(buffer);
    ::write(fd, result.string(), result.size());
    return NO_ERROR;
}

// =========================================================================

AudioTrack::AudioTrackThread::AudioTrackThread(AudioTrack& receiver, bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver)
{
}

bool AudioTrack::AudioTrackThread::threadLoop()
{
    return mReceiver.processAudioBuffer(this);
}

status_t AudioTrack::AudioTrackThread::readyToRun()
{
    return NO_ERROR;
}

void AudioTrack::AudioTrackThread::onFirstRef()
{
}

// =========================================================================

audio_track_cblk_t::audio_track_cblk_t()
    : user(0), server(0), userBase(0), serverBase(0), buffers(0), frameCount(0),
    loopStart(UINT_MAX), loopEnd(UINT_MAX), loopCount(0), volumeLR(0), flowControlFlag(1), forceReady(0)
{
}

uint32_t audio_track_cblk_t::stepUser(uint32_t frameCount)
{
    uint32_t u = this->user;

    u += frameCount;
    // Ensure that user is never ahead of server for AudioRecord
    if (!out && u > this->server) {
        LOGW("stepServer occured after track reset");
        u = this->server;
    }

    if (u >= userBase + this->frameCount) {
        userBase += this->frameCount;
    }

    this->user = u;

    // Clear flow control error condition as new data has been written/read to/from buffer.
    flowControlFlag = 0;

    return u;
}

bool audio_track_cblk_t::stepServer(uint32_t frameCount)
{
    // the code below simulates lock-with-timeout
    // we MUST do this to protect the AudioFlinger server
    // as this lock is shared with the client.
    status_t err;

    err = lock.tryLock();
    if (err == -EBUSY) { // just wait a bit
        usleep(1000);
        err = lock.tryLock();
    }
    if (err != NO_ERROR) {
        // probably, the client just died.
        return false;
    }

    uint32_t s = this->server;

    s += frameCount;
    // It is possible that we receive a flush()
    // while the mixer is processing a block: in this case,
    // stepServer() is called After the flush() has reset u & s and
    // we have s > u
    if (out && s > this->user) {
        LOGW("stepServer occured after track reset");
        s = this->user;
    }

    if (s >= loopEnd) {
        LOGW_IF(s > loopEnd, "stepServer: s %u > loopEnd %u", s, loopEnd);
        s = loopStart;
        if (--loopCount == 0) {
            loopEnd = UINT_MAX;
            loopStart = UINT_MAX;
        }
    }
    if (s >= serverBase + this->frameCount) {
        serverBase += this->frameCount;
    }

    this->server = s;

    cv.signal();
    lock.unlock();
    return true;
}

void* audio_track_cblk_t::buffer(uint32_t offset) const
{
    return (int16_t *)this->buffers + (offset-userBase)*this->channels;
}

uint32_t audio_track_cblk_t::framesAvailable()
{
    Mutex::Autolock _l(lock);
    return framesAvailable_l();
}

uint32_t audio_track_cblk_t::framesAvailable_l()
{
    uint32_t u = this->user;
    uint32_t s = this->server;

    if (out) {
        if (u < loopEnd) {
            return s + frameCount - u;
        } else {
            uint32_t limit = (s < loopStart) ? s : loopStart;
            return limit + frameCount - u;
        }
    } else {
        return frameCount + u - s;
    }
}

uint32_t audio_track_cblk_t::framesReady()
{
    uint32_t u = this->user;
    uint32_t s = this->server;

    if (out) {
        if (u < loopEnd) {
            return u - s;
        } else {
            Mutex::Autolock _l(lock);
            if (loopCount >= 0) {
                return (loopEnd - loopStart)*loopCount + u - s;
            } else {
                return UINT_MAX;
            }
        }
    } else {
        return s - u;
    }
}

// -------------------------------------------------------------------------

}; // namespace android

