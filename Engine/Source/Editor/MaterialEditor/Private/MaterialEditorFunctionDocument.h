#pragma once

#include "CoreMinimal.h"
#include "FMaterialEditor.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Materials/MaterialFunction.h"
#include "MaterialFunctionPreview.h"

namespace Durin { class DTransactor; }

namespace Durin::MaterialEditor
{
	// One toolkit-owned document contains both its editable draft and derived presentation.
	struct FMaterialEditor::FFunctionDocument
	{
		TWeakObjectPtr<DMaterialFunction> Source;
		TStrongObjectPtr<DPackage> WorkingPackage;
		TStrongObjectPtr<DMaterialFunction> Working;
		TWeakObjectPtr<DTransactor> Transactor;
		std::pair<uint64, uint64> SourceRevision;
		bool bPublishing = false;
		FStrongObjectPtr PreviewPackage;
		FStrongObjectPtr PreviewMaterial;
		std::unique_ptr<FMaterialGraphCanvas> Canvas;
		std::shared_ptr<FMaterialPreview> Preview;
		FGuid Output;
		FMaterialFunctionPreviewInvalidation PreviewInvalidation;
		bool bPreviewValid = false;
		bool bResetLayout = false;
		bool bOutputPort = false;
		FGuid EditingPort;
		FGuid SelectedPortNode;
		FMaterialFunctionPort PortDraft;
		std::array<char, 129> PortName{};
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		~FFunctionDocument();
		auto GetSource() const -> DMaterialFunction* { return Source.Get(); }
		auto Function() const -> DMaterialFunction* { return Working.Get(); }
		auto Material() const -> DMaterial*;
		auto Initialize(DMaterialFunction& Original, DTransactor* Transactions, std::string& Error) -> bool;
		auto IsDirty() const -> bool;
		auto Publish(std::string& Error, bool bForce = false) -> bool;
		auto RetirePreview() -> void;
		auto RefreshSourceRevision() -> void;
	};
}
