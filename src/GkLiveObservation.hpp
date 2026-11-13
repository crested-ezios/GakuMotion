#pragma once
#include "GkLiveApi.hpp"
#include <cmath>
#include <cstdint>

namespace GakuMotion::live_takeover
{
    struct RuntimeAccess
    {
        void* (*objectClass)(void*){};
        void* (*findField)(void*, const char*){};
        void* (*fieldType)(void*){};
        uint32_t (*fieldFlags)(void*){};
        int (*typeCode)(void*){};
        void* (*classFromType)(void*){};
        void (*readField)(void*, void*, void*){};
        void* (*invoke)(const void*, void*, void**, void**){};
        void* (*unbox)(void*){};
    };

    struct Observation
    {
        bool confirmed{};
        bool paused{};
        float time{};
        float duration{};
    };

    inline void* SceneField(const Api& api, const RuntimeAccess& a, void* scene,
                            const char* name, void* expected)
    {
        if (!scene || !expected || !a.objectClass || !a.findField || !a.fieldType ||
            !a.fieldFlags || !a.typeCode || !a.classFromType || !a.readField ||
            a.objectClass(scene) != api.sceneClass) return nullptr;
        void* field = a.findField(api.sceneClass, name);
        void* type = field ? a.fieldType(field) : nullptr;
        if (!type || (a.fieldFlags(field) & 0x10) || a.typeCode(type) != 0x12 ||
            a.classFromType(type) != expected) return nullptr;
        void* value = nullptr;
        a.readField(scene, field, &value);
        return value && a.objectClass(value) == expected ? value : nullptr;
    }

    // Runs on the Unity thread with a freshly confirmed Live scene presenter.
    // Returns copied values only; it never retains a managed object or changes
    // playback. Any unexpected metadata, exception or value is rejected.
    inline Observation Observe(const Api& api, const RuntimeAccess& a, void* scene)
    {
        if (!api.Ready() || !scene || !a.objectClass || !a.findField || !a.fieldType ||
            !a.fieldFlags || !a.typeCode || !a.classFromType || !a.readField ||
            !a.invoke || !a.unbox || a.objectClass(scene) != api.sceneClass) return {};
        void* live = SceneField(api, a, scene, "_live", api.presenterClass);
        void* player = SceneField(api, a, scene, "_livePlayer", api.playerClass);
        if (!live || !player) return {};
        auto call = [&](const void* method, void* instance) -> void* {
            void* exception = nullptr;
            void* result = a.invoke(method, instance, nullptr, &exception);
            return exception ? nullptr : result;
        };
        void* execution = call(api.execution, player);
        if (!execution || a.objectClass(execution) != api.executionClass) return {};
        void* boxed = call(api.isPaused, execution);
        auto* pauseValue = boxed ? static_cast<const uint8_t*>(a.unbox(boxed)) : nullptr;
        if (!pauseValue) return {};
        const bool paused = *pauseValue != 0;
        boxed = call(api.currentTime, player);
        auto* timeValue = boxed ? static_cast<const float*>(a.unbox(boxed)) : nullptr;
        if (!timeValue) return {};
        const float time = *timeValue;
        boxed = call(api.duration, player);
        auto* durationValue = boxed ? static_cast<const float*>(a.unbox(boxed)) : nullptr;
        if (!durationValue) return {};
        const float duration = *durationValue;
        if (!std::isfinite(time) || !std::isfinite(duration) || time < 0 || duration <= 0 ||
            time > duration + 1.0f) return {};
        return {true, paused, time, duration};
    }

    // Only a user command on the Unity thread may call this. The caller must
    // independently verify that scene still matches the command's owner token.
    inline bool SetPaused(const Api& api, const RuntimeAccess& a, void* scene, bool desired)
    {
        const auto before = Observe(api, a, scene);
        if (!before.confirmed) return false;
        if (before.paused == desired) return true;
        void* presenter = SceneField(api, a, scene, "_live", api.presenterClass);
        if (!presenter) return false;
        void* exception = nullptr;
        a.invoke(desired ? api.pause : api.resume, presenter, nullptr, &exception);
        if (exception) return false;
        const auto after = Observe(api, a, scene);
        return after.confirmed && after.paused == desired;
    }
}
