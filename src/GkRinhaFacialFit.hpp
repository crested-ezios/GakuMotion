#pragma once
// Read-only CharaAdapter and legacy facial fit clients. The provider's table
// and records remain immutable for the lifetime of the process.
//
// Why the lookup is retried instead of done once at activation:
//   GakuLoader activates plugins in sorted filename order (see
//   PluginHost.cpp ActivatePlugins), and "GakuMotion.dll" sorts before
//   "RinhaReplacement.dll". A single query during activation therefore *always*
//   runs before the provider has registered, and the fit would silently never
//   apply on a real install. The host handle stays valid for process lifetime,
//   so re-asking later is safe; `Ensure()` throttles the retry and bumps a
//   generation counter that forces a binding rebuild once the provider appears.
#include "GakuCharaAdapter.h"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>

namespace GakuMotion::facial_fit
{
    inline RinhaFacialFitService service{};
    inline bool available{false};
    inline GakuCharaAdapterService adapter{};
    inline RinhaFacialFitService adapterFit{};
    // Bumped on every successful lookup; bindings compare their bound value so
    // a provider that appears mid-session rebinds without a clip change.
    inline unsigned generation{0};

    inline GakuHostApi host{};
    inline std::chrono::steady_clock::time_point nextAttempt{};
    inline constexpr std::chrono::milliseconds kRetryInterval{500};

    // Face prefab identity may carry Unity's clone suffix; the provider only
    // matches the authored name.
    inline std::string StripCloneSuffix(std::string name)
    {
        const std::string clone = "(Clone)";
        if (name.size() >= clone.size() && name.compare(name.size() - clone.size(), clone.size(), clone) == 0)
        {
            name.resize(name.size() - clone.size());
            if (!name.empty() && name.back() == ' ') name.pop_back();
        }
        return name;
    }

    inline bool ValidFit(const RinhaFacialFitService& fit)
    {
        return fit.abiVersion == RINHA_FACIAL_FIT_VERSION && fit.structSize >= sizeof(fit) &&
            fit.faceCount && fit.faceAt && fit.findFace && fit.evaluateMouth && fit.evaluateEyes &&
            fit.faceCount(fit.context) != 0;
    }

    inline void Lookup()
    {
        if (!host.queryService) return;
        if (!adapter.resolveFace)
        {
            GakuCharaAdapterService candidate{};
            if (host.queryService(host.hostContext, GAKU_CHARA_ADAPTER_SERVICE, GAKU_CHARA_ADAPTER_VERSION,
                sizeof(candidate), &candidate) == GAKU_OK &&
                candidate.abiVersion == GAKU_CHARA_ADAPTER_VERSION && candidate.structSize >= sizeof(candidate) &&
                candidate.resolveFace && candidate.getFacialFit)
            {
                adapter = candidate;
                RinhaFacialFitService fit{};
                if (adapter.getFacialFit(adapter.context, &fit) == GAKU_OK && ValidFit(fit)) adapterFit = fit;
                ++generation;
            }
        }
        if (available) return;
        RinhaFacialFitService candidate{};
        if (host.queryService(host.hostContext, RINHA_FACIAL_FIT_SERVICE, RINHA_FACIAL_FIT_VERSION,
            sizeof(candidate), &candidate) != GAKU_OK || !ValidFit(candidate)) return;
        service = candidate;
        available = true;
        ++generation;
    }

    inline void Connect(const GakuHostApi& api)
    {
        host = api;
        service = {};
        available = false;
        adapter = {};
        adapterFit = {};
        nextAttempt = std::chrono::steady_clock::now() + kRetryInterval;
        Lookup();
    }

    // One branch in the steady state; a throttled re-query until it succeeds.
    inline void Ensure()
    {
        if ((adapter.resolveFace && available) || !host.queryService) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextAttempt) return;
        nextAttempt = now + kRetryInterval;
        Lookup();
    }

    struct FitMatch
    {
        const RinhaFacialFitFace* face{};
        const RinhaFacialFitService* provider{};
    };

    inline FitMatch FindFit(const std::string& faceName)
    {
        Ensure();
        const std::string name = StripCloneSuffix(faceName);
        if (name.empty()) return {};
        if (adapterFit.findFace)
            if (const auto* face = adapterFit.findFace(adapterFit.context, name.c_str())) return {face, &adapterFit};
        if (available && service.findFace)
            if (const auto* face = service.findFace(service.context, name.c_str())) return {face, &service};
        return {};
    }

    inline const RinhaFacialFitFace* Find(const std::string& faceName)
    {
        return FindFit(faceName).face;
    }

    inline GakuCharaIdentity ResolveIdentity(const std::string& faceName)
    {
        Ensure();
        if (!adapter.resolveFace || faceName.empty()) return {};
        GakuCharaIdentity identity{};
        const std::string name = StripCloneSuffix(faceName);
        if (adapter.resolveFace(adapter.context, name.c_str(), &identity) != GAKU_OK ||
            !identity.character[0] ||
            std::find(std::begin(identity.character), std::end(identity.character), '\0') == std::end(identity.character) ||
            std::find(std::begin(identity.displayName), std::end(identity.displayName), '\0') == std::end(identity.displayName))
            return {};
        return identity;
    }
}
