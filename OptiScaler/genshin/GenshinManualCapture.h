// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace GenshinLegacy::ManualCapture {
enum class Phase : uint8_t { Off, Requested, Starting, Armed, Capturing, Draining, Complete, Failed };
enum class Reason : uint8_t { None, AwaitingNativeBoundary, UserStop, DurationLimit, CountLimit, DiskLimit,
    RequestTimeout, ShaderHookUnavailable, OutputFailure, DeviceChanged, ContextBoundary, Shutdown,
    Busy, SessionLimit, InternalFailure };
struct Snapshot {
    uint32_t apiVersion=1;
    bool supported=false;
    Phase phase=Phase::Off;
    Reason reason=Reason::None;
    uint32_t processId=0;
    uint64_t processCreationFileTime=0,sessionNumber=0,elapsedMilliseconds=0;
    uint64_t shaderArtifactsWritten=0,constantArtifactsWritten=0,bindingRecordsWritten=0,recordsWritten=0,diskBytes=0;
    uint64_t queueDrops=0,writeFailures=0,knownShadersCached=0;
    bool shaderHookInstalled=false,entryHookInstalled=false,workerFinished=false;
    uint64_t maxSessionMilliseconds=60000,maxDiskBytes=32*1024*1024,maxQueuePayloadBytes=8*1024*1024;
    uint32_t maxSessions=8,maxShaderArtifacts=32,maxConstantSamples=128,maxBindingSamples=128,maxQueueJobs=16;
    wchar_t outputDirectory[1024]{};
};
// Menu-thread CPU operations. true acknowledges a request, never successful data export.
bool RequestArm() noexcept;
void RequestStop() noexcept;
Snapshot GetSnapshot() noexcept;
}
