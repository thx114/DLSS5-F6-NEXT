#include <inputs/taa/TaaShaderAnalysis.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <fstream>

using namespace taa;
namespace {
unsigned checks=0;
void Check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
const char* Source=R"(
Texture2D<float> sceneZ:register(t5);
Texture2D<float2> flow:register(t7);
Texture2D<float4> image:register(t9),oldImage:register(t11);
SamplerState linearSampler:register(s0);
cbuffer View:register(b3){float2 inverseSize;float2 offset;}
float4 main(float4 p:SV_Position):SV_Target0 {
    float2 uv=p.xy*inverseSize;
    float z=sceneZ.SampleLevel(linearSampler,uv,0);
    float other=sceneZ.SampleLevel(linearSampler,uv+inverseSize,0);
    float2 velocity=flow.SampleLevel(linearSampler,uv+float2(other<z?inverseSize.x:0,0),0);
    DECODE
    float4 current=image.SampleLevel(linearSampler,uv-offset,0);
    float4 history=oldImage.SampleLevel(linearSampler,uv-velocity,0);
    return lerp(current,history,.5)+min(z,other)*.00001;
})";
Microsoft::WRL::ComPtr<ID3DBlob> Compile(std::string source,bool stripped=false) {
    Microsoft::WRL::ComPtr<ID3DBlob> code,errors;
    const auto hr=D3DCompile(source.data(),source.size(),"temporal-coordinate fixture",nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(hr)&&errors)std::fprintf(stderr,"%s",static_cast<const char*>(errors->GetBufferPointer()));
    Check(SUCCEEDED(hr),"compile real temporal shader");
    if(stripped){Microsoft::WRL::ComPtr<ID3DBlob> compact;Check(SUCCEEDED(D3DStripShader(code->GetBufferPointer(),code->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA|D3DCOMPILER_STRIP_DEBUG_INFO,&compact)),"strip names and reflection");return compact;}
    return code;
}
std::string Shader(std::string_view decode) {std::string source=Source;source.replace(source.find("DECODE"),6,decode);return source;}
void Verify(const std::string& source,Encoding encoding,bool stripped=false) {
    auto code=Compile(source,stripped);
    const auto result=analysis::Analyze(code->GetBufferPointer(),code->GetBufferSize(),Stage::Pixel,123);
    if(result.recipes.empty()) {
        Microsoft::WRL::ComPtr<ID3DBlob> assembly;D3DDisassemble(code->GetBufferPointer(),code->GetBufferSize(),0,nullptr,&assembly);
        if(assembly)std::fprintf(stderr,"%s",static_cast<const char*>(assembly->GetBufferPointer()));
    }
    Check(!result.recipes.empty(),"infer temporal contract from actual DXBC coordinates");
    const auto found=std::find_if(result.recipes.begin(),result.recipes.end(),[](const auto& r){return r.profile.colorSlot==9&&r.historySlot==11&&r.profile.motionSlot==7;});
    Check(found!=result.recipes.end(),"derive color/history/motion roles before runtime typed-view validation");
    const auto& p=found->profile;
    Check(!p.hdrDeclared&&!p.hdr,"automatic shader analysis does not declare source HDR");
    Check(p.jitterSlot==3&&p.jitterOffsetX==8&&p.jitterOffsetY==12,"derive exact CB components including zw swizzles");
    Check(p.motionUnits==Units::Uv&&p.jitterUnits==Units::Uv,"derive normalized coordinates");
    Check(p.encoding==encoding&&std::abs(p.motionScaleX+1)<1e-5f&&std::abs(p.motionScaleY+1)<1e-5f,"preserve motion decoding and previous-current sign");
    Check(p.jitterScaleX==1&&p.jitterScaleY==1,"preserve current-color jitter sign");
}
}
int main(int argc,char** argv) {
    try {
        Verify(Shader(""),Encoding::Linear);
        Verify(Shader(""),Encoding::Linear,true);
        Verify(Shader("float2 delta=velocity-127.0/255.0;velocity=sign(delta)*(2*delta)*(2*delta);"),Encoding::SignedSquare);
        for(const auto decode:{"velocity=abs(velocity);","velocity=saturate(velocity);","velocity=velocity*velocity;"}) {
            auto invalid=Compile(Shader(decode));const auto analyzed=analysis::Analyze(invalid->GetBufferPointer(),invalid->GetBufferSize(),Stage::Pixel,1);
            Check(std::none_of(analyzed.recipes.begin(),analyzed.recipes.end(),[](const auto& r){return r.profile.colorSlot==9&&r.historySlot==11;}),"nonlinear motion cannot be misclassified as raw linear motion");
        }
        auto missing=Shader("");missing.replace(missing.find("uv-offset"),9,"uv");
        auto code=Compile(missing);const auto result=analysis::Analyze(code->GetBufferPointer(),code->GetBufferSize(),Stage::Pixel,1);
        Check(std::any_of(result.recipes.begin(),result.recipes.end(),[](const auto& r){return r.profile.colorSlot==9&&r.historySlot==11&&!r.jitterInResolve;}),"identify temporal roles without inventing absent raster jitter");
        const std::string originalReturn="return lerp(current,history,.5)+min(z,other)*.00001;";
        auto branched=Shader("");branched.replace(branched.find(originalReturn),originalReturn.size(),
            "if(z<.5){ if(other<.9)current=lerp(current,history,.5); else current*=.9;}return current;");
        Verify(branched,Encoding::Linear);
        if(argc>1) {
            std::ifstream file(argv[1],std::ios::binary);const std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
            Check(!bytes.empty(),"read captured StarRail DXBC regression input");
            const auto actual=analysis::Analyze(bytes.data(),bytes.size(),Stage::Pixel,0x0835130E06208EA4ull);
            const auto role=std::find_if(actual.recipes.begin(),actual.recipes.end(),[](const auto& r){return r.profile.colorSlot==1&&r.historySlot==2&&r.profile.motionSlot==3;});
            Check(role!=actual.recipes.end(),"captured StarRail DXBC preserves nested-branch temporal roles");
            Check(!role->jitterInResolve&&role->profile.motionScaleX==-1&&role->profile.motionScaleY==-1,"captured StarRail reprojection sign is known while raster jitter remains unknown");
            Check(actual.constantBuffers.size()==1&&actual.constantBuffers[0].slot==0&&actual.constantBuffers[0].bytes==46*16,"preserve actual declared CB extent for companion evidence");
        }
        const auto mrtControl=Compile("float2 main(float4 p:SV_Position):SV_Target1{clip(p.x-2);return p.xy;}");
        const auto independentSignature=analysis::Analyze(mrtControl->GetBufferPointer(),mrtControl->GetBufferSize(),Stage::Pixel,1);
        Check(independentSignature.recipes.empty()&&independentSignature.pixelOutputsKnown&&independentSignature.pixelOutputMasks[0]==0&&independentSignature.pixelOutputMasks[1]==3,
            "known output signature survives unsupported coordinate control flow without granting a recipe");
        for(int input=3;input<argc;++input) {
            std::ifstream file(argv[input],std::ios::binary);const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
            Check(!bytes.empty(),"read complete captured MRT writer bytecode");
            const auto actual=analysis::Analyze(bytes.data(),bytes.size(),Stage::Pixel,1);
            Check(actual.pixelOutputsKnown&&(input==3?actual.pixelOutputMasks[1]==0:actual.pixelOutputMasks[1]!=0),
                "captured ZZZ o0-only and real o1 writers remain distinguishable even with unsupported loops");
        }
        const analysis::Coordinates masks("ps_5_0\nmov r0.xyzw, l(1.000000,2.000000,3.000000,4.000000)\nmov o0.zw, r0.xxxx\n");
        Check(masks.outputs.at(0)[0]==0&&masks.Expand(masks.outputs.at(0)[2]).constant==1&&masks.Expand(masks.outputs.at(0)[3]).constant==1,"partial destination masks preserve lane positions");
        const analysis::Coordinates depthOutput("ps_5_0\nmov oDepth, l(1.000000)\n");
        Check(depthOutput.outputs.empty(),"SV_Depth is never a color render-target index");
        const analysis::Coordinates multi("ps_5_0\numul null, r0.xy, r1.xy, r2.xy\n");
        Check(multi.unsupportedControl,"unknown multi-destination writes cannot leave stale register authority");
        Check(multi.unsupportedInstruction=="umul","unsupported instructions retain their exact diagnostic identity");
        const analysis::Coordinates conditional("ps_5_0\nretc_nz r0.x\n");
        Check(conditional.unsupportedControl,"conditional exits cannot authorize full-frame copyback");
        const analysis::Coordinates branchReturn("ps_5_0\nmov o0.xyzw, l(1.000000)\nif_nz v0.x\nret\nendif\nmov o0.xyzw, v0.xyzw\nret\n");
        Check(!branchReturn.unsupportedControl&&branchReturn.exitCount==2&&branchReturn.mustWrite[0]==15,"complete early-return exits retain full color coverage");
        const analysis::Coordinates partialReturn("ps_5_0\nif_nz v0.x\nmov o0.xyz, v0.xyzw\nret\nmov o0.w,l(1.000000)\nendif\nmov o0.xyzw, v0.xyzw\nret\n");
        Check(!partialReturn.unsupportedControl&&partialReturn.exitCount==2&&partialReturn.mustWrite[0]==7,"unreachable alpha write cannot repair an incomplete early-return exit");
        const analysis::Coordinates nestedReturns("ps_5_0\nif_z v0.x\nif_nz v0.y\nmov o0.xyzw,l(1.000000)\nmov o1.x,l(1.000000)\nret\nelse\nmov o0.xyzw,l(2.000000)\nmov o1.x,l(2.000000)\nret\nendif\nmov o0.xyz,l(3.000000)\nelse\nmov o0.xyzw,l(4.000000)\nmov o1.x,l(4.000000)\nret\nendif\nmov o0.w,l(0)\n");
        Check(!nestedReturns.unsupportedControl&&nestedReturns.exitCount==3&&nestedReturns.mustWrite[0]==15&&nestedReturns.mustWrite[1]==1,"nested terminated arms keep MRT exit coverage and skip unreachable continuation");
        const analysis::Coordinates missingElse("ps_5_0\nif_nz v0.x\nmov o0.xyzw,l(1.000000)\nelse\nmov o0.xyz,l(2.000000)\nendif\nret\n");
        Check(missingElse.mustWrite[0]==7,"fallthrough branch merge intersects actual component writes");
        const analysis::Coordinates malformed("ps_5_0\nif_nz v0.x\nmov o0.xyzw,l(1.000000)\nret\n");
        Check(malformed.unsupportedControl&&malformed.unsupportedInstruction=="unterminated-if","unmatched structured exits remain unsupported");
        auto incomplete=Shader("");incomplete.replace(incomplete.find(originalReturn),originalReturn.size(),"if(z<.5)discard;"+originalReturn);
        auto discardCode=Compile(incomplete);Check(analysis::Analyze(discardCode->GetBufferPointer(),discardCode->GetBufferSize(),Stage::Pixel,1).recipes.empty(),"discard never authorizes full-frame copyback");
        if(argc>2) {
            std::ifstream file(argv[2],std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(file),{}};
            Check(bytes.size()==4668,"read captured ZenlessZoneZero DXBC regression input");
            std::string assembly;const auto actual=analysis::Analyze(bytes.data(),bytes.size(),Stage::Pixel,0xB12FF59247A26796ull,&assembly);
            const analysis::Coordinates ir(assembly);
            Check(!ir.unsupportedControl&&ir.exitCount==2&&ir.mustWrite[0]==15&&ir.mustWrite[1]==1,"actual ZZZ early returns prove both main-color and auxiliary MRT writes");
            Check(actual.recipes.size()==1&&actual.recipes[0].validatedProjection&&actual.recipes[0].profile.motionSampling==MotionSampling::NearestDepth5Max&&actual.observation&&actual.observation->roles.size()==5,"actual ZZZ program requires its live producer projection and exact five-depth motion selection");
            Check(!actual.recipes[0].profile.hdrDeclared&&actual.recipes[0].profile.hdr,"packed automatic recipe reports its HDR fallback without declaring source transfer");
            Check(actual.recipes[0].profile.outputRoute==OutputRoute::ObserveUpscale,"packed automatic recipe observes the actual downstream upscale consumer");
            Check(actual.constantBuffers.size()==1&&actual.constantBuffers[0].bytes>=168*16,"actual ZZZ observation retains jitter and motion-zero CB extent");
            Check(!analysis::Observe(bytes.data(),bytes.size(),1),"observation rejects an incorrect complete-program identity");
            bytes.back()^=1;Check(!analysis::Observe(bytes.data(),bytes.size(),0xB12FF59247A26796ull),"observation rehashes complete bytes rather than trusting caller identity or embedded checksum");
        }
        const analysis::Coordinates unreachable("ps_5_0\nmov o0.xyzw, l(1.000000)\nret\nmov o0.xyzw, l(2.000000)\n");
        Check(unreachable.Expand(unreachable.outputs.at(0)[0]).constant==1,"top-level return ends reachable output analysis");
        std::string extrema="ps_5_0\nld r0.x, v0.xy, t0.xyzw\nld r1.x, v1.xy, t0.xyzw\n";
        for(unsigned i=0;i<48;++i)extrema+="min r2.x, r0.x, r1.x\nmax r3.x, r0.x, r1.x\nmov r0.x, r2.x\nmov r1.x, r3.x\n";
        extrema+="mov o0.x, r0.x\nret\n";const analysis::Coordinates depthDag(extrema);
        Check(!depthDag.unsupportedControl&&depthDag.Textures(depthDag.outputs.at(0)[0]).test(0),"shared min/max DAGs retain dependencies without recursive depth guesses");
        std::string dag="ps_5_0\nld r0.xy, v0.xy, t0.xyzw\n";for(unsigned i=0;i<48;++i)dag+="mul r0.xy, r0.xy, r0.xy\n";
        dag+="mov o0.xy, r0.xy\n";const analysis::Coordinates deep(dag);
        Check(!deep.Motion(deep.outputs.at(0)[0],0,0),"shared nonlinear DAGs terminate without exponential numeric probes");
        std::printf("PASS TAA shader analysis: %u checks\n",checks);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
