#pragma once

namespace Durin
{
	class DSkeletalMesh;
	class DAnimationClip;

	// Owner-thread application of a detached, lazily acquired canonical payload.
	auto PrepareSkeletalMeshPayload(DSkeletalMesh& Mesh, std::string& OutError) -> bool;
	auto PrepareAnimationClipPayload(DAnimationClip& Clip, std::string& OutError) -> bool;
}
