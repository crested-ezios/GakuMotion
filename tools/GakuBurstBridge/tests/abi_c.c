#include "GakuBurstBridge.h"
#include <stddef.h>
/* Compiling this file as C catches accidental C++ constructs in the ABI. */
int gbb_c_layout(void) {
    return sizeof(GbbQuaternion) == 16 && sizeof(GbbVector3) == 12 &&
        offsetof(GbbApi, configure) == 8 && sizeof(GbbApi) == 64 &&
        offsetof(GbbApiV3, configure) == 8 && sizeof(GbbApiV3) == 80 && sizeof(GbbContext) == 8;
}
