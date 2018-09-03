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
#include "pxr/pxr.h"
#include "pxrUsdMayaGL/proxyDrawOverride.h"

#include "pxrUsdMayaGL/batchRenderer.h"
#include "pxrUsdMayaGL/renderParams.h"
#include "pxrUsdMayaGL/usdProxyShapeAdapter.h"
#include "usdMaya/proxyShape.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hdx/intersector.h"

#include <maya/MBoundingBox.h>
#include <maya/MDagPath.h>
#include <maya/MDrawContext.h>
#include <maya/MFrameContext.h>
#include <maya/MMatrix.h>
#include <maya/MObject.h>
#include <maya/MPoint.h>
#include <maya/MPxDrawOverride.h>
#include <maya/MSelectionContext.h>
#include <maya/MSelectionMask.h>
#include <maya/MStatus.h>
#include <maya/MString.h>
#include <maya/MUserData.h>
#include <maya/MViewport2Renderer.h>


PXR_NAMESPACE_OPEN_SCOPE


MString UsdMayaProxyDrawOverride::sm_drawDbClassification("drawdb/geometry/usdMaya");
MString UsdMayaProxyDrawOverride::sm_drawRegistrantId("pxrUsdPlugin");

/* static */
MHWRender::MPxDrawOverride*
UsdMayaProxyDrawOverride::Creator(const MObject& obj)
{
    UsdMayaGLBatchRenderer::Init();
    return new UsdMayaProxyDrawOverride(obj);
}

// Note that isAlwaysDirty became available as an MPxDrawOverride constructor
// parameter beginning with Maya 2016 Extension 2.
UsdMayaProxyDrawOverride::UsdMayaProxyDrawOverride(const MObject& obj) :
        MHWRender::MPxDrawOverride(obj,
                                   UsdMayaProxyDrawOverride::draw
#if MAYA_API_VERSION >= 201651
                                   , /* isAlwaysDirty = */ false)
#else
                                   )
#endif
{
}

/* virtual */
UsdMayaProxyDrawOverride::~UsdMayaProxyDrawOverride()
{
    UsdMayaGLBatchRenderer::GetInstance().RemoveShapeAdapter(&_shapeAdapter);
}

/* virtual */
MHWRender::DrawAPI
UsdMayaProxyDrawOverride::supportedDrawAPIs() const
{
#if MAYA_API_VERSION >= 201600
    return MHWRender::kOpenGL | MHWRender::kOpenGLCoreProfile;
#else
    return MHWRender::kOpenGL;
#endif
}

/* virtual */
MMatrix
UsdMayaProxyDrawOverride::transform(
        const MDagPath& objPath,
        const MDagPath& cameraPath) const
{
    // Propagate changes in the proxy shape's transform to the shape adapter's
    // delegate.
    MStatus status;
    const MMatrix transform = objPath.inclusiveMatrix(&status);
    if (status == MS::kSuccess) {
        const_cast<PxrMayaHdUsdProxyShapeAdapter&>(_shapeAdapter).SetRootXform(
            GfMatrix4d(transform.matrix));
    }

    return MHWRender::MPxDrawOverride::transform(objPath, cameraPath);
}

/* virtual */
MBoundingBox
UsdMayaProxyDrawOverride::boundingBox(
        const MDagPath& objPath,
        const MDagPath& /* cameraPath */) const
{
    UsdMayaProxyShape* pShape = UsdMayaProxyShape::GetShapeAtDagPath(objPath);
    if (!pShape) {
        return MBoundingBox();
    }

    return pShape->boundingBox();
}

/* virtual */
bool
UsdMayaProxyDrawOverride::isBounded(
        const MDagPath& objPath,
        const MDagPath& /* cameraPath */) const
{
    return UsdMayaIsBoundingBoxModeEnabled();
}

/* virtual */
MUserData*
UsdMayaProxyDrawOverride::prepareForDraw(
        const MDagPath& objPath,
        const MDagPath& /* cameraPath */,
        const MHWRender::MFrameContext& frameContext,
        MUserData* oldData)
{
    UsdMayaProxyShape* shape = UsdMayaProxyShape::GetShapeAtDagPath(objPath);
    if (!shape) {
        return nullptr;
    }

    if (!_shapeAdapter.Sync(
            objPath,
            frameContext.getDisplayStyle(),
            MHWRender::MGeometryUtilities::displayStatus(objPath))) {
        return nullptr;
    }

    UsdMayaGLBatchRenderer::GetInstance().AddShapeAdapter(&_shapeAdapter);

    bool drawShape;
    bool drawBoundingBox;
    _shapeAdapter.GetRenderParams(&drawShape, &drawBoundingBox);

    if (!drawBoundingBox && !drawShape) {
        // We weren't asked to do anything.
        return nullptr;
    }

    MBoundingBox boundingBox;
    MBoundingBox* boundingBoxPtr = nullptr;
    if (drawBoundingBox) {
        // Only query for the bounding box if we're drawing it.
        boundingBox = shape->boundingBox();
        boundingBoxPtr = &boundingBox;
    }

    return _shapeAdapter.GetMayaUserData(oldData, boundingBoxPtr);
}

#if MAYA_API_VERSION >= 201800

/* virtual */
bool
UsdMayaProxyDrawOverride::wantUserSelection() const
{
    const MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    if (!renderer) {
        return false;
    }

    return renderer->drawAPIIsOpenGL();
}

/* virtual */
bool
UsdMayaProxyDrawOverride::userSelect(
        MHWRender::MSelectionInfo& selectInfo,
        const MHWRender::MDrawContext& context,
        MPoint& hitPoint,
        const MUserData* data)
{
    MSelectionMask objectsMask(MSelectionMask::kSelectObjectsMask);
    if (!selectInfo.selectable(objectsMask)) {
        return false;
    }

    const unsigned int displayStyle = context.getDisplayStyle();
    const MHWRender::DisplayStatus displayStatus =
        MHWRender::MGeometryUtilities::displayStatus(_shapeAdapter._shapeDagPath);

    // At this point, we expect the shape to have already been drawn and our
    // shape adapter to have been added to the batch renderer, but just in
    // case, we still treat the shape adapter as if we're populating it for the
    // first time. We do not add it to the batch renderer though, since that
    // must have already been done to have caused the shape to be drawn and
    // become eligible for selection.
    if (!_shapeAdapter.Sync(
            _shapeAdapter._shapeDagPath, displayStyle, displayStatus)) {
        return false;
    }

    const HdxIntersector::HitSet* hitSet =
        UsdMayaGLBatchRenderer::GetInstance().TestIntersection(
            &_shapeAdapter,
            selectInfo,
            context,
            selectInfo.singleSelection());

    const HdxIntersector::Hit* nearestHit =
        UsdMayaGLBatchRenderer::GetNearestHit(hitSet);

    if (!nearestHit) {
        return false;
    }

    const GfVec3f& gfHitPoint = nearestHit->worldSpaceHitPoint;
    hitPoint = MPoint(gfHitPoint[0], gfHitPoint[1], gfHitPoint[2]);

    return true;
}

#endif // MAYA_API_VERSION >= 201800

/* static */
void
UsdMayaProxyDrawOverride::draw(
        const MHWRender::MDrawContext& context,
        const MUserData* data)
{
    UsdMayaGLBatchRenderer::GetInstance().Draw(context, data);
}


PXR_NAMESPACE_CLOSE_SCOPE
