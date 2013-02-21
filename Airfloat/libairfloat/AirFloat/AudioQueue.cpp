//
//  AudioQueue.cpp
//  AirFloat
//
//  Created by Kristian Trenskow on 1/21/12.
//  Copyright (c) 2012 The Famous Software Company. All rights reserved.
//

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>

#include "NotificationCenter.h"
#include "Log.h"
#include "AudioQueue.h"

#define MAX_QUEUE_COUNT 500
#define UINT16_MIDDLE (0xFFFF > 1)
#define LoopFrom(x, y, d, c) for (AudioPacket* x = y ; x != c ; x = x->d)
#define MIN(x,y) (x < y ? x : y)
#define MAX(x,y) (x > y ? x : y)
#define SafeDR(x,y) if (x != NULL) *x = y

#define IS_UPPER(x) (((x % 0xFFFF) & 0x8000) != 0)
#define IS_LOWER(x) (((x % 0xFFFF) & 0xC000) == 0)

static const char* statestr(AudioPacketState state) {
    
    switch (state) {
        case kAudioPacketStateComplete:
            return "kAudioPacketStateComplete";
        case kAudioPacketStateMissing:
            return "kAudioPacketStateMissing";
        case kAudioPacketStateRequested:
            return "kAudioPacketStateRequested";
        default:
            return "kAudioPacketStateUnknown";

    }
    
}

static void dumpqueue(AudioPacket* head) {
    
    assert(head != NULL);
    
    LoopFrom(currentPacket, head, next, NULL)
        log(LOG_ERROR, " %05d --> %s", currentPacket->seqNo, statestr(currentPacket->state));
    
}

const char* AudioQueue::flushNotificationName = "audioQueueFlush";
const char* AudioQueue::syncNotificationName = "audioQueueSync";

AudioQueue::AudioQueue(double packetSize, double frameSize, double sampleRate) {
    
    assert(packetSize > 0 && frameSize > 0);
    
    _packetSize = packetSize;
    _frameSize = frameSize;
    _sampleRate = sampleRate;
    
    _synchronizationEnabled = true;
    _synchronizationNotificationSent = false;
    
    _foldCount = 0;
    _queueCount = 0;
    _missingCount = 0;
    _queueFrameCount = 0;
    
    _queueHead = _queueTail = NULL;
    
    _lastKnowSampleTime = _lastKnowSampleTimesTime = 0;
    
    _disposed = false;
    
}

AudioQueue::~AudioQueue() {
    
    _mutex.lock();
    
    while (_queueHead != NULL) {
        AudioPacket* packet = _queueHead;
        _queueHead = _queueHead->next;
        delete packet;
    }
    
    _disposed = true;
    
    _condition.broadcast();
    
    _mutex.unlock();
    
}

void AudioQueue::_checkQueueConsistency() {
    
    int count = 0;
    int missing = 0;
    
    LoopFrom(currentPacket, _queueHead, next, NULL) {
        if (currentPacket->state >= kAudioPacketStateMissing)
            missing++;
        
        count++;
    }
    
    if (count != _queueCount || missing != _missingCount) {
        log(LOG_ERROR, "Queue is currupted (Missing %d/%d - Count %d/%d)!", _missingCount, missing, _queueCount, count);
        dumpqueue(_queueHead);
        assert(0);
    }
    
}

AudioPacket* AudioQueue::_createPacket(AudioPacketState state) {
    
    return new AudioPacket(state);
        
}

void AudioQueue::_disposePacket(AudioPacket* packet) {
    
    assert(packet != NULL);
    
    delete packet;
    
}

void AudioQueue::_addPacketToQueueTail(AudioPacket* packet) {
    
    assert(packet != NULL);
    
    packet->prev = _queueTail;
    
    if (_queueTail != NULL) {
        if (packet->sampleTime == 0)
            packet->sampleTime = _queueTail->sampleTime + _packetSize;
        
        _queueTail->next = packet;
    } else
        _queueHead = packet;
    
    if (packet->state == kAudioPacketStateMissing)
        _missingCount++;
    
    _queueCount++;
    
    _queueTail = packet;
    
    while (_queueCount > MAX_QUEUE_COUNT)
        _disposePacket(_popQueueFromHead());
    
}

AudioPacket* AudioQueue::_addEmptyPacket() {
    
    AudioPacket* newPacket = _createPacket(kAudioPacketStateMissing);
    
    if (_queueTail != NULL)
        newPacket->seqNo = _handleSequenceOverflow(_queueTail->seqNo + 1);
    
    uint32_t size = _frameSize * _packetSize;
    char* emptyBuffer[size];
    bzero(emptyBuffer, size);
    newPacket->setBuffer(emptyBuffer, size);
    
    _queueFrameCount += _frameSize;
    
    _addPacketToQueueTail(newPacket);
    
    return newPacket;
    
}

AudioPacket* AudioQueue::_popQueueFromHead(bool keepQueueFilled) {
    
    AudioPacket* headPacket = _queueHead;
    
    if (_queueHead != NULL) {
        _queueHead = headPacket->next;
        if (_queueHead != NULL)
            _queueHead->prev = NULL;
        else
            _queueTail = NULL;

        headPacket->next = NULL;
        headPacket->prev = NULL;
        
        if (headPacket->state >= kAudioPacketStateMissing)
            _missingCount--;
        
        _queueCount--;
        _queueFrameCount -= headPacket->getBufferSize() / _frameSize;
        
        if (_queueCount < 2 && keepQueueFilled)
            while (_queueCount < 2)
                _addEmptyPacket();
        
        _condition.signal();
        
        return headPacket;
    
    }
    
    return NULL;    
    
}

void AudioQueue::_flush() {
    
    _mutex.lock();
    
    while (_queueHead != NULL)
        _disposePacket(_popQueueFromHead(false));
    
    _queueTail = NULL;
    
    _foldCount = 0;
    _lastKnowSampleTime = _lastKnowSampleTimesTime = 0;
    
    _mutex.unlock();
    
    log(LOG_INFO, "Queue flushed");
    
    NotificationCenter::defaultCenter()->postNotification(AudioQueue::flushNotificationName, this, NULL);
    
}

int AudioQueue::_handleSequenceOverflow(int seqNo) {
    
    if (_queueTail != NULL) {
        
        if ((seqNo & 0xFFFF) == 0xFFFF)
            return seqNo * (_foldCount++ * 0xFFFF);
        
        if (IS_UPPER(seqNo) && IS_LOWER(_queueTail->seqNo))
            return seqNo + ((_foldCount - 1) * 0xFFFF);
        
    }
    
    return seqNo + (_foldCount * 0xFFFF);
    
}

double AudioQueue::_convertTime(uint32_t fromSampleTime, double fromTime, uint32_t toSampleTime) {
    
    return fromTime + (((double)toSampleTime - (double)fromSampleTime) / (double)_sampleRate);
    
}

int AudioQueue::_addAudioPacket(void* buffer, uint32_t size, int seqNo, uint32_t sampleTime) {
        
    assert(buffer != NULL && size > 0);
    assert(size == _frameSize * _packetSize);
    
    int ret = 0;
    
    _mutex.lock();
    
    seqNo = _handleSequenceOverflow(seqNo);
    
    if (_queueHead == NULL || _queueTail->seqNo < seqNo) {
        
        // Add missing packets
        ret += (_queueHead != NULL ? seqNo - (_queueTail->seqNo + 1) : 0);
        for (int i = 0 ; i < ret ; i++)
            _addEmptyPacket();
        
        AudioPacket* newPacket = _createPacket(kAudioPacketStateComplete);
        
        newPacket->sampleTime = sampleTime;
        newPacket->seqNo = seqNo;
        newPacket->setBuffer(buffer, size);
        
        _queueFrameCount += size / _frameSize;
        
        _addPacketToQueueTail(newPacket);
        
    } else {
        
        bool found = false;
        LoopFrom(currentPacket, _queueTail, prev, _queueHead) {
            
            if (currentPacket->seqNo == seqNo) {
                if (currentPacket->state != kAudioPacketStateComplete) {
                    _missingCount--;
                    
                    currentPacket->setBuffer(buffer, size);
                    currentPacket->state = kAudioPacketStateComplete;
                    
                } else
                    log(LOG_INFO, "Packet %d already in queue", seqNo);
                
                found = true;
                break;
            }
            
        }
        
        if (!found)
            log(LOG_INFO, "Packet %d came too late", seqNo);
        
    }
    
    //_checkQueueConsistency();
    
    _mutex.unlock();
    
    if (_missingCount < _queueCount / 2)
        return MIN(ret, _missingCount);
    
    return 0;
    
}

void AudioQueue::_disableSynchronization() {
    
    _mutex.lock();
    
    log(LOG_INFO, "Synchronization is disabled");
    NotificationCenter::defaultCenter()->postNotification(AudioQueue::syncNotificationName, this, NULL);
    _synchronizationEnabled = false;
    
    _mutex.unlock();
    
}

bool AudioQueue::hasAvailablePacket() {
    
    _mutex.lock();
    
    bool ret = (_queueHead != NULL);
    
    if (_synchronizationEnabled)
        ret = ret && (_lastKnowSampleTime > 0 && _lastKnowSampleTimesTime > 0);
    else {
        
        ret = ret && (_queueFrameCount >= _sampleRate * 2);
        
        if (ret && !_synchronizationNotificationSent) {
            
            _synchronizationNotificationSent = true;
            
            log(LOG_INFO, "Queue was synced");
            NotificationCenter::defaultCenter()->postNotification(AudioQueue::syncNotificationName, this, NULL);
            
        }
        
    }
        
    _mutex.unlock();
    
    return ret;
    
}

double AudioQueue::getPacketTime() {
    
    double ret = 0;
    
    if (hasAvailablePacket()) {
        _mutex.lock();
        ret = _convertTime(_lastKnowSampleTime, _lastKnowSampleTimesTime, _queueHead->sampleTime);
        _mutex.unlock();
    }
    
    return ret;
    
}

void AudioQueue::getAudio(void* buffer, uint32_t* size, double* time, uint32_t* sampleTime) {
    
    assert(buffer != NULL && size != NULL);
    
    _mutex.lock();
    
    char* bufWriteHead = (char*)buffer;
    uint32_t bufWriteSize = *size;
    
    uint32_t outSize = 0;
    SafeDR(time, 0);
    
    if (_queueHead != NULL) {
        
        uint32_t packetByteSize = _frameSize * _packetSize;
        while (bufWriteSize > 0) {
            
            if (_queueHead) {
                
                AudioPacket *audioPacket = _queueHead;
                
                if (outSize == 0) {
                    
                    if (sampleTime != NULL)
                        *sampleTime = audioPacket->sampleTime;
                    if (time != NULL && _lastKnowSampleTime > 0 && _lastKnowSampleTimesTime > 0)
                        *time = _convertTime(_lastKnowSampleTime, _lastKnowSampleTimesTime, audioPacket->sampleTime);
                    
                }
                
                uint32_t written = audioPacket->getBuffer(bufWriteHead, bufWriteSize);
                
                _queueFrameCount -= written / _frameSize;
                
                audioPacket->shiftBuffer(written);
                
                if (audioPacket->getBufferSize() > 0) {
                    if (audioPacket->time > 0)
                        audioPacket->time += ((written / _frameSize) / _sampleRate);
                    audioPacket->sampleTime += (written / _frameSize);
                }
                
                bufWriteHead = &bufWriteHead[written];
                bufWriteSize -= written;
                outSize += written;
                
                if (audioPacket->seqNo % 100 == 0)
                    log(LOG_INFO, "%d packages in queue (%d missing / seq %d)", _queueCount, _missingCount, audioPacket->seqNo);
                
                if (audioPacket->getBufferSize() == 0)
                    _disposePacket(_popQueueFromHead(false));
                
            } else {
                uint32_t silenceSize = MIN(packetByteSize, bufWriteSize);
                bzero(bufWriteHead, silenceSize);
                bufWriteHead = &bufWriteHead[silenceSize];
                bufWriteSize -= silenceSize;
                outSize += packetByteSize;
            }
            
        }
        
    }
    
    if (_queueCount == 0)
        _lastKnowSampleTime = _lastKnowSampleTimesTime = 0;
    
    _mutex.unlock();
    
    *size = outSize;
    
}

void AudioQueue::discardPacket() {
    
    _mutex.lock();
    
    if (_queueHead != NULL)
        _disposePacket(_popQueueFromHead()); 
    
    _mutex.unlock();
    
}

void AudioQueue::synchronize(uint32_t currentSampleTime, double currentTime, uint32_t nextSampleTime) {
    
    _mutex.lock();
    
    currentSampleTime -= 11025;
    
    if (_lastKnowSampleTime == 0 && _lastKnowSampleTimesTime == 0) {
        
        while (_queueHead && _queueHead->sampleTime < currentSampleTime)
            _disposePacket(_popQueueFromHead());
        
        log(LOG_INFO, "Queue was synced");
        NotificationCenter::defaultCenter()->postNotification(AudioQueue::syncNotificationName, this, NULL);
    }
    
    _lastKnowSampleTime = currentSampleTime;
    _lastKnowSampleTimesTime = currentTime;
    
    _mutex.unlock();
    
}

int AudioQueue::getNextMissingWindow(int* seqNo) {
    
    assert(seqNo != NULL);
    
    int ret = 0;
    int count = 0;
    
    _mutex.lock();
    
    LoopFrom(currentPacket, _queueHead, next, NULL) {
        
        if (currentPacket->state == kAudioPacketStateMissing) {
            *seqNo = currentPacket->seqNo % 0xFFFF;
            
            LoopFrom(missingPacket, currentPacket, next, NULL) {
                if (missingPacket->state == kAudioPacketStateMissing) {
                    missingPacket->state = kAudioPacketStateRequested;
                    ret++;
                    count++;
                } else
                    break;
            }
            
        }

        count++;
        
        if (ret > 0 || count >= _queueCount - 100)
            break;
        
    }
    
    _mutex.unlock();
    
    if (_missingCount < _queueCount / 2)
        return MIN(ret, _missingCount);
    
    return 0;
    
}

int AudioQueue::getQueuePacketCount() {
    
    int ret = 0;
    _mutex.lock();
    ret = _queueCount;
    _mutex.unlock();
    
    return ret;
    
}

bool AudioQueue::awaitAvailabilty() {
    
    _mutex.lock();
    
    while ((_synchronizationEnabled && _queueCount == MAX_QUEUE_COUNT) || (!_synchronizationEnabled && (_queueFrameCount >= _sampleRate * 4)))
        _condition.wait(&_mutex);
    
    bool ret = !_disposed;
    
    _mutex.unlock();
    
    return ret;
    
}
