#pragma once
#include "GkLiveObservation.hpp"

namespace GakuMotion::live_takeover
{
    struct ViewingGate
    {
        void* modelClass{};
        void* fixedClass{};
        void* enumClass{};
        const void* fixedGetter{};
        const void* typeGetter{};
        int pictureBook{}, rosterLive{};
        bool ready{};

        static ViewingGate Resolve(void* (*findClass)(const char*, const char*, const char*),
            const ExactMethodMetadata& metadata, void* (*findField)(void*, const char*),
            void* (*fieldType)(void*), uint32_t (*fieldFlags)(void*),
            void (*readStatic)(void*, void*), int (*valueSize)(void*, uint32_t*))
        {
            ViewingGate g;
            if (!findClass || !findField || !fieldType || !fieldFlags || !readStatic || !valueSize) return g;
            g.modelClass = findClass("Assembly-CSharp.dll", "Campus.Live", "LiveModel");
            g.fixedClass = findClass("Assembly-CSharp.dll", "Campus.Live.Data", "LiveFixedData");
            g.enumClass = findClass("Assembly-CSharp.dll", "Campus.Live", "LiveFromType");
            if (!g.modelClass || !g.fixedClass || !g.enumClass) return g;
            uint32_t alignment = 0;
            if (valueSize(g.enumClass, &alignment) != sizeof(int)) return g;
            g.fixedGetter = metadata.Find(g.modelClass, "get_FixedData", false, {0x12, g.fixedClass}, {});
            g.typeGetter = metadata.Find(g.fixedClass, "get_LiveFromType", false, {0x11, g.enumClass}, {});
            void* picture = findField(g.enumClass, "PictureBook");
            void* roster = findField(g.enumClass, "RosterLive");
            if (!g.fixedGetter || !g.typeGetter || !picture || !roster ||
                (fieldFlags(picture) & 0x50) != 0x50 || (fieldFlags(roster) & 0x50) != 0x50 ||
                !metadata.Matches(fieldType(picture), {0x11, g.enumClass}) ||
                !metadata.Matches(fieldType(roster), {0x11, g.enumClass})) return g;
            readStatic(picture, &g.pictureBook);
            readStatic(roster, &g.rosterLive);
            g.ready = g.pictureBook > 0 && g.rosterLive > 0 && g.pictureBook != g.rosterLive;
            return g;
        }

        bool Allows(const Api& api, const RuntimeAccess& a, void* scene) const
        {
            if (!ready || !a.invoke || !a.unbox) return false;
            void* model = SceneField(api, a, scene, "_liveModel", modelClass);
            if (!model) return false;
            void* exception = nullptr;
            void* fixed = a.invoke(fixedGetter, model, nullptr, &exception);
            if (exception || !fixed || a.objectClass(fixed) != fixedClass) return false;
            exception = nullptr;
            void* boxed = a.invoke(typeGetter, fixed, nullptr, &exception);
            if (exception || !boxed) return false;
            auto* type = static_cast<const int*>(a.unbox(boxed));
            return type && (*type == pictureBook || *type == rosterLive);
        }
    };
}
