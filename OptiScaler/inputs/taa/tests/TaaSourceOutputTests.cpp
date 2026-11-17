// SPDX-License-Identifier: GPL-3.0-or-later
#include "../TaaSourceOutput.h"
#include <iostream>
#include <stdexcept>
using namespace taa::source_output;
int main(){
    unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("source output capability");};
    Call a{1,2,3,4,5,6,7,8};Reset();check(!Selected(true));check(!Authorized(7));
    {Scope producer(a);check(!Selected(false));check(SelectBackend(a,true));check(Selected(false));check(Authorized(7));check(!Authorized(8));
     {Call b=a;b.output=9;Scope nested(b);check(!Authorized(7));check(!SelectBackend(a,true));check(!Selected(false));}
     check(Authorized(7));check(Selected(false));
     for(unsigned i=0;i<8;++i){Call b=a;switch(i){case 0:b.context++;break;case 1:b.parameters++;break;case 2:b.color++;break;case 3:b.depth++;break;case 4:b.motion++;break;case 5:b.output++;break;case 6:b.handle++;break;case 7:b.request++;break;}check(!SelectBackend(b,true));check(!Authorized(7));check(SelectBackend(a,true));}
     check(!SelectBackend(a,false));check(!Authorized(7));check(!Selected(true));check(SelectBackend(a,true));}
    check(!Authorized(7));check(Selected(true));check(!Selected(false));
    // Unrelated callbacks cannot acquire or revoke the producer selection.
    check(!SelectBackend(a,false));check(Selected(true));Reset();check(!Selected(true));
    {a.request=0;Scope invalid(a);check(!SelectBackend(a,true));check(!Authorized(7));}
    check(!Creating(1,2));
    {CreationScope create(1,2);check(Creating(1,2));check(!Creating(1,3));
     {CreationScope nested(3,4);check(Creating(3,4));check(!Creating(1,2));}
     check(Creating(1,2));}
    check(!Creating(1,2));
    a.request=8;
    {Scope producer(a);check(!PreparedRasterGuides(7));check(!SelectBackend(a,false));
     check(PreparedRasterGuides(7));check(!Authorized(7));check(!PreparedRasterGuides(8));
     Call wrong=a;wrong.color++;check(!SelectBackend(wrong,true));check(!PreparedRasterGuides(7));
     check(SelectBackend(a,true));check(PreparedRasterGuides(7));check(Authorized(7));}
    check(!PreparedRasterGuides(7));
    std::cout<<"TaaSourceOutput: "<<checks<<" checks passed\n";
}
