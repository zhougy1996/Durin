#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

#include "DObject/StrongObjectPtr.h"
#include "DObject/Archive.h"
#include "Materials/Material.h"

namespace Durin { class DTransactor; }

namespace Durin::MaterialEditor
{
	class DMaterialGraph;
	class FMaterialGraphCanvas;
	class FMaterialPreview;

	// Owns a transient base-material working copy. Publication keeps source identity
	// and refuses to overwrite an externally changed source package.
	class FMaterialEditingSession
	{
		friend class FMaterialEditorReloadParticipant;
	public:
		FMaterialEditingSession();
		~FMaterialEditingSession();
		auto Initialize(DMaterial& Source, EMaterialEditCompileMode Mode,
			std::string& Error, DTransactor* InTransactor = nullptr) -> bool;
		auto GetWorkingMaterial() const -> DMaterial* { return Working.Get(); }
		auto GetSourceMaterial() const -> DMaterial* { return Source.Get(); }
		auto HasUnappliedChanges() const -> bool;
		auto IsApplyPending() const -> bool { return bApplyPending; }
		// Starts preview compilation when necessary; Tick publishes only its current result.
		auto RequestApply(std::string& Error) -> bool;
		auto Tick(std::string& Error) -> void;
		auto CancelApply() -> void { bApplyPending = false; }
		// Save's selected-owner barrier; never drains unrelated compilation.
		auto FinishAndApply(std::string& Error) -> bool;
		auto MarkSaved() -> void;

		TStrongObjectPtr<DMaterialGraph> Graph;
		std::unique_ptr<FMaterialGraphCanvas> Canvas;
		std::shared_ptr<FMaterialPreview> Preview;

	private:
		struct FAuthoredState
		{
			std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
			std::vector<std::pair<FMaterialExpressionInput*, FGuid>> Connections;
			FMaterialStaticProperties Properties;
			EMaterialDomain Domain = EMaterialDomain::Surface;
			FMaterialGraphPresentation Presentation;
			std::pair<int32, int32> RootPosition;
			FMaterialOutputBindings RootBindings;
			auto Matches(const DMaterial& Material, bool bIncludeLayout = true) const -> bool;
		};
		static auto Capture(const DMaterial& Material, FAuthoredState& OutState) -> std::expected<void, FObjectGraphError>;
		auto ApplyCurrent(std::string& Error) -> bool;
		auto CheckSource(std::string& Error) const -> bool;
		TStrongObjectPtr<DMaterial> Source;
		TStrongObjectPtr<DPackage> WorkingPackage;
		TStrongObjectPtr<DMaterial> Working;
		TObjectPtr<DTransactor> Transactor;
		FAuthoredState Applied;
		uint64 SourceRevision = 0;
		uint64 RequestedWorkingRevision = 0;
		mutable uint64 ObservedWorkingRevision = 0;
		mutable bool bHasChanges = false;
		bool bApplyPending = false;
	};
}
