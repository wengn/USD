//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/usdImaging/usdImagingGL/hydraMaterialAdapter.h"
#include "pxr/usdImaging/usdImagingGL/package.h"
#include "pxr/usdImaging/usdImagingGL/textureUtils.h"

#include "pxr/usdImaging/usdImaging/debugCodes.h"
#include "pxr/usdImaging/usdImaging/delegate.h"
#include "pxr/usdImaging/usdImaging/indexProxy.h"
#include "pxr/usdImaging/usdImaging/tokens.h"

#include "pxr/imaging/glf/glslfx.h"
#include "pxr/imaging/glf/ptexTexture.h"

#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/tokens.h"

#include "pxr/usd/usdHydra/tokens.h"
#include "pxr/usd/usdShade/connectableAPI.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (surfaceShader)
    (displacementShader)
    (texture)
    (primvar)
    (st)
    (file)
    (varname)
    (fallback)
);

TF_REGISTRY_FUNCTION(TfType)
{
    typedef UsdImagingGLHydraMaterialAdapter Adapter;
    TfType t = TfType::Define<Adapter, TfType::Bases<Adapter::BaseAdapter> >();
    t.SetFactory< UsdImagingPrimAdapterFactory<Adapter> >();
}

UsdImagingGLHydraMaterialAdapter::~UsdImagingGLHydraMaterialAdapter()
{
}

bool
UsdImagingGLHydraMaterialAdapter::IsSupported(
    UsdImagingIndexProxy const* index) const
{
    return index->IsSprimTypeSupported(HdPrimTypeTokens->material);
}

bool
UsdImagingGLHydraMaterialAdapter::IsPopulatedIndirectly()
{
    // Materials are populated as a consequence of populating a prim
    // which uses the material.
    return true;
}

SdfPath
UsdImagingGLHydraMaterialAdapter::Populate(UsdPrim const& prim,
                            UsdImagingIndexProxy* index,
                            UsdImagingInstancerContext const* instancerContext)
{
    // Since shaders are populated by reference, they need to take care not to
    // be populated multiple times.
    SdfPath cachePath = prim.GetPath();
    if (index->IsPopulated(cachePath)) {
        return cachePath;
    }

    // Extract the textures from the graph of this material.
    SdfPathVector textures;
    TfTokenVector primvars;
    HdMaterialParamVector params;
    UsdPrim surfaceShaderPrim;
    UsdPrim displacementShaderPrim;
    if (!_GatherMaterialData(prim, &surfaceShaderPrim, 
                            &displacementShaderPrim,
                            &textures, &primvars, 
                            &params)) {
        return prim.GetPath();
    }

    index->InsertSprim(HdPrimTypeTokens->material,
                       cachePath,
                       prim, shared_from_this());
    HD_PERF_COUNTER_INCR(UsdImagingTokens->usdPopulatedPrimCount);

    if (index->IsBprimTypeSupported(HdPrimTypeTokens->texture)) {
        TF_FOR_ALL(textureIt, textures) {
            // Textures are inserted as property paths, with the property being
            // the texture asset path.  Some textures will have sibling
            // attributes specifying things like filtering modes; that's
            // currently all picked up in UsdImagingDelegate via
            // - GetTextureResourceID
            // - GetTextureResource
            // ... which will get the prim path and explore.
            //
            if (index->IsPopulated(*textureIt)) {
                continue;
            }
            UsdPrim texturePrim = _GetPrim(textureIt->GetPrimPath());
            TF_DEBUG(USDIMAGING_TEXTURES).Msg("Populating texture found: %s\n",
                    texturePrim.GetPath().GetText());
            index->InsertBprim(HdPrimTypeTokens->texture,
                    *textureIt,
                    texturePrim, shared_from_this());
            HD_PERF_COUNTER_INCR(UsdImagingTokens->usdPopulatedPrimCount);
        }
    }

    return prim.GetPath();
}

/* virtual */
void
UsdImagingGLHydraMaterialAdapter::TrackVariability(UsdPrim const& prim,
                                          SdfPath const& cachePath,
                                          HdDirtyBits* timeVaryingBits,
                                          UsdImagingInstancerContext const*
                                              instancerContext) const
{
    if (IsChildPath(cachePath)) {
        // Textures aren't time-varying.
        return;
    }

    UsdPrim surfaceShaderPrim = _GetSurfaceShaderPrim(UsdShadeMaterial(prim));
    if (!surfaceShaderPrim)
        return;

    // XXX: This is terrifying. Run through all attributes of the prim,
    // and if any are time varying, assume all shader params are time-varying.
    const std::vector<UsdAttribute> &attrs = surfaceShaderPrim.GetAttributes();
    TF_FOR_ALL(attrIter, attrs) {
        const UsdAttribute& attr = *attrIter;
        if (attr.GetNumTimeSamples()>1){
            *timeVaryingBits |= HdMaterial::DirtyParams;
        }
    }
}

static bool
_IsLegacyTextureOrPrimvarInput(const UsdShadeInput &shaderInput)
{
    UsdAttribute attr = shaderInput.GetAttr();

    TfToken baseName = attr.GetBaseName();
    return  attr.SplitName().size() >= 2 && 
            (baseName == _tokens->texture || baseName == _tokens->primvar);
}

// XXX : This should use the shader node registry
static TfToken 
GetFilenameInput(TfToken const& id)
{
    if(id == UsdHydraTokens->HwUvTexture_1) {
        return UsdHydraTokens->infoFilename;
    } else if (id == UsdImagingTokens->UsdUVTexture) {
        return _tokens->file;
    }
    return UsdHydraTokens->infoFilename;
}

// XXX : This should use the shader node registry
static bool
IsPtexTexture(TfToken const& id)
{
    return (id == UsdHydraTokens->HwPtexTexture_1);
}

// XXX : This should use the shader node registry
static bool
IsTextureFamilyNode(TfToken const& id)
{
    return (id == UsdHydraTokens->HwUvTexture_1 || 
            id == UsdHydraTokens->HwPtexTexture_1||
            id == UsdImagingTokens->UsdUVTexture);
}

// XXX : This should use the shader node registry
static bool
IsPrimvarFamilyNode(TfToken const& id)
{
    return (id == UsdHydraTokens->HwPrimvar_1 || 
            id == UsdImagingTokens->UsdPrimvarReader_float ||
            id == UsdImagingTokens->UsdPrimvarReader_float2 ||
            id == UsdImagingTokens->UsdPrimvarReader_float3 ||
            id == UsdImagingTokens->UsdPrimvarReader_float4 );
}

static bool
IsSupportedShaderInputType(SdfValueTypeName const& input)
{
    // This simple material adapter does not support
    // tokens in the shader.
    if(input == "token") {
        return false;
    }
    return true;
}


// XXX : This should use the shader node registry
static TfToken
GetFallbackPrimvar(TfToken const& id)
{
    if (id == UsdImagingTokens->UsdPrimvarReader_float ||
        id == UsdImagingTokens->UsdPrimvarReader_float2 ||
        id == UsdImagingTokens->UsdPrimvarReader_float3 ||
        id == UsdImagingTokens->UsdPrimvarReader_float4) {
        return _tokens->fallback;
    } else if (id == UsdImagingTokens->UsdUVTexture) {
        return _tokens->fallback;
    }
    return TfToken();
}

// XXX : This should use the shader node registry
static TfToken
GetVarnamePrimvar(TfToken const& id)
{
    if (id == UsdImagingTokens->UsdPrimvarReader_float ||
        id == UsdImagingTokens->UsdPrimvarReader_float2 ||
        id == UsdImagingTokens->UsdPrimvarReader_float3 ||
        id == UsdImagingTokens->UsdPrimvarReader_float4) {
        return _tokens->varname;
    } else if (id == UsdHydraTokens->HwPrimvar_1){
        return UsdHydraTokens->infoVarname;
    }
    return TfToken();
}

// XXX : This should use the shader node registry
static TfTokenVector
GetPrimvars(TfToken const& id)
{
    TfTokenVector t;
    if (id == UsdHydraTokens->HwPrimvar_1) {
        t.push_back(UsdHydraTokens->infoVarname);
    } else if (id == UsdImagingTokens->UsdPrimvarReader_float ||
               id == UsdImagingTokens->UsdPrimvarReader_float2 ||
               id == UsdImagingTokens->UsdPrimvarReader_float3 ||
               id == UsdImagingTokens->UsdPrimvarReader_float4) {
        t.push_back(_tokens->varname);
        t.push_back(_tokens->fallback);
    } else if (id == UsdHydraTokens->HwUvTexture_1) {
        t.push_back(UsdHydraTokens->uv);
    } else if (id == UsdImagingTokens->UsdUVTexture) {
        t.push_back(_tokens->st);
        t.push_back(_tokens->fallback);
    } else if (id == UsdHydraTokens->HwPtexTexture_1) {
        t.push_back(UsdImagingTokens->faceIndexPrimvar);
        t.push_back(UsdImagingTokens->faceOffsetPrimvar);
    }
    return t;
}

static
UsdPrim
_GetDeprecatedSurfaceShaderPrim(const UsdShadeMaterial &material)
{
    // ---------------------------------------------------------------------- //
    // Hydra-only shader style - displayLook:bxdf
    // ---------------------------------------------------------------------- //
    static const TfToken displayLookBxdf("displayLook:bxdf");

    // ---------------------------------------------------------------------- //
    // Deprecated shader style - hydraLook:Surface
    // ---------------------------------------------------------------------- //
    static const TfToken hdSurf("hydraLook:surface");
    static const TfToken surfType("HydraPbsSurface");

    UsdRelationship displayShaderRel = material.GetPrim().GetRelationship(
        displayLookBxdf);

    if (!displayShaderRel) {
        displayShaderRel = material.GetPrim().GetRelationship(hdSurf);
    }

    // Return if neither deprecated relationship can be found.
    if (!displayShaderRel)
        return UsdPrim();

    SdfPathVector targets;
    if (!displayShaderRel.GetForwardedTargets(&targets))
        return UsdPrim();

    if (targets.size() != 1) {
        // XXX: This should really be a validation error once USD gets that
        // feature.
        TF_WARN("We expect only one target on relationship %s of prim <%s>, "
                "but got %zu.",
                displayShaderRel.GetName().GetText(),
                material.GetPath().GetText(),
                targets.size());
        return UsdPrim();
    }

    if (!targets[0].IsPrimPath()) {
        // XXX: This should really be a validation error once USD gets that
        // feature.
        TF_WARN("We expect the target of the relationship %s of prim <%s> "
                "to be a prim, instead it is <%s>.",
                displayShaderRel.GetName().GetText(),
                material.GetPath().GetText(),
                targets[0].GetText());
        return UsdPrim();
    }

    UsdPrim shaderPrim = displayShaderRel.GetStage()->GetPrimAtPath(targets[0]);
    if (displayShaderRel.GetName() == hdSurf) {
        if (TF_VERIFY(shaderPrim.GetTypeName() == surfType)) {
            TF_DEBUG(USDIMAGING_SHADERS).Msg(
                     "\t Deprecated hydraLook:surface binding found: %s\n", 
                     shaderPrim.GetPath().GetText());
                return shaderPrim;
        }
    } else {
        TF_DEBUG(USDIMAGING_SHADERS).Msg("\t Deprecated displayLook:bxdf "
                "binding found: %s\n", shaderPrim.GetPath().GetText());
        return shaderPrim;
    }

    return UsdPrim();
}

UsdPrim
UsdImagingGLHydraMaterialAdapter::_GetSurfaceShaderPrim(
    const UsdShadeMaterial &material) const
{
    // Determine the path to the preview shader and return it.
    const TfToken context = _GetMaterialNetworkSelector();
    if (UsdShadeShader surface =  
        material.ComputeSurfaceSource(context)) {
        TF_DEBUG(USDIMAGING_SHADERS).Msg("\t GLSLFX surface: %s\n", 
            surface.GetPath().GetText());
        return surface.GetPrim();
    }

    return _GetDeprecatedSurfaceShaderPrim(material);
}

UsdPrim
UsdImagingGLHydraMaterialAdapter::_GetDisplacementShaderPrim(
    const UsdShadeMaterial &material) const
{
    // Determine the path to the preview displacement shader and return it.
    const TfToken context = _GetMaterialNetworkSelector();
    if (UsdShadeShader displacement = 
        material.ComputeDisplacementSource(context)) {
        TF_DEBUG(USDIMAGING_SHADERS).Msg("\t GLSLFX displacement: %s\n", 
            displacement.GetPath().GetText());
        return displacement.GetPrim();
    }

    return UsdPrim();
}

/* virtual */
void
UsdImagingGLHydraMaterialAdapter::UpdateForTime(
    UsdPrim const& prim,
    SdfPath const& cachePath,
    UsdTimeCode time,
    HdDirtyBits requestedBits,
    UsdImagingInstancerContext const* instancerContext) const
{
    if (IsChildPath(cachePath)) {
        // Textures aren't stored in the value cache.
        // XXX: For bonus points, we could move the logic from
        // - GetTextureResourceID and GetTextureResource here.
        return;
    }

    UsdPrim surfaceShaderPrim;
    UsdPrim displacementShaderPrim;
    SdfPathVector textures;
    TfTokenVector primvars;
    HdMaterialParamVector params;

    if (requestedBits & HdMaterial::DirtySurfaceShader ||
        requestedBits & HdMaterial::DirtyParams) 
    {
        if (!_GatherMaterialData(prim, &surfaceShaderPrim, 
                                &displacementShaderPrim,
                                &textures, &primvars, 
                                &params)) {
            TF_CODING_ERROR("Failed to gather material data for already "
                "populated material prim <%s>.", prim.GetPath().GetText());
            return;
        }
    }

    UsdImagingValueCache* valueCache = _GetValueCache();
    if (requestedBits & HdMaterial::DirtySurfaceShader) {
        std::string surfaceSource;
        std::string displacementSource;

        if (surfaceShaderPrim) {
            surfaceSource = _GetShaderSource(surfaceShaderPrim,
                                             _tokens->surfaceShader);
        }

        if (displacementShaderPrim) {
            displacementSource = _GetShaderSource(displacementShaderPrim,
                                                  _tokens->displacementShader);
        }

        // DirtySurfaceShader triggers a refresh of both shader sources.
        valueCache->GetSurfaceShaderSource(cachePath) = surfaceSource;
        valueCache->GetDisplacementShaderSource(cachePath) = displacementSource;

        // Extract the primvars
        valueCache->GetMaterialPrimvars(cachePath) = primvars;
    }

    if (requestedBits & HdMaterial::DirtyParams) {
        // XXX: The param list isn't actually time-varying... we should find
        // a way to only do this once.
        HdMaterialParamVector& materialParams =
            valueCache->GetMaterialParams(cachePath);
        materialParams = params;

        // Hydra expects values in the value cache for any param that's
        // a "fallback" param (constant, as opposed to texture- or
        // primvar-based).
        TF_FOR_ALL(paramIt, materialParams) {
            if (paramIt->IsFallback()) {
                VtValue& param = valueCache->GetMaterialParam(
                    cachePath, paramIt->GetName());
                param = _GetMaterialParamValue(surfaceShaderPrim,
                            paramIt->GetName(), time);
            }
        }
    }
}

/* virtual */
HdDirtyBits
UsdImagingGLHydraMaterialAdapter::ProcessPropertyChange(UsdPrim const& prim,
                                               SdfPath const& cachePath,
                                               TfToken const& propertyName)
{
    // XXX: This doesn't get notifications for dependent nodes.
    return HdChangeTracker::AllDirty;
}

/* virtual */
void
UsdImagingGLHydraMaterialAdapter::MarkDirty(UsdPrim const& prim,
                                   SdfPath const& cachePath,
                                   HdDirtyBits dirty,
                                   UsdImagingIndexProxy* index)
{
    if (IsChildPath(cachePath)) {
        index->MarkBprimDirty(cachePath, dirty);
    } else {
        index->MarkSprimDirty(cachePath, dirty);
    }
}

/* virtual */
void
UsdImagingGLHydraMaterialAdapter::MarkMaterialDirty(UsdPrim const& prim,
                                                    SdfPath const& cachePath,
                                                    UsdImagingIndexProxy* index)
{
    if (!IsChildPath(cachePath)) {
        index->MarkSprimDirty(cachePath, HdMaterial::DirtySurfaceShader |
                                         HdMaterial::DirtyParams);
    }
}

/* virtual */
void
UsdImagingGLHydraMaterialAdapter::_RemovePrim(SdfPath const& cachePath,
                                 UsdImagingIndexProxy* index)
{
    if (IsChildPath(cachePath)) {
        index->RemoveBprim(HdPrimTypeTokens->texture, cachePath);
    } else {
        index->RemoveSprim(HdPrimTypeTokens->material, cachePath);
    }
}

std::string
UsdImagingGLHydraMaterialAdapter::_GetShaderSource(
    UsdPrim const& shaderPrim, 
    TfToken const& shaderType) const
{
    TfToken shaderId;
    UsdAttribute srcAttr;
    if (UsdShadeShader shader = UsdShadeShader(shaderPrim)) {
        // Extract the id of the node
        UsdAttribute attr = shader.GetIdAttr();
        attr.Get(&shaderId);
        if (shaderId == UsdImagingTokens->UsdPreviewSurface) {
            TF_DEBUG(USDIMAGING_SHADERS).Msg(
                "Loading UsdShade preview surface\n");
            GlfGLSLFX gfx (UsdImagingGLPackagePreviewSurfaceShader());
            if (shaderType == _tokens->surfaceShader){
                return gfx.GetSurfaceSource();
            } else if (shaderType == _tokens->displacementShader){
                return gfx.GetDisplacementSource();
            }
        }

        TfToken filename = GetFilenameInput(shaderType);
        srcAttr = shader.GetInput(filename);
        TF_DEBUG(USDIMAGING_SHADERS).Msg("Loading UsdShade shader: %s\n",
                    srcAttr.GetPath().GetText());
    }

    if (!srcAttr) {
        // ------------------------------------------------------------------ //
        // Deprecated
        // ------------------------------------------------------------------ //
        srcAttr = shaderPrim.GetAttribute(UsdImagingTokens->infoSource);
        if (!srcAttr) {
            TF_DEBUG(USDIMAGING_SHADERS).Msg("No shader source attribute: %s\n",
                    shaderPrim.GetPath().GetText());
            return std::string();
        }
        TF_DEBUG(USDIMAGING_SHADERS).Msg("Loading deprecated shader: %s\n",
                    srcAttr.GetPath().GetText());
        // ------------------------------------------------------------------ //
    }

    // PERFORMANCE: We're opening the file on every request currently, but we'd
    // like to share this in some sort of registry in the future.
    SdfAssetPath asset;
    std::string filePath;
    if (!srcAttr.Get(&asset)){
        return std::string();
    }

    filePath = asset.GetResolvedPath();

    // Fallback to the literal path if it couldn't be resolved.
    if (filePath.empty()){
        filePath = asset.GetAssetPath();
    }

    GlfGLSLFX gfx(filePath);
    if (!gfx.IsValid()){
        return std::string();
    }

    if (shaderType == _tokens->surfaceShader){
        return gfx.GetSurfaceSource();
    } else if (shaderType == _tokens->displacementShader){
        return gfx.GetDisplacementSource();
    }

    TF_CODING_ERROR("Unsupported shader type: <%s>\n", shaderType.GetText());
    return std::string();
}

VtValue
UsdImagingGLHydraMaterialAdapter::_GetMaterialParamValue(
    UsdPrim const &shaderPrim, 
    TfToken const &paramName,
    UsdTimeCode time) const
{
    VtValue value;
    UsdShadeConnectableAPI source;
    TfToken sourceName;
    UsdShadeAttributeType sourceType;

    if (UsdShadeShader shader = UsdShadeShader(shaderPrim)){
        if(UsdShadeInput shaderInput = shader.GetInput(paramName)) {
            // Check if it is connected to an input on the public interface.
            // If so, pull the information from the public interface.
            if (shaderInput.GetConnectedSource(
                &source, &sourceName, &sourceType)) {
                if (sourceType == UsdShadeAttributeType::Input) {
                    if (UsdShadeInput connectedInput = 
                        source.GetInput(sourceName)) {
                        connectedInput.Get(&value, time);
                    }
                }
            } else {
                shaderInput.Get(&value, time);
            }
        }
    } else {
        // ------------------------------------------------------------------ //
        // Deprecated
        // ------------------------------------------------------------------ //

        // First we try to read the attribute prefixed by "inputs:", if
        // that fails then we try the legacy name without "inputs:".
        TfToken inputAttr = 
            UsdShadeUtils::GetFullName(paramName, UsdShadeAttributeType::Input);
        UsdAttribute attr = shaderPrim.GetAttribute(inputAttr);
        if (!attr) {
            attr = shaderPrim.GetAttribute(paramName);
        }

        if (TF_VERIFY(attr)) {
            attr.Get(&value, time);
        }
        // ------------------------------------------------------------------ //
    }

    return value;
}

bool
UsdImagingGLHydraMaterialAdapter::_GatherMaterialData(
    UsdPrim const &materialPrim,
    UsdPrim *shaderPrim,
    UsdPrim *displacementShaderPrim,
    SdfPathVector *textureIDs,
    TfTokenVector *primvars,
    HdMaterialParamVector *params) const
{
    TF_DEBUG(USDIMAGING_SHADERS).Msg("Material caching : <%s>\n", 
        materialPrim.GetPath().GetText());

    *shaderPrim = _GetSurfaceShaderPrim(UsdShadeMaterial(materialPrim));
    if (*shaderPrim) {
        TF_DEBUG(USDIMAGING_SHADERS).Msg("- found surface shader: <%s>\n",
            shaderPrim->GetPath().GetText());
    } else {
        TF_DEBUG(USDIMAGING_SHADERS).Msg("- No valid surface shader!\n");
        return false;
    }

    *displacementShaderPrim = 
        _GetDisplacementShaderPrim(UsdShadeMaterial(materialPrim));
    if (*displacementShaderPrim) {
        TF_DEBUG(USDIMAGING_SHADERS).Msg("- found displacement shader: <%s>\n",
            displacementShaderPrim->GetPath().GetText());
    } else {
        TF_DEBUG(USDIMAGING_SHADERS).Msg("- No valid displacement shader!\n");
    }

    if (UsdShadeShader s = UsdShadeShader(*shaderPrim)) {
        _WalkShaderNetwork(*shaderPrim, textureIDs, primvars, params);
    } else {
        _WalkShaderNetworkDeprecated(*shaderPrim, textureIDs, primvars, 
                                     params);
    }

    return true;
}

void
UsdImagingGLHydraMaterialAdapter::_WalkShaderNetworkDeprecated(
    UsdPrim const &shaderPrim,
    SdfPathVector *textureIDs,
    TfTokenVector *primvars,
    HdMaterialParamVector *materialParams) const
{
    UsdShadeShader shader(shaderPrim);
    
    for (const UsdShadeInput &shaderInput : shader.GetInputs()) {
        if (_IsLegacyTextureOrPrimvarInput(shaderInput)) {
            continue;
        }

        UsdAttribute attr = shaderInput.GetAttr();
        if (!attr) {
            continue;
        }

        TF_DEBUG(USDIMAGING_SHADERS).Msg("\tShader input  found: %s\n",
                attr.GetPath().GetText());

        HdMaterialParam::ParamType paramType =
                HdMaterialParam::ParamTypeFallback;
        VtValue fallbackValue;
        SdfPath connection;
        TfTokenVector samplerCoords;
        bool isPtex = false;
        TfToken t;

        if (!TF_VERIFY(attr.Get(&fallbackValue),
                    "No fallback value for: <%s>\n",
                    attr.GetPath().GetText())) {
            continue;
        }

        if (UsdAttribute texAttr = shaderPrim.GetAttribute(
                                        TfToken(attr.GetPath().GetName() 
                                                + ":texture"))) {
            paramType = HdMaterialParam::ParamTypeTexture;
            connection = texAttr.GetPath();
            textureIDs->push_back(connection);

            TF_DEBUG(USDIMAGING_SHADERS).Msg(
                "\t\tFound texture: <%s>\n", connection.GetText());

            SdfAssetPath ap;
            texAttr.Get(&ap, UsdTimeCode::Default());

            isPtex = GlfIsSupportedPtexTexture(TfToken(ap.GetAssetPath()));
            if (isPtex) {
                t = UsdImagingTokens->ptexFaceIndex;
                // Allow the client to override this name
                texAttr.GetMetadata(UsdImagingTokens->faceIndexPrimvar, &t);
                primvars->push_back(t);

                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\t\tFound primvar: <%s>\n", t.GetText());

                t = UsdImagingTokens->ptexFaceOffset;
                // Allow the client to override this name
                texAttr.GetMetadata(UsdImagingTokens->faceOffsetPrimvar, &t);
                primvars->push_back(t);
                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\t\tFound primvar: <%s>\n", t.GetText());

            } else {
                texAttr.GetMetadata(UsdImagingTokens->uvPrimvar, &t);
                primvars->push_back(t);
                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\t\tFound primvar: <%s>\n", t.GetText());
                samplerCoords.push_back(t);
                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\t\tFound sampler: <%s>\n", t.GetText());
            }

        } else if (UsdAttribute pvAttr = shaderPrim.GetAttribute(
                                        TfToken(attr.GetPath().GetName() 
                                                + ":primvar"))) {
            paramType = HdMaterialParam::ParamTypePrimvar;
            connection = SdfPath("primvar."
                                + pvAttr.GetName().GetString());
            TF_DEBUG(USDIMAGING_SHADERS).Msg(
                "\t\tFound primvar: <%s>\n", connection.GetText());

            if (TF_VERIFY(pvAttr.Get(&t, UsdTimeCode::Default()))) {
                primvars->push_back(t);
                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\t\tFound primvar: <%s>\n", t.GetText());
                samplerCoords.push_back(t);
                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\t\tFound sampler: <%s>\n", t.GetText());
            }
        }

        shaderInput.Get(&fallbackValue);
        materialParams->push_back(HdMaterialParam(paramType,
                                                  shaderInput.GetBaseName(),
                                                  fallbackValue,
                                                  connection,
                                                  samplerCoords,
                                                  isPtex));
    }
}

void
UsdImagingGLHydraMaterialAdapter::_WalkShaderNetwork(
    UsdPrim const &shaderPrim,
    SdfPathVector *textureIDs,
    TfTokenVector *primvars,
    HdMaterialParamVector *materialParams) const
{
    UsdShadeShader shader(shaderPrim);

    // Vector used to walk the graph iteratively.
    SdfPathVector stack(1, shader.GetPath());

    // Internal data structure to keep the parameters organize before 
    // we return them in the actual HdMaterialParamVector.
    struct _MaterialParams {
        HdMaterialParam::ParamType _paramType;
        TfToken _name;
        VtValue _fallbackValue;
        SdfPath _connection;
        SdfPath _connectionPrimvar;
        TfTokenVector _samplerCoords;
        bool _isPtex;
    };
    std::vector<_MaterialParams> params;
    TfToken id;
    bool hasId;
    bool isRootNode = true;
    UsdShadeConnectableAPI source;
    TfToken sourceName;
    UsdShadeAttributeType sourceType;

    // Iteratively walk the graph visiting each node and collecting
    // textures, primvars and material parameters
    while (!stack.empty()) {
        SdfPath shaderPath = stack.back();
        stack.pop_back();
        shader = UsdShadeShader(_GetPrim(shaderPath));

        // Extract the id of the node
        UsdAttribute attr = shader.GetIdAttr();
        hasId = attr.Get(&id);

        TF_DEBUG(USDIMAGING_SHADERS).Msg("\tEvaluating node : <%s>\n",
            shader.GetPath().GetText());

        // For preview materials Hydra Stream material the current
        // assumption is that we have a root material which is typically the
        // first node. This node has a bunch of inputs that can be pointing
        // to a texture or a primvar (or a default value). 
        // The current algorithm is made exclusively to walk this basic
        // materials.

        // XXX : Currently, we identify the root node because it is
        // the first node, in the future this assumption 
        // needs to be revisited.
        if (isRootNode) {
            isRootNode = false;

            for (UsdShadeInput shaderInput: shader.GetInputs()) {
                // Early out for any legacy texture/primvar inputs.
                if (_IsLegacyTextureOrPrimvarInput(shaderInput)) {
                    continue;
                }

                // The current simplified shading system does not support
                // tokens as inputs, so we will drop them at this point.
                if (!IsSupportedShaderInputType(shaderInput.GetTypeName())){
                    continue;
                }

                // Extract the fallback value for this input
                VtValue fallbackValue;
                const bool hasFallbackValue = shaderInput.Get(&fallbackValue);

                SdfPath connection;
                if (UsdShadeConnectableAPI::GetConnectedSource(
                    shaderInput, &source, &sourceName, &sourceType)) {

                     if (sourceType == UsdShadeAttributeType::Output) {
                        connection = source.GetPath();
                        // We need to have a valid fallback value based on the
                        // input's type, otherwise codeGen won't know the 
                        // correct function signature and will generate faulty 
                        // shader code.
                        if (!hasFallbackValue) {
                            fallbackValue = 
                                shaderInput.GetTypeName().GetDefaultValue();
                        }
                     } else if (sourceType == UsdShadeAttributeType::Input) {
                        if (UsdShadeInput connectedInput = 
                            source.GetInput(sourceName)) {
                            connectedInput.Get(&fallbackValue);
                        }
                     }
                }

                // Finally, initialize data for this potential input to the 
                // material we are loading.
                _MaterialParams matParam = {
                        HdMaterialParam::ParamTypeFallback,/*paramType*/
                        shaderInput.GetBaseName(),/*name*/
                        fallbackValue,/*fallbackValue*/
                        connection,/*_connection*/
                        SdfPath(), /*_connectionPrimvar*/
                        TfTokenVector(), /*_samplerCoords*/
                        false /*_isPtex*/};
                params.push_back(matParam);

                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\tAdding attribute : <%s> at <%s>\n", 
                    shaderInput.GetBaseName().GetText(),
                    connection.GetText());
            }
        } else if (hasId) {
            // For nodes with ids we can actually detect if they are 
            // primvars or textures and add them to the pipeline.
            if (IsTextureFamilyNode(id)) {
                const TfToken filename = GetFilenameInput(id);

                // Extract the filename from the shader node
                // and store the paths in the texture array.
                UsdAttribute a = shader.GetInput(filename);
                SdfPath connection = a.GetPath();

                // It is possible that there is no path available, in that
                // case we won't try to load the texture and we will just
                // use the fallback value
                if (!connection.IsEmpty()) {
                    textureIDs->push_back(connection);
                }

                TF_DEBUG(USDIMAGING_SHADERS).Msg(
                    "\t\tFound texture: <%s>\n", connection.GetText());

                bool isPtex = false;
                SdfPath connectionPrimvar;
                VtValue fallback;
                if (IsPtexTexture(id)){
                    isPtex = true;

                    TfToken varname;
                    TfToken trackPrimvar;
                    TfTokenVector primvarsInputsInNode = GetPrimvars(id);
                    for (auto const & input : primvarsInputsInNode) {
                        // Special handling for ptex primvar redirection.
                        if (input == UsdImagingTokens->faceIndexPrimvar) {
                            trackPrimvar = attr.GetMetadata(
                                UsdImagingTokens->faceIndexPrimvar,&varname)
                                ? varname : UsdImagingTokens->ptexFaceIndex;
                        } else if(input == UsdImagingTokens->faceOffsetPrimvar){
                            trackPrimvar = attr.GetMetadata(
                                UsdImagingTokens->faceOffsetPrimvar, &varname)
                                ? varname : UsdImagingTokens->ptexFaceOffset;
                        }
                    }

                    // If the primvar acess mesh data we store it in the 
                    // array of primvars that the material will return to inform
                    // the meshes of the information it needs.
                    if (!trackPrimvar.IsEmpty()) {
                        if (std::find(primvars->begin(), primvars->end(), 
                            trackPrimvar) == primvars->end()) {

                            primvars->push_back(trackPrimvar);
                            TF_DEBUG(USDIMAGING_SHADERS).Msg(
                                "\t\tFound primvar: <%s>\n", 
                                    primvars->back().GetText());
                        }
                    }
                } else {
                    // For regular textures we need to resolve what node
                    // will be providing the texture coordinates.
                    TfTokenVector primvarsInputsInNode = GetPrimvars(id);
                    for (auto const & input : primvarsInputsInNode ) {
                        if (UsdShadeInput uv = shader.GetInput(input)) {
                            if (uv.GetConnectedSource(
                                &source, &sourceName, &sourceType)) {
                                connectionPrimvar = source.GetPath();
                            } else {
                                if (input == GetFallbackPrimvar(id)) {
                                    uv.Get(&fallback);
                                }
                            }
                        }
                    }
                }

                for(auto &p : params) {
                    if (p._connection == shader.GetPath()){
                        p._paramType = HdMaterialParam::ParamTypeTexture;
                        p._isPtex = isPtex;
                        p._connectionPrimvar = connectionPrimvar;
                        p._connection = connection;
                        if (!fallback.IsEmpty()) {
                            p._fallbackValue = fallback;
                        }
                    }
                }
            } else if (IsPrimvarFamilyNode(id)) {
                SdfPath connection = SdfPath("primvar." 
                                    + shader.GetPrim()
                                            .GetName()
                                            .GetString());

                // Primvars can be providing data to an input to the material
                // or to a texture. We need this distinction in our current
                // design of HdMaterialParam.
                TfToken varname;
                VtValue fallback;
                TfTokenVector primvarsInputsInNode = GetPrimvars(id);
                for (auto const& input : primvarsInputsInNode) {
                    UsdShadeInput pv = shader.GetInput(input);
                    if (!pv) {
                        continue;
                    }

                    TfToken trackPrimvar;
                    if (input == GetVarnamePrimvar(id)) {
                        if (pv.GetConnectedSource(&source,
                            &sourceName, &sourceType)) {
                            if (UsdShadeInput connectedInput = 
                                source.GetInput(sourceName)) {
                                connectedInput.Get(&varname);
                            }
                        } else {
                            pv.Get(&varname);
                        }

                        // Track this primvar as this shader accesses mesh data.
                        if (!varname.IsEmpty()) {
                            trackPrimvar = varname;
                        }
                    } else if (input == GetFallbackPrimvar(id)) {
                        pv.Get(&fallback);
                    }

                    // If the primvar acess mesh data we store it in the 
                    // array of primvars that the material will return to inform
                    // the meshes of the information it needs.
                    if (!trackPrimvar.IsEmpty()) {
                        if (std::find(primvars->begin(), primvars->end(), 
                            trackPrimvar) == primvars->end()) {

                            primvars->push_back(trackPrimvar);
                            TF_DEBUG(USDIMAGING_SHADERS).Msg(
                                "\t\tFound primvar: <%s>\n", 
                                primvars->back().GetText());
                        }
                    }
                }

                for(auto &p : params) {
                    if (p._connectionPrimvar == shader.GetPath()){
                        TF_DEBUG(USDIMAGING_SHADERS).Msg(
                            "\t\tPrimvar connected : <%s>\n", 
                            varname.GetText());

                        // No need to change the paramType here.
                        p._samplerCoords.push_back(varname);
                    } else if (p._connection == shader.GetPath()){
                        TF_DEBUG(USDIMAGING_SHADERS).Msg(
                            "\t\tPrimvar connected : <%s>\n", 
                            varname.GetText());
                        p._paramType = HdMaterialParam::ParamTypePrimvar;
                        p._connection = connection;
                        p._samplerCoords.push_back(varname);

                        if (!fallback.IsEmpty()) {
                            p._fallbackValue = fallback;
                        }
                    }
                }
            }
        }

        // Add nodes to the stack to keep walking the graph
        for (UsdShadeInput const & shaderInput: shader.GetInputs()) {
            if (_IsLegacyTextureOrPrimvarInput(shaderInput)) {
                continue;
            }

            if (UsdShadeConnectableAPI::GetConnectedSource(shaderInput, 
                &source, &sourceName, &sourceType)) {
                // When we find a connection to a shading node output,
                // walk the upstream shading node.  Do not do this for
                // other sources (ex: a connection to a material
                // public interface parameter), since they are not
                // part of the shading node graph.
                if (sourceType == UsdShadeAttributeType::Output) {
                    stack.push_back(source.GetPath());
                }
            }
        }
    }

    // Fill the material parameters structure with all the information
    // we have compiled after walking the material.
    for(_MaterialParams const & param : params) {
        materialParams->emplace_back(param._paramType,
                param._name, param._fallbackValue,
                param._connection, param._samplerCoords, param._isPtex);
    }
}

HdTextureResource::ID
UsdImagingGLHydraMaterialAdapter::GetTextureResourceID(UsdPrim const& usdPrim,
                                                       SdfPath const &id,
                                                       UsdTimeCode time,
                                                       size_t salt) const
{
    return UsdImagingGL_GetTextureResourceID(usdPrim, id, time, salt);
}

HdTextureResourceSharedPtr
UsdImagingGLHydraMaterialAdapter::GetTextureResource(UsdPrim const& usdPrim,
                                                     SdfPath const &id,
                                                     UsdTimeCode time) const
{
    return UsdImagingGL_GetTextureResource(usdPrim, id, time);
}

PXR_NAMESPACE_CLOSE_SCOPE
