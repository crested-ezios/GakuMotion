#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace gke2e {

enum class Status {
    Pass,
    Fail,
    PendingImpl
};

inline constexpr const char* StatusString(Status s) noexcept {
    switch (s) {
        case Status::Pass:        return "PASS";
        case Status::Fail:        return "FAIL";
        case Status::PendingImpl: return "PENDING_IMPL";
    }
    return "FAIL";
}

struct TestResult {
    std::string id;
    int tier{1};
    std::string feature;
    std::string name;
    Status status{Status::Pass};
    std::string detail;
};

inline void AppendJsonEscaped(std::string& out, std::string_view input) {
    out.push_back('"');
    for (unsigned char ch : input) {
        switch (ch) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (ch < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(ch));
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

inline std::string EscapeJsonString(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    AppendJsonEscaped(out, input);
    return out;
}

inline std::string SerializeResultsJson(const std::vector<TestResult>& results) {
    std::string out;
    out.reserve(results.size() * 160 + 16);
    out.append("[\n");
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out.append("  {\"id\":");
        AppendJsonEscaped(out, r.id);
        out.append(",\"tier\":");
        out.append(std::to_string(r.tier));
        out.append(",\"feature\":");
        AppendJsonEscaped(out, r.feature);
        out.append(",\"name\":");
        AppendJsonEscaped(out, r.name);
        out.append(",\"status\":");
        AppendJsonEscaped(out, StatusString(r.status));
        out.append(",\"detail\":");
        AppendJsonEscaped(out, r.detail);
        out.push_back('}');
        if (i + 1 < results.size()) {
            out.push_back(',');
        }
        out.push_back('\n');
    }
    out.append("]\n");
    return out;
}

inline std::filesystem::path PathFromUtf8(std::string_view utf8Path) {
    std::u8string u8;
    u8.reserve(utf8Path.size());
    for (unsigned char c : utf8Path) {
        u8.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(u8);
}

inline bool WriteUtf8NoBomFile(std::string_view utf8Path, std::string_view content, std::string* errOut = nullptr) {
    try {
        const std::filesystem::path p = PathFromUtf8(utf8Path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            if (errOut) *errOut = "Failed to open output file for writing.";
            return false;
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs.good()) {
            if (errOut) *errOut = "Failed while writing output file.";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        if (errOut) *errOut = ex.what();
        return false;
    }
}

namespace detail {

template <typename T>
inline std::string FormatValue(const T& value) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, bool>) {
        oss << (value ? "true" : "false");
    } else if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::nullptr_t>) {
        oss << "nullptr";
    } else {
        oss << value;
    }
    return oss.str();
}

template <typename A, typename B>
inline bool ValuesEqual(const A& a, const B& b) {
    using Da = std::remove_cvref_t<A>;
    using Db = std::remove_cvref_t<B>;
    if constexpr ((std::is_convertible_v<A, std::string_view> && std::is_convertible_v<B, std::string_view>)) {
        return std::string_view(a) == std::string_view(b);
    } else if constexpr (std::is_integral_v<Da> && std::is_integral_v<Db> &&
                         !std::is_same_v<Da, bool> && !std::is_same_v<Db, bool>) {
        return std::cmp_equal(a, b);
    } else {
        return a == b;
    }
}

} // namespace detail

class CaseContext {
public:
    void Pass(std::string_view detail = "") {
        if (status_ == Status::Pass) {
            if (!detail.empty()) {
                detail_.assign(detail);
            }
        }
    }

    void Fail(std::string_view detail) {
        if (status_ != Status::Fail) {
            status_ = Status::Fail;
            detail_.assign(detail);
        } else if (!detail.empty()) {
            if (!detail_.empty()) {
                detail_.append("; ");
            }
            detail_.append(detail);
        }
    }

    void Pending(std::string_view reason) {
        if (status_ != Status::Fail) {
            status_ = Status::PendingImpl;
            detail_.assign(reason);
        }
    }

    bool Check(bool cond, std::string_view exprText, const char* file, int line, std::string_view msg = "") {
        if (cond) {
            return true;
        }
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << (file ? file : "<unknown>") << ':' << line << ": check failed: " << exprText;
        if (!msg.empty()) {
            oss << " (" << msg << ')';
        }
        Fail(oss.str());
        return false;
    }

    template <typename A, typename B>
    bool CheckEq(const A& actual, const B& expected, const char* actualExpr, const char* expectedExpr,
                 const char* file, int line) {
        if (detail::ValuesEqual(actual, expected)) {
            return true;
        }
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << (file ? file : "<unknown>") << ':' << line << ": expected "
            << actualExpr << " == " << expectedExpr
            << " (actual=" << detail::FormatValue(actual)
            << ", expected=" << detail::FormatValue(expected) << ')';
        Fail(oss.str());
        return false;
    }

    bool CheckNear(double actual, double expected, double eps, const char* actualExpr,
                   const char* expectedExpr, const char* file, int line) {
        const bool ok = std::isfinite(actual) && std::isfinite(expected) &&
                        std::isfinite(eps) && eps >= 0.0 &&
                        std::fabs(actual - expected) <= eps;
        if (ok) {
            return true;
        }
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << (file ? file : "<unknown>") << ':' << line << ": expected "
            << actualExpr << " ~= " << expectedExpr << " (eps=" << eps
            << ", actual=" << actual << ", expected=" << expected << ')';
        Fail(oss.str());
        return false;
    }

    Status GetStatus() const noexcept { return status_; }
    const std::string& GetDetail() const noexcept { return detail_; }
    bool HasFailed() const noexcept { return status_ == Status::Fail; }

private:
    Status status_{Status::Pass};
    std::string detail_;
};

class Runner {
public:
    struct TestEntry {
        std::string id;
        int tier{1};
        std::string feature;
        std::string name;
        std::function<void(CaseContext&)> fn;
    };

    void Add(std::string id, int tier, std::string feature, std::string name,
             std::function<void(CaseContext&)> fn) {
        entries_.push_back(TestEntry{
            std::move(id), tier, std::move(feature), std::move(name), std::move(fn)
        });
    }

    std::vector<TestResult> ExecuteAll(int tierFilter = 0, std::string_view featureFilter = "All") const {
        std::vector<TestResult> results;
        results.reserve(entries_.size());
        for (const auto& entry : entries_) {
            if (tierFilter != 0 && entry.tier != tierFilter) {
                continue;
            }
            if (!featureFilter.empty() && featureFilter != "All" && entry.feature != featureFilter) {
                continue;
            }
            CaseContext ctx;
            try {
                if (entry.fn) {
                    entry.fn(ctx);
                } else {
                    ctx.Fail("Null test function.");
                }
            } catch (const std::exception& ex) {
                ctx.Fail(std::string("Unhandled std::exception: ") + ex.what());
            } catch (...) {
                ctx.Fail("Unhandled unknown C++ exception.");
            }
            results.push_back(TestResult{
                entry.id,
                entry.tier,
                entry.feature,
                entry.name,
                ctx.GetStatus(),
                ctx.GetDetail()
            });
        }
        return results;
    }

    int Run(int argc, char** argv) const {
        std::string outPath;
        int tierFilter = 0; // 0 = All
        std::string featureFilter = "All";
        bool allowPending = false;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i] ? std::string_view(argv[i]) : std::string_view{};
            if (arg == "--out" && i + 1 < argc) {
                outPath = argv[++i];
            } else if (arg == "--tier" && i + 1 < argc) {
                const std::string_view val = argv[++i];
                if (val == "All" || val == "all") {
                    tierFilter = 0;
                } else if (val == "1") {
                    tierFilter = 1;
                } else if (val == "2") {
                    tierFilter = 2;
                } else if (val == "3") {
                    tierFilter = 3;
                } else if (val == "4") {
                    tierFilter = 4;
                } else {
                    std::cerr << "[gke2e] Invalid --tier value: " << val << '\n';
                    return 2;
                }
            } else if (arg == "--feature" && i + 1 < argc) {
                featureFilter = argv[++i];
            } else if (arg == "--allow-pending") {
                allowPending = true;
            }
        }

        const std::vector<TestResult> results = ExecuteAll(tierFilter, featureFilter);
        const std::string json = SerializeResultsJson(results);

        if (!outPath.empty()) {
            std::string writeErr;
            if (!WriteUtf8NoBomFile(outPath, json, &writeErr)) {
                std::cerr << "[gke2e] Failed writing JSON report to '" << outPath << "': " << writeErr << '\n';
                return 2;
            }
        } else {
            std::cout << json;
        }

        std::size_t passCount = 0;
        std::size_t failCount = 0;
        std::size_t pendingCount = 0;
        for (const auto& r : results) {
            switch (r.status) {
                case Status::Pass:        ++passCount; break;
                case Status::Fail:        ++failCount; break;
                case Status::PendingImpl: ++pendingCount; break;
            }
        }

        std::cerr << "[gke2e] Executed " << results.size()
                  << " tests: PASS=" << passCount
                  << ", FAIL=" << failCount
                  << ", PENDING_IMPL=" << pendingCount << '\n';

        if (allowPending) {
            return (failCount == 0 && (passCount + pendingCount) > 0) ? 0 : 1;
        }
        return (failCount == 0 && pendingCount == 0 && passCount > 0) ? 0 : 1;
    }

private:
    std::vector<TestEntry> entries_;
};

namespace probes {

template <typename T>
concept HasIsGroup = requires(const T& x) { x.isGroup; };

template <typename T>
concept HasPositionCount = requires(const T& x) { x.positionCount; };

template <typename T>
concept HasPositions = requires(const T& x) { x.positions; };

template <typename T>
concept HasCharacter = requires(const T& x) { x.character; };

template <typename T>
concept HasDisplayName = requires(const T& x) { x.displayName; };

template <typename T>
concept HasShortName = requires(const T& x) { x.shortName; };

template <typename T>
concept HasFollowSlotIdx = requires(const T& x) { x.followSlotIndex; };

template <typename T>
concept HasFormationState = requires(const T& x) { x.formation; };

template <typename T>
concept HasSelectSlot = requires(T& x) { x.SelectSlot(0); };

template <typename T>
concept HasSwapSlots = requires(T& x) { x.SwapSlots(0, 1); };

} // namespace probes

} // namespace gke2e

#define GKE2E_CHECK(ctx, cond) \
    (void)(ctx).Check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define GKE2E_CHECK_MSG(ctx, cond, msg) \
    (void)(ctx).Check(static_cast<bool>(cond), #cond, __FILE__, __LINE__, (msg))

#define GKE2E_REQUIRE(ctx, cond)                                               \
    do {                                                                       \
        if (!(ctx).Check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)) { \
            return;                                                            \
        }                                                                      \
    } while (false)

#define GKE2E_REQUIRE_MSG(ctx, cond, msg)                                              \
    do {                                                                               \
        if (!(ctx).Check(static_cast<bool>(cond), #cond, __FILE__, __LINE__, (msg))) { \
            return;                                                                    \
        }                                                                              \
    } while (false)

#define GKE2E_CHECK_EQ(ctx, actual, expected) \
    (void)(ctx).CheckEq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define GKE2E_REQUIRE_EQ(ctx, actual, expected)                                              \
    do {                                                                                     \
        if (!(ctx).CheckEq((actual), (expected), #actual, #expected, __FILE__, __LINE__)) { \
            return;                                                                          \
        }                                                                                    \
    } while (false)

#define GKE2E_CHECK_NEAR(ctx, actual, expected, eps) \
    (void)(ctx).CheckNear(static_cast<double>(actual), static_cast<double>(expected), \
                          static_cast<double>(eps), #actual, #expected, __FILE__, __LINE__)

#define GKE2E_PENDING(ctx, reason)    \
    do {                              \
        (ctx).Pending((reason));      \
        return;                       \
    } while (false)

#define GKE2E_PASS(ctx, detail) \
    (ctx).Pass((detail))

#define GKE2E_REQUIRE_HEADER(ctx, flagMacro, headerName)                          \
    do {                                                                          \
        if (!(flagMacro)) {                                                       \
            (ctx).Pending(std::string("Missing header: ") + (headerName));        \
            return;                                                               \
        }                                                                         \
    } while (false)

#define GKE2E_WITH_MEMBER_PROBE(ctx, Concept, obj, reason, ...) \
    do {                                                        \
        using _GkProbeT = std::remove_cvref_t<decltype(obj)>;   \
        if constexpr (Concept<_GkProbeT>) {                     \
            [&](auto& probed) __VA_ARGS__(obj);                 \
        } else {                                                \
            (ctx).Pending((reason));                            \
            return;                                             \
        }                                                       \
    } while (false)

#define GK_E2E_DEFINE_LOG_SINK() \
    namespace GakuMotion::Log { void Write(Level, const char*, ...) {} }
