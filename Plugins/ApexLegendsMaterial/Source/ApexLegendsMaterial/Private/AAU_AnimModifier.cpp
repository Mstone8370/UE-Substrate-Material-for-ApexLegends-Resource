// Copyright (c) 2024 Minseok Kim


#include "AAU_AnimModifier.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"

#include "EditorUtilityLibrary.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Kismet/KismetMathLibrary.h"

void UAAU_AnimModifier::ModifyAnimation(float Scale, bool bUnrotateRootBone, bool bStart)
{
    if (Scale < UE_KINDA_SMALL_NUMBER)
    {
        Scale = 0.0254f;
    }

    TArray<FAssetData> SelectedAssetDatas = UEditorUtilityLibrary::GetSelectedAssetData();
    for (FAssetData& SelectedAssetData : SelectedAssetDatas)
    {
        UObject* SelectedObject = SelectedAssetData.GetAsset();
        if (!SelectedObject || !SelectedObject->IsA<UAnimSequence>())
        {
            continue;
        }

        const FString ObjectPath = SelectedObject->GetPathName();
        const FString FilePath = FPaths::GetBaseFilename(ObjectPath, false);
        const FString FolderPath = FPaths::GetPath(FilePath);
        const FString FileName = FPaths::GetBaseFilename(FilePath, true);

        const FString NewFileName = FileName + FString(TEXT("_Scaled"));
        const FString NewFilePath = FPaths::ConvertRelativePathToFull(FolderPath, NewFileName);
        
        if (UObject* DuplicatedObject = UEditorAssetLibrary::DuplicateLoadedAsset(SelectedObject, NewFilePath))
        {
            FAssetRegistryModule::AssetCreated(DuplicatedObject);

            ModifyAnimation_Internal(DuplicatedObject, Scale, bUnrotateRootBone, bStart);

            UEditorAssetLibrary::SaveAsset(NewFilePath, false);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("[Apex Legends Tool] Asset Duplication Failed"));
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::FromString(TEXT("Asset Duplication Failed.")),
                FText::FromString(TEXT("Error"))
            );
        }
    }
}

void UAAU_AnimModifier::ModifyAnimation_Internal(UObject* Object, float Scale, bool bUnrotateRootBone, bool bStart)
{
    UAnimSequence* AnimSeq = Cast<UAnimSequence>(Object);
    if (!AnimSeq)
    {
        return;
    }

    // Get base pose info
    const USkeleton* Skeleton = AnimSeq->GetSkeleton();
    const TArray<FTransform>& RefBonePose = Skeleton->GetReferenceSkeleton().GetRawRefBonePose();
    const TArray<FMeshBoneInfo>& RefBoneInfo = Skeleton->GetReferenceSkeleton().GetRawRefBoneInfo();
    const int32 BoneNum = RefBonePose.Num();
    if (BoneNum < 1)
    {
        return;
    }

    const IAnimationDataModel* AnimDataModel = AnimSeq->GetDataModel();
    IAnimationDataController& AnimDataController = AnimSeq->GetController();

    const int32 KeyNum = AnimDataModel->GetNumberOfKeys();

    // Show Dialog
    FScopedSlowTask ProgressDialog(BoneNum, FText::FromString(FString("Converting Animation Scale...")));
    ProgressDialog.MakeDialog();

    // Start
    TArray<FTransform> RootRelativeStart;
    if (bStart)
    {
        for (int32 KeyIdx = 0; KeyIdx < KeyNum; KeyIdx++)
        {
            RootRelativeStart.Add(FTransform::Identity);
        }
    }

    for (int32 BoneIdx = 0; BoneIdx < BoneNum; BoneIdx++)
    {
        const FName& BoneName = RefBoneInfo[BoneIdx].Name;
        const FTransform& RefBoneTransform = RefBonePose[BoneIdx];

        // Get animation bone transforms
        TArray<FTransform> OriginalBoneTrack;
        AnimDataModel->GetBoneTrackTransforms(BoneName, OriginalBoneTrack);

        TArray<FVector> PositionalKeys;
        TArray<FQuat> RotationalKeys;
        TArray<FVector> ScalingKeys;

        // Fill scaled bone track keys
        for (int32 KeyIdx = 0; KeyIdx < KeyNum; KeyIdx++)
        {
            const FTransform& OriginalTransform = OriginalBoneTrack[KeyIdx];

            const FVector RefBoneLocation = RefBoneTransform.GetLocation();
            const FVector AnimBoneLocation = OriginalTransform.GetLocation();

            const FVector DeltaLocation = AnimBoneLocation - RefBoneLocation;
            const FVector DeltaDirection = DeltaLocation.GetSafeNormal();
            const double DeltaLength = DeltaLocation.Length();

            const FVector ScaledBoneLocation = RefBoneLocation + (DeltaDirection * (DeltaLength * Scale));

            FQuat BoneRotation = OriginalTransform.GetRotation();
            if (bUnrotateRootBone && BoneIdx == 0 /* BoneIdx == 0: root bone */)
            {
                FQuat RotationQuat = FRotator(0.f, 0.f, -90.f).Quaternion();
                BoneRotation = RotationQuat * BoneRotation;
            }

            PositionalKeys.Add(ScaledBoneLocation);
            RotationalKeys.Add(BoneRotation);
            ScalingKeys.Add(OriginalTransform.GetScale3D());

            if (bStart && BoneIdx < 3 /* BoneIdx == 0: root bone, BoneIdx == 1: delta bone, BoneIdx == 2: start bone */)
            {
                /**
                * Space transformation accumulation.
                * UKismetMathLibrary::ComposeTransforms(A, B) == A * B.
                * ChildTransformInParentSpace = ChildLocalTransform * ParentTransform;
                */
                RootRelativeStart[KeyIdx] = UKismetMathLibrary::ComposeTransforms(
                    FTransform(BoneRotation, ScaledBoneLocation, OriginalTransform.GetScale3D()),
                    RootRelativeStart[KeyIdx]
                );
            }
        }

        // Set scale bone track keys
        AnimDataController.SetBoneTrackKeys(BoneName, PositionalKeys, RotationalKeys, ScalingKeys);

        ProgressDialog.EnterProgressFrame(1, FText::FromString(FString::Printf(TEXT("Converting Animation Scale... [%d/%d]"), BoneIdx + 1, BoneNum)));
    }

    if (bStart)
    {
        const FName& RootBoneName = RefBoneInfo[0].Name;

        TArray<FTransform> RootTrack;
        AnimDataModel->GetBoneTrackTransforms(RootBoneName, RootTrack);

        TArray<FVector> PositionalKeys;
        TArray<FQuat> RotationalKeys;
        TArray<FVector> ScalingKeys;

        for (int32 KeyIdx = 0; KeyIdx < KeyNum; KeyIdx++)
        {
            FVector RootTranslation = RootTrack[KeyIdx].GetLocation() - RootRelativeStart[KeyIdx].GetLocation();

            const FQuat RotationQuat = RootRelativeStart[KeyIdx].GetRotation().GetAxisX().ToOrientationQuat().Inverse();
            RootTranslation = RotationQuat.RotateVector(RootTranslation);

            PositionalKeys.Add(RootTranslation);
            RotationalKeys.Add(RotationQuat* RootTrack[KeyIdx].GetRotation());
            ScalingKeys.Add(RootTrack[KeyIdx].GetScale3D());
        }

        AnimDataController.SetBoneTrackKeys(RootBoneName, PositionalKeys, RotationalKeys, ScalingKeys);
    }

    AnimSeq->PostEditChange();
    AnimSeq->MarkPackageDirty();
}
