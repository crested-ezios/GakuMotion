#pragma once
#include "GkExactMethod.hpp"

namespace GakuMotion::live_takeover
{
    // Metadata only. No scene discovery, object dereference or game calls.
    // Method handles can be invoked only after fresh ownership checks.
    struct Api
    {
        void* sceneClass{};
        void* presenterClass{};
        void* playerClass{};
        void* executionClass{};
        const void* pause{};
        const void* resume{};
        const void* execution{};
        const void* isPaused{};
        const void* currentTime{};
        const void* duration{};
        const void* seek{};

        bool Ready() const
        {
            return sceneClass && presenterClass && playerClass && executionClass && pause && resume &&
                execution && isPaused && currentTime && duration && seek;
        }

        static Api Resolve(void* (*findClass)(const char*, const char*, const char*),
                           const ExactMethodMetadata& metadata)
        {
            Api out;
            if (!findClass) return out;
            out.sceneClass = findClass("Assembly-CSharp.dll", "Campus.Live", "LiveScenePresenter");
            out.presenterClass = findClass("Assembly-CSharp.dll", "Campus.Live", "LivePresenter");
            out.playerClass = findClass("Assembly-CSharp.dll", "Campus.Live", "LivePlayerPresenter");
            out.executionClass = findClass("Assembly-CSharp.dll", "Campus.Live.Data", "LiveExecutionData");
            out.pause = metadata.Find(out.presenterClass, "Pause", false, {0x01}, {});
            out.resume = metadata.Find(out.presenterClass, "UnPause", false, {0x01}, {});
            out.execution = metadata.Find(out.playerClass, "get_ExecutionData", false, {0x12, out.executionClass}, {});
            out.isPaused = metadata.Find(out.executionClass, "get_IsPause", false, {0x02}, {});
            out.currentTime = metadata.Find(out.playerClass, "get_TimelineTime", false, {0x0c}, {});
            out.duration = metadata.Find(out.playerClass, "get_Duration", false, {0x0c}, {});
            out.seek = metadata.Find(out.presenterClass, "Seek", false, {0x01}, {{0x0c}});
            return out;
        }
    };
}
