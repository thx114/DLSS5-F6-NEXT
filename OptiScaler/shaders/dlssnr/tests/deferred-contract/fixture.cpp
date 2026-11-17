// SPDX-License-Identifier: GPL-3.0-or-later
// Control/identity seam only. Real shader pixels and GPU lifetime have separate fixtures.
#define NOMINMAX
#include <cstdint>
struct ID3D12Resource;
struct DlssNrFrameInfo {bool BeforeUpscale=false,PrivateColorCopy=false,IndependentCommands=false,DepthInverted=false,
ColourEncodingFlagKnown=true,ColourIsLinearHdr=true,Reset=false;unsigned RenderSubrectWidth=0,RenderSubrectHeight=0,ColourEncoding=0;
unsigned long long SubmissionEpoch=0;float MvScaleX=1,MvScaleY=1,PreExposure=1;ID3D12Resource* ExposureTexture=nullptr;};
struct DlssNrConstants {unsigned Mode=0,Width=0,Height=0;float ExposurePreMul=0,WhitePoint=0,TransferStrength=0,MaxRatio=0;};
enum {DlssNrMode_EncodeResidual=5,DlssNrMode_ApplyResidual=6,DlssNrMode_ApplyInterpolatedResidual=10};
#include <shaders/dlssnr/DlssNr_ColourEncoding.h>
#include <algorithm>
#include <optional>
#include <memory>
#include <map>
#include <string>
#include <stdexcept>
#include <cstdio>
template<class...T>void Log(T&&...){}
#define LOG_DEBUG(...) Log(__VA_ARGS__)
#define LOG_INFO(...) Log(__VA_ARGS__)
#define LOG_TRACE(...) Log(__VA_ARGS__)
unsigned checks=0;void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
enum {D3D12_RESOURCE_STATE_UNORDERED_ACCESS=1,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE=2,
D3D12_RESOURCE_STATE_COPY_SOURCE=3,D3D12_RESOURCE_STATE_COPY_DEST=4,D3D12_RESOURCE_STATE_COMMON=5,
D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT=1,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX=2,DXGI_FORMAT_R8_UNORM=61};
using D3D12_RESOURCE_STATES=int;
struct ID3D12Resource {int state=1;unsigned value=0;};
struct D3D12_TEXTURE_COPY_LOCATION {ID3D12Resource* pResource=nullptr;unsigned Type=0;struct {struct {unsigned a,b,c,d,e;} Footprint;} PlacedFootprint;};
struct ID3D12GraphicsCommandList {unsigned copies=0;void CopyResource(ID3D12Resource* to,ID3D12Resource* from){++copies;to->value=from->value;}
void CopyBufferRegion(ID3D12Resource*,unsigned,ID3D12Resource*,unsigned,unsigned){}
void CopyTextureRegion(D3D12_TEXTURE_COPY_LOCATION*,unsigned,unsigned,unsigned,D3D12_TEXTURE_COPY_LOCATION*,void*){}};
struct ID3D12CommandQueue {};
struct NVSDK_NGX_Parameter {std::map<std::string,double> values;std::map<std::string,ID3D12Resource*> resources;
void Set(const char* key,ID3D12Resource* v){resources[key]=v;}template<class T>void Set(const char* key,T v){values[key]=double(v);}};
constexpr unsigned NVSDK_NGX_Result_Success=1,NVSDK_NGX_DLSS_Feature_Flags_IsHDR=1,NVSDK_NGX_DLSS_Feature_Flags_DepthInverted=2;
constexpr const char* NVSDK_NGX_Parameter_Reset="NVSDK_NGX_Parameter_Reset";
constexpr const char* NVSDK_NGX_Parameter_MV_Scale_X="NVSDK_NGX_Parameter_MV_Scale_X";
constexpr const char* NVSDK_NGX_Parameter_MV_Scale_Y="NVSDK_NGX_Parameter_MV_Scale_Y";
constexpr const char* NVSDK_NGX_Parameter_Color="NVSDK_NGX_Parameter_Color";
constexpr const char* NVSDK_NGX_Parameter_Output="NVSDK_NGX_Parameter_Output";
constexpr const char* NVSDK_NGX_Parameter_Depth="NVSDK_NGX_Parameter_Depth";
constexpr const char* NVSDK_NGX_Parameter_MotionVectors="NVSDK_NGX_Parameter_MotionVectors";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X="NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y="NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X="NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y="NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X="NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y="NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X="NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y="NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width="NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height="NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags="NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags";
constexpr const char* NVSDK_NGX_Parameter_Width="NVSDK_NGX_Parameter_Width";
constexpr const char* NVSDK_NGX_Parameter_Height="NVSDK_NGX_Parameter_Height";
constexpr const char* NVSDK_NGX_Parameter_OutWidth="NVSDK_NGX_Parameter_OutWidth";
constexpr const char* NVSDK_NGX_Parameter_OutHeight="NVSDK_NGX_Parameter_OutHeight";
constexpr const char* NVSDK_NGX_Parameter_CreationNodeMask="NVSDK_NGX_Parameter_CreationNodeMask";
constexpr const char* NVSDK_NGX_Parameter_VisibilityNodeMask="NVSDK_NGX_Parameter_VisibilityNodeMask";
constexpr const char* NVSDK_NGX_Parameter_PerfQualityValue="NVSDK_NGX_Parameter_PerfQualityValue";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Pre_Exposure="NVSDK_NGX_Parameter_DLSS_Pre_Exposure";
constexpr const char* NVSDK_NGX_Parameter_ExposureTexture="NVSDK_NGX_Parameter_ExposureTexture";
constexpr const char* NVSDK_NGX_Parameter_Jitter_Offset_X="NVSDK_NGX_Parameter_Jitter_Offset_X";
constexpr const char* NVSDK_NGX_Parameter_Jitter_Offset_Y="NVSDK_NGX_Parameter_Jitter_Offset_Y";
constexpr const char* NVSDK_NGX_Parameter_FrameTimeDeltaInMsec="NVSDK_NGX_Parameter_FrameTimeDeltaInMsec";
constexpr const char* NVSDK_NGX_Parameter_DLSS_Exposure_Scale="NVSDK_NGX_Parameter_DLSS_Exposure_Scale";
constexpr const char* NVSDK_NGX_Parameter_Sharpness="NVSDK_NGX_Parameter_Sharpness";
unsigned UInt(NVSDK_NGX_Parameter* p,const char* key,unsigned fallback=0){auto i=p->values.find(key);return i==p->values.end()?fallback:unsigned(i->second);}
float Float(NVSDK_NGX_Parameter* p,const char* key,float fallback=0){auto i=p->values.find(key);return i==p->values.end()?fallback:float(i->second);}
ID3D12Resource* GetResource(NVSDK_NGX_Parameter* p,const char* key,const char*){return p->resources[key];}
template<class T>struct Setting{T value{};T value_or_default()const{return value;}};
struct Config {Setting<bool> DlssNrFinishedPicture,RestoreComputeSignature,RestoreGraphicSignature;
Setting<unsigned> DlssNrColourEncoding;Setting<float> DlssNrMaxRatio{4};std::optional<int> OutputResourceBarrier;
static Config* Instance(){static Config c;return &c;}};
void Barrier(ID3D12GraphicsCommandList*,ID3D12Resource* r,int before,int after){Check(r&&r->state==before,"production barrier has the expected seam state");r->state=after;}
unsigned nrCalls=0,encodeCalls=0,composeCalls=0,evaluateCalls=0,captureCalls=0;
bool nrOk=true,encodeOk=true,evaluateOk=true,captureOk=true,composeOk=true,useOk=true,restoreOk=true,lastSceneLinear=false;
DlssNrConstants lastEncode{};
struct Nr {unsigned successfulDispatches=0;bool exposureOfferedNow=false,exposureEverOffered=false;unsigned exposureFrames=0;}g_nr;
struct ScopedNrStateEnvelope{explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList*){}};
namespace D3D12Hooks {bool CanRestoreRootSignature(ID3D12GraphicsCommandList*){return restoreOk;}}
struct DlssNr_Dx12 {
DlssNr_Dx12(const char*,void*){}
void Dispatch(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,const DlssNrFrameInfo&,ID3D12CommandQueue*){++nrCalls;if(nrOk)++g_nr.successfulDispatches;}
bool DispatchPass(ID3D12GraphicsCommandList*,const DlssNrConstants& c,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*){if(c.Mode==5){++encodeCalls;lastEncode=c;return encodeOk;}++composeCalls;return composeOk;}
bool DispatchResidualPass(ID3D12GraphicsCommandList*,const DlssNrConstants& c,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*){++encodeCalls;lastEncode=c;return encodeOk;}
};
std::unique_ptr<DlssNr_Dx12> g_compose;
namespace NVNGXProxy {
unsigned Evaluate(ID3D12GraphicsCommandList*,void*,NVSDK_NGX_Parameter*,void*){++evaluateCalls;return evaluateOk?NVSDK_NGX_Result_Success:0;}
auto D3D12_EvaluateFeature(){return &Evaluate;}}
namespace Late {bool CaptureResidual(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*,float,bool linear){++captureCalls;lastSceneLinear=linear;return captureOk;}}
bool FormatCanHoldLinearHdr(unsigned format){return format==10;}
void ApplyColourEncoding(DlssNrFrameInfo& frame,unsigned setting,bool hdr,bool known,unsigned,bool){
auto choice=DlssNrColourEncoding::Resolve(setting,hdr,true,known);frame.ColourEncoding=unsigned(choice.encoding);
frame.ColourIsLinearHdr=choice.LinearHdr();frame.ColourEncodingFlagKnown=choice.flagKnown;}
struct Fg {template<class...T>unsigned Evaluate(T...){return 1;}};
struct Hold{void SampleSucceeded(unsigned long long){}void Reset(){}};
struct Half {bool failed=false,havePrevious=false,previousWasAnchor=false,interpolationReadable=false,suppressionReadable=false,previousReadable=false;
bool historyReadable[2]{};unsigned writeIndex=0,nrAnchors=0,skippedNr=0;unsigned long long anchorId=0;float historyScale[2]{1,1};
ID3D12Resource *suppression=nullptr,*zeroUpload=nullptr,*interpolated=nullptr,*anchorMotion=nullptr,*motion=nullptr,*suppressionTexture=nullptr,*previousMotion=nullptr,*history[2]{};Fg* fg=nullptr;int camera=0;void Reset(){}};
struct Generation {std::unique_ptr<DlssNr_Dx12> codec=std::make_unique<DlssNr_Dx12>("codec",nullptr);bool failed=false,reset=true,smallReadable=false,sampleAndHold=false,halfRequested=false;unsigned w=8,h=8,outW=16,outH=16,flags=0,outputFormat=10;
void* device=nullptr;void* feature=nullptr;NVSDK_NGX_Parameter* parameters=nullptr;Half* half=nullptr;Hold hold;std::string halfStatus;
ID3D12Resource *edited=nullptr,*residualInput=nullptr,*residualOutput=nullptr,*clean=nullptr,*composed=nullptr,*exposure=nullptr;};
std::unique_ptr<Generation> current;
struct Use {bool valid;Use(Generation&,ID3D12GraphicsCommandList*):valid(useOk){}};
std::string status;void Say(const std::string& s){status=s;}
#include "deferred-pending.inl"
struct ColourSource {std::optional<unsigned> effectiveColorCreateFlags;std::optional<bool> syntheticColorFlagKnown;};
void BeforeTail(ID3D12GraphicsCommandList* cmd,NVSDK_NGX_Parameter* source,ID3D12Resource* output,ID3D12Resource* color,ColourSource colourSource){
auto& g=*current;const auto& cfg=*Config::Instance();const bool privateJob=false,half=false;const unsigned long long epoch=77,submittedEpoch=76;
const unsigned flags=0;ID3D12Resource *depth=nullptr,*nrMotion=nullptr;ID3D12CommandQueue* queue=nullptr;
struct {unsigned Format=10;}outDesc;const int arrival=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
#include "deferred-before.inl"
}
#include "deferred-after.inl"
struct World {
ID3D12GraphicsCommandList commands,other;NVSDK_NGX_Parameter parameters,privateParameters;ID3D12Resource output,color,edited,residualInput,residualOutput,clean,composed,exposure;
World(){nrCalls=encodeCalls=composeCalls=evaluateCalls=captureCalls=0;nrOk=encodeOk=evaluateOk=captureOk=composeOk=useOk=restoreOk=true;
*Config::Instance()={};g_nr={};status.clear();pending={};current=std::make_unique<Generation>();auto& g=*current;
g.edited=&edited;edited.state=2;g.residualInput=&residualInput;g.residualOutput=&residualOutput;g.clean=&clean;g.composed=&composed;g.exposure=&exposure;g.parameters=&privateParameters;
parameters.Set(NVSDK_NGX_Parameter_Output,&output);parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,1u);}
void Before(bool finished=true,bool known=true,bool hdr=true){Config::Instance()->DlssNrFinishedPicture.value=finished;parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,hdr?1u:0u);BeforeTail(&commands,&parameters,&output,&color,{{hdr?1u:0u},{known}});}
void After(unsigned long long epoch=77){::After(&commands,&parameters,epoch);}
};
int main(){try{
{World w;w.Before();Check(encodeCalls==1&&lastEncode.TransferStrength==1,"Before encoded linear finished-picture changes");Config::Instance()->DlssNrFinishedPicture.value=false;w.parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,0u);w.After(999);Check(captureCalls==1&&lastSceneLinear&&composeCalls==0&&evaluateCalls==1,"After consumes frozen finished/linear choice across UI flags and epoch");w.After();Check(captureCalls==1&&evaluateCalls==1,"pending seam is consumed only once");}
{World w;w.Before(false);Config::Instance()->DlssNrFinishedPicture.value=true;w.After();Check(captureCalls==0&&composeCalls==1&&evaluateCalls==1,"ordinary seam remains ordinary after UI changes");}
{World w;w.Before(true,true,false);w.parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,1u);w.After();Check(captureCalls==1&&!lastSceneLinear&&lastEncode.TransferStrength==0,"tone-mapped Before cannot be relabelled linear by After flags");}
{World w;w.Before(true,false);Check(nrCalls==0&&encodeCalls==0&&pending.cmd==nullptr&&w.edited.state==1,"unknown finished transfer does not encode or arm and restores edited state");w.After();Check(captureCalls==0&&evaluateCalls==0,"unknown finished transfer cannot publish a residual");}
{World w;w.Before(false,false);w.After();Check(nrCalls==1&&composeCalls==1,"ordinary passthrough keeps its existing unknown-transfer route");}
for(unsigned reason=0;reason<3;++reason){World w;w.Before();if(reason==0)::After(&w.other,&w.parameters,77);else if(reason==1){NVSDK_NGX_Parameter other;::After(&w.commands,&other,77);}else{w.parameters.Set(NVSDK_NGX_Parameter_Output,&w.color);w.After();}Check(captureCalls==0&&evaluateCalls==0&&current->reset,"wrong cmd/caller/output rejects pairing");}
for(unsigned failure=0;failure<5;++failure){World w;if(failure==0)nrOk=false;if(failure==1)encodeOk=false;w.Before();if(failure==2)evaluateOk=false;if(failure==3)captureOk=false;if(failure==4)useOk=false;w.After();Check(composeCalls==0,"failed finished seam never commits early colour");Check(failure==3?captureCalls==1:captureCalls==0,"failures cannot publish through the capture seam");}
std::printf("PASS actual DeferredSr colour/arm/After seam: %u checks\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL DeferredSr after %u checks: %s\n",checks,e.what());return 1;}}

