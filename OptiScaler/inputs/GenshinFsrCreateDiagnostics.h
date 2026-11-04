// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

// Observation only. No process-wide handler, provider replacement or recovery.
namespace GenshinFsrCreateDiagnostics
{
enum class Event : unsigned { Begin, Returned, Exception };
struct Attempt
{
    uintptr_t context=0, commandList=0, parameters=0, device=0;
    unsigned renderWidth=0, renderHeight=0, outputWidth=0, outputHeight=0, flags=0, layout=0;
};
struct Image
{
    uintptr_t address=0, base=0, rva=0;
    DWORD timestamp=0, size=0;
    char path[512]{};
    bool pathComplete=false;
};
struct Report
{
    Event event{};
    Attempt attempt{};
    DWORD pid=0, tid=0, code=0, parameterCount=0;
    ULONG_PTR operation=0, faultAddress=0;
    unsigned result=0, frameCount=0;
    bool stackComplete=false;
    Image frames[12]{}; // Frame zero is the exception PC, never an observer backtrace.
};
using Call=unsigned (*)(void*);
using Sink=void (*)(const Report&) noexcept;
inline std::atomic<unsigned> attempts{0}, exceptions{0};
inline void Emit(Sink sink,const Report* report) noexcept
{
    // Contain only failures in our observation sink, never the provider call.
    __try {if(sink)sink(*report);}
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
inline bool Take(std::atomic<unsigned>& counter,unsigned limit) noexcept
{
    auto n=counter.load(std::memory_order_relaxed);
    while(n<limit)
        if(counter.compare_exchange_weak(n,n+1,std::memory_order_relaxed))return true;
    return false;
}
inline Image Locate(uintptr_t address) noexcept
{
    Image out{};out.address=address;
    MEMORY_BASIC_INFORMATION memory{};
    if(!address || VirtualQuery(reinterpret_cast<void*>(address),&memory,sizeof(memory))!=sizeof(memory) ||
       memory.Type!=MEM_IMAGE)return out;
    out.base=reinterpret_cast<uintptr_t>(memory.AllocationBase);out.rva=address-out.base;
    const auto count=GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase),out.path,sizeof(out.path));
    out.pathComplete=count>0 && count<sizeof(out.path);out.path[sizeof(out.path)-1]=0;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};SIZE_T read=0;
    if(ReadProcessMemory(GetCurrentProcess(),memory.AllocationBase,&dos,sizeof(dos),&read) && read==sizeof(dos) &&
       dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>0 && dos.e_lfanew<1048576 &&
       ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(out.base+dos.e_lfanew),&nt,sizeof(nt),&read) &&
       read==sizeof(nt) && nt.Signature==IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {out.timestamp=nt.FileHeader.TimeDateStamp;out.size=nt.OptionalHeader.SizeOfImage;}
    return out;
}
inline bool Interesting(DWORD code) noexcept
{
    return code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_IN_PAGE_ERROR ||
           code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_INT_DIVIDE_BY_ZERO ||
           code==EXCEPTION_INT_OVERFLOW;
}
// Isolate unwind-metadata/stack faults from the original exception. No handlers run.
inline void CaptureStack(Report* report,const CONTEXT* fault) noexcept
{
#if defined(_M_X64)
    __try
    {
        CONTEXT frame=*fault;
        for(unsigned i=0;i<12 && frame.Rip;++i)
        {
            report->frames[report->frameCount++]=Locate(static_cast<uintptr_t>(frame.Rip));
            const DWORD64 previousSp=frame.Rsp, previousPc=frame.Rip;
            DWORD64 base=0;
            auto* function=RtlLookupFunctionEntry(frame.Rip,&base,nullptr);
            if(function)
            {
                void* handler=nullptr;DWORD64 establisher=0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,frame.Rip,function,&frame,&handler,&establisher,nullptr);
            }
            else
            {
                DWORD64 next=0;SIZE_T read=0;
                if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(frame.Rsp),&next,sizeof(next),&read) ||
                   read!=sizeof(next))return;
                frame.Rip=next;frame.Rsp+=sizeof(next);
            }
            if(!frame.Rip){report->stackComplete=true;return;}
            if(frame.Rsp<previousSp || (frame.Rsp==previousSp && frame.Rip==previousPc) ||
               frame.Rsp-previousSp>1048576)return;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {} // Diagnostic unwind failed; original still propagates.
#else
    (void)report;(void)fault;
#endif
}
inline LONG Filter(EXCEPTION_POINTERS* pointers,const Attempt* attempt,Sink sink) noexcept
{
    if(!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord || !attempt || !sink ||
       !Interesting(pointers->ExceptionRecord->ExceptionCode) || !Take(exceptions,2))return EXCEPTION_CONTINUE_SEARCH;
    Report report{};report.event=Event::Exception;report.attempt=*attempt;
    report.pid=GetCurrentProcessId();report.tid=GetCurrentThreadId();
    const auto& exception=*pointers->ExceptionRecord;
    report.code=exception.ExceptionCode;report.parameterCount=exception.NumberParameters;
    if(exception.NumberParameters>0)report.operation=exception.ExceptionInformation[0];
    if(exception.NumberParameters>1)report.faultAddress=exception.ExceptionInformation[1];
    CaptureStack(&report,pointers->ContextRecord);
    if(!report.frameCount)
        report.frames[report.frameCount++]=Locate(reinterpret_cast<uintptr_t>(exception.ExceptionAddress));
    Emit(sink,&report);
    return EXCEPTION_CONTINUE_SEARCH;
}
// Keep this SEH leaf free of C++ objects requiring unwinding (/EHsc, not /EHa).
inline unsigned Invoke(Call call,void* args,const Attempt* attempt,Sink sink)
{
    __try {return call(args);}
    __except(Filter(GetExceptionInformation(),attempt,sink)) {return 0;} // Filter always continues search.
}
inline unsigned Observe(Call call,void* args,const Attempt& attempt,Sink sink)
{
    const bool sampled=sink && Take(attempts,8);
    Report report{};
    if(sampled)
    {
        report.event=Event::Begin;report.attempt=attempt;
        report.pid=GetCurrentProcessId();report.tid=GetCurrentThreadId();Emit(sink,&report);
    }
    const unsigned result=Invoke(call,args,&attempt,sink);
    if(sampled){report.event=Event::Returned;report.result=result;Emit(sink,&report);}
    return result;
}
}
