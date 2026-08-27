#include "AssetForge/Builtins/Texture2DImportData.h"

#include "DObject/DObjectGlobals.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto ValidateState(
			const FTexture2DImportDataState& State,
			std::string& OutError) -> bool
		{
			if (State.SourceData.Sources.empty() && State.DecoderId.empty()
				&& State.DecoderVersion == 0)
			{
				OutError.clear();
				return true;
			}
			if (State.SchemaVersion != AssetImportDataSchemaVersion
				|| !State.SourceData.Validate(OutError)) return false;
			const FSourceFile* Source =
				State.SourceData.FindByRole("source");
			if (State.SourceData.Sources.size() != 1 || !Source
				|| State.DecoderId != "DurinImage" || State.DecoderVersion != 1)
			{
				OutError = "Texture2D import data requires exactly one source role and the supported decoder.";
				return false;
			}
			OutError.clear();
			return true;
		}
	}

	DTexture2DImportData::DTexture2DImportData(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DTexture2DImportData::SetState(
		FTexture2DImportDataState State, std::string& OutError) -> bool
	{
		State.SourceData.Normalize();
		if (!ValidateState(State, OutError)) return false;
		FAssetImportDataState BaseState = State;
		if (!ApplyState(std::move(BaseState), OutError)) return false;
		DecoderId = std::move(State.DecoderId);
		DecoderVersion = State.DecoderVersion;
		OutError.clear();
		return true;
	}

	auto DTexture2DImportData::GetTexture2DState() const
		-> FTexture2DImportDataState
	{
		FTexture2DImportDataState State;
		static_cast<FAssetImportDataState&>(State) = {
			.SchemaVersion = GetSchemaVersion(), .SourceData = GetSourceData()};
		State.DecoderId = DecoderId;
		State.DecoderVersion = DecoderVersion;
		return State;
	}

	auto DTexture2DImportData::Validate(std::string& OutError) const -> bool
	{
		return ValidateState(GetTexture2DState(), OutError);
	}

	auto DTexture2DImportData::CloneToOwner(
		DObject* Owner, FName Name, std::string& OutError) const
		-> DAssetImportData*
	{
		if (!Owner || Name.IsNone() || !Validate(OutError))
		{
			if (OutError.empty()) OutError = "Texture2D import-data clone requires a valid owner and name.";
			return nullptr;
		}
		auto* Clone = NewObject<DTexture2DImportData>(
			Owner, Name, EObjectConstructionPurpose::Duplication);
		return Clone && Clone->SetState(GetTexture2DState(), OutError) ? Clone : nullptr;
	}
}
