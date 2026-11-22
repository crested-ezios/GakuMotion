#pragma once
#include "GakuBurstBridge.h"
#include "GkIdolController.hpp"
#include <deque>
#include <chrono>
#include <string>
#include <windows.h>

namespace GakuMotion {
// One module/session, multiple actor-lifetime contexts. The v2 client remains the
// compatibility path for the fixed master actor when v3 is unavailable.
class BurstBridgeMultiClient {
    friend struct BurstBridgeMultiClientTestAccess;
public:
    struct Slot {
        GbbContext token{}; void* actor{};
        bool faulted{}, outstanding{};
        uint64_t sequence{}, generation{};
        int minimumFrame{-1};
        std::chrono::steady_clock::time_point matched{};
        GbbStatus status{}; GbbPerf perf{};
        std::string reason{"not_requested"};
    };
private:
    HMODULE module_{}; GbbApiV3 api_{}; GbbConfig config_{};
    std::wstring path_; bool attempted_{},stopped_{};
    std::deque<Slot> slots_{};
    static HMODULE Load(const wchar_t* path) {
        __try { return LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    static int Negotiate(GbbGetApiV3Fn fn,GbbApiV3* api) {
        __try { return fn(GBB_API_VERSION_V3,api); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static int ConfigureApi(const GbbApiV3& api,const GbbConfig* config) {
        __try { return api.configure(config); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static int Create(const GbbApiV3& api,GbbContext* token) {
        __try { return api.create_context(token); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static int Destroy(const GbbApiV3& api,GbbContext token) {
        __try { return api.destroy_context(token); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static int ClearApi(const GbbApiV3& api,GbbContext token,uint32_t flags) {
        __try { return api.clear_context(token,flags); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static int PublishApi(const GbbApiV3& api,GbbContext token,const GbbFrame* frame) {
        __try { return api.publish_context(token,frame); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static int Status(const GbbApiV3& api,GbbContext token,GbbStatus* status) {
        __try { return api.get_context_status(token,status); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static int Perf(const GbbApiV3& api,GbbContext token,GbbPerf* perf) {
        __try { return api.get_context_perf(token,perf); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    static void StopApi(const GbbApiV3& api) {
        __try { api.shutdown(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    static int DumpApi(const GbbApiV3& api,const wchar_t* path) {
        __try { return api.dump_traces(path); } __except(EXCEPTION_EXECUTE_HANDLER) { return GBB_E_FAULT; }
    }
    bool Fail(size_t index,std::string why) {
        auto& slot=slots_[index];
        if (slot.token && api_.clear_context) ClearApi(api_,slot.token,0);
        slot.faulted=true; slot.reason=std::move(why); slot.status.state=GBB_STATE_FAULTED;
        slot.status.injection_fault=1; return false;
    }
public:
    std::string reason{"not_requested"};
    Slot& State(size_t index) { if (slots_.size() <= index) slots_.resize(index + 1); return slots_[index]; }
    const Slot& State(size_t index) const
    {
        static const Slot empty{};
        return index < slots_.size() ? slots_[index] : empty;
    }
    void Configure(const wchar_t* root,GbbLogFn log,void* context) {
        if (root && *root) path_=std::wstring(root)+L"/GakuBurstBridge.dll";
        config_={}; config_.size=sizeof(config_); config_.version=GBB_API_VERSION;
        config_.log=log; config_.log_context=context;
    }
    bool Ensure() {
        if (stopped_) { reason="client_shutdown"; return false; }
        if (api_.publish_context) return true;
        if (attempted_) return false;
        attempted_=true;
        if (path_.empty()) { reason="companion_path_missing"; return false; }
        const auto attributes=GetFileAttributesW(path_.c_str());
        if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY)) { reason="companion_missing"; return false; }
        module_=Load(path_.c_str());
        if (!module_) { reason="companion_load_failed"; return false; }
        auto get=reinterpret_cast<GbbGetApiV3Fn>(GetProcAddress(module_,"GakuBurstBridge_GetApi"));
        GbbApiV3 next{}; next.size=sizeof(next); next.version=GBB_API_VERSION_V3;
        if (!get || Negotiate(get,&next)!=GBB_OK || next.size!=sizeof(next) || next.version!=GBB_API_VERSION_V3 ||
            !next.configure || !next.create_context || !next.destroy_context || !next.publish_context || !next.clear_context ||
            !next.get_context_status || !next.get_context_perf || !next.shutdown || !next.dump_traces) {
            reason="companion_context_abi_missing"; return false;
        }
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Load),&pinned)) { reason="client_pin_failed"; return false; }
        if (ConfigureApi(next,&config_)!=GBB_OK) { reason="companion_configure_failed"; return false; }
        api_=next; reason="discovering"; return true;
    }
    bool Publish(size_t index,const IdolBoneBinding& binding,const IdolController& ctrl,float time,int frame,
        bool quartz=false,bool diagnostics=false) {
        if (!Ensure()) return false;
        if (slots_.size() <= index) slots_.resize(index + 1);
        auto& slot=slots_[index]; if (slot.faulted) return false;
        if (slot.token && slot.actor!=binding.actor && !Clear(index)) return false;
        if (!slot.token) {
            if (Create(api_,&slot.token)!=GBB_OK || !slot.token) return Fail(index,"context_create_failed");
            slot.actor=binding.actor;
            GbbStatus initial{}; initial.size=sizeof(initial); initial.version=GBB_API_VERSION;
            if (Status(api_,slot.token,&initial)!=GBB_OK || initial.size!=sizeof(initial) || initial.version!=GBB_API_VERSION ||
                !initial.generation || initial.actor || initial.sequence || initial.writes || initial.injected || initial.injection_fault ||
                initial.state==GBB_STATE_SHUTDOWN || initial.state==GBB_STATE_FAULTED)
                return Fail(index,"context_lifetime_unconfirmed");
            slot.generation=initial.generation; slot.minimumFrame=frame;
        }
        if (!binding.cachedAnimator || !ctrl.fnGetLocalRotation || !ctrl.fnGetLocalPosition) {
            ClearApi(api_,slot.token,0); slot.reason="body_binding_pending"; return false;
        }
        GbbFrame packet{}; packet.size=sizeof(packet); packet.version=GBB_API_VERSION;
        packet.actor=binding.actor; packet.animator=binding.cachedAnimator;
        packet.clip_time=time; packet.unity_frame=frame; packet.flags=GBB_FRAME_INJECT;
        if (quartz) packet.flags|=GBB_FRAME_BEFORE_QUARTZ;
        if (diagnostics) packet.flags|=GBB_FRAME_SLEEVE_DIAGNOSTICS;
        for (size_t i=0;i<binding.boneTransforms.size() && packet.bone_count<GBB_MAX_BONES;++i) {
            if (!binding.boneTransforms[i] || i>=binding.boneHids.size()) continue;
            auto& pose=packet.bones[packet.bone_count++];
            pose.transform=binding.boneTransforms[i]; pose.humanoid_id=binding.boneHids[i];
            ctrl.fnGetLocalRotation(pose.transform,&pose.local_rotation.x);
            ctrl.fnGetLocalPosition(pose.transform,&pose.local_position.x);
        }
        slot.outstanding=true;
        const auto result=PublishApi(api_,slot.token,&packet);
        slot.status={}; slot.status.size=sizeof(slot.status); slot.status.version=GBB_API_VERSION;
        slot.perf={}; slot.perf.size=sizeof(slot.perf); slot.perf.version=GBB_API_VERSION;
        if (Status(api_,slot.token,&slot.status)!=GBB_OK || Perf(api_,slot.token,&slot.perf)!=GBB_OK ||
            slot.status.size!=sizeof(slot.status) || slot.status.version!=GBB_API_VERSION || slot.status.state>GBB_STATE_SHUTDOWN ||
            slot.status.writes>GBB_MAX_BONES || slot.perf.size!=sizeof(slot.perf) || slot.perf.version!=GBB_API_VERSION)
            return Fail(index,"context_observer_fault");
        if (result!=GBB_OK && result!=GBB_PENDING) {
            std::string why="context_publish_rejected result="+std::to_string(result);
            const auto* detail=slot.status.detail;
            const auto* end=std::find(detail,detail+sizeof(slot.status.detail),'\0');
            if (end!=detail) why+=" detail="+std::string(detail,end);
            return Fail(index,std::move(why));
        }
        if (slot.status.injection_fault || slot.status.state==GBB_STATE_FAULTED) return Fail(index,"context_faulted");
        if (slot.status.generation<slot.generation) return Fail(index,"context_generation_regressed");
        if (slot.status.generation!=slot.generation) {
            slot.generation=slot.status.generation; slot.minimumFrame=frame;
            slot.sequence=0; slot.matched={};
        }
        if (slot.status.state==GBB_STATE_INJECTING && slot.status.injected && slot.status.writes && slot.status.actor==binding.actor) {
            const auto now=std::chrono::steady_clock::now();
            if (slot.status.sequence<slot.sequence) return Fail(index,"context_sequence_regressed");
            if (slot.status.sequence!=slot.sequence) { slot.sequence=slot.status.sequence; slot.matched=now; }
            if (!slot.status.sequence || slot.status.source_frame<slot.minimumFrame || slot.status.source_frame>frame ||
                !slot.status.generation) return Fail(index,"context_source_invalid");
            if (static_cast<int64_t>(frame)-slot.status.source_frame>4 || now-slot.matched>std::chrono::milliseconds(100)) {
                slot.reason="native_pending_stale"; return false;
            }
            slot.reason="native"; return true;
        }
        slot.reason="native_pending"; return false;
    }
    bool HasCaptureInput() const { for (const auto& slot:slots_) if (slot.outstanding) return true; return false; }
    bool ClearForCapture(size_t index) {
        if (index>=slots_.size()) return true;
        auto& slot=slots_[index]; if (!slot.outstanding) return true;
        if (!slot.token || !api_.clear_context) return false;
        GbbStatus before{}; before.size=sizeof(before); before.version=GBB_API_VERSION;
        if (Status(api_,slot.token,&before)!=GBB_OK || before.size!=sizeof(before) || before.version!=GBB_API_VERSION ||
            ClearApi(api_,slot.token,0)!=GBB_OK) return false;
        GbbStatus after{}; after.size=sizeof(after); after.version=GBB_API_VERSION;
        if (Status(api_,slot.token,&after)!=GBB_OK || after.size!=sizeof(after) || after.version!=GBB_API_VERSION ||
            after.state>GBB_STATE_SHUTDOWN || after.generation<=before.generation || after.actor || after.injected ||
            after.writes || after.sequence || after.injection_fault || after.state==GBB_STATE_FAULTED || after.state==GBB_STATE_SHUTDOWN) return false;
        slot.outstanding=false; slot.sequence=0; slot.matched={}; slot.generation=after.generation;
        slot.minimumFrame=-1; slot.status=after; slot.reason="capture_cleared"; return true;
    }
    bool ClearForCapture() { bool clean=true; for (size_t i=0;i<slots_.size();++i) clean=ClearForCapture(i)&&clean; return clean; }
    bool Clear(size_t index) {
        if (index>=slots_.size()) return true;
        auto& slot=slots_[index];
        if (slot.token && (!api_.destroy_context || Destroy(api_,slot.token)!=GBB_OK)) {
            slot.outstanding=true; slot.faulted=true; slot.reason="capture_destroy_unconfirmed"; return false;
        }
        slot=Slot{}; return true;
    }
    bool Clear() { bool clean=true; for (size_t i=0;i<slots_.size();++i) clean=Clear(i)&&clean; return clean; }
    int DumpTraces(const wchar_t* path) const { return api_.dump_traces ? DumpApi(api_,path) : GBB_E_UNAVAILABLE; }
    bool Shutdown() {
        stopped_=true; const bool active=api_.shutdown!=nullptr;
        if (active) StopApi(api_);
        api_={}; return active;
    }
};
}
