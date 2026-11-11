"""Exercise atomic observation using the actual legacy CustomOptional base."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[3]
config=(ROOT / 'OptiScaler/Config.h').read_text(encoding='utf-8-sig')
base=config[config.index('enum HasDefaultValue'):config.index('constexpr inline int UnboundKey')]
fixture=r'''
#include "runtime/ObservedSetting.h"
#include <concepts>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <thread>
// BASE
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAILED: %s\n",message);std::abort();}}
enum class Choice {Off,On,Other};
int main(){
    using Setting=FramePipeline::ObservedSetting<CustomOptional<bool>,bool>;
    Setting value(false);
    Check(!value.ReadObserved() && !value.ObservedValue(),"unset false default publication");
    value.set_from_config(true);Check(value.ReadObserved()==true && value.value_for_config()==true,"INI load publishes and preserves save semantics");
    value.set_from_config(false);Check(value.ObservedValue(),"second config load cannot override user value");
    value.set_volatile_value(false);Check(value.ReadObserved()==false && value.value_for_config()==true,"volatile mutation publishes without changing saved value");
    value=true;Check(value.ObservedValue() && value.value_for_config()==true,"normal assignment clears volatile semantics");
    value=std::optional<bool>{false};Check(value.ReadObserved()==false && !value.value_for_config(),"optional false assignment saves auto default");
    value.reset();Check(!value.ReadObserved() && !value.ObservedValue(),"reset publishes unset with default");
    Setting trueDefault(true);Check(!trueDefault.ReadObserved() && trueDefault.ObservedValue(),"unset true fallback is atomically encoded");
    value=trueDefault;Check(!value.ReadObserved() && value.ObservedValue(),"copy assignment publishes source fallback and presence");
    Setting copy(value);Check(copy.ObservedValue() && !copy.ReadObserved(),"copy constructor preserves publication");
    Setting moved(std::move(copy));Check(moved.ObservedValue(),"move construction preserves publication");
    using EnumSetting=FramePipeline::ObservedSetting<CustomOptional<Choice>,Choice>;
    EnumSetting choice(Choice::Off);choice=Choice::Other;
    Check(choice.ReadObserved()==Choice::Other && choice.ObservedValue()==Choice::Other,"enum setting publication");
    choice.reset();Check(!choice.ReadObserved() && choice.ObservedValue()==Choice::Off,"enum reset publication");
    std::atomic<bool> start{false},finished{false},valid{true};
    std::thread reader([&]{while(!start.load(std::memory_order_acquire)){}
        do {auto snapshot=choice.ReadObserved();if(snapshot && *snapshot!=Choice::On && *snapshot!=Choice::Other)valid=false;
            const auto effective=choice.ObservedValue();if(effective!=Choice::Off && effective!=Choice::On && effective!=Choice::Other)valid=false;
        }while(!finished.load(std::memory_order_acquire));});
    start.store(true,std::memory_order_release);
    for(unsigned i=0;i<50000;++i){choice=Choice::On;choice.set_volatile_value(Choice::Other);choice.reset();}
    finished.store(true,std::memory_order_release);reader.join();
    Check(valid.load() && !choice.ReadObserved() && choice.ObservedValue()==Choice::Off,"150000 mutations with concurrent snapshot reader preserve valid publications");
    std::printf("Observed settings CPU: %u checks passed; legacy INI and concurrent observation\n",checks);
}
'''.replace('// BASE',base)
for name in ('DlssNrEnabled','HoYoShadeEnabled','FGEnabled','FGInput','FGOutput'):
    assert any('FramePipeline::ObservedSetting<' in line and name+' ' in line for line in config.splitlines()), name
with tempfile.TemporaryDirectory(prefix='observed-settings-',dir=ROOT / 'build',ignore_cleanup_errors=True) as directory:
    out=Path(directory)
    (out / 'test.cpp').write_text(fixture,encoding='utf-8')
    command=(f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" '
             f'/Fe:"{out / "test.exe"}" /Fo:"{out}/" "{out / "test.cpp"}"')
    batch=out / 'compile.cmd'
    batch.write_text('@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
                     'if errorlevel 1 exit /b %errorlevel%\n'+command+'\nexit /b %errorlevel%\n',encoding='utf-8',newline='\r\n')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=out,check=True)
    subprocess.run([str(out / 'test.exe')],cwd=out,check=True)
