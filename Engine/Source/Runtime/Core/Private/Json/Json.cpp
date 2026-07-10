#include "Json/Json.h"

#include "Misc/FileHelper.h"

#include "yyjson.h"

namespace Durin
{
	struct FJsonNodeAccess
	{
		static auto MakeView(void* InDocumentPtr, void* InValuePtr, bool bInIsMutable) -> FJsonNodeView
		{
			return FJsonNodeView::FromOpaque(InDocumentPtr, InValuePtr, bInIsMutable);
		}

		static auto MakeRef(
			void* InDocumentPtr,
			void* InValuePtr,
			void* InParentPtr,
			std::string InKey,
			size_t InIndex,
			bool bInIsRoot,
			EJsonNodeLink InLink
		) -> FJsonNodeRef
		{
			return FJsonNodeRef::FromOpaque(InDocumentPtr, InValuePtr, InParentPtr, std::move(InKey), InIndex, bInIsRoot, InLink);
		}
	};

	namespace
	{
		constexpr size_t InvalidJsonIndex = static_cast<size_t>(-1);

		auto MakeJsonDocument(void* InDocumentPtr) -> yyjson_doc*
		{
			return static_cast<yyjson_doc*>(InDocumentPtr);
		}

		auto MakeJsonValue(void* InValuePtr) -> yyjson_val*
		{
			return static_cast<yyjson_val*>(InValuePtr);
		}

		auto MakeMutableDocument(void* InDocumentPtr) -> yyjson_mut_doc*
		{
			return static_cast<yyjson_mut_doc*>(InDocumentPtr);
		}

		auto MakeMutableValue(void* InValuePtr) -> yyjson_mut_val*
		{
			return static_cast<yyjson_mut_val*>(InValuePtr);
		}

		auto ToNodeView(yyjson_doc* InDocument, yyjson_val* InValue) -> FJsonNodeView
		{
			return InValue != nullptr
				? FJsonNodeAccess::MakeView(InDocument, InValue, false)
				: FJsonNodeView{};
		}

		auto ToNodeView(yyjson_mut_doc* InDocument, yyjson_mut_val* InValue) -> FJsonNodeView
		{
			return InValue != nullptr
				? FJsonNodeAccess::MakeView(InDocument, InValue, true)
				: FJsonNodeView{};
		}

		auto ToNodeRef(
			yyjson_mut_doc* InDocument,
			yyjson_mut_val* InValue,
			yyjson_mut_val* InParent,
			std::string InKey,
			size_t InIndex,
			bool bInIsRoot,
			EJsonNodeLink InLink
		) -> FJsonNodeRef
		{
			return InValue != nullptr
				? FJsonNodeAccess::MakeRef(InDocument, InValue, InParent, std::move(InKey), InIndex, bInIsRoot, InLink)
				: FJsonNodeRef{};
		}

		auto PopulateParseError(FJsonParseError* OutError, const yyjson_read_err& ReadError, std::string_view JsonText) -> void
		{
			if (!OutError)
			{
				return;
			}

			OutError->Code = static_cast<int32>(ReadError.code);
			OutError->BytePosition = ReadError.pos;
			OutError->Message = ReadError.msg ? ReadError.msg : "";

			size_t Line = 0;
			size_t Column = 0;
			size_t Character = 0;
			if (yyjson_locate_pos(JsonText.data(), JsonText.size(), ReadError.pos, &Line, &Column, &Character))
			{
				OutError->Line = Line;
				OutError->Column = Column;
				OutError->Character = Character;
			}
		}

		auto IsObject(void* InValuePtr, bool bInIsMutable) -> bool
		{
			return bInIsMutable ? yyjson_mut_is_obj(MakeMutableValue(InValuePtr)) : yyjson_is_obj(MakeJsonValue(InValuePtr));
		}

		auto IsArray(void* InValuePtr, bool bInIsMutable) -> bool
		{
			return bInIsMutable ? yyjson_mut_is_arr(MakeMutableValue(InValuePtr)) : yyjson_is_arr(MakeJsonValue(InValuePtr));
		}

		auto MakeMutableNull(yyjson_mut_doc* InDocument) -> yyjson_mut_val*
		{
			return yyjson_mut_null(InDocument);
		}

		auto MakeMutableString(yyjson_mut_doc* InDocument, std::string_view InValue) -> yyjson_mut_val*
		{
			return yyjson_mut_strncpy(InDocument, InValue.data(), InValue.size());
		}

		auto MakeMutableBool(yyjson_mut_doc* InDocument, bool bInValue) -> yyjson_mut_val*
		{
			return yyjson_mut_bool(InDocument, bInValue);
		}

		auto MakeMutableInt(yyjson_mut_doc* InDocument, int64 InValue) -> yyjson_mut_val*
		{
			return yyjson_mut_sint(InDocument, InValue);
		}

		auto MakeMutableUInt(yyjson_mut_doc* InDocument, uint64 InValue) -> yyjson_mut_val*
		{
			return yyjson_mut_uint(InDocument, InValue);
		}

		auto MakeMutableDouble(yyjson_mut_doc* InDocument, double InValue) -> yyjson_mut_val*
		{
			return yyjson_mut_real(InDocument, InValue);
		}

		auto MakeMutableObject(yyjson_mut_doc* InDocument) -> yyjson_mut_val*
		{
			return yyjson_mut_obj(InDocument);
		}

		auto MakeMutableArray(yyjson_mut_doc* InDocument) -> yyjson_mut_val*
		{
			return yyjson_mut_arr(InDocument);
		}
	} // namespace

	struct FJsonDocument::FImpl
	{
		yyjson_doc* ImmutableDocument = nullptr;
		yyjson_mut_doc* MutableDocument = nullptr;

		~FImpl()
		{
			Reset();
		}

		auto Reset() -> void
		{
			if (ImmutableDocument)
			{
				yyjson_doc_free(ImmutableDocument);
				ImmutableDocument = nullptr;
			}
			if (MutableDocument)
			{
				yyjson_mut_doc_free(MutableDocument);
				MutableDocument = nullptr;
			}
		}

		auto IsValid() const -> bool
		{
			return ImmutableDocument != nullptr || MutableDocument != nullptr;
		}

		auto EnsureMutableDocument() -> yyjson_mut_doc*
		{
			if (MutableDocument)
			{
				return MutableDocument;
			}

			if (ImmutableDocument)
			{
				MutableDocument = yyjson_doc_mut_copy(ImmutableDocument, nullptr);
				return MutableDocument;
			}

			MutableDocument = yyjson_mut_doc_new(nullptr);
			if (!MutableDocument)
			{
				return nullptr;
			}

			yyjson_mut_val* Root = yyjson_mut_null(MutableDocument);
			yyjson_mut_doc_set_root(MutableDocument, Root);
			return MutableDocument;
		}
	};

	auto FJsonNodeView::IsValid() const -> bool
	{
		return ValuePtr != nullptr;
	}

	auto FJsonNodeView::IsNull() const -> bool
	{
		return bIsMutable ? yyjson_mut_is_null(MakeMutableValue(ValuePtr)) : yyjson_is_null(MakeJsonValue(ValuePtr));
	}

	auto FJsonNodeView::IsObject() const -> bool
	{
		return ::Durin::IsObject(ValuePtr, bIsMutable);
	}

	auto FJsonNodeView::IsArray() const -> bool
	{
		return ::Durin::IsArray(ValuePtr, bIsMutable);
	}

	auto FJsonNodeView::IsString() const -> bool
	{
		return bIsMutable ? yyjson_mut_is_str(MakeMutableValue(ValuePtr)) : yyjson_is_str(MakeJsonValue(ValuePtr));
	}

	auto FJsonNodeView::IsBool() const -> bool
	{
		return bIsMutable ? yyjson_mut_is_bool(MakeMutableValue(ValuePtr)) : yyjson_is_bool(MakeJsonValue(ValuePtr));
	}

	auto FJsonNodeView::IsInt() const -> bool
	{
		return bIsMutable ? yyjson_mut_is_int(MakeMutableValue(ValuePtr)) : yyjson_is_int(MakeJsonValue(ValuePtr));
	}

	auto FJsonNodeView::IsUInt() const -> bool
	{
		return bIsMutable ? yyjson_mut_is_uint(MakeMutableValue(ValuePtr)) : yyjson_is_uint(MakeJsonValue(ValuePtr));
	}

	auto FJsonNodeView::IsNumber() const -> bool
	{
		return bIsMutable ? yyjson_mut_is_num(MakeMutableValue(ValuePtr)) : yyjson_is_num(MakeJsonValue(ValuePtr));
	}

	auto FJsonNodeView::Num() const -> size_t
	{
		return bIsMutable ? yyjson_mut_get_len(MakeMutableValue(ValuePtr)) : yyjson_get_len(MakeJsonValue(ValuePtr));
	}

	auto FJsonNodeView::Contains(std::string_view InKey) const -> bool
	{
		return bIsMutable
			? (IsObject() && yyjson_mut_obj_getn(MakeMutableValue(ValuePtr), InKey.data(), InKey.size()) != nullptr)
			: (IsObject() && yyjson_obj_getn(MakeJsonValue(ValuePtr), InKey.data(), InKey.size()) != nullptr);
	}

	auto FJsonNodeView::GetView(std::string_view InKey) const -> FJsonNodeView
	{
		return bIsMutable
			? ToNodeView(MakeMutableDocument(DocumentPtr), yyjson_mut_obj_getn(MakeMutableValue(ValuePtr), InKey.data(), InKey.size()))
			: ToNodeView(MakeJsonDocument(DocumentPtr), yyjson_obj_getn(MakeJsonValue(ValuePtr), InKey.data(), InKey.size()));
	}

	auto FJsonNodeView::GetView(size_t Index) const -> FJsonNodeView
	{
		return bIsMutable
			? ToNodeView(MakeMutableDocument(DocumentPtr), yyjson_mut_arr_get(MakeMutableValue(ValuePtr), Index))
			: ToNodeView(MakeJsonDocument(DocumentPtr), yyjson_arr_get(MakeJsonValue(ValuePtr), Index));
	}

	auto FJsonNodeView::ForEachObjectMember(const std::function<void(std::string_view, FJsonNodeView)>& Visitor) const -> void
	{
		if (!IsObject()) return;
		if (bIsMutable)
		{
			yyjson_mut_obj_iter Iter = yyjson_mut_obj_iter_with(static_cast<yyjson_mut_val*>(ValuePtr));
			yyjson_mut_val* Key = nullptr;
			while ((Key = yyjson_mut_obj_iter_next(&Iter)) != nullptr)
			{
				yyjson_mut_val* Value = yyjson_mut_obj_iter_get_val(Key);
				Visitor(std::string_view(yyjson_mut_get_str(Key), yyjson_mut_get_len(Key)), ToNodeView(static_cast<yyjson_mut_doc*>(DocumentPtr), Value));
			}
			return;
		}

		yyjson_obj_iter Iter = yyjson_obj_iter_with(static_cast<yyjson_val*>(ValuePtr));
		yyjson_val* Key = nullptr;
		while ((Key = yyjson_obj_iter_next(&Iter)) != nullptr)
		{
			yyjson_val* Value = yyjson_obj_iter_get_val(Key);
			Visitor(std::string_view(yyjson_get_str(Key), yyjson_get_len(Key)), ToNodeView(static_cast<yyjson_doc*>(DocumentPtr), Value));
		}
	}

	auto FJsonNodeView::GetString(std::string DefaultValue) const -> std::string
	{
		const char* Value = bIsMutable
			? yyjson_mut_get_str(MakeMutableValue(ValuePtr))
			: yyjson_get_str(MakeJsonValue(ValuePtr));
		return Value ? std::string(Value) : DefaultValue;
	}

	auto FJsonNodeView::GetBool(bool DefaultValue) const -> bool
	{
		if (IsBool())
		{
			return bIsMutable ? yyjson_mut_get_bool(MakeMutableValue(ValuePtr)) : yyjson_get_bool(MakeJsonValue(ValuePtr));
		}
		return DefaultValue;
	}

	auto FJsonNodeView::GetInt(int64 DefaultValue) const -> int64
	{
		if (IsInt())
		{
			return bIsMutable ? yyjson_mut_get_sint(MakeMutableValue(ValuePtr)) : yyjson_get_sint(MakeJsonValue(ValuePtr));
		}
		return DefaultValue;
	}

	auto FJsonNodeView::GetUInt(uint64 DefaultValue) const -> uint64
	{
		if (IsUInt())
		{
			return bIsMutable ? yyjson_mut_get_uint(MakeMutableValue(ValuePtr)) : yyjson_get_uint(MakeJsonValue(ValuePtr));
		}
		return DefaultValue;
	}

	auto FJsonNodeView::GetDouble(double DefaultValue) const -> double
	{
		if (IsNumber())
		{
			return bIsMutable ? yyjson_mut_get_num(MakeMutableValue(ValuePtr)) : yyjson_get_num(MakeJsonValue(ValuePtr));
		}
		return DefaultValue;
	}

	auto FJsonNodeView::GetValue(std::string& OutValue) const -> bool
	{
		if (!IsString())
		{
			return false;
		}

		OutValue = GetString();
		return true;
	}

	auto FJsonNodeView::GetValue(bool& bOutValue) const -> bool
	{
		if (!IsBool())
		{
			return false;
		}

		bOutValue = GetBool();
		return true;
	}

	auto FJsonNodeView::GetValue(int64& OutValue) const -> bool
	{
		if (!IsInt())
		{
			return false;
		}

		OutValue = GetInt();
		return true;
	}

	auto FJsonNodeView::GetValue(uint64& OutValue) const -> bool
	{
		if (!IsUInt())
		{
			return false;
		}

		OutValue = GetUInt();
		return true;
	}

	auto FJsonNodeView::GetValue(double& OutValue) const -> bool
	{
		if (!IsNumber())
		{
			return false;
		}

		OutValue = GetDouble();
		return true;
	}

	auto FJsonNodeView::GetChildValue(std::string_view InKey, std::string& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FJsonNodeView::GetChildValue(std::string_view InKey, bool& bOutValue) const -> bool
	{
		return GetView(InKey).GetValue(bOutValue);
	}

	auto FJsonNodeView::GetChildValue(std::string_view InKey, int64& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FJsonNodeView::GetChildValue(std::string_view InKey, uint64& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FJsonNodeView::GetChildValue(std::string_view InKey, double& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FJsonNodeRef::ReplaceWith(void* InNewValuePtr) -> void*
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		yyjson_mut_val* NewValue = MakeMutableValue(InNewValuePtr);
		if (!Document || !NewValue)
		{
			return nullptr;
		}

		if (bIsRoot)
		{
			yyjson_mut_doc_set_root(Document, NewValue);
			ValuePtr = NewValue;
			return NewValue;
		}

		yyjson_mut_val* Parent = MakeMutableValue(ParentPtr);
		if (!Parent)
		{
			return nullptr;
		}

		switch (Link)
		{
		case EJsonNodeLink::ObjectKey:
		{
			yyjson_mut_val* KeyValue = yyjson_mut_strncpy(Document, Key.data(), Key.size());
			if (!KeyValue || !yyjson_mut_obj_put(Parent, KeyValue, NewValue))
			{
				return nullptr;
			}
			ValuePtr = yyjson_mut_obj_getn(Parent, Key.data(), Key.size());
			return ValuePtr;
		}
		case EJsonNodeLink::ArrayIndex:
			if (!yyjson_mut_arr_replace(Parent, Index, NewValue))
			{
				return nullptr;
			}
			ValuePtr = yyjson_mut_arr_get(Parent, Index);
			return ValuePtr;
		default:
			return nullptr;
		}
	}

	auto FJsonNodeRef::SetObjectEntryInternal(std::string_view InKey, void* InValuePtr) -> void*
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		yyjson_mut_val* ObjectValue = MakeMutableValue(ValuePtr);
		yyjson_mut_val* NewValue = MakeMutableValue(InValuePtr);
		if (!Document || !ObjectValue || !yyjson_mut_is_obj(ObjectValue) || !NewValue)
		{
			return nullptr;
		}

		yyjson_mut_val* KeyValue = yyjson_mut_strncpy(Document, InKey.data(), InKey.size());
		if (!KeyValue || !yyjson_mut_obj_put(ObjectValue, KeyValue, NewValue))
		{
			return nullptr;
		}

		return yyjson_mut_obj_getn(ObjectValue, InKey.data(), InKey.size());
	}

	auto FJsonNodeRef::AppendArrayEntryInternal(void* InValuePtr, size_t* OutIndex) -> void*
	{
		yyjson_mut_val* ArrayValue = MakeMutableValue(ValuePtr);
		yyjson_mut_val* NewValue = MakeMutableValue(InValuePtr);
		if (!ArrayValue || !yyjson_mut_is_arr(ArrayValue) || !NewValue)
		{
			return nullptr;
		}

		const size_t EntryIndex = yyjson_mut_get_len(ArrayValue);
		if (!yyjson_mut_arr_append(ArrayValue, NewValue))
		{
			return nullptr;
		}

		if (OutIndex)
		{
			*OutIndex = EntryIndex;
		}

		return NewValue;
	}

	auto FJsonNodeRef::GetRef(std::string_view InKey) const -> FJsonNodeRef
	{
		if (!bIsMutable)
		{
			return {};
		}

		yyjson_mut_val* Node = MakeMutableValue(ValuePtr);
		if (!Node || !yyjson_mut_is_obj(Node))
		{
			return {};
		}

		return ToNodeRef(MakeMutableDocument(DocumentPtr), yyjson_mut_obj_getn(Node, InKey.data(), InKey.size()), Node, std::string(InKey), InvalidJsonIndex, false, EJsonNodeLink::ObjectKey);
	}

	auto FJsonNodeRef::GetRef(size_t InIndex) const -> FJsonNodeRef
	{
		if (!bIsMutable)
		{
			return {};
		}

		yyjson_mut_val* Node = MakeMutableValue(ValuePtr);
		if (!Node || !yyjson_mut_is_arr(Node))
		{
			return {};
		}

		return ToNodeRef(MakeMutableDocument(DocumentPtr), yyjson_mut_arr_get(Node, InIndex), Node, {}, InIndex, false, EJsonNodeLink::ArrayIndex);
	}

	auto FJsonNodeRef::EnsureObject() -> FJsonNodeRef&
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return *this;
		}

		if (!ValuePtr)
		{
			if (bIsRoot)
			{
				ReplaceWith(MakeMutableObject(Document));
			}
			return *this;
		}

		if (!yyjson_mut_is_obj(MakeMutableValue(ValuePtr)))
		{
			yyjson_mut_set_obj(MakeMutableValue(ValuePtr));
		}
		return *this;
	}

	auto FJsonNodeRef::EnsureArray() -> FJsonNodeRef&
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return *this;
		}

		if (!ValuePtr)
		{
			if (bIsRoot)
			{
				ReplaceWith(MakeMutableArray(Document));
			}
			return *this;
		}

		if (!yyjson_mut_is_arr(MakeMutableValue(ValuePtr)))
		{
			yyjson_mut_set_arr(MakeMutableValue(ValuePtr));
		}
		return *this;
	}

	auto FJsonNodeRef::SetValue(std::nullptr_t) -> void
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return;
		}

		if (!ValuePtr)
		{
			if (bIsRoot)
			{
				ReplaceWith(MakeMutableNull(Document));
			}
			return;
		}

		yyjson_mut_set_null(MakeMutableValue(ValuePtr));
	}

	auto FJsonNodeRef::SetValue(std::string_view InValue) -> void
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return;
		}

		yyjson_mut_val* NewValue = MakeMutableString(Document, InValue);
		if (!NewValue)
		{
			return;
		}

		if (!ValuePtr && !bIsRoot)
		{
			return;
		}

		ReplaceWith(NewValue);
	}

	auto FJsonNodeRef::SetValue(const char* InValue) -> void
	{
		SetValue(InValue != nullptr ? std::string_view(InValue) : std::string_view{});
	}

	auto FJsonNodeRef::SetValue(bool bInValue) -> void
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return;
		}

		if (!ValuePtr)
		{
			if (bIsRoot)
			{
				ReplaceWith(MakeMutableBool(Document, bInValue));
			}
			return;
		}

		yyjson_mut_set_bool(MakeMutableValue(ValuePtr), bInValue);
	}

	auto FJsonNodeRef::SetValue(int32 InValue) -> void
	{
		SetValue(static_cast<int64>(InValue));
	}

	auto FJsonNodeRef::SetValue(int64 InValue) -> void
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return;
		}

		if (!ValuePtr)
		{
			if (bIsRoot)
			{
				ReplaceWith(MakeMutableInt(Document, InValue));
			}
			return;
		}

		yyjson_mut_set_sint(MakeMutableValue(ValuePtr), InValue);
	}

	auto FJsonNodeRef::SetValue(uint32 InValue) -> void
	{
		SetValue(static_cast<uint64>(InValue));
	}

	auto FJsonNodeRef::SetValue(uint64 InValue) -> void
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return;
		}

		if (!ValuePtr)
		{
			if (bIsRoot)
			{
				ReplaceWith(MakeMutableUInt(Document, InValue));
			}
			return;
		}

		yyjson_mut_set_uint(MakeMutableValue(ValuePtr), InValue);
	}

	auto FJsonNodeRef::SetValue(double InValue) -> void
	{
		yyjson_mut_doc* Document = MakeMutableDocument(DocumentPtr);
		if (!Document)
		{
			return;
		}

		if (!ValuePtr)
		{
			if (bIsRoot)
			{
				ReplaceWith(MakeMutableDouble(Document, InValue));
			}
			return;
		}

		yyjson_mut_set_real(MakeMutableValue(ValuePtr), InValue);
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, std::nullptr_t) -> void
	{
		EnsureObject();
		SetObjectEntryInternal(InKey, MakeMutableNull(MakeMutableDocument(DocumentPtr)));
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, std::string_view InValue) -> void
	{
		EnsureObject();
		SetObjectEntryInternal(InKey, MakeMutableString(MakeMutableDocument(DocumentPtr), InValue));
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, const char* InValue) -> void
	{
		SetChildValue(InKey, InValue != nullptr ? std::string_view(InValue) : std::string_view{});
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, bool bInValue) -> void
	{
		EnsureObject();
		SetObjectEntryInternal(InKey, MakeMutableBool(MakeMutableDocument(DocumentPtr), bInValue));
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, int32 InValue) -> void
	{
		SetChildValue(InKey, static_cast<int64>(InValue));
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, int64 InValue) -> void
	{
		EnsureObject();
		SetObjectEntryInternal(InKey, MakeMutableInt(MakeMutableDocument(DocumentPtr), InValue));
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, uint32 InValue) -> void
	{
		SetChildValue(InKey, static_cast<uint64>(InValue));
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, uint64 InValue) -> void
	{
		EnsureObject();
		SetObjectEntryInternal(InKey, MakeMutableUInt(MakeMutableDocument(DocumentPtr), InValue));
	}

	auto FJsonNodeRef::SetChildValue(std::string_view InKey, double InValue) -> void
	{
		EnsureObject();
		SetObjectEntryInternal(InKey, MakeMutableDouble(MakeMutableDocument(DocumentPtr), InValue));
	}

	auto FJsonNodeRef::AddObject(std::string_view InKey) -> FJsonNodeRef
	{
		EnsureObject();
		yyjson_mut_val* Value = MakeMutableValue(SetObjectEntryInternal(InKey, MakeMutableObject(MakeMutableDocument(DocumentPtr))));
		return ToNodeRef(MakeMutableDocument(DocumentPtr), Value, MakeMutableValue(ValuePtr), std::string(InKey), InvalidJsonIndex, false, EJsonNodeLink::ObjectKey);
	}

	auto FJsonNodeRef::AddArray(std::string_view InKey) -> FJsonNodeRef
	{
		EnsureObject();
		yyjson_mut_val* Value = MakeMutableValue(SetObjectEntryInternal(InKey, MakeMutableArray(MakeMutableDocument(DocumentPtr))));
		return ToNodeRef(MakeMutableDocument(DocumentPtr), Value, MakeMutableValue(ValuePtr), std::string(InKey), InvalidJsonIndex, false, EJsonNodeLink::ObjectKey);
	}

	auto FJsonNodeRef::AppendValue(std::nullptr_t) -> FJsonNodeRef&
	{
		EnsureArray();
		AppendArrayEntryInternal(MakeMutableNull(MakeMutableDocument(DocumentPtr)));
		return *this;
	}

	auto FJsonNodeRef::AppendValue(std::string_view InValue) -> FJsonNodeRef&
	{
		EnsureArray();
		AppendArrayEntryInternal(MakeMutableString(MakeMutableDocument(DocumentPtr), InValue));
		return *this;
	}

	auto FJsonNodeRef::AppendValue(const char* InValue) -> FJsonNodeRef&
	{
		return AppendValue(InValue != nullptr ? std::string_view(InValue) : std::string_view{});
	}

	auto FJsonNodeRef::AppendValue(bool bInValue) -> FJsonNodeRef&
	{
		EnsureArray();
		AppendArrayEntryInternal(MakeMutableBool(MakeMutableDocument(DocumentPtr), bInValue));
		return *this;
	}

	auto FJsonNodeRef::AppendValue(int32 InValue) -> FJsonNodeRef&
	{
		return AppendValue(static_cast<int64>(InValue));
	}

	auto FJsonNodeRef::AppendValue(int64 InValue) -> FJsonNodeRef&
	{
		EnsureArray();
		AppendArrayEntryInternal(MakeMutableInt(MakeMutableDocument(DocumentPtr), InValue));
		return *this;
	}

	auto FJsonNodeRef::AppendValue(uint32 InValue) -> FJsonNodeRef&
	{
		return AppendValue(static_cast<uint64>(InValue));
	}

	auto FJsonNodeRef::AppendValue(uint64 InValue) -> FJsonNodeRef&
	{
		EnsureArray();
		AppendArrayEntryInternal(MakeMutableUInt(MakeMutableDocument(DocumentPtr), InValue));
		return *this;
	}

	auto FJsonNodeRef::AppendValue(double InValue) -> FJsonNodeRef&
	{
		EnsureArray();
		AppendArrayEntryInternal(MakeMutableDouble(MakeMutableDocument(DocumentPtr), InValue));
		return *this;
	}

	auto FJsonNodeRef::AppendObject() -> FJsonNodeRef
	{
		EnsureArray();
		size_t Index = InvalidJsonIndex;
		yyjson_mut_val* Value = MakeMutableValue(AppendArrayEntryInternal(MakeMutableObject(MakeMutableDocument(DocumentPtr)), &Index));
		return ToNodeRef(MakeMutableDocument(DocumentPtr), Value, MakeMutableValue(ValuePtr), {}, Index, false, EJsonNodeLink::ArrayIndex);
	}

	auto FJsonNodeRef::AppendArray() -> FJsonNodeRef
	{
		EnsureArray();
		size_t Index = InvalidJsonIndex;
		yyjson_mut_val* Value = MakeMutableValue(AppendArrayEntryInternal(MakeMutableArray(MakeMutableDocument(DocumentPtr)), &Index));
		return ToNodeRef(MakeMutableDocument(DocumentPtr), Value, MakeMutableValue(ValuePtr), {}, Index, false, EJsonNodeLink::ArrayIndex);
	}

	FJsonDocument::FJsonDocument()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FJsonDocument::~FJsonDocument() = default;

	FJsonDocument::FJsonDocument(FJsonDocument&& Other) noexcept = default;

	auto FJsonDocument::operator=(FJsonDocument&& Other) noexcept -> FJsonDocument& = default;

	auto FJsonDocument::Parse(std::string_view JsonText, FJsonParseError* OutError) -> bool
	{
		Reset();

		if (OutError)
		{
			*OutError = {};
		}

		yyjson_read_err ReadError{};
		Impl->ImmutableDocument = yyjson_read_opts(const_cast<char*>(JsonText.data()), JsonText.size(), YYJSON_READ_NOFLAG, nullptr, &ReadError);
		if (!Impl->ImmutableDocument)
		{
			PopulateParseError(OutError, ReadError, JsonText);
			return false;
		}

		return true;
	}

	auto FJsonDocument::LoadFromFile(std::string_view FileName, FJsonParseError* OutError) -> bool
	{
		std::string JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, FileName))
		{
			if (OutError)
			{
				*OutError = {};
				OutError->Message = std::format("Failed to load JSON file: {}", FileName);
			}
			return false;
		}

		return Parse(JsonText, OutError);
	}

	auto FJsonDocument::Reset() -> void
	{
		Impl->Reset();
	}

	auto FJsonDocument::IsValid() const -> bool
	{
		return Impl->IsValid();
	}

	auto FJsonDocument::GetRootView() const -> FJsonNodeView
	{
		if (Impl->MutableDocument)
		{
			return ToNodeView(Impl->MutableDocument, yyjson_mut_doc_get_root(Impl->MutableDocument));
		}

		if (Impl->ImmutableDocument)
		{
			return ToNodeView(Impl->ImmutableDocument, yyjson_doc_get_root(Impl->ImmutableDocument));
		}

		return {};
	}

	auto FJsonDocument::GetMutableRoot() -> FJsonNodeRef
	{
		yyjson_mut_doc* Document = Impl->EnsureMutableDocument();
		if (!Document)
		{
			return {};
		}

		yyjson_mut_val* Root = yyjson_mut_doc_get_root(Document);
		if (!Root)
		{
			Root = yyjson_mut_null(Document);
			yyjson_mut_doc_set_root(Document, Root);
		}

		return ToNodeRef(Document, Root, nullptr, {}, InvalidJsonIndex, true, EJsonNodeLink::None);
	}

	auto FJsonDocument::ToString() const -> std::string
	{
		yyjson_mut_doc* Document = const_cast<FImpl*>(Impl.get())->EnsureMutableDocument();
		if (!Document)
		{
			return {};
		}

		const yyjson_write_flag Flags = YYJSON_WRITE_PRETTY | YYJSON_WRITE_ESCAPE_UNICODE;
		size_t Length = 0;
		char* Raw = yyjson_mut_write_opts(Document, Flags, nullptr, &Length, nullptr);
		if (!Raw)
		{
			return {};
		}

		std::string Result(Raw, Length);
		std::free(Raw);
		return Result;
	}

	auto FJsonDocument::SaveToFile(std::string_view FilePath) const -> bool
	{
		yyjson_mut_doc* Document = const_cast<FImpl*>(Impl.get())->EnsureMutableDocument();
		if (!Document)
		{
			return false;
		}

		const yyjson_write_flag Flags = YYJSON_WRITE_PRETTY | YYJSON_WRITE_ESCAPE_UNICODE;
		return yyjson_mut_write_file(std::string(FilePath).c_str(), Document, Flags, nullptr, nullptr);
	}
}
