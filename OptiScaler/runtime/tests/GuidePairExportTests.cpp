// SPDX-License-Identifier: GPL-3.0-or-later
#include "../GuidePairExport.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
unsigned checks=0;
void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
std::string Key(const char* key){return std::string(1,char(34))+key+char(34)+':';}
GuideCapture::Image Image(uint64_t source,uint64_t tick){
    // Synthetic CPU fixture; these flags never certify game/SDK phase.
    GuideCapture::Image im;im.width=12;im.height=10;im.valid=true;im.completedTick=tick+1;
    im.colourPresent=im.depthPresent=im.motionPresent=true;im.motionToUvX=1.f/12;im.motionToUvY=1.f/10;
    im.colour.resize(120);im.depth.resize(120,.5f);im.motion.resize(120,{.5f,.5f});
    auto& s=im.stamp;s.source=source;s.epoch=50;s.feature=3;s.configuration=4;s.owner=5;s.tick=tick;
    s.stage=GuideDiagnostics::Stage::Completed;s.scaleX=s.scaleY=1;
    s.colour={true,2,12,10,{0,0,12,10}};s.depth=s.motion=s.colour;
    s.addressPhaseKnown=s.motionJitterKnown=true;
    for(unsigned i=0;i<static_cast<unsigned>(GuideDiagnostics::Field::Count);++i)
        s.Set(static_cast<GuideDiagnostics::Field>(i),0,true,GuideDiagnostics::Origin::ActualCall);
    s.Set(GuideDiagnostics::Field::RenderWidth,12,true,GuideDiagnostics::Origin::ActualCall);
    s.Set(GuideDiagnostics::Field::RenderHeight,10,true,GuideDiagnostics::Origin::ActualCall);
    s.Set(GuideDiagnostics::Field::ScaleX,1,true,GuideDiagnostics::Origin::ActualCall);
    s.Set(GuideDiagnostics::Field::ScaleY,1,true,GuideDiagnostics::Origin::ActualCall);
    s.Set(GuideDiagnostics::Field::DepthBounds,1,true,GuideDiagnostics::Origin::ActualCall);
    s.Set(GuideDiagnostics::Field::MotionBounds,1,true,GuideDiagnostics::Origin::ActualCall);
    for(unsigned y=0;y<10;++y)for(unsigned x=0;x<12;++x)im.colour[y*12+x]={float(x)/12,float(y)/10,.2f,1};
    return im;
}
void Reject(const std::vector<GuideCapture::Image>& pair,const GuidePairExport::ExportIdentity& id){
    std::ostringstream out;Check(GuidePairExport::Validate(pair,id)!=nullptr,"must identify structural refusal");
    Check(!GuidePairExport::Write(out,pair,id)&&out.str().empty(),"input refusal writes no bytes");
}
}
int main(int argc,char** argv){
    try {
        const auto now=GuideDiagnostics::Now();
        GuidePairExport::ExportIdentity id;id.pid=42;id.diskDllSHA=std::string(64,'a');
        id.configuration=std::string("synthetic")+char(34)+char(10)+char(9);id.capturedUtc="2026-11-10T00:00:00Z";
        const std::vector<GuideCapture::Image> pair={Image(100,now-30),Image(101,now-14)};
        Check(!id.loadedMemoryKnown,"loaded memory unknown by default");
        Check(GuidePairExport::Validate(pair,id)==nullptr,"complete same-epoch pair exports");
        std::ostringstream out;Check(GuidePairExport::Write(out,pair,id),"write observation");const auto json=out.str();
        Check(json.find("f6.guide-pair.v1")!=std::string::npos,"schema");
        Check(json.find(Key("loadedMemoryKnown")+"false")!=std::string::npos,"disk SHA is not memory SHA");
        Check(json.find(Key("loadedMemorySHA")+"null")!=std::string::npos,"no invented memory hash");
        Check(json.find(Key("phaseVerified")+"false")!=std::string::npos,"unknown raw phase preserved");
        Check(json.find("exploratory")!=std::string::npos,"unknown phase only permits exploratory errors");
        Check(json.find(Key("autoApply")+"false")!=std::string::npos,"no automatic application");
        for(unsigned i=0;i<8;++i)Check(json.find(Key("index")+std::to_string(i))!=std::string::npos,"eight candidates");
        auto changed=pair;changed.pop_back();Reject(changed,id);
        changed=pair;changed.push_back(pair[1]);Reject(changed,id);
        changed=pair;changed[1].stream=GuideCapture::Stream::Fg;Reject(changed,id);
        for(auto field:{&GuideDiagnostics::Snapshot::feature,&GuideDiagnostics::Snapshot::epoch,
                       &GuideDiagnostics::Snapshot::configuration,&GuideDiagnostics::Snapshot::owner}){
            changed=pair;++(changed[1].stamp.*field);Reject(changed,id);
        }
        changed=pair;changed[1].stamp.source+=1;Reject(changed,id);
        changed=pair;changed[1].stamp.source=pair[0].stamp.source;Reject(changed,id);
        changed=pair;changed[1].width-=1;Reject(changed,id);
        changed=pair;changed[1].depth.pop_back();Reject(changed,id);
        changed=pair;changed[1].motionPresent=false;Reject(changed,id);
        changed=pair;changed[1].colourPresent=false;Reject(changed,id);
        changed=pair;changed[1].motionToUvY=0;Reject(changed,id);
        changed=pair;changed[1].motionToUvX=std::numeric_limits<float>::infinity();Reject(changed,id);
        changed=pair;changed[1].valid=false;Reject(changed,id);
        changed=pair;changed[1].completedTick=0;Reject(changed,id);
        auto badId=id;badId.pid=0;Reject(pair,badId);
        badId=id;badId.diskDllSHA="not a hash";Reject(pair,badId);
        badId=id;badId.loadedMemoryKnown=true;Reject(pair,badId);
        changed=pair;changed[0].colour[0].r=std::numeric_limits<float>::quiet_NaN();
        changed[1].depth[0]=-std::numeric_limits<float>::infinity();
        std::ostringstream special;Check(GuidePairExport::Write(special,changed,id),"invalid raw pixels export with masks");
        Check(special.str().find("NaN")!=std::string::npos&&special.str().find("-Infinity")!=std::string::npos,"nonfinite tokens preserved");
        // Existing ScorePair requires epoch+1 even when synthetic phase is known.
        changed=pair;changed[0].stamp.phaseVerified=changed[1].stamp.phaseVerified=true;
        const auto p=GuidePairExport::Detail::Convert(changed[0],nullptr,now);
        const auto c=GuidePairExport::Detail::Convert(changed[1],&p,now);
        const auto r=GuideCalibration::ScorePair(p,c,now);
        Check(!r.valid&&r.rejection==GuideCalibration::Rejection::EpochGap,"never synthesize epoch continuity");
        const auto offline=GuidePairExport::Detail::Compare(changed,p,c);
        Check(offline.numericAvailable,"same-generation offline errors available despite old calibration EpochGap");
        Check(offline.pair.valid,"explicit phase-verified same-generation fixture can be compared");
        Check(c.metadata.epoch==p.metadata.epoch&&changed[1].stamp.epoch==50,"actual generation preserved");
        changed[0].stamp.phaseVerified=changed[1].stamp.phaseVerified=false;
        const auto unknown=GuidePairExport::Detail::Compare(changed,p,c);
        Check(unknown.numericAvailable&&!unknown.pair.valid&&unknown.mode=="exploratory","unknown phase never valid direction evidence");
        changed=pair;
        for(auto& im:changed){im.stream=GuideCapture::Stream::Fg;im.stamp.addressPhaseKnown=false;}
        const auto fp=GuidePairExport::Detail::Convert(changed[0],nullptr,now);
        const auto fc=GuidePairExport::Detail::Convert(changed[1],&fp,now);
        const auto fg=GuidePairExport::Detail::Compare(changed,fp,fc);
        Check(fg.numericAvailable&&!fg.pair.valid&&fg.mode=="exploratory","real FG same-generation unknown-phase contract produces exploratory errors");
        Check(fc.metadata.addressSemantics==GuideCalibration::AddressSemantics::Unknown&&!changed[1].stamp.phaseVerified,
              "exploratory coordinate hypothesis does not certify actual metadata");
        changed[1].stamp.motionJitterKnown=false;
        const auto noJitter=GuidePairExport::Detail::Convert(changed[1],&fp,now);
        Check(!GuidePairExport::Detail::Compare(changed,fp,noJitter).numericAvailable,"unknown jitter blocks exploratory pixel scores");
        for(unsigned i=0;i<8;++i){
            Check(r.candidates[i].flags.Index()==i&&!r.candidates[i].valid,"refused candidate flags preserved");
            const auto images=GuideCalibration::MakeImages(p,c,GuideCalibration::Candidate(i),now);
            Check(images.residual.pixels.empty(),"metadata refusal produces no fake error map");
        }
        std::ostringstream failed;failed.setstate(std::ios::badbit);Check(!GuidePairExport::Write(failed,pair,id),"stream failure");
        if(argc==3&&std::string(argv[1])=="--write-fixture"){
            std::ofstream file(argv[2],std::ios::binary);Check(GuidePairExport::Write(file,pair,id),"cross-language fixture");
        }else Check(argc==1,"usage: GuidePairExportTests [--write-fixture path]");
        std::cout<<checks<<" GuidePairExport CPU checks passed"<<std::endl;return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
