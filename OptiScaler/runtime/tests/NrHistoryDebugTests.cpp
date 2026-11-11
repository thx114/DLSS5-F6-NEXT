// SPDX-License-Identifier: GPL-3.0-or-later
#include <shaders/dlssnr/DlssNr_HistoryDebug.h>
#include <cstdio>
#include <cstdlib>
static unsigned checks=0;
static void Check(bool ok,const char* text){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
int main() {
    using namespace NrHistoryDebug;
    View v;
    Frame f;f.source=1;f.owner=2;f.configuration=3;f.recorded=true;
    Observe(f);Read(v);Check(v.count==0,"closed inspector records nothing");
    Request(true);
    for(unsigned n=1;n<=150;++n){f.source=n;f.historySource=n-1;f.tick=Now();Observe(f);}
    Read(v);Check(v.count==120&&v.next==30,"source-by-source ledger has a fixed 120-frame bound");
    const auto last=(v.next+v.frames.size()-1)%v.frames.size();
    Check(v.frames[last].source==150&&v.frames[last].historySource==149,"latest history source is the consumed predecessor");
    Lifecycle(150,2,3,true,false,false,false);Read(v);
    Check(v.frames[last].submitted&&!v.frames[last].completed,"real submission cannot masquerade as GPU completion");
    Lifecycle(150,2,3,false,true,false,false);Read(v);
    Check(v.frames[last].completed&&!v.frames[last].reset,"GPU completion cannot masquerade as Reset");
    f.source=150;f.reason="recorded";Observe(f);Read(v);
    Check(v.frames[last].submitted&&v.frames[last].completed&&!v.frames[last].reset,"later CPU observations preserve proven lifecycle flags");
    Lifecycle(150,2,4,true,true,true,true);Read(v);
    Check(!v.frames[last].reset&&!v.frames[last].quarantined,"other configuration cannot change this frame's proof");
    Lifecycle(150,2,3,false,false,true,false);Read(v);Check(v.frames[last].reset,"successful recording Reset is a separate event");
    Request(false);const auto revision=v.revision;f.source=151;Observe(f);Read(v);
    Check(v.revision==revision,"closing stops new observations");
    Storage().until.store(Now()-1);Observe(f);Read(v);Check(v.revision==revision,"expired visibility demand cannot collect frames");
    Request(false);
    std::printf("NR history debug ledger: %u checks passed\n",checks);
}
