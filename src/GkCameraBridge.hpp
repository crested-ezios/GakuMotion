#pragma once
#include "GkLog.hpp"

#include "minhook/include/MinHook.h"
#include "GakuLocalifyCameraWrite.h"
#include "GakuVrifyCameraBegin.h"

// Included inside the plugin's anonymous namespace, after IL2CPP resolution helpers.
// No game ABI offsets are used. All methods/icalls are resolved on Unity's thread.
#include "camera/GkCameraBridgeTypes.hpp"
#include "camera/GkCameraBridgeFinder.hpp"
#include "camera/GkCameraBridgeApply.hpp"
#include "camera/GkCameraBridgeHooks.hpp"
#include "camera/GkCameraBridgeInstall.hpp"
