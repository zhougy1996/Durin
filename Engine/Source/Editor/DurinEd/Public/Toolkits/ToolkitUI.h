#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin
{
	struct FToolkitPanelSpawner
	{
		std::string Id;
		std::string Label;
		std::function<void()> Draw;
		bool bInitiallyOpen = true;
	};
	struct FToolkitCommand
	{
		std::string Id;
		std::string Label;
		std::function<void()> Execute;
		std::function<bool()> CanExecute;
		bool bMutating = true;
		bool bToolbar = false;
	};
	// Tools and modes attach UI to an existing toolkit, without another asset route.
	struct FToolkitUIExtension
	{
		std::vector<FToolkitPanelSpawner> Panels;
		std::vector<FToolkitCommand> Commands;
	};
	class FToolkitUIRegistration
	{
	public:
		FToolkitUIRegistration() = default;
		DURINED_API ~FToolkitUIRegistration();
		FToolkitUIRegistration(const FToolkitUIRegistration&) = delete;
		auto operator=(const FToolkitUIRegistration&) -> FToolkitUIRegistration& = delete;
		DURINED_API FToolkitUIRegistration(FToolkitUIRegistration&& Other) noexcept;
		DURINED_API auto operator=(FToolkitUIRegistration&& Other) noexcept -> FToolkitUIRegistration&;
		DURINED_API auto Reset() -> void;
		explicit operator bool() const { return static_cast<bool>(Release); }
	private:
		friend class FBaseAssetToolkit;
		std::function<void()> Release;
	};
	struct FToolkitUIState;
}
