/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "MemoryTracker.h"

#include <sanitizer.h>

#include <iostream>
#include <map>
#include <vector>

// TODO: handle multiple contexts

// TODO: allow override of array size through env variable

// TODO: write in a file instead of stdout

struct LaunchData
{
    std::string functionName;
    MemoryAccessTracker* pTracker;
};

struct CallbackTracker
{
    std::map<CUstream, std::vector<LaunchData>> memoryTrackers;
};

void ModuleLoaded(Sanitizer_ResourceModuleData* pModuleData)
{
    // Instrument user code!
    sanitizerAddPatchesFromFile("MemoryTrackerPatches.fatbin", 0);
    sanitizerPatchInstructions(SANITIZER_INSTRUCTION_MEMORY_ACCESS, pModuleData->module, "MemoryAccessCallback");
    sanitizerPatchModule(pModuleData->module);
}

void LaunchBegin(
    CallbackTracker* pCallbackTracker,
    std::string functionName,
    CUstream stream)
{
    constexpr size_t MemAccessDefaultSize = 10240000;

    // alloc MemoryAccess array
    MemoryAccess* accesses = nullptr;
    sanitizerAlloc((void**)&accesses, sizeof(MemoryAccess) * MemAccessDefaultSize);
    sanitizerMemset(accesses, 0, sizeof(MemoryAccess) * MemAccessDefaultSize, stream);

    MemoryAccessTracker hTracker;
    hTracker.currentEntry = 0;
    hTracker.maxEntry = MemAccessDefaultSize;
    hTracker.accesses = accesses;

    MemoryAccessTracker* dTracker = nullptr;
    sanitizerAlloc((void**)&dTracker, sizeof(*dTracker));
    sanitizerMemcpyHostToDeviceAsync(dTracker, &hTracker, sizeof(*dTracker), stream);

    sanitizerSetCallbackData(0, dTracker);

    LaunchData launchData = {functionName, dTracker};
    std::vector<LaunchData>& deviceTrackers = pCallbackTracker->memoryTrackers[stream];
    deviceTrackers.push_back(launchData);
}

static std::string GetMemoryRWString(uint32_t flags)
{
    if (flags & SANITIZER_MEMORY_DEVICE_FLAG_READ)
    {
        return "Read";
    }
    else if (flags & SANITIZER_MEMORY_DEVICE_FLAG_WRITE)
    {
        return "Write";
    }
    else
    {
        return "Unknown";
    }
}

static std::string GetMemoryTypeString(uint32_t flags)
{
    if (flags & SANITIZER_MEMORY_DEVICE_FLAG_LOCAL)
    {
        return "local";
    }
    else if (flags & SANITIZER_MEMORY_DEVICE_FLAG_SHARED)
    {
        return  "shared";
    }
    else
    {
        return "global";
    }
}
/** There is another attribute needed to be considered.*/
void MallocedMemory(Sanitizer_ResourceMemoryData *pMallocedMemory){
//    start address
    std::cout<<"Malloc|"<<std::hex<<pMallocedMemory->flags<<"|"<<pMallocedMemory->address<<std::dec
// size (bytes)
    <<"|"<<pMallocedMemory->size<<std::endl;
}

void StreamSynchronized(
    CallbackTracker* pCallbackTracker,
    CUstream stream)
{
    MemoryAccessTracker hTracker = {0};

    std::vector<LaunchData>& deviceTrackers = pCallbackTracker->memoryTrackers[stream];

    for (auto& tracker : deviceTrackers)
    {
        std::cout << "Kernel Launch: " << tracker.functionName << std::endl;

        sanitizerMemcpyDeviceToHost(&hTracker, tracker.pTracker, sizeof(*tracker.pTracker), stream);

        uint32_t numEntries = std::min(hTracker.currentEntry, hTracker.maxEntry);

        std::cout << "  Memory accesses: " << numEntries << std::endl;

        std::vector<MemoryAccess> accesses(numEntries);
        sanitizerMemcpyDeviceToHost(accesses.data(), hTracker.accesses, sizeof(MemoryAccess) * numEntries, stream);

        for (uint32_t i = 0; i < numEntries; ++i)
        {
            MemoryAccess& access = accesses[i];

//            std::cout << "  [" << i << "] " << GetMemoryRWString(access.flags)
//                      << " pc 0x"<<std::hex<<access.pc
//                      << " access of " << GetMemoryTypeString(access.flags)
//                      << " memory by thread (" << access.threadId.z
//                      << "," << access.threadId.y
//                      << "," << access.threadId.x
//                      << ") at address 0x" << std::hex << access.address << std::dec
//                      << " (size is " << access.accessSize << " bytes)" << std::endl;


// Use simple format can greatly reduce log filesize.
            std::cout << i << "|" << GetMemoryRWString(access.flags)
//            pc
                      << "|"<<std::hex<<access.pc<<std::dec
// memory type
                      << "|" << GetMemoryTypeString(access.flags)
//                      thread id
                      << "|" << access.blockId.z
                      << "," << access.blockId.y
                      << "," << access.blockId.x
                      << "," << access.threadId.z
                      << "," << access.threadId.y
                      << "," << access.threadId.x
                      << "|"
                      << std::hex << access.address << std::dec
                      << "|" << access.accessSize << std::endl;
//
//
        }

        sanitizerFree(hTracker.accesses);
        sanitizerFree(tracker.pTracker);
    }

    deviceTrackers.clear();
}

void ContextSynchronized(CallbackTracker* pCallbackTracker)
{
    for (auto& streamTracker : pCallbackTracker->memoryTrackers)
    {
        StreamSynchronized(pCallbackTracker, streamTracker.first);
    }
}

void MemoryTrackerCallback(
    void* userdata,
    Sanitizer_CallbackDomain domain,
    Sanitizer_CallbackId cbid,
    const void* cbdata)
{
    auto* callbackTracker = (CallbackTracker*)userdata;

    switch (domain)
    {
        case SANITIZER_CB_DOMAIN_RESOURCE:
            switch (cbid)
            {
                case SANITIZER_CBID_RESOURCE_MODULE_LOADED:
                {
                    auto* pModuleData = (Sanitizer_ResourceModuleData*)cbdata;
                    ModuleLoaded(pModuleData);
                    break;
                }
                case SANITIZER_CBID_RESOURCE_DEVICE_MEMORY_ALLOC:{
                    auto* pMallocData = (Sanitizer_ResourceMemoryData*)cbdata;
                    MallocedMemory(pMallocData);
                    break;
                }
                default:
                    break;
            }
            break;
        case SANITIZER_CB_DOMAIN_LAUNCH:
            switch (cbid)
            {
                case SANITIZER_CBID_LAUNCH_BEGIN:
                {
                    auto* pLaunchData = (Sanitizer_LaunchData*)cbdata;
                    LaunchBegin(callbackTracker, pLaunchData->functionName, pLaunchData->stream);
                    break;
                }
                default:
                    break;
            }
            break;
        case SANITIZER_CB_DOMAIN_SYNCHRONIZE:
            switch (cbid)
            {
                case SANITIZER_CBID_SYNCHRONIZE_STREAM_SYNCHRONIZED:
                {
                    auto* pSyncData = (Sanitizer_LaunchData*)cbdata;
                    StreamSynchronized(callbackTracker, pSyncData->stream);
                    break;
                }
                case SANITIZER_CBID_SYNCHRONIZE_CONTEXT_SYNCHRONIZED:
                {
                    ContextSynchronized(callbackTracker);
                    break;
                }
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

int InitializeInjection()
{
    Sanitizer_SubscriberHandle handle;
    std::cout << "  Init findhao"<< std::endl;
    CallbackTracker* tracker = new CallbackTracker();

    sanitizerSubscribe(&handle, MemoryTrackerCallback, tracker);
    sanitizerEnableAllDomains(1, handle);

    return 0;
}

int __global_initializer__ = InitializeInjection();
