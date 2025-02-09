// Copyright 2020-2022, Roberto De Ioris.

#include "glTFRuntimeParser.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#if WITH_EDITOR
#include "Editor/EditorEngine.h"
#endif
#include "PhysicsEngine/BodySetup.h"
#include "Kismet/KismetMathLibrary.h"
#include "Runtime/Launch/Resources/Version.h"
#include "StaticMeshResources.h"

#define RESTRICT_POINTCLOUD_SIZE_FOR_TESTING_ON_LAPTOP true

#define MODE_POINTS 0
#define MODE_LINES 1
#define MODE_TRIANGLES 4

#define GLYPHER_DEFAULT_MESH TEXT("StaticMesh'/glTFRuntime/SM_Sphere_glTFRuntime.SM_Sphere_glTFRuntime'")
#define GLYPHER_SCALING_FACTOR 0.1f

#define NUM_CUSTOM_FLOATS_PER_INSTANCE 4

FglTFRuntimeStaticMeshContext::FglTFRuntimeStaticMeshContext(TSharedRef<FglTFRuntimeParser> InParser, const FglTFRuntimeStaticMeshConfig& InStaticMeshConfig) :
	Parser(InParser),
	StaticMeshConfig(InStaticMeshConfig)
{
	StaticMesh = NewObject<UStaticMesh>(StaticMeshConfig.Outer ? StaticMeshConfig.Outer : GetTransientPackage(), NAME_None, RF_Public);
#if PLATFORM_ANDROID || PLATFORM_IOS
	StaticMesh->bAllowCPUAccess = false;
#else
	StaticMesh->bAllowCPUAccess = StaticMeshConfig.bAllowCPUAccess;
#endif

#if ENGINE_MAJOR_VERSION < 5 && ENGINE_MINOR_VERSION > 26
	StaticMesh->SetIsBuiltAtRuntime(true);
#endif
	StaticMesh->NeverStream = true;

#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MINOR_VERSION > 26)
	if (StaticMesh->GetRenderData())
	{
		StaticMesh->GetRenderData()->ReleaseResources();
	}
	StaticMesh->SetRenderData(MakeUnique<FStaticMeshRenderData>());
	RenderData = StaticMesh->GetRenderData();
#else
	if (StaticMesh->RenderData.IsValid())
	{
		StaticMesh->RenderData->ReleaseResources();
	}
	StaticMesh->RenderData = MakeUnique<FStaticMeshRenderData>();
	RenderData = StaticMesh->RenderData.Get();
#endif
}


void FglTFRuntimeParser::LoadStaticMeshAsync(const int32 MeshIndex, const FglTFRuntimeStaticMeshAsync& AsyncCallback, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	// first check cache
	if (CanReadFromCache(StaticMeshConfig.CacheMode) && StaticMeshesCache.Contains(MeshIndex))
	{
		AsyncCallback.ExecuteIfBound(StaticMeshesCache[MeshIndex]);
		return;
	}

	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);

	Async(EAsyncExecution::Thread, [this, StaticMeshContext, MeshIndex, AsyncCallback]()
		{

			TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
			if (JsonMeshObject)
			{

				FglTFRuntimeMeshLOD* LOD = nullptr;
				if (LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, StaticMeshContext->StaticMeshConfig.MaterialsConfig))
				{
					StaticMeshContext->LODs.Add(LOD);

					StaticMeshContext->StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);
				}
			}

			FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([MeshIndex, StaticMeshContext, AsyncCallback]()
				{
					if (StaticMeshContext->StaticMesh)
					{
						StaticMeshContext->StaticMesh = StaticMeshContext->Parser->FinalizeStaticMesh(StaticMeshContext);
					}

					if (StaticMeshContext->StaticMesh)
					{
						if (StaticMeshContext->Parser->CanWriteToCache(StaticMeshContext->StaticMeshConfig.CacheMode))
						{
							StaticMeshContext->Parser->StaticMeshesCache.Add(MeshIndex, StaticMeshContext->StaticMesh);
						}
					}

					AsyncCallback.ExecuteIfBound(StaticMeshContext->StaticMesh);
				}, TStatId(), nullptr, ENamedThreads::GameThread);
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
		});
}

UStaticMesh* FglTFRuntimeParser::LoadStaticMesh_Internal(TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext) {
	UStaticMeshComponent* Dummy = NewObject<UStaticMeshComponent>();
	return LoadStaticMesh_Internal(StaticMeshContext, Dummy);
}

UStaticMesh* FglTFRuntimeParser::LoadStaticMesh_Internal(TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext, UStaticMeshComponent*& StaticMeshComponent)
{
	SCOPED_NAMED_EVENT(FglTFRuntimeParser_LoadStaticMesh_Internal, FColor::Magenta);

	OnPreCreatedStaticMesh.Broadcast(StaticMeshContext);

	bFinalizeStaticMesh = true;
	UStaticMesh* StaticMesh = StaticMeshContext->StaticMesh;
	FStaticMeshRenderData* RenderData = StaticMeshContext->RenderData;
	const FglTFRuntimeStaticMeshConfig& StaticMeshConfig = StaticMeshContext->StaticMeshConfig;
	FName ExportOriginalPivotToSocket = FName(StaticMeshConfig.ExportOriginalPivotToSocket);
	const TArray<const FglTFRuntimeMeshLOD*>& LODs = StaticMeshContext->LODs;

	bool bHasVertexColors = false;

	RenderData->AllocateLODResources(LODs.Num());

	int32 LODIndex = 0;

	const float TangentsDirection = StaticMeshConfig.bReverseTangents ? -1 : 1;

	// this is used for inheriting materials while in multi LOD mode
	TMap<int32, int32> SectionMaterialMap;

	for (const FglTFRuntimeMeshLOD* LOD : LODs)
	{
		const int32 CurrentLODIndex = LODIndex++;
		FStaticMeshLODResources& LODResources = RenderData->LODResources[CurrentLODIndex];

#if ENGINE_MAJOR_VERSION > 4
		FStaticMeshSectionArray& Sections = LODResources.Sections;
#else
		FStaticMeshLODResources::FStaticMeshSectionArray& Sections = LODResources.Sections;
#endif
		TArray<uint32> LODIndices;
		int32 NumUVs = 1;
		FVector PivotDelta = FVector::ZeroVector;

		int32 NumVertexInstancesPerLOD = 0;

		for (const FglTFRuntimePrimitive& Primitive : LOD->Primitives)
		{
			if (Primitive.UVs.Num() > NumUVs)
			{
				NumUVs = Primitive.UVs.Num();
			}

			if (Primitive.Colors.Num() > 0)
			{
				bHasVertexColors = true;
			}

			NumVertexInstancesPerLOD += Primitive.Indices.Num();
		}

		TArray<FStaticMeshBuildVertex> StaticMeshBuildVertices;
		StaticMeshBuildVertices.SetNum(NumVertexInstancesPerLOD);

		FBox BoundingBox;
		BoundingBox.Init();

		bool bHighPrecisionUVs = false;

		int32 VertexInstanceBaseIndex = 0;

		const bool bApplyAdditionalTransforms = LOD->Primitives.Num() == LOD->AdditionalTransforms.Num();

		int32 AdditionalTransformsPrimitiveIndex = 0; // used only when applying additional transforms

		for (const FglTFRuntimePrimitive& Primitive : LOD->Primitives)
		{
			bool bMissingNormals = false;
			bool bMissingTangents = false;
			bool bMissingIgnore = false;

			FName MaterialName = FName(FString::Printf(TEXT("LOD_%d_Section_%d_%s"), CurrentLODIndex, StaticMeshContext->StaticMaterials.Num(), *Primitive.MaterialName));
			FStaticMaterial StaticMaterial(Primitive.Material, MaterialName);
			StaticMaterial.UVChannelData.bInitialized = true;

			FStaticMeshSection& Section = Sections.AddDefaulted_GetRef();
			int32 NumVertexInstancesPerSection = Primitive.Indices.Num();

			if (Primitive.Mode == MODE_LINES) 
			{
				const FTransform MeshTransform = FTransform(
					StaticMeshComponent->GetComponentRotation(),
					StaticMeshComponent->GetComponentLocation(), 
					StaticMeshComponent->GetComponentScale()
				);

				// Create Niagara beam with that material.
				UNiagaraSystem* NS = LoadObject<UNiagaraSystem>(nullptr, TEXT("/glTFRuntime/P_Line_glTFRuntime"), nullptr, LOAD_None, nullptr);

				if (NS) 
				{
					// Spawn beam between start and end of each line.
					for (int32 LineIndex = 0; LineIndex < NumVertexInstancesPerSection / 2; LineIndex++) 
					{
						int32 VertexIndexStart = Primitive.Indices[LineIndex * 2];
						FVector PositionStart = FVector(GetSafeValue(Primitive.Positions, VertexIndexStart, FVector::ZeroVector, bMissingIgnore));

						int32 VertexIndexEnd = Primitive.Indices[LineIndex * 2 + 1];
						FVector PositionEnd = FVector(GetSafeValue(Primitive.Positions, VertexIndexEnd, FVector::ZeroVector, bMissingIgnore));

						// Lines and Points are not handled by UStaticMesh, therefore local2world transforms
						// applied manually.
						PositionStart = MeshTransform.TransformPosition(PositionStart);
						PositionEnd = MeshTransform.TransformPosition(PositionEnd);
						 
						FVector Diff = PositionEnd - PositionStart;
						FVector UnitDiff = FVector(Diff);
						UnitDiff.Normalize(0.00001f);

						FRotator Rotation = FRotator(FQuat::FindBetweenNormals(FVector3d(1.0, 0.0, 0.0), UnitDiff));
						UNiagaraComponent* Beam = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
							StaticMesh->GetWorld(), NS, PositionStart, Rotation, FVector::One(), true, true, ENCPoolMethod::AutoRelease, true);
						
						// Set length
						Beam->SetVariableFloat("Length", Diff.Length());

						// Set colour
						if (!Primitive.Colors.IsEmpty())
						{
							FLinearColor StartColor = FLinearColor(Primitive.Colors[LineIndex * 2]).ToFColor(true);
							FLinearColor EndColor = FLinearColor(Primitive.Colors[LineIndex * 2 + 1]).ToFColor(true);

							//// Colours are appearing too bright. Should they be squared, cubed?
							//StartColor.R *= StartColor.R * StartColor.R;
							//StartColor.G *= StartColor.G * StartColor.G;
							//StartColor.B *= StartColor.B * StartColor.B;

							//// Colours are appearing too bright. Should they be squared, cubed?
							//EndColor.R *= EndColor.R * EndColor.R;
							//EndColor.G *= EndColor.G * EndColor.G;
							//EndColor.B *= EndColor.B * EndColor.B;

							Beam->SetVariableLinearColor("StartColor", StartColor);
							Beam->SetVariableLinearColor("EndColor", EndColor);
						}
					}
				}
				else 
				{
					UE_LOG(LogTemp, Log, TEXT("Failed to create NiagaraSystem with Line. :("));
				}

				return nullptr;
				//return StaticMesh;
			}

			if (Primitive.Mode == MODE_POINTS) 
			{
				const FTransform MeshTransform = FTransform(
					StaticMeshComponent->GetComponentRotation(),
					StaticMeshComponent->GetComponentLocation(),
					StaticMeshComponent->GetComponentScale()
				);

				// In future, Glypher qualities will be sent via glTF.
				// For now, just have that be set client side.
				bool bGlyphers = true;

				// I'm getting crashes on my laptop due to large pointclouds with InstancedStaticMesh.
				// Note that this hasn't happened with the particle system.
				// Also note that the particle system isn't currently working due to a memory bug :(.
				// Try particle system for large pointclouds
				if (RESTRICT_POINTCLOUD_SIZE_FOR_TESTING_ON_LAPTOP && NumVertexInstancesPerSection > 1000)
				{
					UE_LOG(LogTemp, Log, TEXT("Large pointcloud (size %d) ignored due to computer constraints."), NumVertexInstancesPerSection);
					bGlyphers = false;
					return nullptr; // Comment this line out to try rendering particles.
				}

				// Glyphers use instanced rendering do render many objects.
				if (bGlyphers)
				{
					// Use instanced mesh for optimal rendering.
					TArray<USceneComponent*> Parents;
					StaticMeshComponent->GetParentComponents(Parents);
					UInstancedStaticMeshComponent* InstancedStaticMeshComponent = NewObject<UInstancedStaticMeshComponent>(
						Parents[0], MakeUniqueObjectName(StaticMeshComponent, UInstancedStaticMeshComponent::StaticClass(), "Glyphs"));

					// Assume glyphs are spheres
					// Note this redefines StaticMesh, meaning StaticMeshContext needs to be suitably updated.
					StaticMesh = LoadObject<UStaticMesh>(Parents[0], GLYPHER_DEFAULT_MESH);
					StaticMeshContext->StaticMesh = StaticMesh;
					bFinalizeStaticMesh = false;

					InstancedStaticMeshComponent->NumCustomDataFloats = NUM_CUSTOM_FLOATS_PER_INSTANCE;

					TArray<FVector> Positions;
					Positions.Init(FVector::ZeroVector, NumVertexInstancesPerSection);
					TArray<FLinearColor> Colors;
					Colors.Init(FLinearColor::Green, NumVertexInstancesPerSection);

					// Collect points.
					for (int32 PointIndex = 0; PointIndex < NumVertexInstancesPerSection; PointIndex++)
					{
						int32 VertexIndexStart = Primitive.Indices[PointIndex];

						FVector Position = FVector(GetSafeValue(Primitive.Positions, VertexIndexStart, FVector::ZeroVector, bMissingIgnore));
						FLinearColor Color;

						// MINOR ISSUE: Glyphs are not appearing as a subobject of the correct component.
						// This isn't a big concern, but it means we may have to apply this transformation manually.
						//Position = MeshTransform.TransformPosition(Position);

						// The base static mesh is very large, therefore it must be scaled down.
						// Currently, a copy of the default engine sphere is used (with custom material).
						// Perhaps, in future, a specially designed mesh should be used instead.
						float Scale = GLYPHER_SCALING_FACTOR;
						FTransform Transform = FTransform(Position);
						Transform.SetScale3D(Scale * FVector::One());

						// In future, a rotation may be applied to Transform in order to render vector (arrow) glyphers.
						// Something like, Transform.SetRotation(some rotation);

						int32 InstanceIndex = InstancedStaticMeshComponent->AddInstance(Transform);

						// Add colors to custom data.
						// For each instance, custom data: [R, G, B, A]
						if (!Primitive.Colors.IsEmpty())
						{
							Color = FLinearColor(Primitive.Colors[PointIndex]).ToFColor(true);

							//// Colours are appearing too bright. Should they be squared, cubed?
							//Color.R *= Color.R * Color.R;
							//Color.G *= Color.G * Color.G;
							//Color.B *= Color.B * Color.B;

							TArray<float> ColorArray;
							ColorArray.Init(0.0f, 4);
							ColorArray[0] = Color.R;
							ColorArray[1] = Color.G;
							ColorArray[2] = Color.B;
							ColorArray[3] = Color.A;

							InstancedStaticMeshComponent->SetCustomData(InstanceIndex, ColorArray);
						}

						// NOTE FOR FUTURE PROGRAMMERS: If you want to make greater use of 
						// static mesh instance custom data (for instance: radius),
						// you will need to adjust the material used by the base static mesh:
						// glTFRuntime/M_GlypherBase_glTFRuntime
					}
					StaticMeshComponent = InstancedStaticMeshComponent;
					
					return StaticMesh;
				}


				UNiagaraSystem* NS = LoadObject<UNiagaraSystem>(nullptr, TEXT("/glTFRuntime/P_Point_glTFRuntime"), nullptr, LOAD_None, nullptr);

				if (NS) 
				{
					// Each element of Positions or Colors is one component of a Vector4.
					// i.e. Positions[4*n]     = X,		Colours[4*n]     = B
					//      Positions[4*n + 1] = Y,		Colours[4*n + 1] = G
					//      Positions[4*n + 2] = Z,		Colours[4*n + 2] = R
					//      Positions[4*n + 3] =  ,		Colours[4*n + 3] = A
					TArray<float> Positions;
					Positions.Init(0.0f, NumVertexInstancesPerSection * 4);
					TArray<uint8_t> Colors;
					Colors.Init(0, NumVertexInstancesPerSection * 4);

					// Spawn particle for each point.
					// NOTE: Glyphers are not yet supported.

					// Collect points.
					for (int32 PointIndex = 0; PointIndex < NumVertexInstancesPerSection; PointIndex++)
					{
						int32 VertexIndexStart = Primitive.Indices[PointIndex];

						FVector Position = FVector(GetSafeValue(Primitive.Positions, VertexIndexStart, FVector::ZeroVector, bMissingIgnore));
						FLinearColor Color;
						// Lines and Points are not handled by UStaticMesh, therefore local2world transforms
						// applied manually.
						Position = MeshTransform.TransformPosition(Position);

						if (!Primitive.Colors.IsEmpty())
						{
							Color = FLinearColor(Primitive.Colors[PointIndex]).ToFColor(true);

							//// Colours are appearing too bright. Should they be squared, cubed?
							//Colors[PointIndex].R *= Colors[PointIndex].R * Colors[PointIndex].R;
							//Colors[PointIndex].G *= Colors[PointIndex].G * Colors[PointIndex].G;
							//Colors[PointIndex].B *= Colors[PointIndex].B * Colors[PointIndex].B;
						}

						Positions[4 * PointIndex] = Position.X;
						Positions[4 * PointIndex + 1] = Position.Y;
						Positions[4 * PointIndex + 2] = Position.Z;

						Colors[4 * PointIndex] = Color.B * 255;
						Colors[4 * PointIndex + 1] = Color.G * 255;
						Colors[4 * PointIndex + 2] = Color.R * 255;
						Colors[4 * PointIndex + 3] = Color.A * 255;
					}

					UNiagaraComponent* PointCloud = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
						StaticMeshComponent->GetWorld(), NS, FVector::Zero(), FRotator(0, 0, 0), FVector::One(), true, true, ENCPoolMethod::AutoRelease, true);

					InjectPointcloudData(PointCloud, Positions, Colors, NumVertexInstancesPerSection);

					return nullptr;
					
				}
				else 
				{
					UE_LOG(LogTemp, Log, TEXT("Failed to create NiagaraSystem with Points. :("));
				}
			}

			if (Primitive.Mode == MODE_TRIANGLES) {
				Section.NumTriangles = NumVertexInstancesPerSection / 3;
				Section.FirstIndex = VertexInstanceBaseIndex;
				Section.bEnableCollision = true;
				Section.bCastShadow = true;

				if (Primitive.bHighPrecisionUVs)
				{
					bHighPrecisionUVs = true;
				}

				const int32 SectionIndex = Sections.Num() - 1;

				int32 MaterialIndex = 0;
				if (Primitive.bHasMaterial || !SectionMaterialMap.Contains(SectionIndex))
				{
					MaterialIndex = StaticMeshContext->StaticMaterials.Add(StaticMaterial);
					if (!SectionMaterialMap.Contains(SectionIndex))
					{
						SectionMaterialMap.Add(SectionIndex, MaterialIndex);
					}
					else
					{
						SectionMaterialMap[SectionIndex] = MaterialIndex;
					}
				}
				else if (SectionMaterialMap.Contains(SectionIndex))
				{
					MaterialIndex = SectionMaterialMap[SectionIndex];
				}

				Section.MaterialIndex = MaterialIndex;

#if WITH_EDITOR
				FMeshSectionInfoMap& SectionInfoMap = StaticMeshContext->StaticMesh->GetSectionInfoMap();
				FMeshSectionInfo MeshSectionInfo;
				MeshSectionInfo.MaterialIndex = MaterialIndex;
				SectionInfoMap.Set(CurrentLODIndex, SectionIndex, MeshSectionInfo);
#endif

				LODIndices.AddUninitialized(NumVertexInstancesPerSection);

				// Geometry generation
				for (int32 VertexInstanceSectionIndex = 0; VertexInstanceSectionIndex < NumVertexInstancesPerSection; VertexInstanceSectionIndex++)
				{
					uint32 VertexIndex = Primitive.Indices[VertexInstanceSectionIndex];
					LODIndices[VertexInstanceBaseIndex + VertexInstanceSectionIndex] = VertexInstanceBaseIndex + VertexInstanceSectionIndex;

					FStaticMeshBuildVertex& StaticMeshVertex = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex];

#if ENGINE_MAJOR_VERSION > 4
					StaticMeshVertex.Position = FVector3f(GetSafeValue(Primitive.Positions, VertexIndex, FVector::ZeroVector, bMissingIgnore));
#else
					StaticMeshVertex.Position = GetSafeValue(Primitive.Positions, VertexIndex, FVector::ZeroVector, bMissingIgnore);
#endif

					FVector4 TangentX = GetSafeValue(Primitive.Tangents, VertexIndex, FVector4(0, 0, 0, 1), bMissingTangents);
#if ENGINE_MAJOR_VERSION > 4
					StaticMeshVertex.TangentX = FVector4f(TangentX);
					StaticMeshVertex.TangentZ = FVector3f(GetSafeValue(Primitive.Normals, VertexIndex, FVector::ZeroVector, bMissingNormals));
					StaticMeshVertex.TangentY = FVector3f(ComputeTangentYWithW(FVector(StaticMeshVertex.TangentZ), FVector(StaticMeshVertex.TangentX), TangentX.W * TangentsDirection));
#else
					StaticMeshVertex.TangentX = TangentX;
					StaticMeshVertex.TangentZ = GetSafeValue(Primitive.Normals, VertexIndex, FVector::ZeroVector, bMissingNormals);
					StaticMeshVertex.TangentY = ComputeTangentYWithW(StaticMeshVertex.TangentZ, StaticMeshVertex.TangentX, TangentX.W * TangentsDirection);
#endif

					for (int32 UVIndex = 0; UVIndex < NumUVs; UVIndex++)
					{
						if (UVIndex < Primitive.UVs.Num())
						{
#if ENGINE_MAJOR_VERSION > 4
							StaticMeshVertex.UVs[UVIndex] = FVector2f(GetSafeValue(Primitive.UVs[UVIndex], VertexIndex, FVector2D::ZeroVector, bMissingIgnore));
#else
							StaticMeshVertex.UVs[UVIndex] = GetSafeValue(Primitive.UVs[UVIndex], VertexIndex, FVector2D::ZeroVector, bMissingIgnore);
#endif
						}
					}

					if (bHasVertexColors)
					{
						if (VertexIndex < static_cast<uint32>(Primitive.Colors.Num()))
						{
							StaticMeshVertex.Color = FLinearColor(Primitive.Colors[VertexIndex]).ToFColor(true);
						}
						else
						{
							StaticMeshVertex.Color = FColor::White;
						}
					}

					if (bApplyAdditionalTransforms)
					{
						UE_LOG(LogTemp, Log, TEXT("Additional transforms wanted for triangles."));
#if ENGINE_MAJOR_VERSION > 4
						StaticMeshVertex.Position = FVector3f(LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformPosition(FVector3d(StaticMeshVertex.Position)));
						StaticMeshVertex.TangentX = FVector3f(LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformVectorNoScale(FVector3d(StaticMeshVertex.TangentX)));
						StaticMeshVertex.TangentY = FVector3f(LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformVectorNoScale(FVector3d(StaticMeshVertex.TangentY)));
						StaticMeshVertex.TangentZ = FVector3f(LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformVectorNoScale(FVector3d(StaticMeshVertex.TangentZ)));
#else
						StaticMeshVertex.Position = LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformPosition(StaticMeshVertex.Position);
						StaticMeshVertex.TangentX = LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformVectorNoScale(StaticMeshVertex.TangentX);
						StaticMeshVertex.TangentY = LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformVectorNoScale(StaticMeshVertex.TangentY);
						StaticMeshVertex.TangentZ = LOD->AdditionalTransforms[AdditionalTransformsPrimitiveIndex].TransformVectorNoScale(StaticMeshVertex.TangentZ);
#endif
					}

#if ENGINE_MAJOR_VERSION > 4
					BoundingBox += FVector(StaticMeshVertex.Position);
#else
					BoundingBox += StaticMeshVertex.Position;
#endif
				}
				// End of Geometry generation

				AdditionalTransformsPrimitiveIndex++;

				if (StaticMeshConfig.bReverseWinding && (NumVertexInstancesPerSection % 3) == 0)
				{
					for (int32 VertexInstanceSectionIndex = 0; VertexInstanceSectionIndex < NumVertexInstancesPerSection; VertexInstanceSectionIndex += 3)
					{
						FStaticMeshBuildVertex StaticMeshVertex1 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 1];
						FStaticMeshBuildVertex StaticMeshVertex2 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 2];

						StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 1] = StaticMeshVertex2;
						StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 2] = StaticMeshVertex1;
					}
				}

				const bool bCanGenerateNormals = (bMissingNormals && StaticMeshConfig.NormalsGenerationStrategy == EglTFRuntimeNormalsGenerationStrategy::IfMissing) ||
					StaticMeshConfig.NormalsGenerationStrategy == EglTFRuntimeNormalsGenerationStrategy::Always;
				if (bCanGenerateNormals && (NumVertexInstancesPerSection % 3) == 0)
				{
					for (int32 VertexInstanceSectionIndex = 0; VertexInstanceSectionIndex < NumVertexInstancesPerSection; VertexInstanceSectionIndex += 3)
					{
						FStaticMeshBuildVertex& StaticMeshVertex0 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex];
						FStaticMeshBuildVertex& StaticMeshVertex1 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 1];
						FStaticMeshBuildVertex& StaticMeshVertex2 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 2];

#if ENGINE_MAJOR_VERSION > 4
						FVector SideA = FVector(StaticMeshVertex1.Position - StaticMeshVertex0.Position);
						FVector SideB = FVector(StaticMeshVertex2.Position - StaticMeshVertex0.Position);
						FVector NormalFromCross = FVector::CrossProduct(SideB, SideA).GetSafeNormal();

						StaticMeshVertex0.TangentZ = FVector3f(NormalFromCross);
						StaticMeshVertex1.TangentZ = FVector3f(NormalFromCross);
						StaticMeshVertex2.TangentZ = FVector3f(NormalFromCross);
#else
						FVector SideA = StaticMeshVertex1.Position - StaticMeshVertex0.Position;
						FVector SideB = StaticMeshVertex2.Position - StaticMeshVertex0.Position;
						FVector NormalFromCross = FVector::CrossProduct(SideB, SideA).GetSafeNormal();

						StaticMeshVertex0.TangentZ = NormalFromCross;
						StaticMeshVertex1.TangentZ = NormalFromCross;
						StaticMeshVertex2.TangentZ = NormalFromCross;
#endif


					}
					bMissingNormals = false;
				}

				const bool bCanGenerateTangents = (bMissingTangents && StaticMeshConfig.TangentsGenerationStrategy == EglTFRuntimeTangentsGenerationStrategy::IfMissing) ||
					StaticMeshConfig.TangentsGenerationStrategy == EglTFRuntimeTangentsGenerationStrategy::Always;
				// recompute tangents if required (need normals and uvs)
				if (bCanGenerateTangents && !bMissingNormals && Primitive.UVs.Num() > 0 && (NumVertexInstancesPerSection % 3) == 0)
				{
					for (int32 VertexInstanceSectionIndex = 0; VertexInstanceSectionIndex < NumVertexInstancesPerSection; VertexInstanceSectionIndex += 3)
					{
						FStaticMeshBuildVertex& StaticMeshVertex0 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex];
						FStaticMeshBuildVertex& StaticMeshVertex1 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 1];
						FStaticMeshBuildVertex& StaticMeshVertex2 = StaticMeshBuildVertices[VertexInstanceBaseIndex + VertexInstanceSectionIndex + 2];


#if ENGINE_MAJOR_VERSION > 4
						FVector Position0 = FVector(StaticMeshVertex0.Position);
						FVector4 TangentZ0 = FVector(StaticMeshVertex0.TangentZ);
						FVector2D UV0 = FVector2D(StaticMeshVertex0.UVs[0]);
#else
						FVector Position0 = StaticMeshVertex0.Position;
						FVector4 TangentZ0 = StaticMeshVertex0.TangentZ;
						FVector2D UV0 = StaticMeshVertex0.UVs[0];
#endif



#if ENGINE_MAJOR_VERSION > 4
						FVector Position1 = FVector(StaticMeshVertex1.Position);
						FVector4 TangentZ1 = FVector(StaticMeshVertex1.TangentZ);
						FVector2D UV1 = FVector2D(StaticMeshVertex1.UVs[0]);
#else
						FVector Position1 = StaticMeshVertex1.Position;
						FVector4 TangentZ1 = StaticMeshVertex1.TangentZ;
						FVector2D UV1 = StaticMeshVertex1.UVs[0];
#endif



#if ENGINE_MAJOR_VERSION > 4
						FVector Position2 = FVector(StaticMeshVertex2.Position);
						FVector4 TangentZ2 = FVector(StaticMeshVertex2.TangentZ);
						FVector2D UV2 = FVector2D(StaticMeshVertex2.UVs[0]);
#else
						FVector Position2 = StaticMeshVertex2.Position;
						FVector4 TangentZ2 = StaticMeshVertex2.TangentZ;
						FVector2D UV2 = StaticMeshVertex2.UVs[0];
#endif


						FVector DeltaPosition0 = Position1 - Position0;
						FVector DeltaPosition1 = Position2 - Position0;

						FVector2D DeltaUV0 = UV1 - UV0;
						FVector2D DeltaUV1 = UV2 - UV0;

						float Factor = 1.0f / (DeltaUV0.X * DeltaUV1.Y - DeltaUV0.Y * DeltaUV1.X);

						FVector TriangleTangentX = ((DeltaPosition0 * DeltaUV1.Y) - (DeltaPosition1 * DeltaUV0.Y)) * Factor;
						FVector TriangleTangentY = ((DeltaPosition0 * DeltaUV1.X) - (DeltaPosition1 * DeltaUV0.X)) * Factor;

						FVector TangentX0 = TriangleTangentX - (TangentZ0 * FVector::DotProduct(TangentZ0, TriangleTangentX));
						TangentX0.Normalize();

						FVector TangentX1 = TriangleTangentX - (TangentZ1 * FVector::DotProduct(TangentZ1, TriangleTangentX));
						TangentX1.Normalize();

						FVector TangentX2 = TriangleTangentX - (TangentZ2 * FVector::DotProduct(TangentZ2, TriangleTangentX));
						TangentX2.Normalize();

#if ENGINE_MAJOR_VERSION > 4
						StaticMeshVertex0.TangentX = FVector3f(TangentX0);
						StaticMeshVertex0.TangentY = FVector3f(ComputeTangentY(FVector(StaticMeshVertex0.TangentZ), FVector(StaticMeshVertex0.TangentX)) * TangentsDirection);

						StaticMeshVertex1.TangentX = FVector3f(TangentX1);
						StaticMeshVertex1.TangentY = FVector3f(ComputeTangentY(FVector(StaticMeshVertex1.TangentZ), FVector(StaticMeshVertex1.TangentX)) * TangentsDirection);

						StaticMeshVertex2.TangentX = FVector3f(TangentX2);
						StaticMeshVertex2.TangentY = FVector3f(ComputeTangentY(FVector(StaticMeshVertex2.TangentZ), FVector(StaticMeshVertex2.TangentX)) * TangentsDirection);
#else
						StaticMeshVertex0.TangentX = TangentX0;
						StaticMeshVertex0.TangentY = ComputeTangentY(StaticMeshVertex0.TangentZ, StaticMeshVertex0.TangentX) * TangentsDirection;

						StaticMeshVertex1.TangentX = TangentX1;
						StaticMeshVertex1.TangentY = ComputeTangentY(StaticMeshVertex1.TangentZ, StaticMeshVertex1.TangentX) * TangentsDirection;

						StaticMeshVertex2.TangentX = TangentX2;
						StaticMeshVertex2.TangentY = ComputeTangentY(StaticMeshVertex2.TangentZ, StaticMeshVertex2.TangentX) * TangentsDirection;
#endif

					}
				}

				VertexInstanceBaseIndex += NumVertexInstancesPerSection;
			}
		}

		// check for pivot repositioning
		if (StaticMeshConfig.PivotPosition != EglTFRuntimePivotPosition::Asset)
		{
			if (StaticMeshConfig.PivotPosition == EglTFRuntimePivotPosition::Center)
			{
				PivotDelta = BoundingBox.GetCenter();
			}
			else if (StaticMeshConfig.PivotPosition == EglTFRuntimePivotPosition::Top)
			{
				PivotDelta = BoundingBox.GetCenter() + FVector(0, 0, BoundingBox.GetExtent().Z);
			}
			else if (StaticMeshConfig.PivotPosition == EglTFRuntimePivotPosition::Bottom)
			{
				PivotDelta = BoundingBox.GetCenter() - FVector(0, 0, BoundingBox.GetExtent().Z);
			}

			for (FStaticMeshBuildVertex& StaticMeshVertex : StaticMeshBuildVertices)
			{
#if ENGINE_MAJOR_VERSION > 4
				StaticMeshVertex.Position -= FVector3f(PivotDelta);
#else
				StaticMeshVertex.Position -= PivotDelta;
#endif
			}

			if (CurrentLODIndex == 0)
			{
				StaticMeshContext->LOD0PivotDelta = PivotDelta;
			}
		}

		if (CurrentLODIndex == 0)
		{
			BoundingBox.GetCenterAndExtents(StaticMeshContext->BoundingBoxAndSphere.Origin, StaticMeshContext->BoundingBoxAndSphere.BoxExtent);
			StaticMeshContext->BoundingBoxAndSphere.SphereRadius = 0.0f;
			for (const FStaticMeshBuildVertex& StaticMeshVertex : StaticMeshBuildVertices)
			{
#if ENGINE_MAJOR_VERSION > 4
				StaticMeshContext->BoundingBoxAndSphere.SphereRadius = FMath::Max((FVector(StaticMeshVertex.Position) - StaticMeshContext->BoundingBoxAndSphere.Origin).Size(), StaticMeshContext->BoundingBoxAndSphere.SphereRadius);
#else
				StaticMeshContext->BoundingBoxAndSphere.SphereRadius = FMath::Max((StaticMeshVertex.Position - StaticMeshContext->BoundingBoxAndSphere.Origin).Size(), StaticMeshContext->BoundingBoxAndSphere.SphereRadius);
#endif
			}

			StaticMeshContext->BoundingBoxAndSphere.Origin -= PivotDelta;
		}

		LODResources.VertexBuffers.PositionVertexBuffer.Init(StaticMeshBuildVertices, StaticMesh->bAllowCPUAccess);
		LODResources.VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(bHighPrecisionUVs || StaticMeshConfig.bUseHighPrecisionUVs);
		LODResources.VertexBuffers.StaticMeshVertexBuffer.Init(StaticMeshBuildVertices, NumUVs, StaticMesh->bAllowCPUAccess);
		if (bHasVertexColors)
		{
			LODResources.VertexBuffers.ColorVertexBuffer.Init(StaticMeshBuildVertices, StaticMesh->bAllowCPUAccess);
		}
		LODResources.bHasColorVertexData = bHasVertexColors;
		if (StaticMesh->bAllowCPUAccess)
		{
			LODResources.IndexBuffer = FRawStaticIndexBuffer(true);
		}
		LODResources.IndexBuffer.SetIndices(LODIndices, StaticMeshBuildVertices.Num() > MAX_uint16 ? EIndexBufferStride::Force32Bit : EIndexBufferStride::Force16Bit);

#if WITH_EDITOR
		if (StaticMeshConfig.bGenerateStaticMeshDescription)
		{
			FStaticMeshSourceModel& SourceModel = StaticMesh->AddSourceModel();
			FMeshDescription* MeshDescription = StaticMesh->CreateMeshDescription(CurrentLODIndex);
			FStaticMeshAttributes StaticMeshAttributes(*MeshDescription);
#if ENGINE_MAJOR_VERSION > 4

			TVertexAttributesRef<FVector3f> MeshDescriptionPositions = MeshDescription->GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = StaticMeshAttributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = StaticMeshAttributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = StaticMeshAttributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = StaticMeshAttributes.GetVertexInstanceColors();
			VertexInstanceUVs.SetNumChannels(NumUVs);
#else
			TVertexAttributesRef<FVector> MeshDescriptionPositions = StaticMeshAttributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector> VertexInstanceNormals = StaticMeshAttributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector> VertexInstanceTangents = StaticMeshAttributes.GetVertexInstanceTangents();
			TVertexInstanceAttributesRef<FVector2D> VertexInstanceUVs = StaticMeshAttributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4> VertexInstanceColors = StaticMeshAttributes.GetVertexInstanceColors();
			VertexInstanceUVs.SetNumIndices(NumUVs);
#endif

			for (int32 PositionIndex = 0; PositionIndex < StaticMeshBuildVertices.Num(); PositionIndex++)
			{
				MeshDescription->CreateVertexWithID(FVertexID(PositionIndex));
				MeshDescriptionPositions[FVertexID(PositionIndex)] = StaticMeshBuildVertices[PositionIndex].Position;
			}

			TArray<TPair<uint32, FPolygonGroupID>> PolygonGroups;
			for (const FStaticMeshSection& Section : LODResources.Sections)
			{
				const FPolygonGroupID PolygonGroupID = MeshDescription->CreatePolygonGroup();
				PolygonGroups.Add(TPair<uint32, FPolygonGroupID>(Section.FirstIndex, PolygonGroupID));
			}

			int32 CurrentPolygonGroupIndex = 0;
			uint32 CleanedNumOfIndices = (LODIndices.Num() / 3) * 3; // avoid crash on non triangles...
			for (uint32 VertexIndex = 0; VertexIndex < CleanedNumOfIndices; VertexIndex += 3)
			{
				const FVertexInstanceID VertexInstanceID0 = MeshDescription->CreateVertexInstance(FVertexID(LODIndices[VertexIndex]));;
				const FVertexInstanceID VertexInstanceID1 = MeshDescription->CreateVertexInstance(FVertexID(LODIndices[VertexIndex + 1]));
				const FVertexInstanceID VertexInstanceID2 = MeshDescription->CreateVertexInstance(FVertexID(LODIndices[VertexIndex + 2]));

				VertexInstanceNormals[VertexInstanceID0] = StaticMeshBuildVertices[LODIndices[VertexIndex]].TangentZ;
				VertexInstanceTangents[VertexInstanceID0] = StaticMeshBuildVertices[LODIndices[VertexIndex]].TangentX;
				VertexInstanceNormals[VertexInstanceID1] = StaticMeshBuildVertices[LODIndices[VertexIndex + 1]].TangentZ;
				VertexInstanceTangents[VertexInstanceID1] = StaticMeshBuildVertices[LODIndices[VertexIndex + 1]].TangentX;
				VertexInstanceNormals[VertexInstanceID2] = StaticMeshBuildVertices[LODIndices[VertexIndex + 2]].TangentZ;
				VertexInstanceTangents[VertexInstanceID2] = StaticMeshBuildVertices[LODIndices[VertexIndex + 2]].TangentX;

				for (int32 UVIndex = 0; UVIndex < NumUVs; UVIndex++)
				{
					VertexInstanceUVs.Set(VertexInstanceID0, UVIndex, StaticMeshBuildVertices[LODIndices[VertexIndex]].UVs[UVIndex]);
					VertexInstanceUVs.Set(VertexInstanceID1, UVIndex, StaticMeshBuildVertices[LODIndices[VertexIndex + 1]].UVs[UVIndex]);
					VertexInstanceUVs.Set(VertexInstanceID2, UVIndex, StaticMeshBuildVertices[LODIndices[VertexIndex + 2]].UVs[UVIndex]);
				}

				if (bHasVertexColors)
				{
					VertexInstanceColors[VertexInstanceID0] = FLinearColor(StaticMeshBuildVertices[LODIndices[VertexIndex]].Color);
					VertexInstanceColors[VertexInstanceID1] = FLinearColor(StaticMeshBuildVertices[LODIndices[VertexIndex + 1]].Color);
					VertexInstanceColors[VertexInstanceID2] = FLinearColor(StaticMeshBuildVertices[LODIndices[VertexIndex + 2]].Color);
				}

				// safe approach given that the section array is built in order
				if (CurrentPolygonGroupIndex + 1 < PolygonGroups.Num())
				{
					if (VertexIndex >= PolygonGroups[CurrentPolygonGroupIndex + 1].Key)
					{
						CurrentPolygonGroupIndex++;
					}
				}
				const FPolygonGroupID PolygonGroupID = PolygonGroups[CurrentPolygonGroupIndex].Value;

				MeshDescription->CreateTriangle(PolygonGroupID, { VertexInstanceID0, VertexInstanceID1, VertexInstanceID2 });
				
			}
			StaticMesh->CommitMeshDescription(CurrentLODIndex);

		}
#endif
	}

	OnPostCreatedStaticMesh.Broadcast(StaticMeshContext);

	return StaticMesh;
}

// Implementation of code from Andre Mühlenbrock, 2020
// See https://www.youtube.com/watch?v=TmG-XIxaLVQ&ab_channel=phirede
void FglTFRuntimeParser::InjectPointcloudData(UNiagaraComponent* PointCloud, TArray<float>& Positions, TArray<uint8_t>& Colors, const int32 PointCount) {
	
	// Find an appropriate texture height and width
	// (Each edge length a power of 2, approximately square shaped, and as small as possible)
	float SqrtPointCount = UKismetMathLibrary::Sqrt(PointCount);
	int32 SqrtNextPow2 = pow(2, ceil(log(SqrtPointCount) / log(2)));
	int32 TextureWidth = SqrtNextPow2;
	int32 TextureHeight = (SqrtNextPow2 * SqrtNextPow2 / 2 > PointCount) ? SqrtNextPow2 / 2 : SqrtNextPow2;

	// Pad positions and colors to size of texture in order to prevent raw memory being read/written later.
	// Initial size is 4 * PointCount
	// Final size should be 4 * TextureWidth * TextureHeight
	Positions.AddDefaulted(4 * (TextureWidth * TextureHeight - PointCount));
	Colors.AddDefaulted(4 * (TextureWidth * TextureHeight - PointCount));

	Region = FUpdateTextureRegion2D(0, 0, 0, 0, TextureWidth, TextureHeight);
	UE_LOG(LogTemp, Log, TEXT("Region Width: %d"), Region.Width);
	UE_LOG(LogTemp, Log, TEXT("Region Height: %d"), Region.Height);

	// Create textures
	PositionTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_A32B32G32R32F, "PositionData");
	PositionTexture->Filter = TF_Nearest;
	PositionTexture->UpdateResource();

	ColorTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_B8G8R8A8, "ColorTexture");
	ColorTexture->Filter = TF_Nearest;
	ColorTexture->UpdateResource();

	// Set the niagara system user variables:
	SetNiagaraVariableTexture(PointCloud, "User.PositionTexture", PositionTexture);
	SetNiagaraVariableTexture(PointCloud, "User.ColorTexture", ColorTexture);

	PointCloud->SetVariableInt("User.Count", PointCount);
	PointCloud->SetVariableInt("User.TextureWidth", TextureWidth);
	PointCloud->SetVariableInt("User.TextureHeight", TextureHeight);


	UE_LOG(LogTemp, Log, TEXT("PositionData nominal size: %d"), 4 * PointCount * sizeof(float));
	UE_LOG(LogTemp, Log, TEXT("ColorData nominal size: %d"), 4 * PointCount * sizeof(uint8_t));

	uint8* TexturePositionData = new uint8[4 * PointCount * sizeof(float)];
	uint8* TextureColorData = new uint8[4 * PointCount * sizeof(uint8_t)];

	TexturePositionData = (uint8*)Positions.GetData();
	TextureColorData = (uint8*)Colors.GetData();

	UE_LOG(LogTemp, Log, TEXT("PositionData size: %d"), sizeof(TexturePositionData));
	UE_LOG(LogTemp, Log, TEXT("ColorData size: %d"), sizeof(TextureColorData));

	UE_LOG(LogTemp, Log, TEXT("PositionData pointing to size: %d"), sizeof(*TexturePositionData));
	UE_LOG(LogTemp, Log, TEXT("ColorData pointing to size: %d"), sizeof(*TextureColorData));

	// Bring the data into the texture
	uint32 PositionPixelWidth = 4 * sizeof(float);
	uint32 PositionPitch = TextureWidth * PositionPixelWidth;


	UE_LOG(LogTemp, Log, TEXT("Color Pitch: %d"), PositionPitch);
	UE_LOG(LogTemp, Log, TEXT("Color Pixel: %d"), PositionPixelWidth);
	
	PositionTexture->UpdateTextureRegions(0, 1, &Region, PositionPitch, PositionPixelWidth,
		TexturePositionData);

	delete [] TexturePositionData;

	uint32 ColorPixelWidth = 4 * sizeof(uint8_t);
	uint32 ColorPitch = TextureWidth * ColorPixelWidth;

	UE_LOG(LogTemp, Log, TEXT("Color Pitch: %d"), ColorPitch);
	UE_LOG(LogTemp, Log, TEXT("Color Pixel: %d"), ColorPixelWidth);

	ColorTexture->UpdateTextureRegions(0, 1, &Region, ColorPitch, ColorPixelWidth, TextureColorData);

	delete [] TextureColorData;
}


/**
 * Just a helper method to set a texture for a user variable because the UNiagaraComponent
 * has no direct way to edit a texture variable compared to float, vectors, ...
 */
void FglTFRuntimeParser::SetNiagaraVariableTexture(UNiagaraComponent* PointCloud, const FString VariableName, UTexture* Texture) {
	if (!PointCloud || !Texture)
		return;

	FNiagaraUserRedirectionParameterStore& OverrideParameters = PointCloud->GetOverrideParameters();
	FNiagaraVariable NiagaraVariable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceTexture::StaticClass()), *VariableName);

	UNiagaraDataInterfaceTexture* DataInterface = (UNiagaraDataInterfaceTexture*)OverrideParameters.GetDataInterface(NiagaraVariable);
	DataInterface->SetTexture(Texture);
}


UStaticMesh* FglTFRuntimeParser::FinalizeStaticMesh(TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext)
{
	SCOPED_NAMED_EVENT(FglTFRuntimeParser_FinalizeStaticMesh, FColor::Magenta);

	UStaticMesh* StaticMesh = StaticMeshContext->StaticMesh;
	FStaticMeshRenderData* RenderData = StaticMeshContext->RenderData;
	const FglTFRuntimeStaticMeshConfig& StaticMeshConfig = StaticMeshContext->StaticMeshConfig;

#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MINOR_VERSION > 26)
	StaticMesh->SetStaticMaterials(StaticMeshContext->StaticMaterials);
#else
	StaticMesh->StaticMaterials = StaticMeshContext->StaticMaterials;
#endif

#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MINOR_VERSION > 26)
	UBodySetup* BodySetup = StaticMesh->GetBodySetup();
#else
	UBodySetup* BodySetup = StaticMesh->BodySetup;
#endif

	StaticMesh->InitResources();

	// set default LODs screen sizes
	float DeltaScreenSize = (1.0f / RenderData->LODResources.Num()) / StaticMeshConfig.LODScreenSizeMultiplier;
	float ScreenSize = 1;
	for (int32 LODIndex = 0; LODIndex < RenderData->LODResources.Num(); LODIndex++)
	{
		RenderData->ScreenSize[LODIndex].Default = ScreenSize;
		ScreenSize -= DeltaScreenSize;
	}

	// Override LODs ScreenSize
	for (const TPair<int32, float>& Pair : StaticMeshConfig.LODScreenSize)
	{
		int32 CurrentLODIndex = Pair.Key;
		if (RenderData && CurrentLODIndex >= 0 && CurrentLODIndex < RenderData->LODResources.Num())
		{
			RenderData->ScreenSize[CurrentLODIndex].Default = Pair.Value;
		}
	}

	RenderData->Bounds = StaticMeshContext->BoundingBoxAndSphere;
	StaticMesh->CalculateExtendedBounds();

	if (!BodySetup)
	{
		StaticMesh->CreateBodySetup();
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MINOR_VERSION > 26)
		BodySetup = StaticMesh->GetBodySetup();
#else
		BodySetup = StaticMesh->BodySetup;
#endif
	}

	BodySetup->bHasCookedCollisionData = false;

	BodySetup->bNeverNeedsCookedCollisionData = !StaticMeshConfig.bBuildComplexCollision;

	BodySetup->bMeshCollideAll = false;

	BodySetup->CollisionTraceFlag = StaticMeshConfig.CollisionComplexity;

	BodySetup->InvalidatePhysicsData();
	
	if (StaticMeshConfig.bBuildSimpleCollision)
	{
		FKBoxElem BoxElem;
		BoxElem.Center = RenderData->Bounds.Origin;
		BoxElem.X = RenderData->Bounds.BoxExtent.X * 2.0f;
		BoxElem.Y = RenderData->Bounds.BoxExtent.Y * 2.0f;
		BoxElem.Z = RenderData->Bounds.BoxExtent.Z * 2.0f;
		BodySetup->AggGeom.BoxElems.Add(BoxElem);
	}

	for (const FBox& Box : StaticMeshConfig.BoxCollisions)
	{
		FKBoxElem BoxElem;
		BoxElem.Center = Box.GetCenter();
		FVector BoxSize = Box.GetSize();
		BoxElem.X = BoxSize.X;
		BoxElem.Y = BoxSize.Y;
		BoxElem.Z = BoxSize.Z;
		BodySetup->AggGeom.BoxElems.Add(BoxElem);
	}

	for (const FVector4 Sphere : StaticMeshConfig.SphereCollisions)
	{
		FKSphereElem SphereElem;
		SphereElem.Center = Sphere;
		SphereElem.Radius = Sphere.W;
		BodySetup->AggGeom.SphereElems.Add(SphereElem);
	}

	if (StaticMeshConfig.bBuildComplexCollision || StaticMeshConfig.CollisionComplexity == ECollisionTraceFlag::CTF_UseComplexAsSimple)
	{
		if (!StaticMesh->bAllowCPUAccess || !StaticMeshConfig.Outer || !StaticMesh->GetWorld() || !StaticMesh->GetWorld()->IsGameWorld())
		{
			AddError("FinalizeStaticMesh", "Unable to generate Complex collision without CpuAccess and a valid StaticMesh Outer (consider setting it to the related StaticMeshComponent)");
		}
		BodySetup->CreatePhysicsMeshes();
	}

	// recreate physics state (if possible)
	if (UActorComponent* ActorComponent = Cast<UActorComponent>(StaticMesh->GetOuter()))
	{
		// TODO: Uncomment and fix.
		// Currently freezes for some reason.
		// 
		//ActorComponent->RecreatePhysicsState();
	}

	for (const TPair<FString, FTransform>& Pair : StaticMeshConfig.Sockets)
	{
		UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(StaticMesh);
		Socket->SocketName = FName(Pair.Key);
		Socket->RelativeLocation = Pair.Value.GetLocation();
		Socket->RelativeRotation = Pair.Value.Rotator();
		Socket->RelativeScale = Pair.Value.GetScale3D();
		StaticMesh->AddSocket(Socket);
	}

	for (const TPair<FString, FTransform>& Pair : StaticMeshContext->AdditionalSockets)
	{
		if (StaticMeshConfig.Sockets.Contains(Pair.Key))
		{
			continue;
		}

		UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(StaticMesh);
		Socket->SocketName = FName(Pair.Key);
		Socket->RelativeLocation = Pair.Value.GetLocation();
		Socket->RelativeRotation = Pair.Value.Rotator();
		Socket->RelativeScale = Pair.Value.GetScale3D();
		StaticMesh->AddSocket(Socket);
	}

	if (!StaticMeshConfig.ExportOriginalPivotToSocket.IsEmpty())
	{
		UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(StaticMesh);
		Socket->SocketName = FName(StaticMeshConfig.ExportOriginalPivotToSocket);
		Socket->RelativeLocation = -StaticMeshContext->LOD0PivotDelta;
		StaticMesh->AddSocket(Socket);
	}

	StaticMesh->bHasNavigationData = StaticMeshConfig.bBuildNavCollision;

	if (StaticMesh->bHasNavigationData)
	{
		StaticMesh->CreateNavCollision();
	}

	OnFinalizedStaticMesh.Broadcast(AsShared(), StaticMesh, StaticMeshConfig);

	OnStaticMeshCreated.Broadcast(StaticMesh);

	return StaticMesh;
}

bool FglTFRuntimeParser::LoadStaticMeshes(TArray<UStaticMesh*>& StaticMeshes, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonMeshes;
	// no meshes ?
	if (!Root->TryGetArrayField("meshes", JsonMeshes))
	{
		return false;
	}

	for (int32 Index = 0; Index < JsonMeshes->Num(); Index++)
	{
		UStaticMesh* StaticMesh = LoadStaticMesh(Index, StaticMeshConfig);
		if (!StaticMesh)
		{
			return false;
		}
		StaticMeshes.Add(StaticMesh);
	}

	return true;
}

bool FglTFRuntimeParser::LoadMeshIntoMeshLOD(TSharedRef<FJsonObject> JsonMeshObject, FglTFRuntimeMeshLOD*& LOD, const FglTFRuntimeMaterialsConfig& MaterialsConfig)
{
	if (LODsCache.Contains(JsonMeshObject))
	{
		LOD = &LODsCache[JsonMeshObject];
		return true;
	}

	TArray<FglTFRuntimePrimitive> Primitives;
	if (!LoadPrimitives(JsonMeshObject, Primitives, MaterialsConfig))
	{
		return false;
	}

	FglTFRuntimeMeshLOD NewLOD;
	NewLOD.Primitives = MoveTemp(Primitives);

	FglTFRuntimeMeshLOD& CachedLOD = LODsCache.Add(JsonMeshObject, MoveTemp(NewLOD));
	LOD = &CachedLOD;
	return true;
}

UStaticMesh* FglTFRuntimeParser::LoadStaticMesh(const int32 MeshIndex, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{

	TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
	if (!JsonMeshObject)
	{
		return nullptr;
	}

	if (CanReadFromCache(StaticMeshConfig.CacheMode) && StaticMeshesCache.Contains(MeshIndex))
	{
		return StaticMeshesCache[MeshIndex];
	}

	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);
	FglTFRuntimeMeshLOD* LOD = nullptr;
	if (!LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, StaticMeshConfig.MaterialsConfig))
	{
		return nullptr;
	}
	StaticMeshContext->LODs.Add(LOD);

	UStaticMesh* StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);
	if (!StaticMesh)
	{
		return nullptr;
	}

	StaticMesh = FinalizeStaticMesh(StaticMeshContext);
	if (!StaticMesh)
	{
		return nullptr;
	}

	if (CanWriteToCache(StaticMeshConfig.CacheMode))
	{
		StaticMeshesCache.Add(MeshIndex, StaticMesh);
	}

	return StaticMesh;
}

TArray<UStaticMesh*> FglTFRuntimeParser::LoadStaticMeshesFromPrimitives(const int32 MeshIndex, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	TArray<UStaticMesh*> StaticMeshes;

	TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
	if (!JsonMeshObject)
	{
		return StaticMeshes;
	}

	FglTFRuntimeMeshLOD* LOD = nullptr;
	if (!LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, StaticMeshConfig.MaterialsConfig))
	{
		return StaticMeshes;
	}

	for (FglTFRuntimePrimitive& Primitive : LOD->Primitives)
	{
		TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);

		FglTFRuntimeMeshLOD PrimitiveLOD;
		PrimitiveLOD.Primitives.Add(Primitive);

		StaticMeshContext->LODs.Add(&PrimitiveLOD);

		UStaticMesh* StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);
		if (!StaticMesh)
		{
			break;
		}

		StaticMesh = FinalizeStaticMesh(StaticMeshContext);
		if (!StaticMesh)
		{
			break;
		}

		StaticMeshes.Add(StaticMesh);
	}

	return StaticMeshes;
}

UStaticMesh* FglTFRuntimeParser::LoadStaticMeshLODs(const TArray<int32>& MeshIndices, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig, UStaticMeshComponent*& StaticMeshComponent)
{

	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);

	for (const int32 MeshIndex : MeshIndices)
	{
		TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
		if (!JsonMeshObject)
		{
			return nullptr;
		}

		FglTFRuntimeMeshLOD* LOD = nullptr;

		if (!LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, StaticMeshConfig.MaterialsConfig))
		{
			return nullptr;
		}

		StaticMeshContext->LODs.Add(LOD);
	}

 	UStaticMesh* StaticMesh = LoadStaticMesh_Internal(StaticMeshContext, StaticMeshComponent);

	if (StaticMesh)
	{
		if (bFinalizeStaticMesh) {
			return FinalizeStaticMesh(StaticMeshContext);
		}
		else
		{
			return StaticMesh;
		}
	}
	return nullptr;
}

void FglTFRuntimeParser::LoadStaticMeshLODsAsync(const TArray<int32>& MeshIndices, const FglTFRuntimeStaticMeshAsync& AsyncCallback, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);

	Async(EAsyncExecution::Thread, [this, StaticMeshContext, MeshIndices, AsyncCallback]()
		{
			bool bSuccess = true;
			for (const int32 MeshIndex : MeshIndices)
			{
				TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
				if (!JsonMeshObject)
				{
					bSuccess = false;
					break;
				}

				FglTFRuntimeMeshLOD* LOD = nullptr;

				if (!LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, StaticMeshContext->StaticMeshConfig.MaterialsConfig))
				{
					bSuccess = false;
					break;
				}

				StaticMeshContext->LODs.Add(LOD);
			}

			if (bSuccess)
			{
				StaticMeshContext->StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);
			}

			FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([StaticMeshContext, AsyncCallback]()
				{
					if (StaticMeshContext->StaticMesh)
					{
						StaticMeshContext->StaticMesh = StaticMeshContext->Parser->FinalizeStaticMesh(StaticMeshContext);
					}

					AsyncCallback.ExecuteIfBound(StaticMeshContext->StaticMesh);
				}, TStatId(), nullptr, ENamedThreads::GameThread);
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
		});
}

bool FglTFRuntimeParser::LoadStaticMeshIntoProceduralMeshComponent(const int32 MeshIndex, UProceduralMeshComponent* ProceduralMeshComponent, const FglTFRuntimeProceduralMeshConfig& ProceduralMeshConfig)
{
	if (!ProceduralMeshComponent)
	{
		return false;
	}

	TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
	if (!JsonMeshObject)
	{
		return false;
	}

	TArray<FglTFRuntimePrimitive> Primitives;
	if (!LoadPrimitives(JsonMeshObject.ToSharedRef(), Primitives, ProceduralMeshConfig.MaterialsConfig))
	{
		return false;
	}

	ProceduralMeshComponent->bUseComplexAsSimpleCollision = ProceduralMeshConfig.bUseComplexAsSimpleCollision;

	int32 SectionIndex = ProceduralMeshComponent->GetNumSections();
	for (FglTFRuntimePrimitive& Primitive : Primitives)
	{
		TArray<FVector2D> UV;
		if (Primitive.UVs.Num() > 0)
		{
			UV = Primitive.UVs[0];
		}
		TArray<int32> Triangles;
		Triangles.AddUninitialized(Primitive.Indices.Num());
		for (int32 Index = 0; Index < Primitive.Indices.Num(); Index++)
		{
			Triangles[Index] = Primitive.Indices[Index];
		}
		TArray<FLinearColor> Colors;
		Colors.AddUninitialized(Primitive.Colors.Num());
		for (int32 Index = 0; Index < Primitive.Colors.Num(); Index++)
		{
			Colors[Index] = FLinearColor(Primitive.Colors[Index]);
		}
		TArray<FProcMeshTangent> Tangents;
		Tangents.AddUninitialized(Primitive.Tangents.Num());
		for (int32 Index = 0; Index < Primitive.Tangents.Num(); Index++)
		{
			Tangents[Index] = FProcMeshTangent(Primitive.Tangents[Index], false);
		}
		ProceduralMeshComponent->CreateMeshSection_LinearColor(SectionIndex, Primitive.Positions, Triangles, Primitive.Normals, UV, Colors, Tangents, ProceduralMeshConfig.bBuildSimpleCollision);
		ProceduralMeshComponent->SetMaterial(SectionIndex, Primitive.Material);
		SectionIndex++;
	}

	return true;
}

UStaticMesh* FglTFRuntimeParser::LoadStaticMeshByName(const FString Name, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonMeshes;
	if (!Root->TryGetArrayField("meshes", JsonMeshes))
	{
		return nullptr;
	}

	for (int32 MeshIndex = 0; MeshIndex < JsonMeshes->Num(); MeshIndex++)
	{
		TSharedPtr<FJsonObject> JsonMeshObject = (*JsonMeshes)[MeshIndex]->AsObject();
		if (!JsonMeshObject)
		{
			return nullptr;
		}
		FString MeshName;
		if (JsonMeshObject->TryGetStringField("name", MeshName))
		{
			if (MeshName == Name)
			{
				return LoadStaticMesh(MeshIndex, StaticMeshConfig);
			}
		}
	}

	return nullptr;
}


UStaticMesh* FglTFRuntimeParser::LoadStaticMeshRecursive(const FString& NodeName, const TArray<FString>& ExcludeNodes, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	FglTFRuntimeNode Node;
	TArray<FglTFRuntimeNode> Nodes;

	if (NodeName.IsEmpty())
	{
		FglTFRuntimeScene Scene;
		if (!LoadScene(0, Scene))
		{
			AddError("LoadStaticMeshRecursive()", "No Scene found in asset");
			return nullptr;
		}

		for (int32 NodeIndex : Scene.RootNodesIndices)
		{
			if (!LoadNodesRecursive(NodeIndex, Nodes))
			{
				AddError("LoadStaticMeshRecursive()", "Unable to build Node Tree from first Scene");
				return nullptr;
			}
		}
	}
	else
	{
		if (!LoadNodeByName(NodeName, Node))
		{
			AddError("LoadStaticMeshRecursive()", FString::Printf(TEXT("Unable to find Node \"%s\""), *NodeName));
			return nullptr;
		}

		if (!LoadNodesRecursive(Node.Index, Nodes))
		{
			AddError("LoadStaticMeshRecursive()", FString::Printf(TEXT("Unable to build Node Tree from \"%s\""), *NodeName));
			return nullptr;
		}
	}

	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);

	FglTFRuntimeMeshLOD CombinedLOD;

	for (FglTFRuntimeNode& ChildNode : Nodes)
	{
		if (ExcludeNodes.Contains(ChildNode.Name))
		{
			continue;
		}

		if (ChildNode.MeshIndex != INDEX_NONE)
		{
			TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", ChildNode.MeshIndex);
			if (!JsonMeshObject)
			{
				return nullptr;
			}

			FglTFRuntimeMeshLOD* LOD = nullptr;
			if (!LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, StaticMeshConfig.MaterialsConfig))
			{
				return nullptr;
			}

			FglTFRuntimeNode CurrentNode = ChildNode;
			FTransform AdditionalTransform = CurrentNode.Transform;

			while (CurrentNode.ParentIndex != INDEX_NONE)
			{
				if (!LoadNode(CurrentNode.ParentIndex, CurrentNode))
				{
					return nullptr;
				}
				AdditionalTransform *= CurrentNode.Transform;
			}

			for (const FglTFRuntimePrimitive& Primitive : LOD->Primitives)
			{
				CombinedLOD.Primitives.Add(Primitive);
				CombinedLOD.AdditionalTransforms.Add(AdditionalTransform);
				if (!ChildNode.Name.IsEmpty())
				{
					StaticMeshContext->AdditionalSockets.Add(ChildNode.Name, AdditionalTransform);
				}
			}
		}
	}

	StaticMeshContext->LODs.Add(&CombinedLOD);

	UStaticMesh* StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);
	if (!StaticMesh)
	{
		return nullptr;
	}

	return FinalizeStaticMesh(StaticMeshContext);
}

void FglTFRuntimeParser::LoadStaticMeshRecursiveAsync(const FString& NodeName, const TArray<FString>& ExcludeNodes, const FglTFRuntimeStaticMeshAsync& AsyncCallback, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{

	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);


	Async(EAsyncExecution::Thread, [this, StaticMeshContext, StaticMeshConfig, ExcludeNodes, NodeName, AsyncCallback]()
		{

			FglTFRuntimeNode Node;
			TArray<FglTFRuntimeNode> Nodes;

			if (NodeName.IsEmpty())
			{
				FglTFRuntimeScene Scene;
				if (!LoadScene(0, Scene))
				{
					AddError("LoadStaticMeshRecursive()", "No Scene found in asset");
					return;
				}

				for (int32 NodeIndex : Scene.RootNodesIndices)
				{
					if (!LoadNodesRecursive(NodeIndex, Nodes))
					{
						AddError("LoadStaticMeshRecursive()", "Unable to build Node Tree from first Scene");
						return;
					}
				}
			}
			else
			{
				if (!LoadNodeByName(NodeName, Node))
				{
					AddError("LoadStaticMeshRecursive()", FString::Printf(TEXT("Unable to find Node \"%s\""), *NodeName));
					return;
				}

				if (!LoadNodesRecursive(Node.Index, Nodes))
				{
					AddError("LoadStaticMeshRecursive()", FString::Printf(TEXT("Unable to build Node Tree from \"%s\""), *NodeName));
					return;
				}
			}

			FglTFRuntimeMeshLOD CombinedLOD;

			for (FglTFRuntimeNode& ChildNode : Nodes)
			{
				if (ExcludeNodes.Contains(ChildNode.Name))
				{
					continue;
				}

				if (ChildNode.MeshIndex != INDEX_NONE)
				{
					TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", ChildNode.MeshIndex);
					if (!JsonMeshObject)
					{
						return;
					}

					FglTFRuntimeMeshLOD* LOD = nullptr;
					if (!LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, StaticMeshConfig.MaterialsConfig))
					{
						return;
					}

					FglTFRuntimeNode CurrentNode = ChildNode;
					FTransform AdditionalTransform = CurrentNode.Transform;

					while (CurrentNode.ParentIndex != INDEX_NONE)
					{
						if (!LoadNode(CurrentNode.ParentIndex, CurrentNode))
						{
							return;
						}
						AdditionalTransform *= CurrentNode.Transform;
					}

					for (const FglTFRuntimePrimitive& Primitive : LOD->Primitives)
					{
						CombinedLOD.Primitives.Add(Primitive);
						CombinedLOD.AdditionalTransforms.Add(AdditionalTransform);
						if (!ChildNode.Name.IsEmpty())
						{
							StaticMeshContext->AdditionalSockets.Add(ChildNode.Name, AdditionalTransform);
						}
					}
				}
			}

			StaticMeshContext->LODs.Add(&CombinedLOD);

			StaticMeshContext->StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);

			FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([StaticMeshContext, AsyncCallback]()
				{
					if (StaticMeshContext->StaticMesh)
					{
						StaticMeshContext->StaticMesh = StaticMeshContext->Parser->FinalizeStaticMesh(StaticMeshContext);
					}

					AsyncCallback.ExecuteIfBound(StaticMeshContext->StaticMesh);
				}, TStatId(), nullptr, ENamedThreads::GameThread);
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
		});
}

bool FglTFRuntimeParser::LoadMeshAsRuntimeLOD(const int32 MeshIndex, FglTFRuntimeMeshLOD& RuntimeLOD, const FglTFRuntimeMaterialsConfig& MaterialsConfig)
{
	TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
	if (!JsonMeshObject)
	{
		return false;
	}

	FglTFRuntimeMeshLOD* LOD;
	if (LoadMeshIntoMeshLOD(JsonMeshObject.ToSharedRef(), LOD, MaterialsConfig))
	{
		RuntimeLOD = *LOD; // slow copy :(
		return true;
	}

	return false;
}

UStaticMesh* FglTFRuntimeParser::LoadStaticMeshFromRuntimeLODs(const TArray<FglTFRuntimeMeshLOD>& RuntimeLODs, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);

	for (const FglTFRuntimeMeshLOD& RuntimeLOD : RuntimeLODs)
	{
		StaticMeshContext->LODs.Add(&RuntimeLOD);
	}

	UStaticMesh* StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);
	if (!StaticMesh)
	{
		return nullptr;
	}

	return FinalizeStaticMesh(StaticMeshContext);
}

void FglTFRuntimeParser::LoadStaticMeshFromRuntimeLODsAsync(const TArray<FglTFRuntimeMeshLOD>& RuntimeLODs, const FglTFRuntimeStaticMeshAsync& AsyncCallback, const FglTFRuntimeStaticMeshConfig& StaticMeshConfig)
{
	TSharedRef<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe> StaticMeshContext = MakeShared<FglTFRuntimeStaticMeshContext, ESPMode::ThreadSafe>(AsShared(), StaticMeshConfig);

	Async(EAsyncExecution::Thread, [this, StaticMeshContext, StaticMeshConfig, RuntimeLODs, AsyncCallback]()
		{
			for (const FglTFRuntimeMeshLOD& RuntimeLOD : RuntimeLODs)
			{
				StaticMeshContext->LODs.Add(&RuntimeLOD);
			}

			StaticMeshContext->StaticMesh = LoadStaticMesh_Internal(StaticMeshContext);

			FGraphEventRef Task = FFunctionGraphTask::CreateAndDispatchWhenReady([StaticMeshContext, AsyncCallback]()
				{
					if (StaticMeshContext->StaticMesh)
					{
						StaticMeshContext->StaticMesh = StaticMeshContext->Parser->FinalizeStaticMesh(StaticMeshContext);
					}

					AsyncCallback.ExecuteIfBound(StaticMeshContext->StaticMesh);
				}, TStatId(), nullptr, ENamedThreads::GameThread);
			FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);
		}
	);
}