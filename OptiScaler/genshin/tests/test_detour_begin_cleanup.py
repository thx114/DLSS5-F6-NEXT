"""Exercise the extracted provenance Begin cleanup with Detours ownership fault injection."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative', action='store_true')
args = parser.parse_args()
source = (ROOT / 'OptiScaler/genshin/GenshinNativeQueryProvenance.h').read_text(encoding='utf-8-sig')

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

helper = function('inline LONG BeginDetourTransaction()')
wrapper = function('inline LONG Begin(InstallRequirement')
assert wrapper.count('BeginDetourTransaction()') == 3
assert 'DetourTransactionBegin()' not in wrapper
batch = (ROOT / 'OptiScaler/genshin/GenshinOwnHookProvenance.h').read_text(encoding='utf-8-sig')
assert 'error_=GenshinNativeQuery::Begin(requirement);begun_=error_==NO_ERROR;' in batch
assert '~Batch(){if(begun_ && !finished_)AbortBatch();}' in batch
if args.negative:
    needle = 'if(result!=NO_ERROR && result!=ERROR_INVALID_OPERATION)'
    assert helper.count(needle) == 1
    helper = helper.replace(needle, 'if(false)')
fixture = r'''
#include <cstdio>
#include <stdexcept>
#include <initializer_list>
using LONG=long;
constexpr LONG NO_ERROR=0,ERROR_INVALID_OPERATION=4317;
bool owned=false,revoked=false;
LONG injectedBegin=0,injectedAbort=0;
unsigned beginCalls=0,abortCalls=0,checks=0;
LONG DetourTransactionBegin() {
    ++beginCalls;
    if(owned) return ERROR_INVALID_OPERATION;
    owned=true;
    return injectedBegin;
}
LONG DetourTransactionAbort() {
    ++abortCalls;
    if(injectedAbort==NO_ERROR) owned=false;
    return injectedAbort;
}
namespace GenshinNativeQuery { void Revoke() { revoked=true; }
'''
fixture += helper + '\n}\n'
fixture += r'''
void Check(bool ok,const char* message) { ++checks; if(!ok)throw std::runtime_error(message); }
int main() {try {
    using GenshinNativeQuery::BeginDetourTransaction;
    for(LONG failure:{8L,5L,14L,487L}) {
        owned=false;revoked=false;injectedBegin=failure;injectedAbort=0;abortCalls=0;
        Check(BeginDetourTransaction()==failure,"Begin preserves exact native error");
        Check(!owned && abortCalls==1,"owned Begin failure must abort before return");
        Check(!revoked,"successful rollback does not revoke provenance");
        injectedBegin=0;
        Check(BeginDetourTransaction()==0 && owned,"later transaction is not stranded");
        Check(abortCalls==1,"successful Begin remains owned by caller");
        DetourTransactionAbort();
    }
    owned=true;abortCalls=0;revoked=false;
    Check(BeginDetourTransaction()==ERROR_INVALID_OPERATION,"foreign owner rejects Begin");
    Check(owned && abortCalls==0,"foreign transaction must never be aborted");
    Check(!revoked,"foreign transaction rejection does not revoke through cleanup");
    owned=false;injectedBegin=8;injectedAbort=5;abortCalls=0;
    Check(BeginDetourTransaction()==8,"Abort failure must not replace original Begin error");
    Check(owned && abortCalls==1 && revoked,"unknown rollback revokes provenance");
    const auto attempts=abortCalls;
    Check(BeginDetourTransaction()==ERROR_INVALID_OPERATION && abortCalls==attempts,
          "unknown rollback owner cannot be blindly aborted by next Begin");
    std::printf("PASS %u production Begin cleanup CPU checks; three wrapper branches wired; Detours fault mocks, no SDK/GPU/game\n",checks);
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL Detours Begin cleanup: %s (check %u)\n",error.what(),checks);
    return 1;
}}
'''
out = ROOT / 'build' / ('detour-begin-negative' if args.negative else 'detour-begin-cleanup')
out.mkdir(parents=True, exist_ok=True)
cpp = out / 'fixture.cpp'
cpp.write_text(fixture, encoding='utf-8')
exe = out / 'fixture.exe'
batchfile = out / 'compile.cmd'
batchfile.write_text('@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\nif errorlevel 1 exit /b %errorlevel%\n' + f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Fe:"{exe}" /Fo:"{out}/" "{cpp}"\nexit /b %errorlevel%\n', encoding='ascii', newline='\r\n')
compiled = subprocess.run(['cmd.exe','/d','/c',str(batchfile)],cwd=ROOT,timeout=60)
if compiled.returncode:
    raise SystemExit(compiled.returncode)
raise SystemExit(subprocess.run([str(exe)],cwd=ROOT,timeout=30).returncode)
