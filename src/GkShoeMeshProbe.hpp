#pragma once

// Included in GakuMotionPlugin.cpp's anonymous namespace after the IL2CPP helpers.
// This is a one-shot evidence collector, not a contact solver. It never changes
// the actor, its bones, renderer settings, sharedMesh, or measurement profiles.
#include "shoe/GkShoeMeshTypes.hpp"
#include "shoe/GkShoeMeshApi.hpp"
#include "shoe/GkShoeMeshFrames.hpp"
#include "shoe/GkShoeMeshExtract.hpp"
#include "shoe/GkShoeMeshEval.hpp"
