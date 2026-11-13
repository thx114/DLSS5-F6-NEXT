#pragma once
#include <Windows.h>
#include <winternl.h>
#include <optional>
#ifndef STATUS_DLL_NOT_FOUND
#define STATUS_DLL_NOT_FOUND static_cast<NTSTATUS>(0xC0000135L)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
