cmake_minimum_required(VERSION 3.13)
project(libslic3r)

include(PrecompiledHeader)

if(NOT DEFINED ORCA_CHECK_GCODE_PLACEHOLDERS)
    set(ORCA_CHECK_GCODE_PLACEHOLDERS "0")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libslic3r_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h @ONLY)

# Orca: infill patterns a build can leave out. A left-out pattern stays a known value: configurations naming
# it load, validate and save unchanged, and slice with the fallback of its row in Fill/InfillPatternRegistry.cpp.
option(ORCA_INFILL_OPTIONAL_DEFAULT "Initial value of the ORCA_INFILL_<NAME> options" ON)
set(ORCA_OPTIONAL_INFILL_PATTERNS
    ZIGZAG CROSSZAG LOCKEDZAG LINE TRIHEXAGON QUARTERCUBIC SUPPORTCUBIC
    LATERALHONEYCOMB LATERALLATTICE TPMSD TPMSFK HILBERTCURVE OCTAGRAMSPIRAL)
set(ORCA_INFILL_PATTERN_DEFINES "")
set(ORCA_INFILL_LEFT_OUT "")
foreach (_pattern IN LISTS ORCA_OPTIONAL_INFILL_PATTERNS)
    option(ORCA_INFILL_${_pattern} "Build the ${_pattern} infill pattern" ${ORCA_INFILL_OPTIONAL_DEFAULT})
    if (ORCA_INFILL_${_pattern})
        string(APPEND ORCA_INFILL_PATTERN_DEFINES "#define ORCA_INFILL_${_pattern} 1\n")
    else ()
        string(APPEND ORCA_INFILL_PATTERN_DEFINES "#define ORCA_INFILL_${_pattern} 0\n")
        list(APPEND ORCA_INFILL_LEFT_OUT ${_pattern})
    endif ()
endforeach ()
if (ORCA_INFILL_LEFT_OUT)
    message(STATUS "Infill patterns left out of this build: ${ORCA_INFILL_LEFT_OUT}")
endif ()
# The CLI tests read the list (tests/cli/CMakeLists.txt).
set(ORCA_INFILL_LEFT_OUT "${ORCA_INFILL_LEFT_OUT}" CACHE INTERNAL "")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libslic3r_infill_patterns.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/libslic3r_infill_patterns.h @ONLY)

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

set(OpenVDBUtils_SOURCES "")
if (TARGET OpenVDB::openvdb)
    set(OpenVDBUtils_SOURCES OpenVDBUtils.cpp OpenVDBUtils.hpp)
endif()

option(BUILD_SHARED_LIBS "Build shared libs" OFF)
option(USE_SLIC3R_CONSOLE_LOG "Enable console logging in RelWithDebInfo builds" OFF)

# SolveSpace constraint solver (2D sketch solver backbone), built in deps/SLVS.
if (SLIC3R_CAD)
    find_package(SLVS REQUIRED)
endif ()

set(lisbslic3r_sources
    AABBMesh.cpp
    AABBMesh.hpp
    AABBTreeIndirect.hpp
    AABBTreeLines.hpp
    Algorithm/LineSplit.cpp
    Algorithm/LineSplit.hpp
    Algorithm/RegionExpansion.cpp
    Algorithm/RegionExpansion.hpp
    AnyPtr.hpp
    AppConfig.cpp
    AppConfig.hpp
    Arrange.cpp
    Arrange.hpp
    BlacklistedLibraryCheck.cpp
    BlacklistedLibraryCheck.hpp
    BridgeDetector.cpp
    BridgeDetector.hpp
    BeltBrim.cpp
    BeltBrim.hpp
    BeltGCode.cpp
    BeltGCode.hpp
    BeltPurge.cpp
    BeltSliceStrategy.cpp
    BeltSliceStrategy.hpp
    BeltTransform.cpp
    BeltTransform.hpp
    Brim.cpp
    BrimEarsPoint.hpp
    Brim.hpp
    BuildVolume.cpp
    BuildVolume.hpp
    calib.cpp
    calib.hpp
    ClipperUtilsParallel.cpp
    ClipperZUtils.cpp
    ClipperZUtils.hpp
    Color.cpp
    Color.hpp
    CommonDefs.hpp
    Config.cpp
    Config.hpp
    ConnectedBodies.cpp
    ConnectedBodies.hpp
    ContourZ.cpp
    CustomGCode.cpp
    CustomGCode.hpp
    CutUtils.cpp
    CutUtils.hpp
    EdgeGridPng.cpp
    ElephantFootCompensation.cpp
    ElephantFootCompensation.hpp
    Emboss.cpp
    Emboss.hpp
    EmbossShape.hpp
    enum_bitmask.hpp
    ExclusionVolumeGeometry.cpp
    ExclusionVolumeGeometry.hpp
    Execution/Execution.hpp
    Execution/ExecutionSeq.hpp
    Execution/ExecutionTBB.hpp
    ExPolygonSerialize.hpp
    ExPolygonsIndex.cpp
    ExPolygonsIndex.hpp
    Extruder.cpp
    Extruder.hpp
    ExtrusionSimulator.cpp
    ExtrusionSimulator.hpp
    FaceDetector.cpp
    FaceDetector.hpp
    Feature/FuzzySkin/FuzzySkin.cpp
    Feature/FuzzySkin/FuzzySkin.hpp
    Feature/Interlocking/InterlockingGenerator.cpp
    Feature/Interlocking/InterlockingGenerator.hpp
    Feature/Interlocking/VoxelUtils.cpp
    Feature/Interlocking/VoxelUtils.hpp
    FileParserError.hpp
    Fill/AdaptiveFillSpacing.cpp
    Fill/FillExtrusion.cpp
    Fill/FillLibrary.cpp
    Fill/FillLibrary.hpp
    Fill/InfillObjectState.hpp
    Fill/InfillPatternLibraries.cpp
    Fill/InfillPatternLibraries.hpp
    Fill/InfillPatternRegistry.cpp
    Fill/InfillPatternRegistry.hpp
    Fill/InfillPlugins.cpp
    Fill/InfillPlugins.hpp
    Fill/LightningParams.cpp
    Fill/LightningParams.hpp
    Fill/FillPlugin.cpp
    Fill/FillPlugin.hpp
    Fill/SharedLibrary.cpp
    Fill/SharedLibrary.hpp
    Fill/Fill.cpp
    Fill/Fill.hpp
    FilamentMixer.cpp
    FilamentMixer.hpp
    FilamentMixerModel.hpp
    ColorDecomposeRecipe.cpp
    ColorDecomposeRecipe.hpp
    TexturePainting.hpp
    TexturePainting.cpp
    TextureToColor/TextureToColor.hpp
    TextureToColor/TextureToColor.cpp
    TextureToColor/ColorUtils.hpp
    TextureToColor/ColorUtils.cpp
    FlowConfig.cpp
    FlushVolCalc.cpp
    FlushVolCalc.hpp
    Format/3mf.cpp
    Format/3mf.hpp
    Format/AMF.cpp
    Format/AMF.hpp
    Format/AssembleList.cpp
    Format/AssembleList.hpp
    Format/DRC.cpp
    Format/DRC.hpp
    Format/bbs_3mf.cpp
    Format/bbs_3mf.hpp
    Format/ProjectStrategy.hpp
    format.hpp
    Format/OBJ.cpp
    Format/OBJ.hpp
    Format/AssimpImport.hpp
    Format/AssimpImport.cpp
    Format/ResourcePathUtils.hpp
    Format/objparser.cpp
    Format/objparser.hpp
    Format/SL1.cpp
    Format/SL1.hpp
    Format/STEP.cpp
    Format/STEP.hpp
    Format/STL.cpp
    Format/STL.hpp
    Format/svg.cpp
    Format/svg.hpp
    Format/ZipperArchiveImport.cpp
    Format/ZipperArchiveImport.hpp
    GCode/AdaptivePAInterpolator.cpp
    GCode/AdaptivePAInterpolator.hpp
    GCode/AdaptivePAProcessor.cpp
    GCode/AdaptivePAProcessor.hpp
    GCode/AvoidCrossingPerimeters.cpp
    GCode/AvoidCrossingPerimeters.hpp
    GCode/BeltBackTransform.cpp
    GCode/BeltBackTransform.hpp
    GCode/MachineFrameTransform.cpp
    GCode/MachineFrameTransform.hpp
    GCode/BeltKinematics.cpp
    GCode/BeltKinematics.hpp
    GCode/MachineKinematics.cpp
    GCode/MachineKinematics.hpp
    GCode/ConflictChecker.cpp
    GCode/ConflictChecker.hpp
    GCode/CoolingBuffer.cpp
    GCode/CoolingBuffer.hpp
    GCode/ExclusionVolumePathCheck.cpp
    GCode/ExclusionVolumePathCheck.hpp
    GCode/ExclusionVolumeTravelAvoidance.cpp
    GCode/ExclusionVolumeTravelAvoidance.hpp
    GCode/TimelapsePosPicker.cpp
    GCode/TimelapsePosPicker.hpp
    GCode.cpp
    GCode/ExtrusionProcessor.hpp
    GCode/FanMover.cpp
    GCode/FanMover.hpp
    GCode/GCodeProcessor.cpp
    GCode/GCodeProcessor.hpp
    GCode/GCodeStatistics.cpp
    GCode/GCodeStatistics.hpp
    GCode/ElegooGCodeProcessorHelper.cpp
    GCode.hpp
    GCode/PchipInterpolatorHelper.cpp
    GCode/PchipInterpolatorHelper.hpp
    GCode/PressureEqualizer.cpp
    GCode/PressureEqualizer.hpp
    GCode/PrintExtents.cpp
    GCode/PrintExtents.hpp
    GCodeReader.cpp
    GCodeReader.hpp
    GCode/RetractWhenCrossingPerimeters.cpp
    GCode/RetractWhenCrossingPerimeters.hpp
    GCode/SeamPlacer.cpp
    GCode/SeamPlacer.hpp
    GCode/SeamContour.cpp
    GCode/SeamContour.hpp
    GCode/PreciseSeam.cpp
    GCode/PreciseSeam.hpp
    GCode/PreciseSeamInternal.hpp
    GCode/SmallAreaInfillFlowCompensator.cpp
    GCode/SmallAreaInfillFlowCompensator.hpp
    GCode/SpiralVase.cpp
    GCode/SpiralVase.hpp
    GCode/WipePathHelpers.cpp
    GCode/WipePathHelpers.hpp
    GCode/ThumbnailData.cpp
    GCode/ThumbnailData.hpp
    GCode/Thumbnails.cpp
    GCode/Thumbnails.hpp
    GCode/ToolOrdering.cpp
    GCode/ToolOrdering.hpp
    GCode/WipeTower2.cpp
    GCode/WipeTower2.hpp
    GCode/WipeTower.cpp
    GCode/WipeTower.hpp
    GCode/WipeTowerEstimate.cpp
    GCode/WipeTowerEstimate.hpp
    GCodeWriter.cpp
    GCodeWriter.hpp
    Geometry/ArcWelder.hpp
    Geometry/ArcWelder.cpp
    Geometry/Bicubic.hpp
    Geometry/Circle.cpp
    Geometry/Circle.hpp
    Geometry/ConvexHull.cpp
    Geometry/ConvexHull.hpp
    Geometry/Curves.hpp
    Geometry/VoronoiVisualUtils.hpp
    IMEXArrange.cpp
    IMEXArrange.hpp
    IMEXHelpers.cpp
    IMEXHelpers.hpp
    IMEXZones.cpp
    IMEXZones.hpp
    InstanceLock.cpp
    InstanceLock.hpp
    Int128.hpp
    KDTreeIndirect.hpp
    Layer.cpp
    Layer.hpp
    LayerRegion.cpp
    LayOnFace.cpp
    LayOnFace.hpp
    LocalesUtils.cpp
    LocalesUtils.hpp
    MarchingSquares.hpp
    Measure.cpp
    Measure.hpp
    MeasureUtils.hpp
    MeshSplitImpl.hpp
    MinAreaBoundingBox.cpp
    MinAreaBoundingBox.hpp
    MinimumSpanningTree.cpp
    MinimumSpanningTree.hpp
    miniz_extension.cpp
    miniz_extension.hpp
    ModelArrange.cpp
    ModelArrange.hpp
    Model.cpp
    Model.hpp
    MTUtils.hpp
    MultiMaterialSegmentation.cpp
    MultiMaterialSegmentation.hpp
    MutablePolygon.cpp
    MutablePolygon.hpp
    MutablePriorityQueue.hpp
    NormalUtils.cpp
    NormalUtils.hpp
    NSVGUtils.cpp
    NSVGUtils.hpp
    ObjColorUtils.cpp
    ObjColorUtils.hpp
    ObjectID.cpp
    ObjectID.hpp
    Optimize/BruteforceOptimizer.hpp
    Optimize/NLoptOptimizer.hpp
    Optimize/Optimizer.hpp
    Orient.cpp
    Orient.hpp
    ParallelFor.hpp
    ParallelResolve.hpp
    ParameterUtils.cpp
    ParameterUtils.hpp
    pchheader.cpp
    pchheader.hpp
    PerimeterGenerator.cpp
    PerimeterGenerator.hpp
    PlaceholderParser.cpp
    PlaceholderParser.hpp
    Platform.cpp
    Platform.hpp
    PNGReadWrite.cpp
    PNGReadWrite.hpp
    PolygonTrimmer.cpp
    PolygonTrimmer.hpp
    PresetBundle.cpp
    PresetBundle.hpp
    PrinterBedAssets.hpp
    PresetCacheFormat.cpp
    PresetCacheFormat.hpp
    Preset.cpp
    Preset.hpp
    PrincipalComponents2D.cpp
    PrincipalComponents2D.hpp
    PublishSettings.cpp
    PublishSettings.hpp
    PrintApply.cpp
    PrintBase.cpp
    PrintBase.hpp
    MaterialType.cpp
    MaterialType.hpp
    PrintConfig.cpp
    PrintConfig.hpp
    Print.cpp
    Print.hpp
    PrintObject.cpp
    PrintObjectSlice.cpp
    PrintRegion.cpp
    ProjectTask.cpp
    ProjectTask.hpp
    QuadricEdgeCollapse.cpp
    QuadricEdgeCollapse.hpp
    Semver.cpp
    ShortEdgeCollapse.cpp
    ShortEdgeCollapse.hpp
    SLA/AGGRaster.hpp
    SLA/BoostAdapter.hpp
    SLA/Clustering.cpp
    SLA/Clustering.hpp
    SLA/ConcaveHull.cpp
    SLA/ConcaveHull.hpp
    SLA/Concurrency.hpp
    SLA/Hollowing.cpp
    SLA/Hollowing.hpp
    SLA/IndexedMesh.cpp
    SLA/IndexedMesh.hpp
    SLA/JobController.hpp
    SLA/Pad.cpp
    SLA/Pad.hpp
    SLAPrint.cpp
    SLAPrint.hpp
    SLAPrintSteps.cpp
    SLAPrintSteps.hpp
    SLA/RasterBase.cpp
    SLA/RasterBase.hpp
    SLA/RasterToPolygons.cpp
    SLA/RasterToPolygons.hpp
    SLA/ReprojectPointsOnMesh.hpp
    SLA/Rotfinder.cpp
    SLA/Rotfinder.hpp
    SLA/SpatIndex.cpp
    SLA/SpatIndex.hpp
    SLA/SupportPointGenerator.cpp
    SLA/SupportPointGenerator.hpp
    SLA/SupportPoint.hpp
    SLA/SupportTreeBuilder.cpp
    SLA/SupportTreeBuilder.hpp
    SLA/SupportTreeBuildsteps.cpp
    SLA/SupportTreeBuildsteps.hpp
    SLA/SupportTree.cpp
    SLA/SupportTree.hpp
    SLA/SupportTreeMesher.cpp
    SLA/SupportTreeMesher.hpp
    SlicesToTriangleMesh.cpp
    SlicesToTriangleMesh.hpp
    SlicingAdaptive.cpp
    SlicingAdaptive.hpp
    Slicing.cpp
    Slicing.hpp
    Support/BeltFloorContext.cpp
    Support/BeltFloorContext.hpp
    Support/SupportCommon.cpp
    Support/SupportCommon.hpp
    Support/SupportLayer.hpp
    Support/SupportMaterial.cpp
    Support/SupportMaterial.hpp
    Support/SupportParameters.hpp
    Support/SupportSpotsGenerator.cpp
    Support/SupportSpotsGenerator.hpp
    Support/TreeModelVolumes.cpp
    Support/TreeModelVolumes.hpp
    Support/TreeSupport3D.cpp
    Support/TreeSupport3D.hpp
    Support/TreeSupportCommon.hpp
    Support/TreeSupport.cpp
    Support/TreeSupport.hpp
    SurfaceCollection.cpp
    SurfaceCollection.hpp
    Surface.cpp
    Surface.hpp
    SurfaceMesh.hpp
    Technologies.hpp
    Tesselate.cpp
    Tesselate.hpp
    TextConfiguration.hpp
    TextureDisplacement.cpp
    TextureDisplacement.hpp
    TextureBake/TextureBakeIndex.cpp
    TextureBake/TextureBakeIndex.hpp
    TextureBake/TextureBakeSubdivide.cpp
    TextureBake/TextureBakeSubdivide.hpp
    TextureBake/TextureBakeRegularize.cpp
    TextureBake/TextureBakeRegularize.hpp
    TextureBake/TextureBakeRelocate.cpp
    TextureBake/TextureBakeRelocate.hpp
    TextureBake/TextureBakeFlip.cpp
    TextureBake/TextureBakeFlip.hpp
    TextureBake/TextureBakeDebug.cpp
    TextureBake/TextureBakeDebug.hpp
    TextureBake/TextureBakeDisplace.cpp
    TextureBake/TextureBakeDisplace.hpp
    TextureBake/TextureBakeDecimate.cpp
    TextureBake/TextureBakeDecimate.hpp
    TextureBake/TextureBakeRepair.cpp
    TextureBake/TextureBakeRepair.hpp
    TextureBake/TextureBakePipeline.cpp
    TextureBake/TextureBakePipeline.hpp
    TextureBake/TextureBakeMesh.cpp
    TextureBake/TextureBakeMesh.hpp
    Thread.cpp
    Thread.hpp
    Time.cpp
    Time.hpp
    Timer.cpp
    Timer.hpp
    TriangleMesh.cpp
    TriangleMesh.hpp
    TriangleMeshDeal.cpp
    TriangleMeshDeal.hpp
    TriangleMeshSlicer.cpp
    TriangleMeshSlicer.hpp
    TriangleSelector.cpp
    TriangleSelector.hpp
    TriangleSetSampling.cpp
    TriangleSetSampling.hpp
    TriangulateWall.cpp
    TriangulateWall.hpp
    utils.cpp
    Utils.hpp
    Zipper.cpp
    Zipper.hpp
    FilamentGroup.hpp
    FilamentGroup.cpp
    FilamentGroupUtils.hpp
    FilamentGroupUtils.cpp
    MultiNozzleUtils.hpp
    MultiNozzleUtils.cpp
    GCode/ToolOrderUtils.hpp
    GCode/ToolOrderUtils.cpp
    FlushVolPredictor.hpp
    FlushVolPredictor.cpp
)

# Parametric Design/CAD kernel. Needs OCCT's ModelingAlgorithms module and the
# vendored SolveSpace solver; both are pulled in only when SLIC3R_CAD is ON.
if (SLIC3R_CAD)
    list(APPEND lisbslic3r_sources
            CAD/GeometryEngine.cpp
            CAD/GeometryEngine.hpp
            CAD/SketchEngine.cpp
            CAD/SketchEngine.hpp
            CAD/SketchConstraints.cpp
            CAD/SketchConstraints.hpp
            CAD/SketchSolver.cpp
            CAD/SketchSolver.hpp
            CAD/SketchInference.cpp
            CAD/SketchInference.hpp
            CAD/SketchImport.cpp
            CAD/SketchImport.hpp
            CAD/CadDocument.cpp
            CAD/CadDocument.hpp
            CAD/ThreadStandards.cpp
            CAD/ThreadStandards.hpp
        )
endif ()

if (APPLE)
    list(APPEND lisbslic3r_sources
            MacUtils.mm
            Format/ModelIO.hpp
            Format/ModelIO.mm
        )
endif ()

# The geometry and filler core of libslic3r_fillkit, which the built-in infill pattern libraries
# (src/infill_patterns) link as well as libslic3r. The list is closed: its sources reference no symbol
# that only the rest of libslic3r defines, but for the seams a pattern library defines itself
# (src/infill_patterns/runtime/ModuleSeams.cpp): the log of Fill/Library/KitLog.hpp,
# Fill::fill_surface_extrusion() and the TBB allocator. The loops of ParallelFor.hpp are a seam as well,
# for the pattern sources that run parallel loops.
set(libslic3r_fillkit_sources
    Arachne/BeadingStrategy/BeadingStrategy.cpp
    Arachne/BeadingStrategy/BeadingStrategy.hpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.cpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.hpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.cpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.hpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.cpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.hpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.cpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.hpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.cpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.hpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.cpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.hpp
    Arachne/SkeletalTrapezoidation.cpp
    Arachne/SkeletalTrapezoidation.hpp
    Arachne/SkeletalTrapezoidationEdge.hpp
    Arachne/SkeletalTrapezoidationGraph.cpp
    Arachne/SkeletalTrapezoidationGraph.hpp
    Arachne/SkeletalTrapezoidationJoint.hpp
    Arachne/utils/ExtrusionJunction.hpp
    Arachne/utils/ExtrusionLine.cpp
    Arachne/utils/ExtrusionLine.hpp
    Arachne/utils/HalfEdge.hpp
    Arachne/utils/HalfEdgeGraph.hpp
    Arachne/utils/HalfEdgeNode.hpp
    Arachne/utils/PolygonsPointIndex.hpp
    Arachne/utils/PolygonsSegmentIndex.hpp
    Arachne/utils/PolylineStitcher.cpp
    Arachne/utils/PolylineStitcher.hpp
    Arachne/utils/SparseGrid.hpp
    Arachne/utils/SparseLineGrid.hpp
    Arachne/utils/SparsePointGrid.hpp
    Arachne/utils/SquareGrid.cpp
    Arachne/utils/SquareGrid.hpp
    Arachne/WallToolPaths.cpp
    Arachne/WallToolPaths.hpp
    ArcFitter.cpp
    ArcFitter.hpp
    BoundingBox.cpp
    BoundingBox.hpp
    Circle.cpp
    Circle.hpp
    ClipperUtils.cpp
    ClipperUtils.hpp
    EdgeGrid.cpp
    EdgeGrid.hpp
    ExPolygon.cpp
    ExPolygon.hpp
    ExtrusionEntity.cpp
    ExtrusionEntity.hpp
    ExtrusionEntityCollection.cpp
    ExtrusionEntityCollection.hpp
    Fill/FillAdaptiveFrame.cpp
    Fill/FillBase.cpp
    Fill/FillBase.hpp
    Fill/FillCornerSmoothing.cpp
    Fill/FillCornerSmoothing.hpp
    Fill/Library/BuiltinMirror.cpp
    Fill/Library/BuiltinMirror.hpp
    Fill/Library/KitLog.hpp
    Fill/Library/orca_infill_builtin.h
    Flow.cpp
    Flow.hpp
    GCode/OrderingStrategies.cpp
    GCode/OrderingStrategies.hpp
    Geometry.cpp
    Geometry.hpp
    Geometry/MedialAxis.cpp
    Geometry/MedialAxis.hpp
    Geometry/Voronoi.cpp
    Geometry/Voronoi.hpp
    Geometry/VoronoiOffset.cpp
    Geometry/VoronoiOffset.hpp
    Geometry/VoronoiUtils.cpp
    Geometry/VoronoiUtils.hpp
    Geometry/VoronoiUtilsCgal.cpp
    Geometry/VoronoiUtilsCgal.hpp
    I18N.cpp
    I18N.hpp
    libslic3r.cpp
    libslic3r.h
    Line.cpp
    Line.hpp
    MultiPoint.cpp
    MultiPoint.hpp
    Point.cpp
    Point.hpp
    Polygon.cpp
    Polygon.hpp
    Polyline.cpp
    Polyline.hpp
    ShortestPath.cpp
    ShortestPath.hpp
    SVG.cpp
    SVG.hpp
    VariableWidth.cpp
    VariableWidth.hpp
)

add_library(libslic3r STATIC ${lisbslic3r_sources}
    "${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h"
    ${OpenVDBUtils_SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lisbslic3r_sources})

if (SLIC3R_STATIC)
    set(CGAL_Boost_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
endif ()
set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ON CACHE BOOL "" FORCE)

cmake_policy(PUSH)
cmake_policy(SET CMP0011 NEW)
# CGAL's config resets policies (cmake_minimum_required ...3.31), so a plain SET
# can't reach it; the default opts its Boost lookup into BoostConfig (CMP0167).
set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)
find_package(CGAL 6.2 REQUIRED)
find_package(OpenCV REQUIRED core)
find_package(assimp REQUIRED)
unset(CMAKE_POLICY_DEFAULT_CMP0167)
cmake_policy(POP)

add_library(libslic3r_cgal STATIC
    CutSurface.hpp CutSurface.cpp
    IntersectionPoints.hpp IntersectionPoints.cpp
    MeshBoolean.hpp MeshBoolean.cpp
    MeshRepair.hpp MeshRepair.cpp
    TryCatchSignal.hpp TryCatchSignal.cpp
    Triangulation.hpp Triangulation.cpp
)
target_include_directories(libslic3r_cgal PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Reset compile options of libslic3r_cgal. Despite it being linked privately, CGAL options
# (-frounding-math) still propagate to dependent libs which is not desired.
get_target_property(_cgal_tgt CGAL::CGAL ALIASED_TARGET)
if (NOT TARGET ${_cgal_tgt})
    set (_cgal_tgt CGAL::CGAL)
endif ()
get_target_property(_opts ${_cgal_tgt} INTERFACE_COMPILE_OPTIONS)
if (_opts)
    set(_opts_bad "${_opts}")
    set(_opts_good "${_opts}")
    list(FILTER _opts_bad INCLUDE REGEX frounding-math)
    list(FILTER _opts_good EXCLUDE REGEX frounding-math)
    set_target_properties(${_cgal_tgt} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_opts_good}")
    target_compile_options(libslic3r_cgal PRIVATE "${_opts_bad}")
endif()

if (IS_CLANG_CL)
    # CGAL passes /fp:strict /fp:except-. clang-cl reports the second as overriding part of
    # the first; the settings cc1 receives are the same ones MSVC produces from that pair.
    target_compile_options(libslic3r_cgal PRIVATE -Wno-overriding-option)
endif ()

# Lets CGAL algorithms given CGAL::Parallel_if_available_tag run on TBB.
target_compile_definitions(libslic3r_cgal PRIVATE CGAL_LINKED_WITH_TBB)
target_link_libraries(libslic3r_cgal PRIVATE ${_cgal_tgt} admesh libigl mcut boost_libs TBB::tbb)

if (MSVC AND "${CMAKE_SIZEOF_VOID_P}" STREQUAL "4") # 32 bit MSVC workaround
    target_compile_definitions(libslic3r_cgal PRIVATE CGAL_DO_NOT_USE_MPZF)
endif ()

encoding_check(libslic3r)

# Find the OCCT and related libraries
set(OpenCASCADE_DIR "${CMAKE_PREFIX_PATH}/lib/cmake/occt")
find_package(OpenCASCADE REQUIRED)

find_package(JPEG REQUIRED)
find_package(Draco REQUIRED)

set(OCCT_LIBS
    TKDESTEP
    TKDE
    TKXCAF
    TKXSBase
    TKVCAF
    TKCAF
    TKLCAF
    TKCDF
    TKV3d
    TKService
    TKMesh
    TKBO
    TKPrim
    TKHLR
    TKShHealing
    TKTopAlgo
    TKGeomAlgo
    TKBRep
    TKGeomBase
    TKG3d
    TKG2d
    TKMath
    TKernel
)
# The CAD kernel is the only consumer of OCCT's ModelingAlgorithms module: TKFillet
# (BRepFilletAPI), TKOffset (BRepOffsetAPI) and TKBool, which the other two need.
#
# PREPEND, never append: this list is single-pass static link order, dependents before
# dependencies — note TKernel, which everything needs, is deliberately last. TKOffset
# references BRepAlgo_Loop, which TKBool defines, so TKOffset must come BEFORE TKBool.
# Appending put it after, and a strictly single-pass linker (the Flatpak build) failed with
#     libTKOffset.a(BRepOffset_MakeLoops.cxx.o): undefined reference to
#     `BRepAlgo_Loop::BRepAlgo_Loop()'
# while the ordinary Linux, macOS and Windows links resolved it anyway. Use set() rather
# than list(PREPEND), which needs CMake 3.15 and this project supports 3.13.
if (SLIC3R_CAD)
    set(OCCT_LIBS TKFillet TKOffset TKBool ${OCCT_LIBS})
    # deps is configured separately, so its SLIC3R_CAD can differ from ours. The module is
    # all-or-nothing, so one absent toolkit proves it; fail here rather than at link time.
    if (NOT TARGET TKFillet)
        message(FATAL_ERROR
            "SLIC3R_CAD is ON, but the OpenCASCADE in ${CMAKE_PREFIX_PATH} was built without "
            "BUILD_MODULE_ModelingAlgorithms. Rebuild the dependencies with -DSLIC3R_CAD=ON, "
            "or configure this project with -DSLIC3R_CAD=OFF.")
    endif ()
endif ()
# Published for the Windows packaging step in the top-level CMakeLists.txt.
set(OCCT_LIBS "${OCCT_LIBS}" CACHE INTERNAL "OCCT toolkits linked by libslic3r")
if (WIN32)
    # OCCT installs RelWithDebInfo and Debug builds to bin/occti and bin/occtd, so each build
    # type's DLL directory is read from the imported target, falling back to the first installed
    # build, as CMake does when linking.
    get_target_property(_occt_configs TKernel IMPORTED_CONFIGURATIONS)
    list(GET _occt_configs 0 _occt_fallback)
    foreach (_build_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(_occt_config ${_occt_fallback})
        if (_build_config IN_LIST _occt_configs)
            set(_occt_config ${_build_config})
        endif ()
        get_target_property(_occt_dll TKernel IMPORTED_LOCATION_${_occt_config})
        get_filename_component(_occt_bin "${_occt_dll}" DIRECTORY)
        set(OCCT_BIN_DIR_${_build_config} "${_occt_bin}" CACHE INTERNAL "OCCT DLL directory for ${_build_config} builds")
    endforeach ()
endif ()

# The settings libslic3r compiles with. libslic3r_public_settings holds what its headers need and is passed on to every
# target that links libslic3r; libslic3r_compile_settings adds what only its own sources need. The sources libslic3r
# shares with the built-in infill pattern libraries (libslic3r_fillkit and the pattern objects below) link the same
# target, so that the application and the libraries run the same machine code.
add_library(libslic3r_public_settings INTERFACE)
target_compile_definitions(libslic3r_public_settings INTERFACE -DUSE_TBB -DTBB_USE_CAPTURED_EXCEPTION=0)
# CGAL changes Kd_tree's member layout when TBB is enabled. Match libslic3r_cgal
# in every consumer so inline constructors/destructors cannot mix the two layouts.
target_compile_definitions(libslic3r_public_settings INTERFACE CGAL_LINKED_WITH_TBB)
if (WIN32)
    # Public, since BlacklistedLibraryCheck.hpp includes windows.h. Empty
    # WIN32_LEAN_AND_MEAN matches the sources that define it themselves; bare
    # NOMINMAX matches the one libigl already passes.
    target_compile_definitions(libslic3r_public_settings INTERFACE "WIN32_LEAN_AND_MEAN=" "NOMINMAX")
endif ()
target_include_directories(libslic3r_public_settings INTERFACE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(libslic3r_public_settings SYSTEM INTERFACE ${EXPAT_INCLUDE_DIRS} ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(libslic3r_public_settings INTERFACE
    Eigen3::Eigen
    admesh
    libigl
    miniz
    opencv_world
    assimp::assimp
    )
if (SLIC3R_CAD)
    target_link_libraries(libslic3r_public_settings INTERFACE SLVS::slvs)
endif ()

add_library(libslic3r_compile_settings INTERFACE)
if (USE_SLIC3R_CONSOLE_LOG)
    target_compile_definitions(libslic3r_compile_settings INTERFACE $<$<CONFIG:RelWithDebInfo>:SLIC3R_CONSOLE_LOG>)
endif()
# The C ABI of infill plugins (Fill/InfillPlugins.cpp, Fill/FillPlugin.cpp), on which the ABI of the built-in pattern
# libraries (Fill/Library/orca_infill_builtin.h) builds.
target_include_directories(libslic3r_compile_settings INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/TextureToColor
                           ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/sdk/infill/include)
target_link_libraries(libslic3r_compile_settings INTERFACE
    libslic3r_public_settings
    ${CMAKE_DL_LIBS}
    ${EXPAT_LIBRARIES}
    ${OCCT_LIBS}
    boost_libs
    cereal::cereal
    Clipper2
    draco::draco
    glu-libtess
    JPEG::JPEG
    libslic3r_cgal
    mcut
    noise::noise
    PNG::PNG
    qhull
    qoi
    semver
    TBB::tbb
    TBB::tbbmalloc
    ZLIB::ZLIB
    OpenSSL::Crypto
    )

if (APPLE)
    find_library(FOUNDATION Foundation REQUIRED)
    find_library(MODELIO ModelIO REQUIRED)
    target_link_libraries(libslic3r_compile_settings INTERFACE ${FOUNDATION} ${MODELIO})
endif ()

if (TARGET OpenVDB::openvdb)
    target_link_libraries(libslic3r_compile_settings INTERFACE OpenVDB::openvdb)
endif()

if(WIN32)
    target_link_libraries(libslic3r_compile_settings INTERFACE Psapi.lib bcrypt.lib)
endif()

if(SLIC3R_PROFILE)
    target_link_libraries(libslic3r_compile_settings INTERFACE Shiny)
endif()

# Sources shared with the built-in infill pattern libraries: the flags of libslic3r, a section per function and per
# datum so that a library's linker drops what its pattern does not reach, and Fill/Library/KitLog.hpp ahead of every
# source. The pattern objects reuse the precompiled header of libslic3r_fillkit, which includes KitLog.hpp.
function(libslic3r_shared_sources target)
    target_link_libraries(${target} PRIVATE libslic3r_compile_settings)
    set_target_properties(${target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    if (NOT MSVC)
        # clang-cl and MSVC put every function in a section of its own with /O1 and /O2.
        target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    endif ()
    if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY AND NOT CMAKE_VERSION VERSION_LESS 3.16)
        if (target STREQUAL "libslic3r_fillkit")
            add_precompiled_header(${target} pchheader.hpp FORCEINCLUDE)
            target_precompile_headers(${target} PRIVATE Fill/Library/KitLog.hpp)
        else ()
            target_precompile_headers(${target} REUSE_FROM libslic3r_fillkit)
            target_compile_options(${target} PRIVATE "$<$<CXX_COMPILER_ID:Clang,AppleClang>:SHELL:-Xclang -fno-pch-timestamp>")
        endif ()
    elseif (MSVC)
        target_compile_options(${target} PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/Fill/Library/KitLog.hpp")
    else ()
        target_compile_options(${target} PRIVATE "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/Fill/Library/KitLog.hpp\"")
    endif ()
    encoding_check(${target})
endfunction()

# Compiled once, and linked into libslic3r and into every pattern library.
add_library(libslic3r_fillkit STATIC ${libslic3r_fillkit_sources})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${libslic3r_fillkit_sources})
libslic3r_shared_sources(libslic3r_fillkit)
target_link_libraries(libslic3r_fillkit PUBLIC libslic3r_public_settings)

# libslic3r's definitions of the seams that a pattern library defines on its host: the log of libslic3r_fillkit and the
# loops of ParallelFor.hpp. Linked after libslic3r_fillkit, so that a linker that reads each archive once finds them
# after the kit's references.
# Fill::fill_surface_extrusion() stays in Fill/FillExtrusion.cpp: only libslic3r's own fill code reaches
# Fill/FillBase.cpp, and that code references the extrusions as well.
add_library(libslic3r_fillkit_host STATIC Fill/Library/KitLogHost.cpp ParallelForTbb.cpp)
target_link_libraries(libslic3r_fillkit_host PRIVATE libslic3r_compile_settings)
encoding_check(libslic3r_fillkit_host)

# The sources of the infill patterns, an object library per group of rows of src/infill_patterns, listed in the global
# property ORCA_INFILL_PATTERN_OBJECTS. A pattern library links those of its row. libslic3r links none of them, unless
# ORCA_INFILL_PATTERN_LINKAGE is STATIC: then it links all of them with the entries of the libraries.
function(libslic3r_add_pattern_objects target)
    add_library(${target} OBJECT ${ARGN})
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ARGN})
    libslic3r_shared_sources(${target})
    set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_PATTERN_OBJECTS ${target})
endfunction()
libslic3r_add_pattern_objects(orca_fill_rectilinear_obj Fill/FillRectilinear.cpp Fill/FillRectilinear.hpp)
if (ORCA_INFILL_LINE)
    libslic3r_add_pattern_objects(orca_fill_line_obj Fill/FillLine.cpp Fill/FillLine.hpp)
endif ()
libslic3r_add_pattern_objects(orca_fill_concentric_obj Fill/FillConcentric.cpp Fill/FillConcentric.hpp)
libslic3r_add_pattern_objects(orca_fill_spiralinset_obj Fill/FillSpiralInset.cpp Fill/FillSpiralInset.hpp)
libslic3r_add_pattern_objects(orca_fill_concentric_internal_obj Fill/FillConcentricInternal.cpp Fill/FillConcentricInternal.hpp)
libslic3r_add_pattern_objects(orca_fill_honeycomb_obj Fill/FillHoneycomb.cpp Fill/FillHoneycomb.hpp)
libslic3r_add_pattern_objects(orca_fill_3dhoneycomb_obj Fill/Fill3DHoneycomb.cpp Fill/Fill3DHoneycomb.hpp)
libslic3r_add_pattern_objects(orca_fill_crosshatch_obj Fill/FillCrossHatch.cpp Fill/FillCrossHatch.hpp)
libslic3r_add_pattern_objects(orca_fill_planepath_obj Fill/FillPlanePath.cpp Fill/FillPlanePath.hpp)
libslic3r_add_pattern_objects(orca_fill_gyroid_obj Fill/FillGyroid.cpp Fill/FillGyroid.hpp)
if (ORCA_INFILL_TPMSD)
    libslic3r_add_pattern_objects(orca_fill_tpmsd_obj Fill/FillTpmsD.cpp Fill/FillTpmsD.hpp)
endif ()
if (ORCA_INFILL_TPMSFK)
    libslic3r_add_pattern_objects(orca_fill_tpmsfk_obj Fill/FillTpmsFK.cpp Fill/FillTpmsFK.hpp)
endif ()
libslic3r_add_pattern_objects(orca_fill_adaptive_field_obj Fill/AdaptiveInfillField.cpp Fill/AdaptiveInfillField.hpp)
libslic3r_add_pattern_objects(orca_fill_adaptive_obj Fill/FillAdaptive.cpp Fill/FillAdaptive.hpp)
libslic3r_add_pattern_objects(orca_fill_lightning_obj
    Fill/FillLightning.cpp
    Fill/FillLightning.hpp
    Fill/Lightning/DistanceField.cpp
    Fill/Lightning/DistanceField.hpp
    Fill/Lightning/Generator.cpp
    Fill/Lightning/Generator.hpp
    Fill/Lightning/Layer.cpp
    Fill/Lightning/Layer.hpp
    Fill/Lightning/TreeNode.cpp
    Fill/Lightning/TreeNode.hpp)

# libslic3r_compile_settings first, so that libslic3r's own include directories come before those of its dependencies.
target_link_libraries(libslic3r PRIVATE libslic3r_compile_settings)
target_link_libraries(libslic3r PUBLIC libslic3r_public_settings libslic3r_fillkit libslic3r_fillkit_host)
# libnest2d links libslic3r back, so it stays out of the settings the shared sources and the pattern libraries link;
# none of those sources includes it.
target_link_libraries(libslic3r PUBLIC libnest2d)

if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(libslic3r pchheader.hpp FORCEINCLUDE)
endif ()

# orca_infill_build_id.h, per configuration: ORCA_INFILL_BUILD_ID names the build that a built-in infill pattern library
# belongs to, and the application and a library bind only when theirs are equal byte for byte. It names the version,
# the commit, the compiler, the architecture, the configuration, the library ABI and a digest of the flags the shared
# sources compile with, so that release builds of one commit agree. A tree with uncommitted changes when it is
# configured adds a random value of its own, so that two such builds never accept each other's libraries.
function(libslic3r_write_infill_build_id)
    set(_inputs "${CMAKE_CXX_FLAGS}")
    foreach (_target IN ITEMS libslic3r_public_settings libslic3r_compile_settings)
        foreach (_property IN ITEMS INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS)
            get_target_property(_value ${_target} ${_property})
            string(APPEND _inputs "|${_value}")
        endforeach ()
    endforeach ()
    get_directory_property(_definitions COMPILE_DEFINITIONS)
    get_directory_property(_options COMPILE_OPTIONS)
    string(APPEND _inputs "|${_definitions}|${_options}")
    set(_digest "")
    if (CMAKE_CONFIGURATION_TYPES)
        foreach (_config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${_config}" _upper)
            string(SHA256 _hash "${_inputs}|${CMAKE_CXX_FLAGS_${_upper}}")
            string(SUBSTRING "${_hash}" 0 16 _hash)
            string(APPEND _digest "$<$<CONFIG:${_config}>:${_hash}>")
        endforeach ()
    else ()
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _upper)
        string(SHA256 _hash "${_inputs}|${CMAKE_CXX_FLAGS_${_upper}}")
        string(SUBSTRING "${_hash}" 0 16 _digest)
    endif ()

    if (APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(_arch "${CMAKE_OSX_ARCHITECTURES}")
    elseif (MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
        set(_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    else ()
        set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif ()
    string(REPLACE ";" "+" _arch "${_arch}")

    # A commit given by the environment, as GitCommitHash.cmake reads it, is a release build of a clean tree.
    set(_nonce "")
    find_package(Git QUIET)
    if (NOT "$ENV{git_commit_hash}" STREQUAL "" OR NOT GIT_FOUND OR NOT EXISTS "${CMAKE_SOURCE_DIR}/.git")
        set(_dirty 0)
    else ()
        execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                        RESULT_VARIABLE _dirty ERROR_QUIET)
    endif ()
    if (_dirty EQUAL 1)
        if (NOT ORCA_INFILL_BUILD_NONCE)
            string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _random)
            set(ORCA_INFILL_BUILD_NONCE "${_random}" CACHE INTERNAL "Part of the infill pattern library build id of a tree with uncommitted changes")
        endif ()
        set(_nonce "|${ORCA_INFILL_BUILD_NONCE}")
    endif ()

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/orca_infill_build_id.h" CONTENT
"#pragma once
// Generated by src/libslic3r/CMakeLists.txt: the build of OrcaSlicer that the built-in infill pattern libraries bind to.
#include \"git_commit_hash.h\"
#include \"libslic3r/Fill/Library/orca_infill_builtin.h\"

#define ORCA_INFILL_BUILD_ID_STRING_(x) #x
#define ORCA_INFILL_BUILD_ID_STRING(x)  ORCA_INFILL_BUILD_ID_STRING_(x)
#define ORCA_INFILL_BUILD_ID                                                                                             \\
    \"${SoftFever_VERSION}|\" GIT_COMMIT_HASH GIT_COMMIT_SUFFIX \"|${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}|${_arch}|$<CONFIG>|builtin\" \\
    ORCA_INFILL_BUILD_ID_STRING(ORCA_INFILL_BUILTIN_ABI_MAJOR) \".\" ORCA_INFILL_BUILD_ID_STRING(ORCA_INFILL_BUILTIN_ABI_MINOR)     \\
    \"|${_digest}${_nonce}\"
")
endfunction()
libslic3r_write_infill_build_id()

# Lets the sources of target include orca_infill_build_id.h, and git_commit_hash.h, which it includes (src/CMakeLists.txt).
function(orca_use_infill_build_id target)
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/src/libslic3r/$<CONFIG>" "${CMAKE_BINARY_DIR}/src/slic3r")
    add_dependencies(${target} git_commit_hash_header)
endfunction()
# The loader of the pattern libraries (Fill/InfillPatternLibraries.cpp) binds only those of its own build.
orca_use_infill_build_id(libslic3r)
