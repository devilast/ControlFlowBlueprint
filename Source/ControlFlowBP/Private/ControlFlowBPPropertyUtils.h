#pragma once

#include "CoreMinimal.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace UE::ControlFlowBP::PropertyUtils
{
	inline FPropertyBagPropertyDesc DescribeValue(const FProperty& Property)
	{
		return FPropertyBagPropertyDesc(NAME_None, &Property);
	}

	inline FString DescribeType(const FProperty& Property)
	{
		const FProperty* Value = &Property;
		FString Prefix;
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
		{
			Value = ArrayProperty->Inner;
			Prefix = TEXT("Array of ");
		}
		else if (const FSetProperty* SetProperty = CastField<FSetProperty>(&Property))
		{
			Value = SetProperty->ElementProp;
			Prefix = TEXT("Set of ");
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Value))
		{
			return Prefix + GetNameSafe(ObjectProperty->PropertyClass);
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Value))
		{
			return Prefix + GetNameSafe(StructProperty->Struct);
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Value))
		{
			return Prefix + GetNameSafe(EnumProperty->GetEnum());
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Value); ByteProperty && ByteProperty->Enum)
		{
			return Prefix + GetNameSafe(ByteProperty->Enum);
		}
		return Prefix + Value->GetCPPType();
	}

	inline FString FormatValue(const FProperty& Property, const void* Value)
	{
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(&Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value);
			return Object ? Object->GetName() : FString(TEXT("None"));
		}

		FString Text;
		Property.ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);

		constexpr int32 MaxLength = 80;
		if (Text.Len() > MaxLength)
		{
			Text = Text.Left(MaxLength - 3) + TEXT("...");
		}
		return Text;
	}

	inline const FNumericProperty* GetEnumStorage(const FProperty& Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
		{
			return EnumProperty->GetUnderlyingProperty();
		}
		const FByteProperty* ByteProperty = CastField<FByteProperty>(&Property);
		return (ByteProperty && ByteProperty->Enum) ? ByteProperty : nullptr;
	}

	inline void CopyValue(const FProperty& DestProperty, void* Dest, const FProperty& SourceProperty, const void* Source)
	{
		if (DestProperty.GetClass() != SourceProperty.GetClass())
		{
			const FNumericProperty* DestEnum = GetEnumStorage(DestProperty);
			const FNumericProperty* SourceEnum = GetEnumStorage(SourceProperty);
			if (DestEnum && SourceEnum)
			{
				DestEnum->SetIntPropertyValue(Dest, SourceEnum->GetSignedIntPropertyValue(Source));
				return;
			}
		}

		DestProperty.CopyCompleteValue(Dest, Source);
	}

	inline bool CanRead(const FProperty& Stored, const void* StoredValue, const FProperty& Pin)
	{
		const FPropertyBagPropertyDesc StoredDesc = DescribeValue(Stored);
		const FPropertyBagPropertyDesc PinDesc = DescribeValue(Pin);
		if (PinDesc.ValueType == EPropertyBagPropertyType::None || StoredDesc.ContainerTypes != PinDesc.ContainerTypes)
		{
			return false;
		}

		if (StoredDesc.ValueType == EPropertyBagPropertyType::Object && PinDesc.ValueType == EPropertyBagPropertyType::Object
			&& StoredDesc.ContainerTypes.IsEmpty())
		{
			const FObjectPropertyBase* StoredObject = CastField<FObjectPropertyBase>(&Stored);
			const FObjectPropertyBase* PinObject = CastField<FObjectPropertyBase>(&Pin);
			const UObject* Value = StoredObject ? StoredObject->GetObjectPropertyValue(StoredValue) : nullptr;
			return PinObject && (!Value || Value->IsA(PinObject->PropertyClass));
		}

		return PinDesc.CompatibleType(StoredDesc);
	}
}
