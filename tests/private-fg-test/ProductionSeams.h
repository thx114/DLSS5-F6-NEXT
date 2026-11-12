// Include Windows declarations before substituting only these production calls.
// File open/read/close and SHA256 calculation remain real. FinishHash can replace
// the completed digest for explicitly labelled state-machine cases.
#include "Harness.h"
#define GetModuleFileNameW PrivateFgFixture::ModuleFileName
#define CreateFileW PrivateFgFixture::OpenFile
#define ReadFile PrivateFgFixture::ReadFile
#define CreateToolhelp32Snapshot PrivateFgFixture::Snapshot
#define Module32FirstW PrivateFgFixture::FirstModule
#define Module32NextW PrivateFgFixture::NextModule
#define BCryptFinishHash PrivateFgFixture::FinishHash
#define ReadProcessMemory PrivateFgFixture::ReadMemory
#define VirtualQuery PrivateFgFixture::QueryMemory
