#pragma once

#include "Asset/AssetCompatibilityAudit.h"

namespace Durin::Editor::MainFrame
{
	class FAssetCompatibilityWindow
	{
	public:
		using FRevealAsset = std::function<void(const FAssetPath&)>;

		auto Draw(bool& bOpen, const FRevealAsset& RevealAsset) -> void;
		auto ProjectChanged() -> void { Audit.ProjectChanged(); SelectedPath = {}; }

	private:
		auto DrawDetails(const FRevealAsset& RevealAsset) -> void;
		auto CopySelectedDiagnostics() const -> void;

		Editor::FAssetCompatibilityAuditModel Audit;
		FAssetPath SelectedPath;
		Editor::EAssetCompatibilityAuditFilter Filter = Editor::EAssetCompatibilityAuditFilter::All;
	};
}
