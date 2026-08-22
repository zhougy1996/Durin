#pragma once

#include "SkeletalMeshEditorAPI.h"

namespace Durin
{
	class DAnimationClip;
	class DSkeletalMesh;
}

namespace Durin::Editor::SkeletalMesh
{

	// Owns one isolated production skeletal component, preview world, viewport, and playback state.
	class SKELETALMESHEDITOR_API FSkeletalAssetPreview final
	{
	public:
		explicit FSkeletalAssetPreview(uint64 PreviewId);
		~FSkeletalAssetPreview();
		auto Draw(DSkeletalMesh* Mesh, DAnimationClip* Clip, float PanelHeight = 0.0f) -> void;
		auto SetVisible(bool bVisible) -> void;
		auto ResetView() -> void;
		auto SetWireframe(bool bWireframe) -> void;
		auto IsWireframe() const -> bool;
		auto SetLit(bool bLit) -> void;
		auto IsLit() const -> bool;
		auto Play(std::string& OutError) -> bool;
		auto Pause() -> void;
		auto ResetPlayback(std::string& OutError) -> bool;
		auto Seek(float TimeSeconds, std::string& OutError) -> bool;
		auto SetLooping(bool bLooping) -> void;
		auto SetPlayRate(float Rate, std::string& OutError) -> bool;
		auto IsPlaying() const -> bool;
		auto IsLooping() const -> bool;
		auto GetPlayRate() const -> float;
		auto GetPlaybackTime() const -> float;

	private:
		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
