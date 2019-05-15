// Copyright 2019 ByteDance Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include <cstring>
#include <memory>
#include <thread>
#include <chrono>
#include <cuda_runtime.h>

#include "logging.h"
#include "operations.h"
#include "global.h"

namespace byteps {
namespace common {

bool RunCoordinateLoopOnce() {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    return true;
}

bool RunCopyDevice2HostLoopOnce() {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    return true;
}

bool RunReduceLoopOnce() {
    QueueType this_op = REDUCE;
    auto q = BytePSGlobal::GetScheduledQueue(this_op);
    auto reduce_stream =  BytePSGlobal::GetReduceStream();
    auto task = q->getTask();
    if (task) {
        BPS_CHECK(task->tensor);
        BPS_CHECK_GE(task->queue_list.size(), 1);
        task->queue_list.erase(task->queue_list.begin());

        if (task->device != CPU_DEVICE_ID) { // GPU
            auto name = task->tensor_name;
            auto len = task->len;
            auto offset = task->offset;
            auto cpubuff = task->cpubuff + offset;
            BPS_CHECK(cpubuff) << name << ": CPU buffer not initialized, size=" << len;
            CUDA_CALL(cudaMemcpyAsync(cpubuff, task->tensor->data() + offset, len, cudaMemcpyDeviceToHost, *reduce_stream));
            CUDA_CALL(cudaStreamSynchronize(*reduce_stream));
        }

        if (task->queue_list.size() > 0) { // TODO: should check the boundary
            BPS_LOG(TRACE) << "Finish reducing tensor: " << task->tensor_name;
            BytePSGlobal::GetScheduledQueue(static_cast<QueueType>(task->queue_list.at(0)))->addTask(task);
        } else {
            task->callback(Status::OK());
        }
        q->reportFinish(task->len);
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunPushLoopOnce() {
    QueueType this_op = PUSH;
    auto q = BytePSGlobal::GetScheduledQueue(PUSH);
    auto task = q->getTask();
    if (task) {
        BPS_CHECK_GE(task->queue_list.size(), 1);
        task->queue_list.erase(task->queue_list.begin());
        // TODO: allow merging
        auto offset = task->offset;
        auto len = task->len;

        char* data;
        if (task->device != CPU_DEVICE_ID) {
            BPS_CHECK(task->cpubuff + offset);
            data = const_cast<char*> (static_cast<const char*> (task->cpubuff + offset));
        } else {
            BPS_CHECK(task->tensor);
            data = const_cast<char*> (static_cast<const char*> (task->tensor->data() + offset));
        }

        // get metadata
        const int dtype = task->tensor->dtype();

        // false means not to delete data when SArray is deleted
        ps::SArray<char> vals(data, len, false);

        int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
        auto& pskv = BytePSGlobal::EncodeDefaultKey(task->key, len);
        BytePSGlobal::GetPS()->ZPush(
            pskv.keys, vals, pskv.lens, cmd,
            [task, q]() {
                if (task->queue_list.size() > 0) {
                    BPS_LOG(TRACE) << "Finish pushing tensor: " << task->tensor_name;
                    BytePSGlobal::GetScheduledQueue(static_cast<QueueType>(task->queue_list.at(0)))->addTask(task);
                } else {
                    BPS_CHECK(task->counter_ptr) << task->tensor_name << " counter_ptr is null";
                    int v = task->counter_ptr.get()->fetch_add(1);
                    if (v == (task->total_partnum-1)) {
                        BPS_LOG(TRACE) << "Finish pushing tensor: " << task->tensor_name
                                   << ", invoking callback.";
                        task->callback(Status::OK());
                    }
                }
                q->reportFinish(task->len);
            }
        );
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunPullLoopOnce() {
    QueueType this_op = PULL;
    auto q = BytePSGlobal::GetScheduledQueue(PULL);
    auto task = q->getTask();
    if (task) {
        BPS_CHECK_GE(task->queue_list.size(), 1);
        task->queue_list.erase(task->queue_list.begin());
        // TODO: allow merging
        auto offset = task->offset;
        auto len = task->len;

        char* data;
        if (task->device != CPU_DEVICE_ID) { // GPU
            BPS_CHECK(task->cpubuff);
            data = const_cast<char*> (static_cast<const char*> (task->cpubuff + offset));
        } else { // CPU
            BPS_CHECK(task->output);
            data = const_cast<char*> (static_cast<const char*> (task->output->data() + offset));
        }

        // get metadata
        const int dtype = task->output->dtype();

        // false means not to delete data when SArray is deleted
        auto vals = new ps::SArray<char>(data, len, false);

        int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
        auto& pskv = BytePSGlobal::EncodeDefaultKey(task->key, len);
        // issue pull
        BytePSGlobal::GetPS()->ZPull(
            pskv.keys, vals, &pskv.lens, cmd,
            [vals, task, q]() {
                delete vals;
                if (task->queue_list.size() > 0) {
                    BPS_LOG(TRACE) << "Finish pulling tensor: " << task->tensor_name;
                    BytePSGlobal::GetScheduledQueue(static_cast<QueueType>(task->queue_list.at(0)))->addTask(task);
                } else {
                    task->callback(Status::OK());
                }
                q->reportFinish(task->len);
            });
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunCopyHost2DeviceLoopOnce() {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    return true;
}

bool RunBroadcastLoopOnce() {
    QueueType this_op = BROADCAST;
    auto q = BytePSGlobal::GetScheduledQueue(BROADCAST);
    auto broadcast_stream = BytePSGlobal::GetBroadcastStream();
    auto task = q->getTask();
    if (task) {
        BPS_CHECK(task->output);
        BPS_CHECK_GE(task->queue_list.size(), 1);
        task->queue_list.erase(task->queue_list.begin());

        if (task->device != CPU_DEVICE_ID) { // GPU
            auto name = task->tensor_name;
            auto len = task->len;
            auto offset = task->offset;

            auto cpubuff = task->cpubuff + offset;
            BPS_CHECK(cpubuff) << name << ": CPU buffer not initialized, size=" << len;
            char* gpu_addr = const_cast<char*> (static_cast<const char*> (task->output->data() + offset));
            CUDA_CALL(cudaMemcpyAsync(gpu_addr, cpubuff, len, cudaMemcpyHostToDevice, *broadcast_stream));
            CUDA_CALL(cudaStreamSynchronize(*broadcast_stream));
        }

        BPS_CHECK_EQ(task->queue_list.size(), 0)
            << "BROADCAST should be the last op, remain queue_list size: " << task->queue_list.size();

        BPS_CHECK(task->counter_ptr) << task->tensor_name << " counter_ptr is null";
        int v = task->counter_ptr.get()->fetch_add(1);
        if (v == (task->total_partnum-1)) {
            BPS_LOG(TRACE) << "Finish broadcasting tensor: " << task->tensor_name;
            task->callback(Status::OK());
        }
        q->reportFinish(task->len);
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

void CoordinateLoop() {
    while (RunCoordinateLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void CopyDevice2HostLoop() {
    while (RunCopyDevice2HostLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void ReduceLoop() {
    while (RunReduceLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void PushLoop() {
    while (RunPushLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void PullLoop() {
    while (RunPullLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void CopyHost2DeviceLoop() {
    while (RunCopyHost2DeviceLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void BroadcastLoop() {
    while (RunBroadcastLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}



extern "C" {

void byteps_init() {
    BytePSGlobal::Init();
    LoopFunction func[ThreadNum] = {CoordinateLoop, CopyDevice2HostLoop, ReduceLoop, PushLoop, PullLoop, CopyHost2DeviceLoop, BroadcastLoop};
    BytePSGlobal::Start(func);
    return;
}

void byteps_shutdown() {
    BytePSGlobal::Shutdown();
    BPS_LOG(TRACE) << "BytePS is shutdown.";
    return;
}

int byteps_rank() {
    return BytePSGlobal::GetRank();
}

int byteps_local_rank() {
    return BytePSGlobal::GetLocalRank();
}

int byteps_size() {
    return BytePSGlobal::GetSize();
}

int byteps_local_size() {
    return BytePSGlobal::GetLocalSize();
}

} // extern "C"

Status CheckInitialized() {
    return BytePSGlobal::CheckInit();
}

void PartitionTensor(std::shared_ptr<TensorTableEntry> entry,
                    std::vector<std::shared_ptr<TensorTableEntry> > &partitions) {
    BPS_CHECK(entry->counter_ptr) << entry->tensor_name << " counter pointer is null";
    auto size = entry->tensor ? entry->tensor->size() : entry->output->size();
    auto bound = BytePSGlobal::GetPartitionBound();
    auto accumulated = 0;
    int i = 0;

    while (accumulated < size) {
        std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
        // will assign the key later, so don't do it now
        // e->key = entry->key;
        e->tensor_name = entry->tensor_name + std::string("_") + std::to_string(i);
        e->context = entry->context;
        e->ready_event = entry->ready_event;
        e->device = entry->device;
        e->priority = entry->priority;
        e->version = entry->version;
        e->callback = entry->callback;
        e->cpubuff = entry->cpubuff;
        e->queue_list = entry->queue_list;
        e->tensor = entry->tensor;
        e->output = entry->output;
        e->offset = accumulated;
        e->len = ((size - accumulated) > bound) ? bound : (size - accumulated);
        e->counter_ptr = entry->counter_ptr;
        e->total_partnum = entry->total_partnum;

        accumulated += e->len;
        ++i;

        partitions.push_back(e);
    }
}

Status EnqueueTensor(BPSContext &context,
                     std::shared_ptr<Tensor> input,
                     std::shared_ptr<Tensor> output,
                     std::shared_ptr<ReadyEvent> ready_event,
                     const std::string &name,
                     const int device, const int priority, const int version,
                     StatusCallback callback, std::vector<QueueType> queue_list) {
    if (input && output) {
        BPS_CHECK_EQ(input->size(), output->size()) << name << " output tensor size does not match";
    }

    std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
    e->tensor_name = name;
    e->context = &context;
    e->tensor = input;
    e->output = output;
    e->ready_event = ready_event;
    e->device = device;
    e->priority = priority;
    e->version = version;
    e->callback = callback;
    e->cpubuff = context.cpubuff;
    e->queue_list = queue_list;
    e->counter_ptr = std::make_shared<std::atomic_int>(0);
    e->total_partnum = context.key_list.size();

    std::vector<std::shared_ptr<TensorTableEntry> > partitions;
    PartitionTensor(e, partitions);
    BPS_CHECK_EQ(context.key_list.size(), partitions.size()) << name
            << ": " << context.key_list.size()
            << ", " << partitions.size();

    unsigned int accumulated = 0;
    for (unsigned int i = 0; i < partitions.size(); ++i) {
        auto task = partitions[i];
        task->key = context.key_list[i]; // assign the key now
        BPS_LOG(TRACE) << "EnqueueTensor: " << task->tensor_name
                       << ", key=" << task->key
                       << ", offset=" << task->offset
                       << ", len=" << task->len
                       << ", device=" << task->device;
        BytePSGlobal::GetScheduledQueue(e->queue_list.at(0))->addTask(task);
        accumulated += task->len;
    }
    BPS_CHECK_EQ(accumulated, (e->tensor ? e->tensor->size(): e->output->size()))
        << "accumulated partition size not equal to original tensor size";

    BPS_LOG(TRACE) << "EnqueueTensor finished: " << name;
    return Status::OK();
}

void InitTensor(BPSContext &context, const std::string &name, int dtype, void *cpubuff) {

    size_t size = context.buff_len;

    // Only local root needs to init cpubuff
    if (IsRootDevice()) {
        if (cpubuff) {
            BPS_LOG(TRACE) << name << " is already on cpu, len=" << size;
            context.cpubuff = cpubuff;
            context.reuse_buff = true;
        }
        else {
            CUDA_CALL(cudaHostAlloc((void **) &(context.cpubuff), size, cudaHostAllocMapped));
            context.reuse_buff = false;
            BPS_LOG(TRACE) << name << ": cudaHostAlloc with len=" << size;
        }
    }

    // Get metadata
    auto key_list = context.key_list;
    char* data = const_cast<char*> (static_cast<const char*> (context.cpubuff));
    auto bound = BytePSGlobal::GetPartitionBound();
    unsigned int accumulated = 0;
    unsigned int i = 0;

    BPS_LOG(TRACE) << "Init " << name
                    << ", size=" << size
                    << ", parts=" << key_list.size();
    BPS_CHECK_GT(key_list.size(), 0) << name << " key_list_size=0";
    BPS_CHECK_EQ(key_list.size(), (unsigned int) (size+bound-1)/bound) // round up
                    << key_list.size()
                    << ", size=" << size
                    << ", bound=" << bound;

    while (accumulated < size) {
        auto key = key_list[i];
        int len = ((size - accumulated) > bound) ? bound : (size - accumulated);
        auto& pskv = BytePSGlobal::EncodeDefaultKey(key, len);

        if (IsRootDevice() && (BytePSGlobal::GetRank() < BytePSGlobal::GetLocalSize())) {
            // false means not to delete data when SArray is deleted
            ps::SArray<char> vals(data + accumulated, len, false);
            int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
            BytePSGlobal::GetPS()->Wait(BytePSGlobal::GetPS()->ZPush(
                pskv.keys, vals, pskv.lens, cmd));
        }

        accumulated += len;
        ++i;

        if (IsRootDevice()) {
            ps::Postoffice::Get()->Barrier(0, ps::kWorkerGroup);
        }
    }
    BPS_CHECK_EQ(accumulated, size);
    BPS_CHECK_EQ(i, key_list.size());

    context.initialized = true;
    BPS_LOG(TRACE) << "Init finish " << name;
}

Status EnqueueTensorInit(BPSContext &context, const std::string &name, int dtype, void *cpubuff,
                         StatusCallback callback) {
    InitTensor(context, name, dtype, cpubuff);
    callback(Status::OK());
    return Status::OK();
}

BPSContext& GetContextFromName(const std::string &name) {
    return BytePSGlobal::GetContextFromName(name);
}

bool IsTensorInitialized(const std::string &name, size_t size) {
    return BytePSGlobal::IsTensorInitialized(name, size);
}

bool IsRootDevice() {
    return BytePSGlobal::IsRootDevice();
}

} // namespace common
} // namespace byteps
