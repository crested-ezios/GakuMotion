#include "../src/GkEnsemble.hpp"
#include <cassert>
#include <limits>

int main()
{
	using GakuMotion::EnsembleAssignments;
	EnsembleAssignments group;
	assert(group.Set(0, 101, 7, "lead", 0.0f));
	assert(group.Set(1, 202, 7, "partner", 0.5f));
	assert(group.Set(2, 303, 7, "third", -0.25f));
	assert(!group.Set(3, 404, 7, "overflow", 0.0f));
	assert(!group.Set(2, 202, 7, "duplicate actor", 0.0f));
	assert(!group.Set(2, 404, 8, "stale roster", 0.0f));
	assert(!group.Set(2, 404, 7, "invalid offset", std::numeric_limits<float>::quiet_NaN()));
	assert(group.Get()[2].actorToken == 303 && group.Get()[2].motionKey == "third");
	assert(group.IsCurrent(7) && !group.IsCurrent(8));
	assert(EnsembleAssignments::SampleTime(2.0f, 0.0f, 10.0f) == 2.0f);
	assert(EnsembleAssignments::SampleTime(2.0f, 0.5f, 10.0f) == 1.5f);
	assert(EnsembleAssignments::SampleTime(2.0f, -0.25f, 10.0f) == 2.25f);
	assert(EnsembleAssignments::SampleTime(0.0f, 0.5f, 10.0f) == 0.0f);
	assert(EnsembleAssignments::SampleTime(20.0f, 0.5f, 10.0f) == 10.0f);
	group.Clear(1);
	assert(group.Get()[1].actorToken == 0 && group.IsCurrent(7));
	group.Reset();
	assert(!group.IsCurrent(7) && group.Epoch() == 0);
	assert(group.Set(0, 404, 8, "new scene", 0.0f));
}
