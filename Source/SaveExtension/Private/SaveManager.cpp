// Copyright 2015-2018 Piperift. All Rights Reserved.

#include "SaveManager.h"

#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/GameModeBase.h"
#include "HighResScreenshot.h"
#include "Misc/Paths.h"

#include <GameDelegates.h>
#include <Misc/CoreDelegates.h>

#include "FileAdapter.h"
#include "Multithreading/LoadSlotInfoTask.h"
#include "LatentActions/LoadInfosAction.h"


USaveManager::USaveManager()
	: Super()
{}

void USaveManager::Init()
{
	FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &USaveManager::OnMapLoadStarted);
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &USaveManager::OnMapLoadFinished);
	FGameDelegates::Get().GetEndPlayMapDelegate().AddUObject(this, &USaveManager::Shutdown);

	//AutoLoad
	if (GetPreset()->bAutoLoad)
		ReloadCurrentSlot();

	TryInstantiateInfo();
	UpdateLevelStreamings();

	AddToRoot();
}

void USaveManager::Shutdown()
{
	if (GetPreset()->bSaveOnExit)
		SaveCurrentSlot();

	FCoreUObjectDelegates::PreLoadMap.RemoveAll(this);
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	FGameDelegates::Get().GetEndPlayMapDelegate().RemoveAll(this);

	// Destroy
	RemoveFromRoot();
	MarkPendingKill();
}

bool USaveManager::SaveSlot(int32 SlotId, bool bOverrideIfNeeded, bool bScreenshot, const FScreenshotSize Size, FOnGameSaved OnSaved)
{
	if (!CanLoadOrSave())
		return false;

	const USavePreset* Preset = GetPreset();
	if (!IsValidSlot(SlotId))
	{
		SELog(Preset, "Invalid Slot. Cant go under 0 or exceed MaxSlots.", true);
		return false;
	}

	//Saving
	SELog(Preset, "Saving to Slot " + FString::FromInt(SlotId));

	UWorld* World = GetWorld();
	check(World);

	//Launch task, always fail if it didn't finish or wasn't scheduled
	auto* Task = CreateTask<USlotDataTask_Saver>()
		->Setup(SlotId, bOverrideIfNeeded, bScreenshot, Size.Width, Size.Height)
		->Bind(OnSaved)
		->Start();

	return Task->IsSucceeded() || Task->IsScheduled();
}

bool USaveManager::LoadSlot(int32 SlotId, FOnGameLoaded OnLoaded)
{
	if (!CanLoadOrSave())
		return false;

	if (!IsSlotSaved(SlotId))
		return false;

	TryInstantiateInfo();

	auto* Task = CreateTask<USlotDataTask_Loader>()
		->Setup(SlotId)
		->Bind(OnLoaded)
		->Start();

	return Task->IsSucceeded() || Task->IsScheduled();
}

bool USaveManager::DeleteSlot(int32 SlotId)
{
	if (!IsValidSlot(SlotId))
		return false;

	const FString InfoSlot = GenerateSlotInfoName(SlotId);
	const FString DataSlot = GenerateSlotDataName(SlotId);
	return FFileAdapter::DeleteFile(InfoSlot) ||
		   FFileAdapter::DeleteFile(DataSlot);
}

void USaveManager::LoadAllSlotInfos(bool bSortByRecent, FOnAllInfosLoaded Delegate)
{
	auto* LoadTask = new FAsyncTask<FLoadAllSlotInfosTask>(this, bSortByRecent, MoveTemp(Delegate));
	LoadTask->StartBackgroundTask();

	LoadInfosTasks.Add(LoadTask);
}


void USaveManager::BPSaveSlotToId(int32 SlotId, bool bScreenshot, const FScreenshotSize Size, ESaveGameResult& Result, struct FLatentActionInfo LatentInfo, bool bOverrideIfNeeded /*= true*/)
{
	if (UWorld* World = GetWorld())
	{
		Result = ESaveGameResult::Saving;

		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		if (LatentActionManager.FindExistingAction<FSaveGameAction>(LatentInfo.CallbackTarget, LatentInfo.UUID) == NULL)
		{
			LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, new FSaveGameAction(this, SlotId, bOverrideIfNeeded, bScreenshot, Size, Result, LatentInfo));
		}
		return;
	}
	Result = ESaveGameResult::Failed;
}

void USaveManager::BPLoadSlotFromId(int32 SlotId, ELoadGameResult& Result, struct FLatentActionInfo LatentInfo)
{
	if (UWorld* World = GetWorld())
	{
		Result = ELoadGameResult::Loading;

		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		if (LatentActionManager.FindExistingAction<FLoadGameAction>(LatentInfo.CallbackTarget, LatentInfo.UUID) == NULL)
		{
			LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, new FLoadGameAction(this, SlotId, Result, LatentInfo));
		}
		return;
	}
	Result = ELoadGameResult::Failed;
}

void USaveManager::BPLoadAllSlotInfos(const bool bSortByRecent, TArray<USlotInfo*>& SaveInfos, ELoadInfoResult& Result, struct FLatentActionInfo LatentInfo)
{
	if (UWorld* World = GetWorld())
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		if (LatentActionManager.FindExistingAction<FLoadInfosAction>(LatentInfo.CallbackTarget, LatentInfo.UUID) == NULL)
		{
			LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, new FLoadInfosAction(this, bSortByRecent, SaveInfos, Result, LatentInfo));
		}
	}
}


bool USaveManager::IsSlotSaved(int32 SlotId) const
{
	if (!IsValidSlot(SlotId))
		return false;

	const FString InfoSlot = GenerateSlotInfoName(SlotId);
	const FString DataSlot = GenerateSlotDataName(SlotId);
	return FFileAdapter::DoesFileExist(InfoSlot) &&
		   FFileAdapter::DoesFileExist(DataSlot);
}

bool USaveManager::CanLoadOrSave()
{
	const AGameModeBase* GameMode = UGameplayStatics::GetGameMode(this);
	if (!GameMode || !GameMode->HasAuthority())
		return false;

	if (!IsValid(GetWorld()))
		return false;

	return true;
}

void USaveManager::TryInstantiateInfo(bool bForced)
{
	if (IsInSlot() && !bForced)
		return;

	const USavePreset* Preset = GetPreset();

	UClass* InfoTemplate = Preset->SlotInfoTemplate.Get();
	if (!InfoTemplate)
		InfoTemplate = USlotInfo::StaticClass();

	UClass* DataTemplate = Preset->SlotDataTemplate.Get();
	if (!DataTemplate)
		DataTemplate = USlotData::StaticClass();

	CurrentInfo = NewObject<USlotInfo>(GetTransientPackage(), InfoTemplate);
	CurrentData = NewObject<USlotData>(GetTransientPackage(), DataTemplate);
}

void USaveManager::UpdateLevelStreamings()
{
	UWorld* World = GetWorld();
	check(World);

	const TArray<ULevelStreaming*>& Levels = World->GetStreamingLevels();

	LevelStreamingNotifiers.Empty(Levels.Num()); // Avoid memory deallocation
	LevelStreamingNotifiers.Reserve(Levels.Num()); // Reserve extra memory
	for (auto* Level : Levels)
	{
		ULevelStreamingNotifier* Notifier = NewObject<ULevelStreamingNotifier>(this);
		Notifier->SetLevelStreaming(Level);
		Notifier->OnLevelShown().BindUFunction(this, GET_FUNCTION_NAME_CHECKED(USaveManager, DeserializeStreamingLevel));
		Notifier->OnLevelHidden().BindUFunction(this, GET_FUNCTION_NAME_CHECKED(USaveManager, SerializeStreamingLevel));
		LevelStreamingNotifiers.Add(Notifier);
	}
}

void USaveManager::SerializeStreamingLevel(ULevelStreaming* LevelStreaming)
{
	CreateTask<USlotDataTask_LevelSaver>()->Setup(LevelStreaming)->Start();
}

void USaveManager::DeserializeStreamingLevel(ULevelStreaming* LevelStreaming)
{
	CreateTask<USlotDataTask_LevelLoader>()->Setup(LevelStreaming)->Start();
}

USlotInfo* USaveManager::LoadInfo(uint32 SlotId) const
{
	if (!IsValidSlot(SlotId))
	{
		SELog(GetPreset(), "Invalid Slot. Cant go under 0 or exceed MaxSlots", true);
		return nullptr;
	}

	FAsyncTask<FLoadSlotInfoTask>* LoadInfoTask = new FAsyncTask<FLoadSlotInfoTask>(this, SlotId);
	LoadInfoTask->StartSynchronousTask();

	check(LoadInfoTask->IsDone());

	return LoadInfoTask->GetTask().GetLoadedSlot();
}

USlotData* USaveManager::LoadData(const USlotInfo* InSaveInfo) const
{
	if (!InSaveInfo)
		return nullptr;

	const FString Card = GenerateSlotDataName(InSaveInfo->Id);

	return Cast<USlotData>(FFileAdapter::LoadFile(Card));
}

USlotDataTask* USaveManager::CreateTask(TSubclassOf<USlotDataTask> TaskType)
{
	USlotDataTask* Task = NewObject<USlotDataTask>(this, TaskType.Get());
	Task->Prepare(CurrentData, GetPreset());
	Tasks.Add(Task);
	return Task;
}

void USaveManager::FinishTask(USlotDataTask* Task)
{
	Tasks.Remove(Task);

	// Start next task
	if (Tasks.Num() > 0)
		Tasks[0]->Start();
}

void USaveManager::Tick(float DeltaTime)
{
	if (Tasks.Num())
	{
		USlotDataTask* Task = Tasks[0];
		check(Task);
		if (Task->IsRunning())
		{
			Task->Tick(DeltaTime);
		}
	}

	// Finish loading info tasks
	LoadInfosTasks.RemoveAllSwap([](auto* Task) {
		if (Task->IsDone())
		{
			Task->GetTask().CallDelegate();
			delete Task;
			return true;
		}
		return false;
	});
}

void USaveManager::SubscribeForEvents(const TScriptInterface<ISaveExtensionInterface>& Interface)
{
	SubscribedInterfaces.AddUnique(Interface);
}

void USaveManager::UnsubscribeFromEvents(const TScriptInterface<ISaveExtensionInterface>& Interface)
{
	SubscribedInterfaces.Remove(Interface);
}


void USaveManager::OnSaveBegan()
{
	IterateSubscribedInterfaces([](auto* Object) {
		ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object);
		if (Interface)
		{
			Interface->Execute_OnSaveBegan(Object);
		}
		else if (Object->GetClass()->ImplementsInterface(USaveExtensionInterface::StaticClass()))
		{
			ISaveExtensionInterface::Execute_OnSaveBegan(Object);
		}
	});
}

void USaveManager::OnSaveFinished(const bool bError)
{
	IterateSubscribedInterfaces([bError](auto* Object) {
		ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object);
		if (Interface)
		{
			Interface->Execute_OnSaveFinished(Object, bError);
		}
		else if (Object->GetClass()->ImplementsInterface(USaveExtensionInterface::StaticClass()))
		{
			ISaveExtensionInterface::Execute_OnSaveFinished(Object, bError);
		}
	});

	if (!bError)
	{
		OnGameSaved.Broadcast(CurrentInfo);
	}
}

void USaveManager::OnLoadBegan()
{
	IterateSubscribedInterfaces([](auto* Object) {
		ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object);
		if (Interface)
		{
			Interface->Execute_OnLoadBegan(Object);
		}
		else if (Object->GetClass()->ImplementsInterface(USaveExtensionInterface::StaticClass()))
		{
			ISaveExtensionInterface::Execute_OnLoadBegan(Object);
		}
	});
}

void USaveManager::OnLoadFinished(const bool bError)
{
	IterateSubscribedInterfaces([bError](auto* Object) {
		ISaveExtensionInterface* Interface = Cast<ISaveExtensionInterface>(Object);
		if (Interface) {
			Interface->Execute_OnLoadFinished(Object, bError);
		}
		else if (Object->GetClass()->ImplementsInterface(USaveExtensionInterface::StaticClass())) {
			ISaveExtensionInterface::Execute_OnLoadFinished(Object, bError);
		}
	});

	if (!bError)
	{
		OnGameLoaded.Broadcast(CurrentInfo);
	}
}

void USaveManager::OnMapLoadStarted(const FString& MapName)
{
	SELog(GetPreset(), "Loading Map '" + MapName + "'", FColor::Purple);
}

void USaveManager::OnMapLoadFinished(UWorld* LoadedWorld)
{
	USlotDataTask_Loader* Loader = Cast<USlotDataTask_Loader>(Tasks.Num() ? Tasks[0] : nullptr);
	if (Loader && Loader->bLoadingMap)
	{
		Loader->OnMapLoaded();
	}

	UpdateLevelStreamings();
}

UWorld* USaveManager::GetWorld() const
{
	// If we are a CDO, we must return nullptr instead of calling Outer->GetWorld() to fool UObject::ImplementsGetWorld.
	if (HasAllFlags(RF_ClassDefaultObject) || !GetOuter())
		return nullptr;

	// Our outer should be the GameInstance
	return GetOuter()->GetWorld();
}

void USaveManager::BeginDestroy()
{
	// Remove this manager from the static list
	GlobalManagers.Remove(OwningGameInstance);

	for (auto* LoadInfoTask : LoadInfosTasks)
	{
		if (!LoadInfoTask->IsIdle())
			LoadInfoTask->EnsureCompletion(false);
		delete LoadInfoTask;
	}

	Super::BeginDestroy();
}


TMap<TWeakObjectPtr<UGameInstance>, TWeakObjectPtr<USaveManager>> USaveManager::GlobalManagers {};

USaveManager* USaveManager::GetSaveManager(const UObject* ContextObject)
{
	UWorld* World = GEngine->GetWorldFromContextObject(ContextObject, EGetWorldErrorMode::LogAndReturnNull);

	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	if (GI)
	{
		TWeakObjectPtr<USaveManager>& Manager = GlobalManagers.FindOrAdd(GI);
		if (!Manager.IsValid())
		{
			Manager = NewObject<USaveManager>(GI);
			Manager->SetGameInstance(GI);
			Manager->Init();
		}
		return Manager.Get();
	}
	return nullptr;
}


