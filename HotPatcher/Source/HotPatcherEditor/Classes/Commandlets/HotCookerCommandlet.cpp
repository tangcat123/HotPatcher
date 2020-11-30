#include "HotCookerCommandlet.h"
#include "ThreadUtils/FProcWorkerThread.hpp"
#include "FCookerConfig.h"
#include "FlibPatchParserHelper.h"

// engine header
#include "CoreMinimal.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/Paths.h"

#include "CreatePatch/ReleaseProxy.h"
#include "CreatePatch/FExportPatchSettings.h"
#include "FlibHotPatcherEditorHelper.h"
#include "ETargetPlatform.h"

#define COOKER_CONFIG_PARAM_NAME TEXT("-config=")
#define COOKER_CONFIG_PARAM_TARGETPLATFORM TEXT("-TargetPlatform=")

#define IncCookReleaseSettingConfigFile "HotCookerConfig/IncCookReleaseSettingConfig.json"
#define IncCookPatchSettingConfigFile "HotCookerConfig/IncCookPatchSettingConfig.json"

DEFINE_LOG_CATEGORY(LogHotCookerCommandlet);

TSharedPtr<FProcWorkerThread> CookerProc;

void ReceiveOutputMsg(const FString& InMsg)
{
	FString FindItem(TEXT("Display:"));
	int32 Index= InMsg.Len() - InMsg.Find(FindItem)- FindItem.Len();
	if (InMsg.Contains(TEXT("Error:")))
	{
		UE_LOG(LogHotCookerCommandlet, Error, TEXT("%s"), *InMsg);
	}
	else if (InMsg.Contains(TEXT("Warning:")))
	{
		UE_LOG(LogHotCookerCommandlet, Warning, TEXT("%s"), *InMsg);
	}
	else
	{
		UE_LOG(LogHotCookerCommandlet, Display, TEXT("%s"), *InMsg.Right(Index));
	}
}

template<typename TStructType>
void LoadFileToSetting(FString LoadFile, TSharedPtr<TStructType> SettingPtr)
{
	FString JsonContent;
	LoadFile = FPaths::Combine(FPaths::ProjectDir(), LoadFile);
	if (UFLibAssetManageHelperEx::LoadFileToString(LoadFile, JsonContent))
	{
		// UFlibHotPatcherEditorHelper::DeserializeReleaseConfig(ExportReleaseSettings, JsonContent);
		UFlibPatchParserHelper::TDeserializeJsonStringAsStruct(JsonContent, *SettingPtr);
	}

}

void OnProcSuccess()
{
	// 成功之后生成对应的Cook文件列表
	UE_LOG(LogHotCookerCommandlet, Display, TEXT("ProcSuccess"));

	TSharedPtr<FExportReleaseSettings> ExportReleaseSettings = MakeShareable(new FExportReleaseSettings);
	LoadFileToSetting<FExportReleaseSettings>(IncCookReleaseSettingConfigFile, ExportReleaseSettings);
	UReleaseProxy* ReleaseProxy = NewObject<UReleaseProxy>();
	ReleaseProxy->AddToRoot();
	ReleaseProxy->SetProxySettings(ExportReleaseSettings.Get());
	ReleaseProxy->DoExportCurCookRelease();
}

int32 UHotCookerCommandlet::Main(const FString& Params)
{
	UE_LOG(LogHotCookerCommandlet, Display, TEXT("UHotCookerCommandlet::Main"));
	FString config_path;
	bool bStatus = FParse::Value(*Params, *FString(COOKER_CONFIG_PARAM_NAME).ToLower(), config_path);
	if (!bStatus)
	{
		UE_LOG(LogHotCookerCommandlet, Error, TEXT("UHotCookerCommandlet error: not -config=xxxx.json params."));
		return -1;
	}

	FString TargetPlatformFromCmdLine;
	bStatus = FParse::Value(*Params, *FString(COOKER_CONFIG_PARAM_TARGETPLATFORM).ToLower(), TargetPlatformFromCmdLine);

	config_path = FPaths::Combine(*FPaths::ProjectDir(), config_path);

	if (!FPaths::FileExists(config_path))
	{
		UE_LOG(LogHotCookerCommandlet, Error, TEXT("UHotCookerCommandlet error: cofnig file %s not exists."), *config_path);
		return -1;
	}


	FString JsonContent;
	if (FFileHelper::LoadFileToString(JsonContent, *config_path))
	{
		FCookerConfig CookConfig;
		UFlibPatchParserHelper::TDeserializeJsonStringAsStruct(JsonContent, CookConfig);

		if(!TargetPlatformFromCmdLine.IsEmpty())
		{
			CookConfig.CookPlatforms.Empty();

			FString PlatformStr = TargetPlatformFromCmdLine;
			{
				TArray<FString> PlatformNames;
				PlatformStr.ParseIntoArray(PlatformNames, TEXT("+"), true);
				CookConfig.CookPlatforms.Append(PlatformNames);
			}

		}

		auto TheadCookByCookConfig = [&CookConfig]()
		{
			if (CookConfig.bCookAllMap)
			{
				CookConfig.CookMaps = UFlibPatchParserHelper::GetAvailableMaps(UKismetSystemLibrary::GetProjectDirectory(), false, false, true);
			}
			FString CookCommand;
			UFlibPatchParserHelper::GetCookProcCommandParams(CookConfig, CookCommand);

			UE_LOG(LogHotCookerCommandlet, Display, TEXT("CookCommand:%s %s"), *CookConfig.EngineBin, *CookCommand);

			if (FPaths::FileExists(CookConfig.EngineBin) && FPaths::FileExists(CookConfig.ProjectPath))
			{
				CookerProc = MakeShareable(new FProcWorkerThread(TEXT("CookThread"), CookConfig.EngineBin, CookCommand));
				CookerProc->ProcOutputMsgDelegate.AddStatic(&::ReceiveOutputMsg);
				CookerProc->ProcSuccessedDelegate.AddStatic(&::OnProcSuccess);
				CookerProc->Execute();
				CookerProc->Join();
			}
		};

		if(CookConfig.IncCook)
		{
			TSharedPtr<FExportPatchSettings> ExportPatchSettings = MakeShareable(new FExportPatchSettings);
			LoadFileToSetting<FExportPatchSettings>(IncCookPatchSettingConfigFile, ExportPatchSettings);
			ExportPatchSettings->bByBaseVersion = true;

			FString CurReleaseJsonFile = UFlibPatchParserHelper::GetCurCookReleaseJsonFile();
			if (!FPaths::FileExists(CurReleaseJsonFile))
			{
				// 首次Cook
				TheadCookByCookConfig();

				OnProcSuccess();

				return 0;
			}

			ExportPatchSettings->BaseVersion.FilePath = UFlibPatchParserHelper::GetCurCookReleaseJsonFile();

			TArray<FString> IncCookAssets;
			// 计算增量
			{
				FChunkInfo DefaultChunk;
				FHotPatcherVersion BaseVersion;

				if (ExportPatchSettings->IsByBaseVersion())
				{
					ExportPatchSettings->GetBaseVersionInfo(BaseVersion);
					DefaultChunk = UFlibHotPatcherEditorHelper::MakeChunkFromPatchVerison(BaseVersion);
					if (!ExportPatchSettings->IsEnableExternFilesDiff())
					{
						BaseVersion.PlatformAssets.Empty();
					}
				}

				FChunkInfo NewVersionChunk = UFlibHotPatcherEditorHelper::MakeChunkFromPatchSettings(ExportPatchSettings.Get());

				FChunkAssetDescribe ChunkAssetsDescrible = UFlibPatchParserHelper::DiffChunkByBaseVersionWithPatchSetting(*ExportPatchSettings.Get(), NewVersionChunk, DefaultChunk, BaseVersion);
				TArray<FString> AllUnselectedAssets = ChunkAssetsDescrible.GetAssetsStrings();
				IncCookAssets.Append(AllUnselectedAssets);

				//for (auto Platform : ExportPatchSettings->GetPakTargetPlatforms())
				//{
				//	TArray<FString> PlatformExFiles;
				//	FString PlatformName = UFlibPatchParserHelper::GetEnumNameByValue(Platform, false);
				//	PlatformExFiles.Append(ChunkAssetsDescrible.GetExFileStrings(Platform));
				//	PlatformExFiles.Append(ChunkAssetsDescrible.GetExFileStrings(ETargetPlatform::AllPlatforms));
				//	IncCookAssets.Append(PlatformExFiles);
				//}

				ETargetPlatform Platform = ETargetPlatform::WindowsNoEditor;
				FString CookedDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Cooked")));
				FString CookForPlatform = UFlibPatchParserHelper::GetEnumNameByValue(Platform);
				TArray<FString> CookForPlatforms{ CookForPlatform };


				for (FString IncCookAsset : IncCookAssets)
				{
					UPackage* AssetPacakge = LoadPackage(nullptr, *IncCookAsset, LOAD_None);
					//FString CookedSavePath = UFlibHotPatcherEditorHelper::GetCookAssetsSaveDir(CookedDir, AssetPacakge, CookForPlatform);

					bool bCookStatus = UFlibHotPatcherEditorHelper::CookPackage(AssetPacakge, CookForPlatforms, CookedDir);
				}

			}
			OnProcSuccess();
		}

		else
		{
			TheadCookByCookConfig();
		}

			
		
}
	
	//system("pause");
	return 0;
}
