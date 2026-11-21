// Runs the production C++ bridge against a real NativeAOT companion, without
// Unity or the GakuMotion plugin. Inputs are self-authored synthetic fixtures.
#include "GkCsharpImport.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace fs=std::filesystem;
namespace bridge=GakuMotion::csharp_import;
using GakuMotion::MotionImportRequest;
using GakuMotion::ImportProgressState;

static std::string Utf8(const fs::path& path)
{
    const auto u8=path.u8string();
    return {reinterpret_cast<const char*>(u8.data()),u8.size()};
}
static std::string Read(const fs::path& path)
{
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
static void Write(const fs::path& path,const std::string& text)
{ std::ofstream(path,std::ios::binary).write(text.data(),static_cast<std::streamsize>(text.size())); }
template<class T> static void Put(std::ofstream& output,T value)
{ output.write(reinterpret_cast<const char*>(&value),sizeof(value)); }
static void Zeros(std::ofstream& output,std::size_t size)
{ const std::string zeros(size,'\0'); output.write(zeros.data(),static_cast<std::streamsize>(zeros.size())); }
static void Vmd(const fs::path& path,bool camera)
{
    std::ofstream output(path,std::ios::binary);
    const std::string signature="Vocaloid Motion Data 0002";
    output.write(signature.data(),static_cast<std::streamsize>(signature.size())); Zeros(output,30-signature.size());
    const std::string model="Synthetic bridge";
    output.write(model.data(),static_cast<std::streamsize>(model.size())); Zeros(output,20-model.size());
    Put<std::uint32_t>(output,camera ? 0 : 2);
    if (!camera) for (std::uint32_t frame : {0u,2u})
    {
        // Shift-JIS センター; byte-level VMD fixture, no external asset.
        const char center[]{static_cast<char>(0x83),static_cast<char>(0x5a),static_cast<char>(0x83),static_cast<char>(0x93),
            static_cast<char>(0x83),static_cast<char>(0x5e),static_cast<char>(0x81),static_cast<char>(0x5b)};
        output.write(center,sizeof(center)); Zeros(output,15-sizeof(center)); Put(output,frame);
        for (int i=0;i<6;++i) Put(output,0.0f); Put(output,1.0f); Zeros(output,64);
    }
    Put<std::uint32_t>(output,0); // Morphs.
    Put<std::uint32_t>(output,camera ? 2 : 0);
    if (camera) for (std::uint32_t frame : {0u,2u})
    {
        Put(output,frame); Put(output,-4.0f);
        Put(output,1.0f); Put(output,2.0f); Put(output,3.0f);
        for (int i=0;i<3;++i) Put(output,0.0f);
        Zeros(output,24); Put<std::uint32_t>(output,45); Put<std::uint8_t>(output,0);
    }
    Put<std::uint32_t>(output,0); Put<std::uint32_t>(output,0); Put<std::uint32_t>(output,0);
}
static int WaitBody(std::string& message,ImportProgressState* progress=nullptr)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while (std::chrono::steady_clock::now()<end)
    {
        const int result=bridge::Poll(message,progress);
        if (result) return result;
        Sleep(1);
    }
    message="body timeout"; return -2;
}
static int WaitCamera(std::string& message)
{
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while (std::chrono::steady_clock::now()<end)
    {
        const int result=bridge::PollCamera(message);
        if (result) return result;
        Sleep(1);
    }
    message="camera timeout"; return -2;
}

int wmain(int argc,wchar_t** argv)
{
    int checks=0; std::string message;
    #define CHECK(x) do { ++checks; if (!(x)) { std::printf("FAIL line %d: %s | %s\n",__LINE__,#x,message.c_str()); bridge::Shutdown(); return 1; } } while(false)
    if (argc==3 && std::wstring(argv[1])==L"--bad-status")
    {
        bridge::SetPluginsRoot(Utf8(fs::absolute(argv[2])));
        MotionImportRequest request;
        ImportProgressState progress;
        CHECK(bridge::Begin(request,message));
        CHECK(WaitBody(message,&progress)==-1);
        CHECK(progress.finished && !progress.succeeded && !bridge::Busy());
        CHECK(bridge::BeginCamera("fixture","fixture",60,0.083f,true,"",message));
        CHECK(WaitCamera(message)==-1 && !bridge::CameraBusy());
        bridge::Shutdown();
        CHECK(bridge::Available());
        std::printf("Native bridge nonzero return-code gate: %d checks PASS\n",checks);
        return 0;
    }
    if (argc!=4)
    { std::puts("usage: native_import_bridge_tests <real DLL dir> <old ABI DLL dir> <new work dir>"); return 2; }
    const auto actual=fs::absolute(argv[1]),old=fs::absolute(argv[2]),work=fs::absolute(argv[3]);
    CHECK(!fs::exists(work)); fs::create_directories(work);
    const auto missing=work/L"missing"; fs::create_directory(missing);
    bridge::SetPluginsRoot(Utf8(missing));
    CHECK(!bridge::Initialize(Utf8(missing)) && !bridge::Available());
    MotionImportRequest request;
    CHECK(!bridge::Begin(request,message) && !message.empty() && !bridge::Busy());
    CHECK(!bridge::BeginCamera("fixture","fixture",60,0.083f,true,"",message) && !message.empty());
    CHECK(!bridge::Initialize(Utf8(old)) && !bridge::Available());
    const auto oldLibrary=old/L"GakuMotion.VmdImporter.Native.dll";
    const HMODULE rejected=GetModuleHandleW(oldLibrary.c_str());
    CHECK(rejected!=nullptr);
    const auto count=reinterpret_cast<std::int32_t(*)()>(GetProcAddress(rejected,"GkFakeAbi_CallCount"));
    CHECK(count && count()==1);
    CHECK(!bridge::Initialize(Utf8(old/L".")) && count()==1);
    CHECK(bridge::Detail().find("ABI v4")!=std::string::npos);

    GakuMotion::RuntimeConfig config;
    CHECK(config.importBackend=="csharp");
    const auto configPath=work/L"legacy-config.json";
    Write(configPath,"{\"import_backend\":\"python\"}"); config.path=Utf8(configPath);
    CHECK(config.Reload() && config.importBackend=="csharp");
    CHECK(GakuMotion::RuntimeConfig::DefaultTemplate().find("\"import_backend\": \"csharp\"")!=std::string::npos);

    bridge::SetPluginsRoot(Utf8(actual));
    const bool loaded=bridge::Initialize(Utf8(actual)); message=bridge::Detail();
    CHECK(loaded && bridge::Available());
    const HMODULE accepted=bridge::detail::Module();
    CHECK(accepted!=rejected && bridge::detail::CameraRun());
    const auto unicode=work/L"中文_日本語_😀"; fs::create_directory(unicode);
    const auto body=unicode/L"动作 源.vmd",camera=unicode/L"镜头 源.vmd",library=unicode/L"演出库";
    Vmd(body,false); Vmd(camera,true);
    // A nonmatching adjacent geometry file must not reject an unrelated VMD.
    Write(unicode/L"target_leg_geometry.json","{\"source_vmd_sha256\":\"not-this-source\"}");
    request.vmd=Utf8(body); request.motionsRoot=Utf8(library); request.title="桥接曲目";
    request.sourceFps=60.0f; request.fps=30.0f;
    request.timing.motion={0.5f,2.0f}; request.timing.facial={1.5f,-1.0f};
    request.timing.camera={0.0f,0.25f}; request.timing.audio={2.0f,-0.5f};
    const auto transactions=unicode/L"managed transactions";
    request.transactionRoot=Utf8(transactions);
    request.log=Utf8(unicode/L"导入.log"); request.clearAudio=true;
    CHECK(bridge::CanHandle(request,message));
    CHECK(bridge::Begin(request,message));
    CHECK(bridge::Busy());
    CHECK(!bridge::Begin(request,message));
    CHECK(!bridge::BeginCamera(Utf8(camera),Utf8(library),60,0.083f,true,"",message));
    ImportProgressState progress;
    CHECK(WaitBody(message,&progress)==1);
    CHECK(progress.finished && progress.succeeded && !bridge::Busy());
    CHECK(fs::exists(transactions) && fs::is_empty(transactions));
    fs::path track;
    for (const auto& entry:fs::directory_iterator(library))
        if (entry.is_directory() && fs::exists(entry.path()/L"motion.gkclip")) track=entry.path();
    CHECK(!track.empty());
    GakuMotion::jsonlite::Value newMetadata; std::string newError;
    CHECK(GakuMotion::jsonlite::ParseText(Read(track/L"info.json"),newMetadata,newError));
    const auto* sourceFps=newMetadata.Find("source_fps");
    const auto* outputFps=newMetadata.Find("fps");
    CHECK(sourceFps && sourceFps->IsNumber() && sourceFps->numberValue==60.0);
    CHECK(outputFps && outputFps->IsNumber() && outputFps->numberValue==30.0);
    const auto newTiming=GakuMotion::ReadPerformanceTiming(newMetadata);
    CHECK(newTiming.motion.speed==0.5f && newTiming.motion.offsetSeconds==2.0f &&
        newTiming.facial.speed==1.5f && newTiming.facial.offsetSeconds==-1.0f &&
        newTiming.camera.speed==0.0f && newTiming.camera.offsetSeconds==0.25f &&
        newTiming.audio.speed==2.0f && newTiming.audio.offsetSeconds==-0.5f);
    const std::string bodyBytes=Read(track/L"motion.gkclip");
    CHECK(!bodyBytes.empty());
    MotionImportRequest edit;
    edit.editDir=Utf8(track); edit.title="原生修改 标题"; edit.log=Utf8(unicode/L"编辑.log");
    edit.timing.motion={3.0f,-0.125f}; edit.timing.audio={0.0f,4.0f};
    edit.transactionRoot=Utf8(transactions);
    CHECK(bridge::CanHandle(edit,message) && bridge::Begin(edit,message));
    CHECK(WaitBody(message,&progress)==1 && Read(track/L"motion.gkclip")==bodyBytes);
    CHECK(fs::is_empty(transactions));
    GakuMotion::jsonlite::Value metadata; std::string error;
    CHECK(GakuMotion::jsonlite::ParseText(Read(track/L"info.json"),metadata,error));
    const auto* title=metadata.Find("title");
    CHECK(title && title->IsString() && title->stringValue==edit.title);
    const auto editedTiming=GakuMotion::ReadPerformanceTiming(metadata);
    CHECK(editedTiming.motion.speed==3.0f && editedTiming.motion.offsetSeconds==-0.125f &&
        editedTiming.audio.speed==0.0f && editedTiming.audio.offsetSeconds==4.0f);
    edit.timing.motion.speed=-1.0f;
    CHECK(!bridge::CanHandle(edit,message) && !bridge::Begin(edit,message) && !bridge::Busy());

    CHECK(bridge::BeginCamera(Utf8(camera),Utf8(track),60,0.083f,true,Utf8(unicode/L"镜头.log"),message));
    CHECK(bridge::CameraBusy() && !bridge::Begin(request,message));
    CHECK(WaitCamera(message)==1 && !bridge::CameraBusy());
    const std::string cameraBytes=Read(track/L"camera.gkcamera");
    CHECK(cameraBytes.size()>20 && cameraBytes.substr(0,5)=="GKCAM");
    const auto invalidCamera=unicode/L"无效镜头.vmd"; Write(invalidCamera,"invalid VMD");
    CHECK(bridge::BeginCamera(Utf8(invalidCamera),Utf8(track),60,0.083f,true,"",message));
    CHECK(WaitCamera(message)==-1 && Read(track/L"camera.gkcamera")==cameraBytes);

    // Group-song ("团曲") round-trip through real NativeAOT DLL under Unicode path
    const auto body2=unicode/L"动作 二号位.vmd";
    Vmd(body2,false);
    MotionImportRequest groupReq;
    groupReq.motionsRoot=Utf8(library);
    groupReq.title="桥接团曲";
    groupReq.clearAudio=true;
    groupReq.transactionRoot=Utf8(transactions);
    groupReq.log=Utf8(unicode/L"团曲导入.log");
    GakuMotion::ImportPositionSlot slot1; slot1.slot=1; slot1.motionVmd=Utf8(body);
    GakuMotion::ImportPositionSlot slot2; slot2.slot=2; slot2.motionVmd=Utf8(body2);
    slot2.x=-1.5f; slot2.y=0.25f; slot2.z=2.0f; slot2.yawDegrees=45.0f;
    groupReq.positions={slot1,slot2};
    groupReq.positionsExplicit=true;
    CHECK(bridge::CanHandle(groupReq,message) && bridge::Begin(groupReq,message));
    CHECK(WaitBody(message,&progress)==1 && fs::is_empty(transactions));

    fs::path groupTrack;
    for (const auto& entry:fs::directory_iterator(library))
        if (entry.is_directory() && entry.path()!=track && fs::exists(entry.path()/L"positions"/L"slot_2"/L"motion.gkclip"))
            groupTrack=entry.path();
    CHECK(!groupTrack.empty());
    GakuMotion::jsonlite::Value grpMeta; std::string grpErr;
    CHECK(GakuMotion::jsonlite::ParseText(Read(groupTrack/L"info.json"),grpMeta,grpErr));
    CHECK(grpMeta.BoolPath("is_group",false) && grpMeta.IntPath("position_count",0)==2);
    const std::string slot1ClipBefore=Read(groupTrack/L"motion.gkclip");
    const std::string slot2ClipBefore=Read(groupTrack/L"positions"/L"slot_2"/L"motion.gkclip");
    CHECK(!slot1ClipBefore.empty() && !slot2ClipBefore.empty());

    // Zero-rebake placement-only edit via C++ bridge
    MotionImportRequest zeroEdit;
    zeroEdit.editDir=Utf8(groupTrack);
    zeroEdit.transactionRoot=Utf8(transactions);
    zeroEdit.positionsExplicit=true;
    GakuMotion::ImportPositionSlot e1; e1.slot=1; e1.preserveExisting=true;
    GakuMotion::ImportPositionSlot e2; e2.slot=2; e2.preserveExisting=true; e2.x=2.25f; e2.yawDegrees=-30.0f;
    zeroEdit.positions={e1,e2};
    CHECK(bridge::CanHandle(zeroEdit,message) && bridge::Begin(zeroEdit,message));
    CHECK(WaitBody(message,&progress)==1);
    CHECK(Read(groupTrack/L"motion.gkclip")==slot1ClipBefore &&
        Read(groupTrack/L"positions"/L"slot_2"/L"motion.gkclip")==slot2ClipBefore);
    GakuMotion::jsonlite::Value grpMeta2;
    CHECK(GakuMotion::jsonlite::ParseText(Read(groupTrack/L"info.json"),grpMeta2,grpErr));
    const auto* posArr=grpMeta2.Find("positions");
    CHECK(posArr && posArr->IsArray() && posArr->arrayItems.size()==2 &&
        std::fabs(posArr->arrayItems[1].FloatPath("x",0.0f)-2.25f)<1e-3f &&
        std::fabs(posArr->arrayItems[1].FloatPath("yaw_degrees",0.0f)-(-30.0f))<1e-3f);

    // Selective slot-2 edit preserves slot 1 clip bytes verbatim
    MotionImportRequest selEdit;
    selEdit.editDir=Utf8(groupTrack);
    selEdit.transactionRoot=Utf8(transactions);
    selEdit.positionsExplicit=true;
    GakuMotion::ImportPositionSlot s2d; s2d.slot=2; s2d.disableMorph=true;
    selEdit.positions={e1,s2d};
    CHECK(bridge::CanHandle(selEdit,message) && bridge::Begin(selEdit,message));
    CHECK(WaitBody(message,&progress)==1 && Read(groupTrack/L"motion.gkclip")==slot1ClipBefore);

    // Reject empty explicit positions and non-finite coordinates before spawning worker
    MotionImportRequest badGroup=zeroEdit;
    badGroup.positions.clear();
    CHECK(!bridge::CanHandle(badGroup,message) && !bridge::Begin(badGroup,message) && !bridge::Busy());
    badGroup.positions={e1,e2};
    badGroup.positions[1].x=std::numeric_limits<float>::quiet_NaN();
    CHECK(!bridge::CanHandle(badGroup,message) && !bridge::Begin(badGroup,message) && !bridge::Busy());

    CHECK(bridge::BeginCamera(Utf8(camera),Utf8(track),60,0.083f,true,"",message));
    bridge::Shutdown();
    CHECK(!bridge::Busy() && !bridge::CameraBusy() && bridge::Available());
    CHECK(bridge::detail::Module()==accepted && GetModuleHandleW(oldLibrary.c_str())==rejected);
    CHECK(bridge::Initialize(Utf8(old)) && bridge::detail::Module()==accepted && count()==1);
    std::printf("Native import bridge actual DLL: %d checks PASS; UTF-8 API inputs, ASCII harness root\n",checks);
    return 0;
}
