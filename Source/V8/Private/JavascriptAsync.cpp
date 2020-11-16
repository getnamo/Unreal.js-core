#include "JavascriptAsync.h"
#include "Async/Async.h"
#include "JavascriptInstance.h"
#include "JavascriptInstanceHandler.h"

int32 UJavascriptAsync::IdCounter = 0;
TSharedPtr<FJavascriptInstanceHandler> UJavascriptAsync::MainHandler = nullptr;

UJavascriptAsync::UJavascriptAsync(class FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
	MainHandler = FJavascriptInstanceHandler::GetMainHandler().Pin();
	IdCounter = 0;
}

UJavascriptAsync* UJavascriptAsync::StaticInstance(UObject* Owner)
{
	if (Owner == nullptr)
	{
		auto Instance = NewObject<UJavascriptAsync>();
		Instance->AddToRoot();
		return Instance;
	}
	else
	{
		return NewObject<UJavascriptAsync>(Owner);
	}
}

int32 UJavascriptAsync::RunScript(const FString& Script, EJavascriptAsyncOption ExecutionContext, bool bPinAfterRun)
{
	FJSInstanceOptions InstanceOptions;
	InstanceOptions.ThreadOption = ExecutionContext;
	const int32 LambdaId = ++IdCounter;

	const FString SafeScript = Script;
	FJavascriptInstanceHandler::GetMainHandler().Pin()->RequestInstance(InstanceOptions, [SafeScript, LambdaId, bPinAfterRun, this](TSharedPtr<FJavascriptInstance> NewInstance)
	{
		//Convert context
		const EAsyncExecution AsyncExecutionContext = FJavascriptAsyncUtil::ToAsyncExecution(NewInstance->Options.ThreadOption);

		//Create a message queue if we're going to be pinned
		if (bPinAfterRun) 
		{
			LambdaMapData.DataForId(LambdaId).bShouldPin = true;
		}

		FJavascriptAsyncLambdaPinData& PinData = LambdaMapData.DataForId(LambdaId);

		Async(AsyncExecutionContext, [NewInstance, SafeScript, LambdaId, bPinAfterRun, &PinData, this]()
		{
			FString ReturnValue = NewInstance->ContextSettings.Context->Public_RunScript(SafeScript);
			OnLambdaComplete.ExecuteIfBound(ReturnValue, LambdaId, 0);

			if (bPinAfterRun)
			{
				PinData.bIsPinned = true;

				//while bShouldRun => spin(), check if we got a message call, call it
				while (PinData.bShouldPin)
				{
					while (!PinData.MessageQueue->IsEmpty())
					{
						FJavascriptRemoteFunctionData MessageFunctionData;
						PinData.MessageQueue->Dequeue(MessageFunctionData);

						FString RemoteFunctionScript = FString::Printf(TEXT("%s(JSON.parse('%s'));"), *MessageFunctionData.Name, *MessageFunctionData.Args);
						FString MessageReturn = NewInstance->ContextSettings.Context->Public_RunScript(RemoteFunctionScript);

						int32 CallbackId = MessageFunctionData.CallbackId;
						//We call back on gamethread always
						Async(EAsyncExecution::TaskGraphMainThread, [this, MessageReturn, LambdaId, CallbackId] {
							OnMessage.ExecuteIfBound(MessageReturn, LambdaId, CallbackId);
						});
					}
					//1ms sleep
					FPlatformProcess::Sleep(0.001f);
					
				}

				PinData.bIsPinned = false;
			}
		});
	});

	return LambdaId;
}

void UJavascriptAsync::CallScriptFunction(int32 InLambdaId, const FString& FunctionName, const FString& Args, int32 CallbackId)
{
	FJavascriptRemoteFunctionData FunctionData;
	FunctionData.Name = FunctionName;
	FunctionData.Args = Args;
	FunctionData.CallbackId = CallbackId;

	//Queue up the function to be pulled on next poll by the pinned lambda
	LambdaMapData.DataForId(InLambdaId).MessageQueue->Enqueue(FunctionData);
}

void UJavascriptAsync::StopLambda(int32 InLambdaId)
{
	LambdaMapData.DataForId(InLambdaId).bShouldPin = false;
}

void UJavascriptAsync::BeginDestroy()
{
	//NB: this doesn't get called early enough... todo: cleanup all lambdas on game world exit
	TArray<int32> LambdaIds = LambdaMapData.LambdaIds();
	for (int32 LambdaId : LambdaIds)
	{
		StopLambda(LambdaId);
	}

	Super::BeginDestroy();
}