#include "SGraphPinControlFlowStepFunction.h"

#include "ControlFlowFunctionPicker.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "SSearchableComboBox.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SGraphPinControlFlowStepFunction"

void SGraphPinControlFlowStepFunction::Construct(const FArguments& InArgs, UEdGraphPin* InGraphPinObj)
{
	SGraphPin::Construct(SGraphPin::FArguments(), InGraphPinObj);
}

IControlFlowFunctionPicker* SGraphPinControlFlowStepFunction::GetPicker() const
{
	if (!GraphPinObj || GraphPinObj->bWasTrashed)
	{
		return nullptr;
	}

	return Cast<IControlFlowFunctionPicker>(GraphPinObj->GetOwningNodeUnchecked());
}

FString SGraphPinControlFlowStepFunction::GetCurrentValue() const
{
	if (!GraphPinObj || GraphPinObj->bWasTrashed)
	{
		return FString();
	}

	const FString Value = GraphPinObj->GetDefaultAsString();
	return Value == TEXT("None") ? FString() : Value;
}

TSharedRef<SWidget> SGraphPinControlFlowStepFunction::GetDefaultValueWidget()
{
	RebuildOptions();

	return SNew(SHorizontalBox)
		.Visibility(this, &SGraphPin::GetDefaultValueVisibility)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.MinDesiredWidth(120.f)
			.MaxDesiredWidth(400.f)
			[
				SAssignNew(ComboBox, SSearchableComboBox)
				.OptionsSource(&Options)
				.bAlwaysSelectItem(true)
				.OnComboBoxOpening(this, &SGraphPinControlFlowStepFunction::RebuildOptions)
				.OnSelectionChanged(this, &SGraphPinControlFlowStepFunction::OnOptionSelected)
				.OnGenerateWidget(this, &SGraphPinControlFlowStepFunction::MakeOptionWidget)
				.IsEnabled(this, &SGraphPin::GetDefaultValueIsEditable)
				.ToolTipText(this, &SGraphPinControlFlowStepFunction::GetComboTooltip)
				.ContentPadding(FMargin(2.f, 0.f))
				.Content()
				[
					SNew(STextBlock)
					.Text(this, &SGraphPinControlFlowStepFunction::GetCurrentLabel)
					.ColorAndOpacity(this, &SGraphPinControlFlowStepFunction::GetCurrentLabelColor)
					.Font(FAppStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))
				]
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), TEXT("SimpleButton"))
			.ContentPadding(FMargin(1.f, 0.f))
			.Visibility(this, &SGraphPinControlFlowStepFunction::GetOpenButtonVisibility)
			.ToolTipText(this, &SGraphPinControlFlowStepFunction::GetOpenButtonTooltip)
			.OnClicked(this, &SGraphPinControlFlowStepFunction::OnOpenClicked)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.Edit")))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

void SGraphPinControlFlowStepFunction::RebuildOptions()
{
	Options.Reset();
	CachedCandidates.Reset();
	CreateOption.Reset();
	NoneOption.Reset();
	NoMatchOption.Reset();

	if (const IControlFlowFunctionPicker* Picker = GetPicker())
	{
		const FText CreateLabel = Picker->GetCreateFunctionLabel(*GraphPinObj);
		if (!CreateLabel.IsEmpty())
		{
			CreateOption = MakeShared<FString>(CreateLabel.ToString());
			Options.Add(CreateOption);
		}

		if (Picker->IsFunctionOptional(*GraphPinObj))
		{
			NoneOption = MakeShared<FString>(LOCTEXT("NoneOption", "None").ToString());
			Options.Add(NoneOption);
		}

		CachedCandidates = Picker->GetPickableFunctions(*GraphPinObj);
		for (const FName Candidate : CachedCandidates)
		{
			Options.Add(MakeShared<FString>(Candidate.ToString()));
		}

		if (CachedCandidates.IsEmpty())
		{
			NoMatchOption = MakeShared<FString>(Picker->GetNoFunctionsLabel(*GraphPinObj).ToString());
			Options.Add(NoMatchOption);
		}

		FitCheckedFor = GetCurrentValue();
		bValueFits = CachedCandidates.Contains(FName(*FitCheckedFor));
	}

	if (ComboBox.IsValid())
	{
		ComboBox->RefreshOptions();
	}
}

TSharedRef<SWidget> SGraphPinControlFlowStepFunction::MakeOptionWidget(TSharedPtr<FString> Option) const
{
	const bool bIsSpecial = Option.IsValid() && (Option == CreateOption || Option == NoneOption || Option == NoMatchOption);
	const bool bIsHint = Option.IsValid() && Option == NoMatchOption;

	return SNew(STextBlock)
		.Text(FText::FromString(Option.IsValid() ? *Option : FString()))
		.Font(FAppStyle::GetFontStyle(bIsSpecial ? TEXT("PropertyWindow.ItalicFont") : TEXT("PropertyWindow.NormalFont")))
		.ColorAndOpacity(bIsHint ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground());
}

void SGraphPinControlFlowStepFunction::OnOptionSelected(TSharedPtr<FString> Option, ESelectInfo::Type SelectInfo)
{
	if (!Option.IsValid()
		|| SelectInfo == ESelectInfo::Direct
		|| SelectInfo == ESelectInfo::OnNavigation
		|| Option == NoMatchOption)
	{
		return;
	}

	IControlFlowFunctionPicker* Picker = GetPicker();
	if (!Picker)
	{
		return;
	}

	if (Option == CreateOption)
	{
		if (UObject* Created = Picker->CreateFunctionFor(*GraphPinObj))
		{
			FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Created);
		}
		return;
	}

	const FString NewValue = Option == NoneOption ? FString() : *Option;
	if (NewValue == GetCurrentValue())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("PickFunction", "Pick Function"));
	GraphPinObj->Modify();
	GraphPinObj->GetSchema()->TrySetDefaultValue(*GraphPinObj, NewValue);
}

FText SGraphPinControlFlowStepFunction::GetCurrentLabel() const
{
	const FString Value = GetCurrentValue();
	if (!Value.IsEmpty())
	{
		return FText::FromString(Value);
	}

	const IControlFlowFunctionPicker* Picker = GetPicker();
	if (!Picker)
	{
		return FText::GetEmpty();
	}

	return Picker->IsFunctionOptional(*GraphPinObj) ? LOCTEXT("NoneLabel", "None") : Picker->GetPickPrompt(*GraphPinObj);
}

FSlateColor SGraphPinControlFlowStepFunction::GetCurrentLabelColor() const
{
	const FString Value = GetCurrentValue();
	if (Value.IsEmpty())
	{
		return FSlateColor::UseSubduedForeground();
	}

	if (Value != FitCheckedFor)
	{
		FitCheckedFor = Value;
		const IControlFlowFunctionPicker* Picker = GetPicker();
		bValueFits = Picker && Picker->GetPickableFunctions(*GraphPinObj).Contains(FName(*Value));
	}

	return bValueFits ? FSlateColor::UseForeground() : FSlateColor(FLinearColor(1.f, 0.35f, 0.3f));
}

FText SGraphPinControlFlowStepFunction::GetComboTooltip() const
{
	const IControlFlowFunctionPicker* Picker = GetPicker();
	return Picker ? Picker->GetFunctionPinHint(*GraphPinObj) : FText::GetEmpty();
}

EVisibility SGraphPinControlFlowStepFunction::GetOpenButtonVisibility() const
{
	return GetCurrentValue().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SGraphPinControlFlowStepFunction::GetOpenButtonTooltip() const
{
	return FText::Format(LOCTEXT("OpenTooltip", "Open {0}"), FText::FromString(GetCurrentValue()));
}

FReply SGraphPinControlFlowStepFunction::OnOpenClicked()
{
	if (const IControlFlowFunctionPicker* Picker = GetPicker())
	{
		if (UObject* Definition = Picker->GetFunctionDefinition(*GraphPinObj))
		{
			FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Definition);
		}
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
