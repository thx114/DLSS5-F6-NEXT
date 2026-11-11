// SPDX-License-Identifier: GPL-3.0-or-later
#include "../GenshinDrawAdmission.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

int main() {
    using namespace GenshinLegacy::DrawAdmission;
    unsigned checks=0;
    const auto require=[&](bool ok,const char* message) {
        ++checks;if(!ok){std::fprintf(stderr,"DrawAdmission: %s\n",message);std::exit(1);}
    };
    // A direct indexed quad is candidate work just like the existing triangle,
    // but cannot inherit its before-native replacement/FamilySkip permission.
    for(uint32_t kind=1;kind<=4;++kind) {
        require(Call{3,kind,true}.KnownWork(),"existing direct triangle lost");
        require(Call{6,kind,true}.KnownWork(),"direct quad wrongly rejected");
        require(Call{3,kind,true}.LegacyReplacement(),"existing replacement lost");
        require(!Call{6,kind,true}.LegacyReplacement(),"quad inherited omit permission");
        require(!Call{0,kind,true}.KnownWork(),"zero work admitted");
        require(!Call{6,kind,false}.KnownWork(),"unknown instances/count admitted");
    }
    for(uint32_t kind: {0u,5u,6u,7u,8u}) {
        require(!Call{0,kind,false}.KnownWork(),"unknown draw promoted to source");
        require(!Call{3,kind,true}.LegacyReplacement(),"unsupported draw inherited omit permission");
    }
    for(const auto hash: {fsr2_family_takeover::k_accumulate_hash,fsr2_family_takeover::k_pre_hash_1,
        fsr2_family_takeover::k_pre_hash_2,fsr2_family_takeover::k_pre_hash_3,fsr2_family_takeover::k_pre_hash_4}) {
        require(!NonFamilyShader(true,hash),"family shader entered negative fast path");
        require(!NonFamilyShader(false,hash),"unknown observation entered negative fast path");
    }
    require(NonFamilyShader(true,0x773B7BD7A2C6971Full),"known scene shader cannot fast reject");
    require(!NonFamilyShader(false,0),"missing shader treated as negative proof");
    for(const auto& profile:GenshinFsrProfiles::Reviewed) {
        require(!NonFamilyShader(true,profile.hash),"reviewed variant fast rejected");
        require(GenshinFsrProfiles::Authenticate(profile.hash,profile.bytes,profile.digest)==&profile,"exact byte certificate refused");
        require(!GenshinFsrProfiles::Authenticate(profile.hash,profile.bytes+1,profile.digest),"wrong length accepted");
        auto wrong=profile.digest;
        for(auto& byte:wrong) {
            byte^=1;
            require(!GenshinFsrProfiles::Authenticate(profile.hash,profile.bytes,wrong),"wrong digest accepted");
            byte^=1;
        }
    }
    require(!GenshinFsrProfiles::Authenticate(0,15388,GenshinFsrProfiles::Reviewed[0].digest),"unknown hash accepted");
    require(!GenshinFsrProfiles::SkipPrepasses(GenshinFsrProfiles::Reviewed[1].hash),"overlay variant inherited old prepass omit");
    std::printf("DrawAdmission: %u checks passed\n",checks);
}
