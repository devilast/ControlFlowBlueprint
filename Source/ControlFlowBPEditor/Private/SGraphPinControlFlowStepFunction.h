#pragma once

#include "CoreMinimal.h"
#include "SGraphPin.h"

class IControlFlowFunctionPicker;
class SSearchableComboBox;

/**
 * A function pin of a node that picks one of this Blueprint's functions (IControlFlowFunctionPicker):
 * a dropdown of the ones that fit plus "[Create a matching ...]", and a button that opens the pick.
 */
class SGraphPinControlFlowStepFunction : public SGraphPin
{
public:
	SLATE_BEGIN_ARGS(SGraphPinControlFlowStepFunction) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UEdGraphPin* InGraphPinObj);

protected:
	virtual TSharedRef<SWidget> GetDefaultValueWidget() override;

private:
	IControlFlowFunctionPicker* GetPicker() const;
	FString GetCurrentValue() const;

	void RebuildOptions();
	TSharedRef<SWidget> MakeOptionWidget(TSharedPtr<FString> Option) const;
	void OnOptionSelected(TSharedPtr<FString> Option, ESelectInfo::Type SelectInfo);

	FText GetCurrentLabel() const;
	FSlateColor GetCurrentLabelColor() const;
	FText GetComboTooltip() const;

	EVisibility GetOpenButtonVisibility() const;
	FText GetOpenButtonTooltip() const;
	FReply OnOpenClicked();

	TArray<TSharedPtr<FString>> Options;
	TSharedPtr<FString> CreateOption;
	TSharedPtr<FString> NoneOption;
	TSharedPtr<FString> NoMatchOption;
	TArray<FName> CachedCandidates;
	mutable FString FitCheckedFor;
	mutable bool bValueFits = false;

	TSharedPtr<SSearchableComboBox> ComboBox;
};
