#pragma once

// Minimal NVIDIA NGX (DLSS) declarations for hooking the game's own DLSS evaluate call and
// harvesting the depth/motion-vector inputs it feeds the upscaler (guaranteed-real full-res data,
// with exact MV scales). Mirrors UEVR-3D's UpscaleHelper.hpp declarations.

#include <d3d11.h>
#include <d3d12.h>

typedef int NVSDK_NGX_Result;

typedef enum NVSDK_NGX_Feature {
    NVSDK_NGX_Feature_SuperSampling = 1,
    NVSDK_NGX_Feature_RayReconstruction = 13,
} NVSDK_NGX_Feature;

typedef struct NVSDK_NGX_Handle {
    unsigned int Id;
} NVSDK_NGX_Handle;

typedef struct NVSDK_NGX_Parameter {
    virtual void Set(const char* InName, unsigned long long InValue) = 0;
    virtual void Set(const char* InName, float InValue) = 0;
    virtual void Set(const char* InName, double InValue) = 0;
    virtual void Set(const char* InName, unsigned int InValue) = 0;
    virtual void Set(const char* InName, int InValue) = 0;
    virtual void Set(const char* InName, ID3D11Resource* InValue) = 0;
    virtual void Set(const char* InName, ID3D12Resource* InValue) = 0;
    virtual void Set(const char* InName, void* InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, float* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, double* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, int* OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char* InName, void** OutValue) const = 0;

    virtual void Reset() = 0;
} NVSDK_NGX_Parameter;

#define NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_Color "Color"
#define NVSDK_NGX_Parameter_Depth "Depth"
#define NVSDK_NGX_Parameter_MotionVectors "MotionVectors"
#define NVSDK_NGX_Parameter_Output "Output"
#define NVSDK_NGX_Parameter_MV_Scale_X "MV.Scale.X"
#define NVSDK_NGX_Parameter_MV_Scale_Y "MV.Scale.Y"
#define NVSDK_NGX_Parameter_Jitter_Offset_X "Jitter.Offset.X"
#define NVSDK_NGX_Parameter_Jitter_Offset_Y "Jitter.Offset.Y"

using NVSDK_NGX_D3D12_CreateFeature_t = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID, const NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle);
using NVSDK_NGX_D3D12_ReleaseFeature_t = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle* InHandle);
using NVSDK_NGX_D3D12_EvaluateFeature_t = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList* InCmdList, const NVSDK_NGX_Handle* InFeatureHandle, NVSDK_NGX_Parameter* InParameters, void* InCallback);
