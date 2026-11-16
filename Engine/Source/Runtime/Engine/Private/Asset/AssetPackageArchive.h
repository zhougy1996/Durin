#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "EngineAPI.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/PackageResource.h"
#include "DObject/Archive.h"
#include "DObject/PackageLinker.h"
#include "DObject/PackageValueCodec.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::AssetPrivate
{
	namespace LinkerSchema { class FLoadSchemaBindings; }
	struct FAuthoredPackageSummary
	{
		std::vector<FPackagePath> Dependencies;
	};

	// Per-package sources shared by all object reads, for authored or cooked data.
	// The loader owns dependency admission/lifetime and keeps these bindings alive
	// during each read. Missing bindings fail instead of falling back to live state.
	struct FPackageLoadBindings
	{
		FPackageResourceHandle BulkResource;
		std::function<std::expected<void, Durin::UnifiedError::FError>(const FSoftObjectPath&, DObject*&)> ResolveExternalObject;
		LinkerSchema::FLoadSchemaBindings* SchemaBindings = nullptr;
	};
	class FPackageLoadContext;
	using FPackageOverrideRestore = std::function<void(FPackageLoadContext&, std::unique_ptr<FOverriddenPropertyRestoreContext>)>;

	// One package application owns its first failure, including nested field reads.
	// Contexts are local to the operation, never shared through global loader state.
	class FPackageLoadContext
	{
	public:
		auto IsError() const -> bool { return Failure.has_value(); }
		auto GetError() const -> const UnifiedError::FError& { require(Failure); return *Failure; }
		ENGINE_API auto Fail(UnifiedError::FError Error, std::string_view ObjectPath = {}) -> void;

		// Applies fields only; graph construction, PostLoad and rollback belong to
		// the loader. Failed contexts do no further work. Serializer writes are in-place.
		// OnValidatedValues consumes restoration evidence while the archive is alive;
		// escaped evidence expires. Callback failures use this same context.
		ENGINE_API auto SerializeExport(
			DObject& Object,
			std::span<const ObjectPackage::FPropertyTag> Fields,
			const ObjectPackage::FLinkerTables& Linker,
			std::span<DObject* const> Objects,
			const FPackageLoadBindings& Bindings,
			const FArchiveState& Context = {},
			const FPackageOverrideRestore& OnValidatedValues = {}) -> void;

	private:
		std::optional<UnifiedError::FError> Failure;
		DObject* CurrentObject = nullptr;
	};

	// Owner-thread dispatch through the object's temporary loader/export binding.
	// Detached/native objects have no pending work; this never discovers packages.
	auto PreloadPackageObject(FPackageLoadContext& Load, DObject* Object) -> void;

	// Byte-tool restoration may resolve external object references. It is a load
	// operation; construct-free inspection only calls it for explicit typed reads.
	auto DecodeReferenceByteToolValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		PackagePrivate::FByteReader& Reader,
		const std::vector<DObject*>& Objects,
		uint32 SourceVersion) -> std::expected<void, Durin::UnifiedError::FError>;
}
