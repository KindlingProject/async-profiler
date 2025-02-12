/*
 * Copyright 2022 The Kindling Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lockRecorder.h"
#include <iostream>
#include "timeUtil.h"

u64 expiredDuration = 30e9;

void LockRecorder::recordLockedThread(uintptr_t lock_address, LockWaitEvent* event) {
    auto last_locked_thread_it = _locked_thread_map->find(lock_address);
    if (last_locked_thread_it == _locked_thread_map->end()) {
        // No last thread found
        _locked_thread_map->emplace(lock_address, event);
    } else {
        LockWaitEvent* lastEvent = last_locked_thread_it->second;
        delete lastEvent;
        (*_locked_thread_map)[lock_address] = event;
    }
}

void LockRecorder::clearLockedThread() {
    jlong current_timestamp = getCurrentTimestamp();
    lock_guard<mutex> lock(_mutex);
    auto i = this->_locked_thread_map->begin();
    while (i != this->_locked_thread_map->end()) {
        auto event = i->second;
        // Check if there is a thread waiting to lock it.
        auto lock_address = i->first;
        auto wait_lock_thread_it = _wait_lock_map->find(lock_address);
        // We never remove the events if there is a thread waiting for.
        if (wait_lock_thread_it != _wait_lock_map->end()) {
            ++i;
            continue;
        }
        if (current_timestamp - event->_wait_timestamp > expiredDuration) {
            delete i->second;
            i = this->_locked_thread_map->erase(i);
        } else {
            ++i;
        }
    }
}

bool isConcurrentLock(const string lock_name) {
    // Do not count synchronizers other than ReentrantLock, ReentrantReadWriteLock
    return lock_name == "Ljava/util/concurrent/locks/ReentrantLock" ||
           lock_name == "Ljava/util/concurrent/locks/ReentrantReadWriteLock";
}

// Thread-safe must be guaranteed.
void LockRecorder::updateWaitLockThread(LockWaitEvent* event) {
    uintptr_t lock_address = event->_lock_object_address;
    jint native_thread_id = event->_native_thread_id;
    bool concurrentLock = event->_lock_type != "UnsafePark" || isConcurrentLock(event->_lock_name);
    if (concurrentLock) {
        jint locked_thread = findContendedThreads(lock_address, native_thread_id);
        event->_wait_thread_id = locked_thread;
    }

    lock_guard<mutex> lock(_mutex);
    auto wait_iterator = _wait_lock_map->find(lock_address);
    auto threads_map = wait_iterator->second;
    // No object in the map
    if (wait_iterator == _wait_lock_map->end()) {
        threads_map = new map<jint, LockWaitEvent*>();
        threads_map->emplace(native_thread_id, event);
        _wait_lock_map->emplace(lock_address, threads_map);
        return;
    }

    // There is the object in the map. Try to find the thread_id.
    auto thread_iterator = threads_map->find(native_thread_id);
    // No the thread_id in the map.
    if (thread_iterator == threads_map->end()) {
        threads_map->emplace(native_thread_id, event);
        return;
    }

    // It is supported not to enter this branch.
    // Because one lock can not be waited by a same thread twice. 
    // TODO: remove the println code.
    // printf("[WARN] A lock is waited by a same thread twice. thread_id=%d, thread_name=%s.\n", native_thread_id, event->_thread_name.data());
    delete event;
}

void LockRecorder::updateWakeThread(uintptr_t lock_address, jint thread_id, string thread_name, jlong wake_timestamp) {
    lock_guard<mutex> lock(_mutex);
    auto wait_iterator = _wait_lock_map->find(lock_address);
    if (wait_iterator == _wait_lock_map->end()) {
        // This should not happen because there should be a same lock.
        // printf("[WARN] There is no the same lock. lock_address=%lu, thread_id=%d, thread_name=%s.\n", lock_address, thread_id, thread_name.data());
        return;
    }

    auto threads_map = wait_iterator->second;
    auto event_iterator = threads_map->find(thread_id);
    if (event_iterator == threads_map->end()) {
        // This should not happen because there should be a waited thread before it is waked.
        // printf("[WARN] There is no the same thread waiting to be waked. thread_id=%d, thread_name=%s.\n", thread_id, thread_name.data());
        return;
    }
    LockWaitEvent* event = event_iterator->second;
    threads_map->erase(thread_id);
    if (threads_map->size() == 0) {
        _wait_lock_map->erase(lock_address);
        delete threads_map;
    }
    event->_wake_timestamp = wake_timestamp;
    event->_wait_duration = wake_timestamp - event->_wait_timestamp;

    recordLockedThread(lock_address, event);

    if (!filter(event)) {
        // event->print();
        event->log();
    }
}

jint LockRecorder::findContendedThreads(uintptr_t lock_address, jint thread_id) {
    unique_lock<mutex> uni_lock(_mutex, defer_lock);
    uni_lock.lock();
    auto last_locked_thread_it = _locked_thread_map->find(lock_address);
    uni_lock.unlock();
    if (last_locked_thread_it == _locked_thread_map->end() 
     || last_locked_thread_it->second->_native_thread_id == thread_id) {
        // No last thread found
        return -1;
    }
    return last_locked_thread_it->second->_native_thread_id;
}

// 11ms
jlong threshold = 11 * 1e6;
inline bool filter(LockWaitEvent* event) {
    jlong duration = event->_wait_duration;
    if (duration < threshold) {
        return true;
    }
    return false;
}

void LockRecorder::reset() {
    lock_guard<mutex> lock(_mutex);
    for (auto it = _locked_thread_map->begin(); it != _locked_thread_map->end(); it++) {
        auto event = it->second;
        delete event;
    }
    _locked_thread_map->clear();
    for (auto it = _wait_lock_map->begin(); it != _wait_lock_map->end(); it++) {
        auto thread_map = it->second;
        for (auto ite = thread_map->begin(); ite != thread_map->end(); ite++) {
            auto event = ite->second;
            delete event;
        }
        delete thread_map;
    }
    _wait_lock_map->clear();
}

void LockRecorder::startClearLockedThreadTask() {
    _clear_map_task = new ClearMapTask(this);
    _clear_map_thread = std::thread([&]{
        VM::attachThread("AsyncProfiler-Lock-Clearer");
        _clear_map_task->run();
        VM::detachThread();
    });
}
void LockRecorder::endClearLockedThreadTask() {
    if (_clear_map_task != NULL) {
        _clear_map_task->stop();
        _clear_map_thread.join();
        delete _clear_map_task;
        _clear_map_task = NULL;
    }
}