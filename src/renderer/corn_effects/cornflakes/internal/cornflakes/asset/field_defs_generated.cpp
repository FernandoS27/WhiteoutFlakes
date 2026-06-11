#include <cornflakes/asset/field_defs.hpp>

#include <algorithm>
#include <array>

namespace whiteout::cornflakes {
namespace {

constexpr std::array<FieldDef, 3> kFields_v25_CAnimationClip{{
    FieldDef{"EntityStreams", "link[]"},
    FieldDef{"LengthInSeconds", "float"},
    FieldDef{"PlaybackSpeed", "float"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CAnimationTrack{{
    FieldDef{"TrackName", "string"},
    FieldDef{"Channels", "link[]"},
    FieldDef{"CoordinateFrame", "uint"},
    FieldDef{"CoordinateFrameAxisX", "uint"},
    FieldDef{"CoordinateFrameAxisY", "uint"},
    FieldDef{"CoordinateFrameAxisZ", "uint"},
}};

constexpr std::array<FieldDef, 0> kFields_v25_CBaseSettings{};

constexpr std::array<FieldDef, 6> kFields_v25_CCompilerBlobCache{{
    FieldDef{"ClassName", "string"},
    FieldDef{"Identifier", "uint"},
    FieldDef{"Blob", "unknown[]"},
    FieldDef{"Externals", "link[]"},
    FieldDef{"ExternalCalls", "link[]"},
    FieldDef{"EntryPoint", "link"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CCompilerBlobCacheEntryPoint{{
    FieldDef{"SymbolName", "string"},
    FieldDef{"BytecodeBegin", "uint"},
    FieldDef{"BytecodeEnd", "uint"},
}};

constexpr std::array<FieldDef, 7> kFields_v25_CCompilerBlobCacheExternal{{
    FieldDef{"NameGUID", "string"},
    FieldDef{"TypeName", "string"},
    FieldDef{"NativeType", "uint"},
    FieldDef{"StorageSize", "uint"},
    FieldDef{"MetaType", "uint"},
    FieldDef{"Attributes", "uint"},
    FieldDef{"AccessMask", "uint"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CCompilerBlobCacheFunctionArg{{
    FieldDef{"Type", "uint"},
    FieldDef{"TypeName", "string"},
    FieldDef{"Attributes", "uint"},
}};

constexpr std::array<FieldDef, 4> kFields_v25_CCompilerBlobCacheFunctionDef{{
    FieldDef{"SymbolName", "string"},
    FieldDef{"SymbolSlot", "uint"},
    FieldDef{"FunctionTraits", "uint"},
    FieldDef{"Args", "link[]"},
}};

constexpr std::array<FieldDef, 46> kFields_v25_CEditorAssetEffect{{
    FieldDef{"StartCameraPosition", "float3"},
    FieldDef{"StartCameraOrientation", "float3"},
    FieldDef{"MeshBackdrop", "bool"},
    FieldDef{"MeshPath", "string"},
    FieldDef{"MeshDiffusePath", "string"},
    FieldDef{"LightDirection", "float3"},
    FieldDef{"LightIntensity", "float"},
    FieldDef{"GridBackdrop", "bool"},
    FieldDef{"MeshBackdropVisible", "bool"},
    FieldDef{"MeshAnimPath", "string"},
    FieldDef{"AttribSamplerBindings", "string[]"},
    FieldDef{"LoopingEnabled", "bool"},
    FieldDef{"SoundBackdrop", "bool"},
    FieldDef{"SoundPath", "string"},
    FieldDef{"SoundVolume", "float"},
    FieldDef{"SoundAVSynchronisation", "bool"},
    FieldDef{"LoopDelay", "float"},
    FieldDef{"SpawnKeyboardRepeatRate", "float"},
    FieldDef{"BackgroundColorTop", "float3"},
    FieldDef{"BackgroundColorBottom", "float3"},
    FieldDef{"LoopDeterminismConstantSeed", "bool"},
    FieldDef{"MeshPosition", "float3"},
    FieldDef{"GridSize", "float"},
    FieldDef{"GridColor", "float4"},
    FieldDef{"GridSubColor", "float4"},
    FieldDef{"LightColor", "float3"},
    FieldDef{"AmbientColor", "float3"},
    FieldDef{"MeshOrientation", "float3"},
    FieldDef{"GridCollisions", "bool"},
    FieldDef{"MeshScale", "float3"},
    FieldDef{"GridPosition", "float3"},
    FieldDef{"AnimationBackdrop", "bool"},
    FieldDef{"AnimationPreset", "string"},
    FieldDef{"AnimSpeedModifier", "float"},
    FieldDef{"ApplyRotation", "bool"},
    FieldDef{"ApplyScale", "bool"},
    FieldDef{"GridSecondarySubdivisions", "int"},
    FieldDef{"WeightedSampling", "bool"},
    FieldDef{"WindBackdrop", "bool"},
    FieldDef{"WindConstantStrength", "float3"},
    FieldDef{"OccluderBackdrop", "bool"},
    FieldDef{"OccluderPosition", "float3"},
    FieldDef{"OccluderOrientation", "float3"},
    FieldDef{"OccluderSize", "float2"},
    FieldDef{"OccluderColor", "float4"},
    FieldDef{"BuildVersion", "string"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CEngineRendererInterface{{
    FieldDef{"InterfaceName", "string"},
    FieldDef{"RendererFeatures", "link[]"},
}};

constexpr std::array<FieldDef, 24> kFields_v25_CLayerCompileCache{{
    FieldDef{"ConstantData", "uint4[]"},
    FieldDef{"RangeData", "uint4[]"},
    FieldDef{"Fields", "link[]"},
    FieldDef{"Attribs", "link[]"},
    FieldDef{"AttribSamplers", "link[]"},
    FieldDef{"Events", "link[]"},
    FieldDef{"SpatialLayers", "link[]"},
    FieldDef{"Samplers", "link[]"},
    FieldDef{"Renderers", "link[]"},
    FieldDef{"BlobCache_IR_TimeVarying", "link"},
    FieldDef{"BlobCache_IR_TimeFixed", "link[]"},
    FieldDef{"BlobCache_Backends", "link[]"},
    FieldDef{"RootEvent", "link"},
    FieldDef{"UsageFlags", "uint"},
    FieldDef{"HasEvolutionSideEffects", "bool"},
    FieldDef{"NeedsEvolveOnDeath", "bool"},
    FieldDef{"PreferredSimLocation", "int"},
    FieldDef{"PreferredStorageSize", "int"},
    FieldDef{"PreferredLocalization", "int"},
    FieldDef{"DeterminismEnabled", "bool"},
    FieldDef{"RandomSeedModifier", "uint"},
    FieldDef{"LODMetricOverride", "bool"},
    FieldDef{"LODDistanceMin", "float"},
    FieldDef{"LODDistanceMax", "float"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CLayerCompileCacheAttrib{{
    FieldDef{"AttrName", "string"},
    FieldDef{"AttrDescription", "string_localized"},
    FieldDef{"AttrType", "uint"},
    FieldDef{"AttrDataSemantic", "int"},
    FieldDef{"AttrFlags", "uint"},
    FieldDef{"AttrMinI4", "int4"},
    FieldDef{"AttrMaxI4", "int4"},
    FieldDef{"AttrDefaultValueI4", "int4"},
    FieldDef{"AttrMinF4", "float4"},
    FieldDef{"AttrMaxF4", "float4"},
    FieldDef{"AttrDefaultValueF4", "float4"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CLayerCompileCacheAttribSampler{{
    FieldDef{"AttrName", "string"},
    FieldDef{"AttrDescription", "string_localized"},
    FieldDef{"AttrDefaultDefinition", "link"},
    FieldDef{"AttrDefaultData", "string"},
    FieldDef{"AttrDefaultType", "int"},
    FieldDef{"AttrUsageFlags", "uint"},
}};

constexpr std::array<FieldDef, 4> kFields_v25_CLayerCompileCacheEvent{{
    FieldDef{"EventName", "string"},
    FieldDef{"EventFlags", "uint"},
    FieldDef{"EventPayload", "link[]"},
    FieldDef{"EventPayloadSizeInBytes", "uint"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CLayerCompileCacheEventPayload{{
    FieldDef{"PayloadName", "string"},
    FieldDef{"PayloadType", "uint"},
    FieldDef{"PayloadFootprintInBytes", "uint"},
    FieldDef{"PayloadFlags", "uint"},
    FieldDef{"PayloadKind", "uint"},
    FieldDef{"PayloadRangeSlot", "uint"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CLayerCompileCacheField{{
    FieldDef{"FieldName", "string"},
    FieldDef{"FieldType", "uint"},
    FieldDef{"FieldStorageSize", "uint"},
    FieldDef{"FieldFlags", "uint"},
    FieldDef{"FieldConstantSlot", "int"},
    FieldDef{"FieldRangeSlot", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v25_CLayerCompileCacheRenderer{{
    FieldDef{"RendererClass", "int"},
    FieldDef{"Streams", "link[]"},
    FieldDef{"Properties", "link[]"},
    FieldDef{"FeatureSetPath", "string"},
    FieldDef{"DrawOrder", "int"},
}};

constexpr std::array<FieldDef, 4> kFields_v25_CLayerCompileCacheRendererParticleInput{{
    FieldDef{"Semantic", "uint"},
    FieldDef{"IndexInStorage", "uint"},
    FieldDef{"AdditionalFieldName", "string"},
    FieldDef{"AdditionalFieldType", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v25_CLayerCompileCacheRendererProperty{{
    FieldDef{"PropertyName", "string"},
    FieldDef{"PropertyType", "int"},
    FieldDef{"PropertyValueNumeric", "uint4"},
    FieldDef{"PropertyValueStr", "string"},
    FieldDef{"PropertySemantic", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v25_CLayerCompileCacheSampler{{
    FieldDef{"SamplerName", "string"},
    FieldDef{"Sampler", "link"},
    FieldDef{"SamplerData", "string"},
    FieldDef{"SamplerType", "int"},
    FieldDef{"SamplerUsageFlags", "uint"},
}};

constexpr std::array<FieldDef, 5> kFields_v25_CLayerCompileCacheSpatialLayer{{
    FieldDef{"SpatialLayerName", "string"},
    FieldDef{"SpatialLayerLocalName", "string"},
    FieldDef{"SpatialLayerFlags", "uint"},
    FieldDef{"SpatialLayerCellSize", "float"},
    FieldDef{"SpatialLayerPayload", "link[]"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CLayerCompileCacheSpatialLayerPayload{{
    FieldDef{"PayloadName", "string"},
    FieldDef{"PayloadType", "uint"},
    FieldDef{"PayloadFlags", "uint"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CLayerGraphCompileCache{{
    FieldDef{"EntryPointID", "int"},
    FieldDef{"EntrySlots", "link[]"},
    FieldDef{"LayerSlots", "link[]"},
    FieldDef{"EventSlots", "link[]"},
    FieldDef{"BroadSlots", "link[]"},
    FieldDef{"CoordinateSystem", "uint4"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CLayerGraphCompileCache_BroadSlot{{
    FieldDef{"BroadName", "string"},
    FieldDef{"ParentBroadSlots", "int[]"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CLayerGraphCompileCache_EntrySlot{{
    FieldDef{"EntryName", "string_unicode"},
    FieldDef{"EventSlot", "int"},
}};

constexpr std::array<FieldDef, 4> kFields_v25_CLayerGraphCompileCache_EventSlot{{
    FieldDef{"EventName", "string"},
    FieldDef{"ParentLayerSlot", "int"},
    FieldDef{"LayerTargets", "int[]"},
    FieldDef{"BroadTargets", "int[]"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CLayerGraphCompileCache_LayerSlot{{
    FieldDef{"LayerCache", "link"},
    FieldDef{"ParentEventSlots", "int[]"},
    FieldDef{"EventSlots", "int[]"},
}};

constexpr std::array<FieldDef, 46> kFields_v25_CParticleAttributeDeclaration{{
    FieldDef{"ExportedName", "string"},
    FieldDef{"ExportedType", "int"},
    FieldDef{"CategoryName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Semantic", "int"},
    FieldDef{"UseSlider", "bool"},
    FieldDef{"HasMin", "bool"},
    FieldDef{"HasMax", "bool"},
    FieldDef{"DefaultValueF1", "float"},
    FieldDef{"DefaultValueF2", "float2"},
    FieldDef{"DefaultValueF3", "float3"},
    FieldDef{"DefaultValueF3C", "float3"},
    FieldDef{"DefaultValueF3D", "float3"},
    FieldDef{"DefaultValueF4", "float4"},
    FieldDef{"DefaultValueF4C", "float4"},
    FieldDef{"DefaultValueI1", "int"},
    FieldDef{"DefaultValueI2", "int2"},
    FieldDef{"DefaultValueI3", "int3"},
    FieldDef{"DefaultValueI3D", "int3"},
    FieldDef{"DefaultValueI4", "int4"},
    FieldDef{"DefaultValueB1", "bool"},
    FieldDef{"DefaultValueB2", "bool2"},
    FieldDef{"DefaultValueB3", "bool3"},
    FieldDef{"DefaultValueB4", "bool4"},
    FieldDef{"DefaultValueO", "float3"},
    FieldDef{"MinValueF1", "float"},
    FieldDef{"MinValueF2", "float2"},
    FieldDef{"MinValueF3", "float3"},
    FieldDef{"MinValueF3D", "float3"},
    FieldDef{"MinValueF4", "float4"},
    FieldDef{"MinValueI1", "int"},
    FieldDef{"MinValueI2", "int2"},
    FieldDef{"MinValueI3", "int3"},
    FieldDef{"MinValueI3D", "int3"},
    FieldDef{"MinValueI4", "int4"},
    FieldDef{"MaxValueF1", "float"},
    FieldDef{"MaxValueF2", "float2"},
    FieldDef{"MaxValueF3", "float3"},
    FieldDef{"MaxValueF3D", "float3"},
    FieldDef{"MaxValueF4", "float4"},
    FieldDef{"MaxValueI1", "int"},
    FieldDef{"MaxValueI2", "int2"},
    FieldDef{"MaxValueI3", "int3"},
    FieldDef{"MaxValueI3D", "int3"},
    FieldDef{"MaxValueI4", "int4"},
    FieldDef{"Pinned", "bool"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CParticleAttributeList{{
    FieldDef{"AttributeList", "link[]"},
    FieldDef{"SamplerList", "link[]"},
}};

constexpr std::array<FieldDef, 7> kFields_v25_CParticleAttributeSamplerDeclaration{{
    FieldDef{"ExportedName", "string"},
    FieldDef{"ExportedType", "int"},
    FieldDef{"CategoryName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Pinned", "bool"},
    FieldDef{"DefaultValue", "link"},
    FieldDef{"UsageFlags", "uint"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleEffect{{
    FieldDef{"LayerGraph", "link"},
    FieldDef{"LayerGraphCompileCache", "link"},
    FieldDef{"Templates", "link[]"},
    FieldDef{"AttributeFlatList", "link"},
    FieldDef{"BoundsMode", "int"},
    FieldDef{"StaticBoundsMin", "float3"},
    FieldDef{"StaticBoundsMax", "float3"},
    FieldDef{"BoundScaleAttributeX", "string"},
    FieldDef{"BoundScaleAttributeY", "string"},
    FieldDef{"BoundScaleAttributeZ", "string"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNode{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 12> kFields_v25_CParticleNodeAnnotation{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Color", "float4"},
    FieldDef{"Sticky", "bool"},
    FieldDef{"SelfSize", "int2"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeArithmetic{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Operation", "uint"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeAssign{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeCompare{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Operation", "uint"},
}};

constexpr std::array<FieldDef, 30> kFields_v25_CParticleNodeConstant{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Type", "int"},
    FieldDef{"Semantic", "int"},
    FieldDef{"ValueF1", "float"},
    FieldDef{"ValueF2", "float2"},
    FieldDef{"ValueF3", "float3"},
    FieldDef{"ValueF4", "float4"},
    FieldDef{"ValueI1", "int"},
    FieldDef{"ValueI2", "int2"},
    FieldDef{"ValueI3", "int3"},
    FieldDef{"ValueI4", "int4"},
    FieldDef{"ValueB1", "bool"},
    FieldDef{"ValueB2", "bool2"},
    FieldDef{"ValueB3", "bool3"},
    FieldDef{"ValueB4", "bool4"},
    FieldDef{"ValueF3D", "float3"},
    FieldDef{"ValueF3C", "float3"},
    FieldDef{"ValueF4C", "float4"},
    FieldDef{"ValueI3D", "int3"},
    FieldDef{"ValueO", "float3"},
    FieldDef{"TransformSpace", "int"},
    FieldDef{"TransformMode", "int"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeConstructor{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Dimension", "uint"},
    FieldDef{"Mode", "int"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeDebugCheck{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Type", "int"},
    FieldDef{"Message", "string_unicode"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeDiscretizationPoint{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeEvaluateAtSpawn{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeEventGenerator{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeEventParent{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 15> kFields_v25_CParticleNodeEventPayloadAppend{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"PayloadName", "string"},
    FieldDef{"ForceOverride", "bool"},
    FieldDef{"IgnoreWarningsUnused", "bool"},
    FieldDef{"PayloadType", "string"},
    FieldDef{"PayloadKind", "int"},
    FieldDef{"Active2", "bool"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleNodeEventPayloadAppendBase{{
    FieldDef{"ForceOverride", "bool"},
    FieldDef{"ForceTypeOverride", "bool"},
    FieldDef{"IgnoreWarningsUnused", "bool"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeEventPayloadAppendInterp{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Interpolation", "int"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleNodeEventPayloadBase{{
    FieldDef{"PayloadName", "string"},
    FieldDef{"PayloadType", "int"},
    FieldDef{"PayloadKind", "int"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeEventPayloadClear{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"ClearAll", "bool"},
    FieldDef{"PayloadName", "string"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeEventPayloadCopy{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 13> kFields_v25_CParticleNodeEventPayloadExtract{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"PayloadName", "string"},
    FieldDef{"PayloadType", "string"},
    FieldDef{"PayloadKind", "int"},
    FieldDef{"IgnoreWarningsMissing", "bool"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleNodeEventPayloadExtractBase{{
    FieldDef{"IgnoreWarningsMissing", "bool"},
    FieldDef{"ExtractEventMask", "bool"},
    FieldDef{"RemoveFromPayload", "bool"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeEventPayloadExtractInterp{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"PayloadName", "string"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeEventStart{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"EventName", "string_unicode"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeEventTrigger{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 15> kFields_v25_CParticleNodeGraph{{
    FieldDef{"CustomName", "string_unicode"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Public", "bool"},
    FieldDef{"SimulationInterface", "bool"},
    FieldDef{"AutoInline", "bool"},
    FieldDef{"ClassType", "uint"},
    FieldDef{"IconKey", "string"},
    FieldDef{"Category", "string"},
    FieldDef{"SearchKeywords", "string_unicode"},
    FieldDef{"Nodes", "link[]"},
    FieldDef{"Timeline", "link"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceZoom", "int"},
    FieldDef{"GraphType", "int"},
    FieldDef{"Collapsed", "bool"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeIf{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeIntegrate{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Base", "uint"},
}};

constexpr std::array<FieldDef, 19> kFields_v25_CParticleNodeLayer{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"AutoEntrypoint", "bool"},
    FieldDef{"LODMetricOverride", "bool"},
    FieldDef{"LODDistanceMin", "float"},
    FieldDef{"LODDistanceMax", "float"},
    FieldDef{"PreferredSimLocation", "int"},
    FieldDef{"PreferredStorageSize", "int"},
    FieldDef{"PreferredLocalization", "int"},
    FieldDef{"Determinism", "int"},
    FieldDef{"RandomSeedModifier", "int"},
    FieldDef{"NodeGraph", "link"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeMathFunction1{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Function", "uint"},
    FieldDef{"AngleUnit", "int"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeMathFunction2{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Function", "uint"},
    FieldDef{"AngleUnit", "int"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeMathFunction3{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Function", "uint"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeMathWaveform{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Waveform", "uint"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodePartialDerivative{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Base", "uint"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodePassThrough{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v25_CParticleNodePinAbstract{{
    FieldDef{"SelfName", "string"},
    FieldDef{"Type", "int"},
    FieldDef{"Visible", "bool"},
    FieldDef{"Owner", "link"},
    FieldDef{"BaseType", "int"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodePinIn{{
    FieldDef{"SelfName", "string"},
    FieldDef{"Type", "string"},
    FieldDef{"Visible", "bool"},
    FieldDef{"Owner", "link"},
    FieldDef{"BaseType", "string"},
    FieldDef{"ConnectedPins", "link[]"},
    FieldDef{"ValueF", "float4"},
    FieldDef{"ValueI", "int4"},
    FieldDef{"ValueB", "bool4"},
    FieldDef{"ValueD", "string"},
    FieldDef{"ExportInParent", "bool"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CParticleNodePinOut{{
    FieldDef{"SelfName", "string"},
    FieldDef{"Type", "string"},
    FieldDef{"Visible", "bool"},
    FieldDef{"Owner", "link"},
    FieldDef{"BaseType", "string"},
    FieldDef{"ConnectedPins", "link[]"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeReinterpret{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"TargetType", "int"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleNodeRendererBase{{
    FieldDef{"FeatureSetPath", "string"},
    FieldDef{"DrawOrder", "int"},
    FieldDef{"RendererFeatures", "string[]"},
}};

constexpr std::array<FieldDef, 12> kFields_v25_CParticleNodeRendererBillboard{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"FeatureSetPath", "string"},
    FieldDef{"RendererFeatures", "link[]"},
    FieldDef{"DrawOrder", "int"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeRendererLight{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"FeatureSetPath", "string"},
    FieldDef{"RendererFeatures", "link[]"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeRendererMesh{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"FeatureSetPath", "string"},
    FieldDef{"RendererFeatures", "link[]"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeRendererRibbon{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"FeatureSetPath", "string"},
    FieldDef{"RendererFeatures", "link[]"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeRendererSound{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"FeatureSetPath", "string"},
    FieldDef{"RendererFeatures", "link[]"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeSamplerData{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 16> kFields_v25_CParticleNodeSamplerData_AnimTrack{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"AnimResource", "string"},
    FieldDef{"TransformTranslate", "bool"},
    FieldDef{"TransformRotate", "bool"},
    FieldDef{"TransformScale", "bool"},
    FieldDef{"Position", "float3"},
    FieldDef{"EulerOrientation", "float3"},
    FieldDef{"Scale", "float3"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeSamplerData_Audio{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"ChannelGroup", "string"},
    FieldDef{"Mode", "uint"},
}};

constexpr std::array<FieldDef, 19> kFields_v25_CParticleNodeSamplerData_Curve{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"ValueType", "uint"},
    FieldDef{"Interpolator", "uint"},
    FieldDef{"MinLimits", "float4"},
    FieldDef{"MaxLimits", "float4"},
    FieldDef{"IsProbabilityCurve", "bool"},
    FieldDef{"Quality", "uint"},
    FieldDef{"IsLoopedCurve", "bool"},
    FieldDef{"Times", "float[]"},
    FieldDef{"FloatValues", "float[]"},
    FieldDef{"FloatTangents", "float[]"},
}};

constexpr std::array<FieldDef, 19> kFields_v25_CParticleNodeSamplerData_DoubleCurve{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"ValueType", "uint"},
    FieldDef{"Interpolator", "uint"},
    FieldDef{"MinLimits", "float4"},
    FieldDef{"MaxLimits", "float4"},
    FieldDef{"Times", "float[]"},
    FieldDef{"FloatValues", "float[]"},
    FieldDef{"FloatTangents", "float[]"},
    FieldDef{"Times1", "float[]"},
    FieldDef{"FloatValues1", "float[]"},
    FieldDef{"FloatTangents1", "float[]"},
}};

constexpr std::array<FieldDef, 12> kFields_v25_CParticleNodeSamplerData_EventStream{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"EventSource", "int"},
    FieldDef{"Times", "float[]"},
    FieldDef{"Payload", "link[]"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CParticleNodeSamplerData_EventStream_Payload{{
    FieldDef{"Description", "string_unicode"},
    FieldDef{"SelfName", "string"},
    FieldDef{"Type", "int"},
    FieldDef{"ValuesF4", "float4[]"},
    FieldDef{"ValuesI4", "int4[]"},
    FieldDef{"ValuesB4", "bool4[]"},
}};

constexpr std::array<FieldDef, 31> kFields_v25_CParticleNodeSamplerData_Shape{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"SampleDimensionality", "uint"},
    FieldDef{"TransformTranslate", "bool"},
    FieldDef{"TransformRotate", "bool"},
    FieldDef{"Weight", "float"},
    FieldDef{"Position", "float3"},
    FieldDef{"EulerOrientation", "float3"},
    FieldDef{"ShapeType", "int"},
    FieldDef{"BoxDimensions", "float3"},
    FieldDef{"Radius", "float"},
    FieldDef{"InnerRadius", "float"},
    FieldDef{"NormalizedInnerRadius", "float"},
    FieldDef{"Height", "float"},
    FieldDef{"Hemisphere", "bool"},
    FieldDef{"NonUniformScale", "float3"},
    FieldDef{"MeshResource", "string"},
    FieldDef{"MeshScale", "float3"},
    FieldDef{"MeshSamplingMode", "uint"},
    FieldDef{"SubMeshIndex", "uint"},
    FieldDef{"DefaultUvStream", "uint"},
    FieldDef{"DefaultColorStream", "uint"},
    FieldDef{"DensityColorStream", "uint"},
    FieldDef{"DensityChannel", "uint"},
}};

constexpr std::array<FieldDef, 14> kFields_v25_CParticleNodeSamplerData_Text{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"DataSource", "uint"},
    FieldDef{"InlineText", "string"},
    FieldDef{"ExternalResource", "string"},
    FieldDef{"FontFile", "string"},
    FieldDef{"UseKerning", "bool"},
}};

constexpr std::array<FieldDef, 17> kFields_v25_CParticleNodeSamplerData_Texture{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"TextureResource", "string"},
    FieldDef{"AtlasDefinition", "string"},
    FieldDef{"ScriptOutputType", "uint"},
    FieldDef{"SampleRawValues", "bool"},
    FieldDef{"SampleGammaSpace", "uint"},
    FieldDef{"DensityPower", "float"},
    FieldDef{"DensitySrc", "uint"},
    FieldDef{"DensityRGBAWeights", "float4"},
}};

constexpr std::array<FieldDef, 31> kFields_v25_CParticleNodeSamplerData_Turbulence{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"DataSource", "uint"},
    FieldDef{"VectorFieldResource", "string"},
    FieldDef{"GlobalScale", "float"},
    FieldDef{"Strength", "float"},
    FieldDef{"WrapSide", "bool"},
    FieldDef{"WrapVertical", "bool"},
    FieldDef{"WrapDepth", "bool"},
    FieldDef{"WrapTime", "bool"},
    FieldDef{"Filtering", "int"},
    FieldDef{"Wavelength", "float"},
    FieldDef{"Octaves", "uint"},
    FieldDef{"Lacunarity", "float"},
    FieldDef{"Gain", "float"},
    FieldDef{"Interpolator", "int"},
    FieldDef{"TimeScale", "float"},
    FieldDef{"TimeBase", "float"},
    FieldDef{"TimeRandomVariation", "float"},
    FieldDef{"FlowFactor", "float"},
    FieldDef{"DivergenceFactor", "float"},
    FieldDef{"InitialSeed", "uint"},
    FieldDef{"FastFakeFlow", "bool"},
    FieldDef{"GainMultiplier", "float"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeScript{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Expression", "string_unicode"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeSetLife{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 9> kFields_v25_CParticleNodeSpatialInsert{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
}};

constexpr std::array<FieldDef, 13> kFields_v25_CParticleNodeSpatialLayer{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"SelfName", "string"},
    FieldDef{"Global", "bool"},
    FieldDef{"CellSize", "float"},
    FieldDef{"Storage", "link[]"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleNodeSpatialLayerStorage{{
    FieldDef{"Description", "string_unicode"},
    FieldDef{"SelfName", "string"},
    FieldDef{"Type", "int"},
}};

constexpr std::array<FieldDef, 13> kFields_v25_CParticleNodeSpatialPayloadAppend{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"PayloadName", "string"},
    FieldDef{"PayloadType", "int"},
    FieldDef{"ForceOverride", "bool"},
    FieldDef{"ForceTypeOverride", "bool"},
}};

constexpr std::array<FieldDef, 12> kFields_v25_CParticleNodeSpatialQuery{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"QueryType", "int"},
    FieldDef{"PayloadName", "string"},
    FieldDef{"PayloadType", "int"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CParticleNodeSplitter{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Dimension", "uint"},
    FieldDef{"Mode", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v25_CParticleNodeStaticTest{{
    FieldDef{"Test", "uint"},
    FieldDef{"CompareOp", "uint"},
    FieldDef{"CompareOp_", "uint"},
    FieldDef{"TestExecStage", "uint"},
    FieldDef{"TestExecFrequency", "uint"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeStaticTypeSwitch{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"TypeEqualTo", "int"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeStaticVersionSwitch{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"Versions", "string[]"},
}};

constexpr std::array<FieldDef, 16> kFields_v25_CParticleNodeTemplate{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"AutoEntrypoint", "bool"},
    FieldDef{"SubGraphFilePath", "string"},
    FieldDef{"SubGraphName", "string_unicode"},
    FieldDef{"PreferredSimLocation", "int"},
    FieldDef{"Determinism", "int"},
    FieldDef{"RandomSeedModifier", "int"},
    FieldDef{"NodeGraph", "link"},
}};

constexpr std::array<FieldDef, 72> kFields_v25_CParticleNodeTemplateExport{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"ExportedName", "string"},
    FieldDef{"ExportedType", "int"},
    FieldDef{"Semantic", "int"},
    FieldDef{"Type", "int"},
    FieldDef{"InputType", "int"},
    FieldDef{"Order", "int"},
    FieldDef{"VisibleByDefault", "bool"},
    FieldDef{"TransformSpace", "int"},
    FieldDef{"TransformMode", "int"},
    FieldDef{"CategoryName", "string_localized"},
    FieldDef{"UseDropDown", "bool"},
    FieldDef{"HasMin", "bool"},
    FieldDef{"HasMax", "bool"},
    FieldDef{"UseSlider", "bool"},
    FieldDef{"EnumList", "string[]"},
    FieldDef{"PassthroughInput", "string"},
    FieldDef{"DefaultValueF1", "float"},
    FieldDef{"DefaultValueF2", "float2"},
    FieldDef{"DefaultValueF3", "float3"},
    FieldDef{"DefaultValueF3C", "float3"},
    FieldDef{"DefaultValueF3D", "float3"},
    FieldDef{"DefaultValueF4", "float4"},
    FieldDef{"DefaultValueF4C", "float4"},
    FieldDef{"DefaultValueI1", "int"},
    FieldDef{"DefaultValueI2", "int2"},
    FieldDef{"DefaultValueI3", "int3"},
    FieldDef{"DefaultValueI3D", "int3"},
    FieldDef{"DefaultValueI4", "int4"},
    FieldDef{"DefaultValueB1", "bool"},
    FieldDef{"DefaultValueB2", "bool2"},
    FieldDef{"DefaultValueB3", "bool3"},
    FieldDef{"DefaultValueB4", "bool4"},
    FieldDef{"DefaultValueO", "float3"},
    FieldDef{"MinValueF1", "float"},
    FieldDef{"MinValueF2", "float2"},
    FieldDef{"MinValueF3", "float3"},
    FieldDef{"MinValueF3D", "float3"},
    FieldDef{"MinValueF4", "float4"},
    FieldDef{"MinValueI1", "int"},
    FieldDef{"MinValueI2", "int2"},
    FieldDef{"MinValueI3", "int3"},
    FieldDef{"MinValueI3D", "int3"},
    FieldDef{"MinValueI4", "int4"},
    FieldDef{"MaxValueF1", "float"},
    FieldDef{"MaxValueF2", "float2"},
    FieldDef{"MaxValueF3", "float3"},
    FieldDef{"MaxValueF3D", "float3"},
    FieldDef{"MaxValueF4", "float4"},
    FieldDef{"MaxValueI1", "int"},
    FieldDef{"MaxValueI2", "int2"},
    FieldDef{"MaxValueI3", "int3"},
    FieldDef{"MaxValueI3D", "int3"},
    FieldDef{"MaxValueI4", "int4"},
    FieldDef{"DefaultValueData", "string"},
    FieldDef{"PinRules", "int"},
    FieldDef{"BaseVisibility", "int"},
    FieldDef{"RuleResult", "int"},
    FieldDef{"DependentProperty", "string"},
    FieldDef{"RuleFunction", "int"},
    FieldDef{"RuleValue", "string"},
    FieldDef{"DependentProperty2", "string"},
    FieldDef{"RuleFunction2", "int"},
    FieldDef{"RuleValue2", "string"},
}};

constexpr std::array<FieldDef, 13> kFields_v25_CParticleNodeTransform{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"InputSpace", "uint"},
    FieldDef{"OutputSpace", "uint"},
    FieldDef{"TransformMode", "uint"},
    FieldDef{"ApplyScale", "bool"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CParticleNodeTransformOrientation{{
    FieldDef{"InputSpace", "uint"},
    FieldDef{"OutputSpace", "uint"},
}};

constexpr std::array<FieldDef, 10> kFields_v25_CParticleNodeTypeCast{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"TargetType", "int"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleRendererFeature{{
    FieldDef{"FeatureName", "string"},
    FieldDef{"Feature", "link"},
    FieldDef{"Properties", "link[]"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleRendererFeatureDesc{{
    FieldDef{"RendererInterfacePath", "string"},
    FieldDef{"RendererFeatureName", "string"},
    FieldDef{"Mandatory", "bool"},
}};

constexpr std::array<FieldDef, 1> kFields_v25_CParticleRendererFeatureSet{{
    FieldDef{"RendererFeatures", "link[]"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CParticleTimeline{{
    FieldDef{"Tracks", "link[]"},
    FieldDef{"WorkspacePosition", "float"},
    FieldDef{"WorkspaceZoom", "int"},
}};

constexpr std::array<FieldDef, 8> kFields_v25_CParticleTimelineTrack{{
    FieldDef{"CustomName", "string_unicode"},
    FieldDef{"Description", "string_unicode"},
    FieldDef{"CategoryName", "string_unicode"},
    FieldDef{"Enabled", "bool"},
    FieldDef{"OverrideColor", "bool"},
    FieldDef{"EditorColor", "uint3"},
    FieldDef{"EventStream", "link"},
    FieldDef{"Collapsed", "bool"},
}};

constexpr std::array<FieldDef, 6> kFields_v25_CProjectSettings{{
    FieldDef{"General", "link"},
    FieldDef{"Scene", "link"},
    FieldDef{"LOD", "link"},
    FieldDef{"Documentation", "link"},
    FieldDef{"Assets", "link"},
    FieldDef{"Baking", "link"},
}};

constexpr std::array<FieldDef, 14> kFields_v25_CProjectSettingsAssets{{
    FieldDef{"IgnoredPaths", "string"},
    FieldDef{"EffectExtensions", "string[]"},
    FieldDef{"MeshExtensions", "string[]"},
    FieldDef{"ImageExtensions", "string[]"},
    FieldDef{"ImageAtlasExtensions", "string[]"},
    FieldDef{"FontExtensions", "string[]"},
    FieldDef{"VectorFieldExtensions", "string[]"},
    FieldDef{"SoundExtensions", "string[]"},
    FieldDef{"SimCacheExtensions", "string[]"},
    FieldDef{"FeatureSetExtensions", "string[]"},
    FieldDef{"VertexShaderExtensions", "string[]"},
    FieldDef{"FragmentShaderExtensions", "string[]"},
    FieldDef{"RendererInterfaceExtensions", "string[]"},
    FieldDef{"VideoExtensions", "string[]"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CProjectSettingsBaking{{
    FieldDef{"PlatformSettingsList", "string[]"},
    FieldDef{"BuildVersions", "string[]"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CProjectSettingsDocumentation{{
    FieldDef{"StyleSheetPath", "string"},
    FieldDef{"InlineStyleSheet", "bool"},
}};

constexpr std::array<FieldDef, 11> kFields_v25_CProjectSettingsGeneral{{
    FieldDef{"ProjectName", "string_unicode"},
    FieldDef{"ProjectDescription", "string_unicode"},
    FieldDef{"RootDir", "string"},
    FieldDef{"TemplatesDir", "string"},
    FieldDef{"AnimationTracksDir", "string"},
    FieldDef{"ThumbnailsDir", "string"},
    FieldDef{"LibraryDir", "string"},
    FieldDef{"RuntimeConfigsDir", "string"},
    FieldDef{"DocumentationDir", "string"},
    FieldDef{"EditorCacheDir", "string"},
    FieldDef{"ExternalResourcesPathList", "string[]"},
}};

constexpr std::array<FieldDef, 3> kFields_v25_CProjectSettingsLOD{{
    FieldDef{"MinDistance", "float"},
    FieldDef{"MaxDistance", "float"},
    FieldDef{"MinMinDistance", "float"},
}};

constexpr std::array<FieldDef, 17> kFields_v25_CProjectSettingsScene{{
    FieldDef{"CoordinateFrame", "int"},
    FieldDef{"CoordinateFrameAxisX", "int"},
    FieldDef{"CoordinateFrameAxisY", "int"},
    FieldDef{"CoordinateFrameAxisZ", "int"},
    FieldDef{"DistanceUnits", "int"},
    FieldDef{"Enable_Scene_SampleWindField", "bool"},
    FieldDef{"DefaultRestitution", "float"},
    FieldDef{"DefaultRestitutionCombineMode", "int"},
    FieldDef{"DefaultFriction", "float"},
    FieldDef{"DefaultFrictionCombineMode", "int"},
    FieldDef{"WindScale", "float"},
    FieldDef{"SurfaceTypeList", "string[]"},
    FieldDef{"CollisionFilterList", "string[]"},
    FieldDef{"AudioSampleCount", "uint"},
    FieldDef{"AudioSpectrumMinFreq", "uint"},
    FieldDef{"AudioSpectrumMaxFreq", "uint"},
    FieldDef{"AudioSpectrumHasLogScale", "bool"},
}};

constexpr std::array<FieldDef, 8> kFields_v25_CSamplerCurve{{
    FieldDef{"BindingSemantic", "string"},
    FieldDef{"ValueType", "uint"},
    FieldDef{"Interpolator", "uint"},
    FieldDef{"Times", "float[]"},
    FieldDef{"FloatValues", "float[]"},
    FieldDef{"FloatTangents", "float[]"},
    FieldDef{"MinLimits", "float4"},
    FieldDef{"MaxLimits", "float4"},
}};

constexpr std::array<FieldDef, 8> kFields_v25_CSkeletonAnimation{{
    FieldDef{"BoneStreams", "link[]"},
    FieldDef{"LengthInSeconds", "float"},
    FieldDef{"PlaybackSpeed", "float"},
    FieldDef{"AnimationName", "string"},
    FieldDef{"CoordinateFrame", "uint"},
    FieldDef{"CoordinateFrameAxisX", "uint"},
    FieldDef{"CoordinateFrameAxisY", "uint"},
    FieldDef{"CoordinateFrameAxisZ", "uint"},
}};

constexpr std::array<FieldDef, 2> kFields_v25_CSkeletonAnimationBone{{
    FieldDef{"TargetBone", "string"},
    FieldDef{"Channels", "link[]"},
}};

constexpr std::array<HandlerDef, 116> kHandlerDefs_v25{{
    HandlerDef{"CAnimationClip", std::span<const FieldDef>(kFields_v25_CAnimationClip.data(), kFields_v25_CAnimationClip.size())},
    HandlerDef{"CAnimationTrack", std::span<const FieldDef>(kFields_v25_CAnimationTrack.data(), kFields_v25_CAnimationTrack.size())},
    HandlerDef{"CBaseSettings", std::span<const FieldDef>(kFields_v25_CBaseSettings.data(), kFields_v25_CBaseSettings.size())},
    HandlerDef{"CCompilerBlobCache", std::span<const FieldDef>(kFields_v25_CCompilerBlobCache.data(), kFields_v25_CCompilerBlobCache.size())},
    HandlerDef{"CCompilerBlobCacheEntryPoint", std::span<const FieldDef>(kFields_v25_CCompilerBlobCacheEntryPoint.data(), kFields_v25_CCompilerBlobCacheEntryPoint.size())},
    HandlerDef{"CCompilerBlobCacheExternal", std::span<const FieldDef>(kFields_v25_CCompilerBlobCacheExternal.data(), kFields_v25_CCompilerBlobCacheExternal.size())},
    HandlerDef{"CCompilerBlobCacheFunctionArg", std::span<const FieldDef>(kFields_v25_CCompilerBlobCacheFunctionArg.data(), kFields_v25_CCompilerBlobCacheFunctionArg.size())},
    HandlerDef{"CCompilerBlobCacheFunctionDef", std::span<const FieldDef>(kFields_v25_CCompilerBlobCacheFunctionDef.data(), kFields_v25_CCompilerBlobCacheFunctionDef.size())},
    HandlerDef{"CEditorAssetEffect", std::span<const FieldDef>(kFields_v25_CEditorAssetEffect.data(), kFields_v25_CEditorAssetEffect.size())},
    HandlerDef{"CEngineRendererInterface", std::span<const FieldDef>(kFields_v25_CEngineRendererInterface.data(), kFields_v25_CEngineRendererInterface.size())},
    HandlerDef{"CLayerCompileCache", std::span<const FieldDef>(kFields_v25_CLayerCompileCache.data(), kFields_v25_CLayerCompileCache.size())},
    HandlerDef{"CLayerCompileCacheAttrib", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheAttrib.data(), kFields_v25_CLayerCompileCacheAttrib.size())},
    HandlerDef{"CLayerCompileCacheAttribSampler", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheAttribSampler.data(), kFields_v25_CLayerCompileCacheAttribSampler.size())},
    HandlerDef{"CLayerCompileCacheEvent", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheEvent.data(), kFields_v25_CLayerCompileCacheEvent.size())},
    HandlerDef{"CLayerCompileCacheEventPayload", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheEventPayload.data(), kFields_v25_CLayerCompileCacheEventPayload.size())},
    HandlerDef{"CLayerCompileCacheField", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheField.data(), kFields_v25_CLayerCompileCacheField.size())},
    HandlerDef{"CLayerCompileCacheRenderer", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheRenderer.data(), kFields_v25_CLayerCompileCacheRenderer.size())},
    HandlerDef{"CLayerCompileCacheRendererParticleInput", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheRendererParticleInput.data(), kFields_v25_CLayerCompileCacheRendererParticleInput.size())},
    HandlerDef{"CLayerCompileCacheRendererProperty", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheRendererProperty.data(), kFields_v25_CLayerCompileCacheRendererProperty.size())},
    HandlerDef{"CLayerCompileCacheSampler", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheSampler.data(), kFields_v25_CLayerCompileCacheSampler.size())},
    HandlerDef{"CLayerCompileCacheSpatialLayer", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheSpatialLayer.data(), kFields_v25_CLayerCompileCacheSpatialLayer.size())},
    HandlerDef{"CLayerCompileCacheSpatialLayerPayload", std::span<const FieldDef>(kFields_v25_CLayerCompileCacheSpatialLayerPayload.data(), kFields_v25_CLayerCompileCacheSpatialLayerPayload.size())},
    HandlerDef{"CLayerGraphCompileCache", std::span<const FieldDef>(kFields_v25_CLayerGraphCompileCache.data(), kFields_v25_CLayerGraphCompileCache.size())},
    HandlerDef{"CLayerGraphCompileCache_BroadSlot", std::span<const FieldDef>(kFields_v25_CLayerGraphCompileCache_BroadSlot.data(), kFields_v25_CLayerGraphCompileCache_BroadSlot.size())},
    HandlerDef{"CLayerGraphCompileCache_EntrySlot", std::span<const FieldDef>(kFields_v25_CLayerGraphCompileCache_EntrySlot.data(), kFields_v25_CLayerGraphCompileCache_EntrySlot.size())},
    HandlerDef{"CLayerGraphCompileCache_EventSlot", std::span<const FieldDef>(kFields_v25_CLayerGraphCompileCache_EventSlot.data(), kFields_v25_CLayerGraphCompileCache_EventSlot.size())},
    HandlerDef{"CLayerGraphCompileCache_LayerSlot", std::span<const FieldDef>(kFields_v25_CLayerGraphCompileCache_LayerSlot.data(), kFields_v25_CLayerGraphCompileCache_LayerSlot.size())},
    HandlerDef{"CParticleAttributeDeclaration", std::span<const FieldDef>(kFields_v25_CParticleAttributeDeclaration.data(), kFields_v25_CParticleAttributeDeclaration.size())},
    HandlerDef{"CParticleAttributeList", std::span<const FieldDef>(kFields_v25_CParticleAttributeList.data(), kFields_v25_CParticleAttributeList.size())},
    HandlerDef{"CParticleAttributeSamplerDeclaration", std::span<const FieldDef>(kFields_v25_CParticleAttributeSamplerDeclaration.data(), kFields_v25_CParticleAttributeSamplerDeclaration.size())},
    HandlerDef{"CParticleEffect", std::span<const FieldDef>(kFields_v25_CParticleEffect.data(), kFields_v25_CParticleEffect.size())},
    HandlerDef{"CParticleNode", std::span<const FieldDef>(kFields_v25_CParticleNode.data(), kFields_v25_CParticleNode.size())},
    HandlerDef{"CParticleNodeAnnotation", std::span<const FieldDef>(kFields_v25_CParticleNodeAnnotation.data(), kFields_v25_CParticleNodeAnnotation.size())},
    HandlerDef{"CParticleNodeArithmetic", std::span<const FieldDef>(kFields_v25_CParticleNodeArithmetic.data(), kFields_v25_CParticleNodeArithmetic.size())},
    HandlerDef{"CParticleNodeAssign", std::span<const FieldDef>(kFields_v25_CParticleNodeAssign.data(), kFields_v25_CParticleNodeAssign.size())},
    HandlerDef{"CParticleNodeCompare", std::span<const FieldDef>(kFields_v25_CParticleNodeCompare.data(), kFields_v25_CParticleNodeCompare.size())},
    HandlerDef{"CParticleNodeConstant", std::span<const FieldDef>(kFields_v25_CParticleNodeConstant.data(), kFields_v25_CParticleNodeConstant.size())},
    HandlerDef{"CParticleNodeConstructor", std::span<const FieldDef>(kFields_v25_CParticleNodeConstructor.data(), kFields_v25_CParticleNodeConstructor.size())},
    HandlerDef{"CParticleNodeDebugCheck", std::span<const FieldDef>(kFields_v25_CParticleNodeDebugCheck.data(), kFields_v25_CParticleNodeDebugCheck.size())},
    HandlerDef{"CParticleNodeDiscretizationPoint", std::span<const FieldDef>(kFields_v25_CParticleNodeDiscretizationPoint.data(), kFields_v25_CParticleNodeDiscretizationPoint.size())},
    HandlerDef{"CParticleNodeEvaluateAtSpawn", std::span<const FieldDef>(kFields_v25_CParticleNodeEvaluateAtSpawn.data(), kFields_v25_CParticleNodeEvaluateAtSpawn.size())},
    HandlerDef{"CParticleNodeEventGenerator", std::span<const FieldDef>(kFields_v25_CParticleNodeEventGenerator.data(), kFields_v25_CParticleNodeEventGenerator.size())},
    HandlerDef{"CParticleNodeEventParent", std::span<const FieldDef>(kFields_v25_CParticleNodeEventParent.data(), kFields_v25_CParticleNodeEventParent.size())},
    HandlerDef{"CParticleNodeEventPayloadAppend", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadAppend.data(), kFields_v25_CParticleNodeEventPayloadAppend.size())},
    HandlerDef{"CParticleNodeEventPayloadAppendBase", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadAppendBase.data(), kFields_v25_CParticleNodeEventPayloadAppendBase.size())},
    HandlerDef{"CParticleNodeEventPayloadAppendInterp", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadAppendInterp.data(), kFields_v25_CParticleNodeEventPayloadAppendInterp.size())},
    HandlerDef{"CParticleNodeEventPayloadBase", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadBase.data(), kFields_v25_CParticleNodeEventPayloadBase.size())},
    HandlerDef{"CParticleNodeEventPayloadClear", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadClear.data(), kFields_v25_CParticleNodeEventPayloadClear.size())},
    HandlerDef{"CParticleNodeEventPayloadCopy", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadCopy.data(), kFields_v25_CParticleNodeEventPayloadCopy.size())},
    HandlerDef{"CParticleNodeEventPayloadExtract", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadExtract.data(), kFields_v25_CParticleNodeEventPayloadExtract.size())},
    HandlerDef{"CParticleNodeEventPayloadExtractBase", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadExtractBase.data(), kFields_v25_CParticleNodeEventPayloadExtractBase.size())},
    HandlerDef{"CParticleNodeEventPayloadExtractInterp", std::span<const FieldDef>(kFields_v25_CParticleNodeEventPayloadExtractInterp.data(), kFields_v25_CParticleNodeEventPayloadExtractInterp.size())},
    HandlerDef{"CParticleNodeEventStart", std::span<const FieldDef>(kFields_v25_CParticleNodeEventStart.data(), kFields_v25_CParticleNodeEventStart.size())},
    HandlerDef{"CParticleNodeEventTrigger", std::span<const FieldDef>(kFields_v25_CParticleNodeEventTrigger.data(), kFields_v25_CParticleNodeEventTrigger.size())},
    HandlerDef{"CParticleNodeGraph", std::span<const FieldDef>(kFields_v25_CParticleNodeGraph.data(), kFields_v25_CParticleNodeGraph.size())},
    HandlerDef{"CParticleNodeIf", std::span<const FieldDef>(kFields_v25_CParticleNodeIf.data(), kFields_v25_CParticleNodeIf.size())},
    HandlerDef{"CParticleNodeIntegrate", std::span<const FieldDef>(kFields_v25_CParticleNodeIntegrate.data(), kFields_v25_CParticleNodeIntegrate.size())},
    HandlerDef{"CParticleNodeLayer", std::span<const FieldDef>(kFields_v25_CParticleNodeLayer.data(), kFields_v25_CParticleNodeLayer.size())},
    HandlerDef{"CParticleNodeMathFunction1", std::span<const FieldDef>(kFields_v25_CParticleNodeMathFunction1.data(), kFields_v25_CParticleNodeMathFunction1.size())},
    HandlerDef{"CParticleNodeMathFunction2", std::span<const FieldDef>(kFields_v25_CParticleNodeMathFunction2.data(), kFields_v25_CParticleNodeMathFunction2.size())},
    HandlerDef{"CParticleNodeMathFunction3", std::span<const FieldDef>(kFields_v25_CParticleNodeMathFunction3.data(), kFields_v25_CParticleNodeMathFunction3.size())},
    HandlerDef{"CParticleNodeMathWaveform", std::span<const FieldDef>(kFields_v25_CParticleNodeMathWaveform.data(), kFields_v25_CParticleNodeMathWaveform.size())},
    HandlerDef{"CParticleNodePartialDerivative", std::span<const FieldDef>(kFields_v25_CParticleNodePartialDerivative.data(), kFields_v25_CParticleNodePartialDerivative.size())},
    HandlerDef{"CParticleNodePassThrough", std::span<const FieldDef>(kFields_v25_CParticleNodePassThrough.data(), kFields_v25_CParticleNodePassThrough.size())},
    HandlerDef{"CParticleNodePinAbstract", std::span<const FieldDef>(kFields_v25_CParticleNodePinAbstract.data(), kFields_v25_CParticleNodePinAbstract.size())},
    HandlerDef{"CParticleNodePinIn", std::span<const FieldDef>(kFields_v25_CParticleNodePinIn.data(), kFields_v25_CParticleNodePinIn.size())},
    HandlerDef{"CParticleNodePinOut", std::span<const FieldDef>(kFields_v25_CParticleNodePinOut.data(), kFields_v25_CParticleNodePinOut.size())},
    HandlerDef{"CParticleNodeReinterpret", std::span<const FieldDef>(kFields_v25_CParticleNodeReinterpret.data(), kFields_v25_CParticleNodeReinterpret.size())},
    HandlerDef{"CParticleNodeRendererBase", std::span<const FieldDef>(kFields_v25_CParticleNodeRendererBase.data(), kFields_v25_CParticleNodeRendererBase.size())},
    HandlerDef{"CParticleNodeRendererBillboard", std::span<const FieldDef>(kFields_v25_CParticleNodeRendererBillboard.data(), kFields_v25_CParticleNodeRendererBillboard.size())},
    HandlerDef{"CParticleNodeRendererLight", std::span<const FieldDef>(kFields_v25_CParticleNodeRendererLight.data(), kFields_v25_CParticleNodeRendererLight.size())},
    HandlerDef{"CParticleNodeRendererMesh", std::span<const FieldDef>(kFields_v25_CParticleNodeRendererMesh.data(), kFields_v25_CParticleNodeRendererMesh.size())},
    HandlerDef{"CParticleNodeRendererRibbon", std::span<const FieldDef>(kFields_v25_CParticleNodeRendererRibbon.data(), kFields_v25_CParticleNodeRendererRibbon.size())},
    HandlerDef{"CParticleNodeRendererSound", std::span<const FieldDef>(kFields_v25_CParticleNodeRendererSound.data(), kFields_v25_CParticleNodeRendererSound.size())},
    HandlerDef{"CParticleNodeSamplerData", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData.data(), kFields_v25_CParticleNodeSamplerData.size())},
    HandlerDef{"CParticleNodeSamplerData_AnimTrack", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_AnimTrack.data(), kFields_v25_CParticleNodeSamplerData_AnimTrack.size())},
    HandlerDef{"CParticleNodeSamplerData_Audio", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_Audio.data(), kFields_v25_CParticleNodeSamplerData_Audio.size())},
    HandlerDef{"CParticleNodeSamplerData_Curve", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_Curve.data(), kFields_v25_CParticleNodeSamplerData_Curve.size())},
    HandlerDef{"CParticleNodeSamplerData_DoubleCurve", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_DoubleCurve.data(), kFields_v25_CParticleNodeSamplerData_DoubleCurve.size())},
    HandlerDef{"CParticleNodeSamplerData_EventStream", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_EventStream.data(), kFields_v25_CParticleNodeSamplerData_EventStream.size())},
    HandlerDef{"CParticleNodeSamplerData_EventStream_Payload", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_EventStream_Payload.data(), kFields_v25_CParticleNodeSamplerData_EventStream_Payload.size())},
    HandlerDef{"CParticleNodeSamplerData_Shape", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_Shape.data(), kFields_v25_CParticleNodeSamplerData_Shape.size())},
    HandlerDef{"CParticleNodeSamplerData_Text", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_Text.data(), kFields_v25_CParticleNodeSamplerData_Text.size())},
    HandlerDef{"CParticleNodeSamplerData_Texture", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_Texture.data(), kFields_v25_CParticleNodeSamplerData_Texture.size())},
    HandlerDef{"CParticleNodeSamplerData_Turbulence", std::span<const FieldDef>(kFields_v25_CParticleNodeSamplerData_Turbulence.data(), kFields_v25_CParticleNodeSamplerData_Turbulence.size())},
    HandlerDef{"CParticleNodeScript", std::span<const FieldDef>(kFields_v25_CParticleNodeScript.data(), kFields_v25_CParticleNodeScript.size())},
    HandlerDef{"CParticleNodeSetLife", std::span<const FieldDef>(kFields_v25_CParticleNodeSetLife.data(), kFields_v25_CParticleNodeSetLife.size())},
    HandlerDef{"CParticleNodeSpatialInsert", std::span<const FieldDef>(kFields_v25_CParticleNodeSpatialInsert.data(), kFields_v25_CParticleNodeSpatialInsert.size())},
    HandlerDef{"CParticleNodeSpatialLayer", std::span<const FieldDef>(kFields_v25_CParticleNodeSpatialLayer.data(), kFields_v25_CParticleNodeSpatialLayer.size())},
    HandlerDef{"CParticleNodeSpatialLayerStorage", std::span<const FieldDef>(kFields_v25_CParticleNodeSpatialLayerStorage.data(), kFields_v25_CParticleNodeSpatialLayerStorage.size())},
    HandlerDef{"CParticleNodeSpatialPayloadAppend", std::span<const FieldDef>(kFields_v25_CParticleNodeSpatialPayloadAppend.data(), kFields_v25_CParticleNodeSpatialPayloadAppend.size())},
    HandlerDef{"CParticleNodeSpatialQuery", std::span<const FieldDef>(kFields_v25_CParticleNodeSpatialQuery.data(), kFields_v25_CParticleNodeSpatialQuery.size())},
    HandlerDef{"CParticleNodeSplitter", std::span<const FieldDef>(kFields_v25_CParticleNodeSplitter.data(), kFields_v25_CParticleNodeSplitter.size())},
    HandlerDef{"CParticleNodeStaticTest", std::span<const FieldDef>(kFields_v25_CParticleNodeStaticTest.data(), kFields_v25_CParticleNodeStaticTest.size())},
    HandlerDef{"CParticleNodeStaticTypeSwitch", std::span<const FieldDef>(kFields_v25_CParticleNodeStaticTypeSwitch.data(), kFields_v25_CParticleNodeStaticTypeSwitch.size())},
    HandlerDef{"CParticleNodeStaticVersionSwitch", std::span<const FieldDef>(kFields_v25_CParticleNodeStaticVersionSwitch.data(), kFields_v25_CParticleNodeStaticVersionSwitch.size())},
    HandlerDef{"CParticleNodeTemplate", std::span<const FieldDef>(kFields_v25_CParticleNodeTemplate.data(), kFields_v25_CParticleNodeTemplate.size())},
    HandlerDef{"CParticleNodeTemplateExport", std::span<const FieldDef>(kFields_v25_CParticleNodeTemplateExport.data(), kFields_v25_CParticleNodeTemplateExport.size())},
    HandlerDef{"CParticleNodeTransform", std::span<const FieldDef>(kFields_v25_CParticleNodeTransform.data(), kFields_v25_CParticleNodeTransform.size())},
    HandlerDef{"CParticleNodeTransformOrientation", std::span<const FieldDef>(kFields_v25_CParticleNodeTransformOrientation.data(), kFields_v25_CParticleNodeTransformOrientation.size())},
    HandlerDef{"CParticleNodeTypeCast", std::span<const FieldDef>(kFields_v25_CParticleNodeTypeCast.data(), kFields_v25_CParticleNodeTypeCast.size())},
    HandlerDef{"CParticleRendererFeature", std::span<const FieldDef>(kFields_v25_CParticleRendererFeature.data(), kFields_v25_CParticleRendererFeature.size())},
    HandlerDef{"CParticleRendererFeatureDesc", std::span<const FieldDef>(kFields_v25_CParticleRendererFeatureDesc.data(), kFields_v25_CParticleRendererFeatureDesc.size())},
    HandlerDef{"CParticleRendererFeatureSet", std::span<const FieldDef>(kFields_v25_CParticleRendererFeatureSet.data(), kFields_v25_CParticleRendererFeatureSet.size())},
    HandlerDef{"CParticleTimeline", std::span<const FieldDef>(kFields_v25_CParticleTimeline.data(), kFields_v25_CParticleTimeline.size())},
    HandlerDef{"CParticleTimelineTrack", std::span<const FieldDef>(kFields_v25_CParticleTimelineTrack.data(), kFields_v25_CParticleTimelineTrack.size())},
    HandlerDef{"CProjectSettings", std::span<const FieldDef>(kFields_v25_CProjectSettings.data(), kFields_v25_CProjectSettings.size())},
    HandlerDef{"CProjectSettingsAssets", std::span<const FieldDef>(kFields_v25_CProjectSettingsAssets.data(), kFields_v25_CProjectSettingsAssets.size())},
    HandlerDef{"CProjectSettingsBaking", std::span<const FieldDef>(kFields_v25_CProjectSettingsBaking.data(), kFields_v25_CProjectSettingsBaking.size())},
    HandlerDef{"CProjectSettingsDocumentation", std::span<const FieldDef>(kFields_v25_CProjectSettingsDocumentation.data(), kFields_v25_CProjectSettingsDocumentation.size())},
    HandlerDef{"CProjectSettingsGeneral", std::span<const FieldDef>(kFields_v25_CProjectSettingsGeneral.data(), kFields_v25_CProjectSettingsGeneral.size())},
    HandlerDef{"CProjectSettingsLOD", std::span<const FieldDef>(kFields_v25_CProjectSettingsLOD.data(), kFields_v25_CProjectSettingsLOD.size())},
    HandlerDef{"CProjectSettingsScene", std::span<const FieldDef>(kFields_v25_CProjectSettingsScene.data(), kFields_v25_CProjectSettingsScene.size())},
    HandlerDef{"CSamplerCurve", std::span<const FieldDef>(kFields_v25_CSamplerCurve.data(), kFields_v25_CSamplerCurve.size())},
    HandlerDef{"CSkeletonAnimation", std::span<const FieldDef>(kFields_v25_CSkeletonAnimation.data(), kFields_v25_CSkeletonAnimation.size())},
    HandlerDef{"CSkeletonAnimationBone", std::span<const FieldDef>(kFields_v25_CSkeletonAnimationBone.data(), kFields_v25_CSkeletonAnimationBone.size())},
}};

constexpr std::array<FieldDef, 7> kFields_v29_CCompilerBlobCache{{
    FieldDef{"ClassName", "string"},
    FieldDef{"Identifier", "uint"},
    FieldDef{"Blob", "unknown[]"},
    FieldDef{"GPUBindingPoints", "int[]"},
    FieldDef{"RuntimeExternalsBlob", "unknown[]"},
    FieldDef{"RuntimeExternalNames", "string[]"},
    FieldDef{"RuntimeExternalMangledCalls", "string[]"},
}};

constexpr std::array<FieldDef, 32> kFields_v29_CLayerCompileCache{{
    FieldDef{"ConstantData", "uint4[]"},
    FieldDef{"RangeData", "uint4[]"},
    FieldDef{"Fields", "link[]"},
    FieldDef{"Attribs", "link[]"},
    FieldDef{"AttribSamplers", "link[]"},
    FieldDef{"Events", "link[]"},
    FieldDef{"SpatialLayers", "link[]"},
    FieldDef{"Samplers", "link[]"},
    FieldDef{"Renderers", "link[]"},
    FieldDef{"BlobCache_Backends", "link[]"},
    FieldDef{"StringTable", "string[]"},
    FieldDef{"RootEvent", "link"},
    FieldDef{"UsageFlags", "uint"},
    FieldDef{"HasEvolutionSideEffects", "bool"},
    FieldDef{"NeedsEvolveOnDeath", "bool"},
    FieldDef{"IsFrameSubdividable", "bool"},
    FieldDef{"ActiveEvaluatorMask", "uint"},
    FieldDef{"WaitingState_Spawn", "uint"},
    FieldDef{"WaitingState_TimeVarying", "uint"},
    FieldDef{"WaitingState_TimeFixed", "uint4"},
    FieldDef{"LayerName", "string"},
    FieldDef{"PreferredSimLocation", "int"},
    FieldDef{"PreferredStorageSize", "int"},
    FieldDef{"PreferredLocalization", "int"},
    FieldDef{"FeatureFlags", "uint"},
    FieldDef{"RandomSeedModifier", "uint"},
    FieldDef{"LODMetricOverride", "bool"},
    FieldDef{"LODDistanceMin", "float"},
    FieldDef{"LODDistanceMax", "float"},
    FieldDef{"PrewarmPreferredSubdivision", "int"},
    FieldDef{"PrewarmMaxTickDelta", "float"},
    FieldDef{"PrewarmMaxTickCount", "uint"},
}};

constexpr std::array<FieldDef, 11> kFields_v29_CLayerCompileCacheAttrib{{
    FieldDef{"AttrName", "string"},
    FieldDef{"AttrDescription", "string_localized"},
    FieldDef{"AttrType", "uint"},
    FieldDef{"AttrDataSemantic", "int"},
    FieldDef{"AttrFlags", "uint"},
    FieldDef{"AttrMinI4", "int4"},
    FieldDef{"AttrMaxI4", "int4"},
    FieldDef{"AttrDefaultValueI4", "int4"},
    FieldDef{"AttrMinF4", "float4"},
    FieldDef{"AttrMaxF4", "float4"},
    FieldDef{"AttrDefaultValueF4", "float4"},
}};

constexpr std::array<FieldDef, 4> kFields_v29_CLayerCompileCacheEvent{{
    FieldDef{"EventName", "string"},
    FieldDef{"EventFlags", "uint"},
    FieldDef{"EventPayload", "link[]"},
    FieldDef{"EventPayloadSizeInBytes", "uint"},
}};

constexpr std::array<FieldDef, 6> kFields_v29_CLayerCompileCacheEventPayload{{
    FieldDef{"PayloadName", "string"},
    FieldDef{"PayloadType", "uint"},
    FieldDef{"PayloadFootprintInBytes", "uint"},
    FieldDef{"PayloadFlags", "uint"},
    FieldDef{"PayloadKind", "uint"},
    FieldDef{"PayloadRangeSlot", "uint"},
}};

constexpr std::array<FieldDef, 6> kFields_v29_CLayerCompileCacheField{{
    FieldDef{"FieldNameID", "uint"},
    FieldDef{"FieldType", "uint"},
    FieldDef{"FieldStorageSize", "uint"},
    FieldDef{"FieldFlags", "uint"},
    FieldDef{"FieldConstantSlot", "int"},
    FieldDef{"FieldRangeSlot", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v29_CLayerCompileCacheRenderer{{
    FieldDef{"RendererClass", "int"},
    FieldDef{"Streams", "link[]"},
    FieldDef{"Properties", "link[]"},
    FieldDef{"RendererUID", "uint"},
    FieldDef{"FeatureSetPath", "string"},
}};

constexpr std::array<FieldDef, 4> kFields_v29_CLayerCompileCacheRendererParticleInput{{
    FieldDef{"Semantic", "uint"},
    FieldDef{"IndexInStorage", "uint"},
    FieldDef{"AdditionalFieldName", "string"},
    FieldDef{"AdditionalFieldType", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v29_CLayerCompileCacheRendererProperty{{
    FieldDef{"PropertyName", "string"},
    FieldDef{"PropertyType", "int"},
    FieldDef{"PropertyValueNumeric", "uint4"},
    FieldDef{"PropertyValueStr", "string"},
    FieldDef{"PropertySemantic", "int"},
}};

constexpr std::array<FieldDef, 5> kFields_v29_CLayerCompileCacheSampler{{
    FieldDef{"SamplerName", "string"},
    FieldDef{"Sampler", "link"},
    FieldDef{"SamplerData", "string"},
    FieldDef{"SamplerType", "int"},
    FieldDef{"SamplerUsageFlags", "uint"},
}};

constexpr std::array<FieldDef, 7> kFields_v29_CLayerGraphCompileCache{{
    FieldDef{"BuildVersionName", "string"},
    FieldDef{"EntryPointID", "int"},
    FieldDef{"EntrySlots", "link[]"},
    FieldDef{"LayerSlots", "link[]"},
    FieldDef{"EventSlots", "int[]"},
    FieldDef{"BroadSlots", "link[]"},
    FieldDef{"CoordinateSystem", "uint4"},
}};

constexpr std::array<FieldDef, 2> kFields_v29_CLayerGraphCompileCache_EntrySlot{{
    FieldDef{"EntryName", "string_unicode"},
    FieldDef{"EventSlot", "int"},
}};

constexpr std::array<FieldDef, 4> kFields_v29_CLayerGraphCompileCache_EventSlot{{
    FieldDef{"EventName", "string"},
    FieldDef{"ParentLayerSlot", "int"},
    FieldDef{"LayerTargets", "int[]"},
    FieldDef{"BroadTargets", "int[]"},
}};

constexpr std::array<FieldDef, 3> kFields_v29_CLayerGraphCompileCache_LayerSlot{{
    FieldDef{"LayerCache", "link"},
    FieldDef{"ParentEventSlots", "int[]"},
    FieldDef{"EventSlots", "int[]"},
}};

constexpr std::array<FieldDef, 52> kFields_v29_CParticleAttributeDeclaration{{
    FieldDef{"ExportedName", "string"},
    FieldDef{"ExportedType", "int"},
    FieldDef{"CategoryName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Semantic", "int"},
    FieldDef{"UseSlider", "bool"},
    FieldDef{"HasMin", "bool"},
    FieldDef{"HasMax", "bool"},
    FieldDef{"DefaultValueF1", "float"},
    FieldDef{"DefaultValueF2", "float2"},
    FieldDef{"DefaultValueF3", "float3"},
    FieldDef{"DefaultValueF3C", "float3"},
    FieldDef{"DefaultValueF3D", "float3"},
    FieldDef{"DefaultValueF3S", "float3"},
    FieldDef{"DefaultValueF4", "float4"},
    FieldDef{"DefaultValueF4C", "float4"},
    FieldDef{"DefaultValueI1", "int"},
    FieldDef{"DefaultValueI2", "int2"},
    FieldDef{"DefaultValueI3", "int3"},
    FieldDef{"DefaultValueI3D", "int3"},
    FieldDef{"DefaultValueI3S", "int3"},
    FieldDef{"DefaultValueI4", "int4"},
    FieldDef{"DefaultValueB1", "bool"},
    FieldDef{"DefaultValueB2", "bool2"},
    FieldDef{"DefaultValueB3", "bool3"},
    FieldDef{"DefaultValueB4", "bool4"},
    FieldDef{"DefaultValueO", "float3"},
    FieldDef{"MinValueF1", "float"},
    FieldDef{"MinValueF2", "float2"},
    FieldDef{"MinValueF3", "float3"},
    FieldDef{"MinValueF3D", "float3"},
    FieldDef{"MinValueF3S", "float3"},
    FieldDef{"MinValueF4", "float4"},
    FieldDef{"MinValueI1", "int"},
    FieldDef{"MinValueI2", "int2"},
    FieldDef{"MinValueI3", "int3"},
    FieldDef{"MinValueI3D", "int3"},
    FieldDef{"MinValueI3S", "int3"},
    FieldDef{"MinValueI4", "int4"},
    FieldDef{"MaxValueF1", "float"},
    FieldDef{"MaxValueF2", "float2"},
    FieldDef{"MaxValueF3", "float3"},
    FieldDef{"MaxValueF3D", "float3"},
    FieldDef{"MaxValueF3S", "float3"},
    FieldDef{"MaxValueF4", "float4"},
    FieldDef{"MaxValueI1", "int"},
    FieldDef{"MaxValueI2", "int2"},
    FieldDef{"MaxValueI3", "int3"},
    FieldDef{"MaxValueI3D", "int3"},
    FieldDef{"MaxValueI3S", "int3"},
    FieldDef{"MaxValueI4", "int4"},
    FieldDef{"Pinned", "bool"},
}};

constexpr std::array<FieldDef, 2> kFields_v29_CParticleAttributeList{{
    FieldDef{"AttributeList", "link[]"},
    FieldDef{"SamplerList", "link[]"},
}};

constexpr std::array<FieldDef, 14> kFields_v29_CParticleEffect{{
    FieldDef{"LayerGraph", "link"},
    FieldDef{"LayerGraphCompileCaches", "link[]"},
    FieldDef{"Templates", "link[]"},
    FieldDef{"AttributeFlatList", "link"},
    FieldDef{"BoundsMode", "int"},
    FieldDef{"StaticBoundsMin", "float3"},
    FieldDef{"StaticBoundsMax", "float3"},
    FieldDef{"LODMetricOverride", "bool"},
    FieldDef{"LODDistanceMin", "float"},
    FieldDef{"LODDistanceMax", "float"},
    FieldDef{"EnablePrewarm", "bool"},
    FieldDef{"PrewarmTime", "float"},
    FieldDef{"PrewarmMaxTickDelta", "float"},
    FieldDef{"PrewarmMaxTickCount", "uint"},
}};

constexpr std::array<FieldDef, 11> kFields_v29_CParticleNodePinIn{{
    FieldDef{"SelfName", "string"},
    FieldDef{"Type", "int"},
    FieldDef{"Visible", "bool"},
    FieldDef{"ExportInParent", "bool"},
    FieldDef{"Owner", "link"},
    FieldDef{"BaseType", "int"},
    FieldDef{"ConnectedPins", "link[]"},
    FieldDef{"ValueF", "float4"},
    FieldDef{"ValueI", "int4"},
    FieldDef{"ValueB", "bool4"},
    FieldDef{"ValueD", "string"},
}};

constexpr std::array<FieldDef, 7> kFields_v29_CParticleNodePinOut{{
    FieldDef{"SelfName", "string"},
    FieldDef{"Type", "int"},
    FieldDef{"Visible", "bool"},
    FieldDef{"ExportInParent", "bool"},
    FieldDef{"Owner", "link"},
    FieldDef{"BaseType", "int"},
    FieldDef{"ConnectedPins", "link[]"},
}};

constexpr std::array<FieldDef, 21> kFields_v29_CParticleNodeSamplerData_Curve{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"HasTimeBounds", "bool"},
    FieldDef{"ValueType", "uint"},
    FieldDef{"Interpolator", "uint"},
    FieldDef{"MinLimits", "float4"},
    FieldDef{"MaxLimits", "float4"},
    FieldDef{"Quality", "uint"},
    FieldDef{"IsLoopedCurve", "bool"},
    FieldDef{"NeverOptimize", "bool"},
    FieldDef{"Times", "float[]"},
    FieldDef{"FloatValues", "float[]"},
    FieldDef{"FloatTangents", "float[]"},
    FieldDef{"Annotations", "link[]"},
}};

constexpr std::array<FieldDef, 14> kFields_v29_CParticleNodeSamplerData_EventStream{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"NeverOptimize", "bool"},
    FieldDef{"EventSource", "int"},
    FieldDef{"Times", "float[]"},
    FieldDef{"Payload", "link[]"},
    FieldDef{"Annotations", "link[]"},
}};

constexpr std::array<FieldDef, 31> kFields_v29_CParticleNodeSamplerData_Shape{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"NormalizedInnerRadius", "float"},
    FieldDef{"Height", "float"},
    FieldDef{"Radius", "float"},
    FieldDef{"BoxDimensions", "float3"},
    FieldDef{"SampleDimensionality", "uint"},
    FieldDef{"ShapeType", "int"},
    FieldDef{"MeshResource", "string"},
    FieldDef{"InnerRadius", "float"},
    FieldDef{"Hemisphere", "bool"},
    FieldDef{"NonUniformScale", "float3"},
    FieldDef{"MeshSamplingMode", "uint"},
    FieldDef{"SubMeshIndex", "uint"},
    FieldDef{"DefaultUvStream", "uint"},
    FieldDef{"DefaultColorStream", "uint"},
    FieldDef{"DensityColorStream", "uint"},
    FieldDef{"DensityChannel", "uint"},
    FieldDef{"TransformRotate", "bool"},
    FieldDef{"Orientation", "float3"},
    FieldDef{"TransformTranslate", "bool"},
    FieldDef{"NeverOptimize", "bool"},
    FieldDef{"MeshScale", "float3"},
    FieldDef{"Position", "float3"},
}};

constexpr std::array<FieldDef, 15> kFields_v29_CParticleNodeSamplerData_Text{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"NeverOptimize", "bool"},
    FieldDef{"DataSource", "uint"},
    FieldDef{"InlineText", "string"},
    FieldDef{"ExternalResource", "string"},
    FieldDef{"FontFile", "string"},
    FieldDef{"UseKerning", "bool"},
}};

constexpr std::array<FieldDef, 32> kFields_v29_CParticleNodeSamplerData_Turbulence{{
    FieldDef{"CustomName", "string_localized"},
    FieldDef{"Description", "string_localized"},
    FieldDef{"Active", "bool"},
    FieldDef{"ExecStage", "uint"},
    FieldDef{"ExecFrequency", "uint"},
    FieldDef{"InputPins", "link[]"},
    FieldDef{"OutputPins", "link[]"},
    FieldDef{"WorkspacePosition", "int2"},
    FieldDef{"WorkspaceRollupState", "int"},
    FieldDef{"NeverOptimize", "bool"},
    FieldDef{"GlobalScale", "float"},
    FieldDef{"Strength", "float"},
    FieldDef{"WrapSide", "bool"},
    FieldDef{"WrapVertical", "bool"},
    FieldDef{"WrapDepth", "bool"},
    FieldDef{"WrapTime", "bool"},
    FieldDef{"FlowFactor", "float"},
    FieldDef{"DivergenceFactor", "float"},
    FieldDef{"InitialSeed", "uint"},
    FieldDef{"DataSource", "uint"},
    FieldDef{"VectorFieldResource", "string"},
    FieldDef{"Filtering", "int"},
    FieldDef{"Wavelength", "float"},
    FieldDef{"TimeRandomVariation", "float"},
    FieldDef{"Octaves", "uint"},
    FieldDef{"Lacunarity", "float"},
    FieldDef{"Gain", "float"},
    FieldDef{"Interpolator", "int"},
    FieldDef{"TimeScale", "float"},
    FieldDef{"TimeBase", "float"},
    FieldDef{"FastFakeFlow", "bool"},
    FieldDef{"GainMultiplier", "float"},
}};

constexpr std::array<HandlerDef, 24> kHandlerDefs_v29{{
    HandlerDef{"CCompilerBlobCache", std::span<const FieldDef>(kFields_v29_CCompilerBlobCache.data(), kFields_v29_CCompilerBlobCache.size())},
    HandlerDef{"CLayerCompileCache", std::span<const FieldDef>(kFields_v29_CLayerCompileCache.data(), kFields_v29_CLayerCompileCache.size())},
    HandlerDef{"CLayerCompileCacheAttrib", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheAttrib.data(), kFields_v29_CLayerCompileCacheAttrib.size())},
    HandlerDef{"CLayerCompileCacheEvent", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheEvent.data(), kFields_v29_CLayerCompileCacheEvent.size())},
    HandlerDef{"CLayerCompileCacheEventPayload", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheEventPayload.data(), kFields_v29_CLayerCompileCacheEventPayload.size())},
    HandlerDef{"CLayerCompileCacheField", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheField.data(), kFields_v29_CLayerCompileCacheField.size())},
    HandlerDef{"CLayerCompileCacheRenderer", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheRenderer.data(), kFields_v29_CLayerCompileCacheRenderer.size())},
    HandlerDef{"CLayerCompileCacheRendererParticleInput", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheRendererParticleInput.data(), kFields_v29_CLayerCompileCacheRendererParticleInput.size())},
    HandlerDef{"CLayerCompileCacheRendererProperty", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheRendererProperty.data(), kFields_v29_CLayerCompileCacheRendererProperty.size())},
    HandlerDef{"CLayerCompileCacheSampler", std::span<const FieldDef>(kFields_v29_CLayerCompileCacheSampler.data(), kFields_v29_CLayerCompileCacheSampler.size())},
    HandlerDef{"CLayerGraphCompileCache", std::span<const FieldDef>(kFields_v29_CLayerGraphCompileCache.data(), kFields_v29_CLayerGraphCompileCache.size())},
    HandlerDef{"CLayerGraphCompileCache_EntrySlot", std::span<const FieldDef>(kFields_v29_CLayerGraphCompileCache_EntrySlot.data(), kFields_v29_CLayerGraphCompileCache_EntrySlot.size())},
    HandlerDef{"CLayerGraphCompileCache_EventSlot", std::span<const FieldDef>(kFields_v29_CLayerGraphCompileCache_EventSlot.data(), kFields_v29_CLayerGraphCompileCache_EventSlot.size())},
    HandlerDef{"CLayerGraphCompileCache_LayerSlot", std::span<const FieldDef>(kFields_v29_CLayerGraphCompileCache_LayerSlot.data(), kFields_v29_CLayerGraphCompileCache_LayerSlot.size())},
    HandlerDef{"CParticleAttributeDeclaration", std::span<const FieldDef>(kFields_v29_CParticleAttributeDeclaration.data(), kFields_v29_CParticleAttributeDeclaration.size())},
    HandlerDef{"CParticleAttributeList", std::span<const FieldDef>(kFields_v29_CParticleAttributeList.data(), kFields_v29_CParticleAttributeList.size())},
    HandlerDef{"CParticleEffect", std::span<const FieldDef>(kFields_v29_CParticleEffect.data(), kFields_v29_CParticleEffect.size())},
    HandlerDef{"CParticleNodePinIn", std::span<const FieldDef>(kFields_v29_CParticleNodePinIn.data(), kFields_v29_CParticleNodePinIn.size())},
    HandlerDef{"CParticleNodePinOut", std::span<const FieldDef>(kFields_v29_CParticleNodePinOut.data(), kFields_v29_CParticleNodePinOut.size())},
    HandlerDef{"CParticleNodeSamplerData_Curve", std::span<const FieldDef>(kFields_v29_CParticleNodeSamplerData_Curve.data(), kFields_v29_CParticleNodeSamplerData_Curve.size())},
    HandlerDef{"CParticleNodeSamplerData_EventStream", std::span<const FieldDef>(kFields_v29_CParticleNodeSamplerData_EventStream.data(), kFields_v29_CParticleNodeSamplerData_EventStream.size())},
    HandlerDef{"CParticleNodeSamplerData_Shape", std::span<const FieldDef>(kFields_v29_CParticleNodeSamplerData_Shape.data(), kFields_v29_CParticleNodeSamplerData_Shape.size())},
    HandlerDef{"CParticleNodeSamplerData_Text", std::span<const FieldDef>(kFields_v29_CParticleNodeSamplerData_Text.data(), kFields_v29_CParticleNodeSamplerData_Text.size())},
    HandlerDef{"CParticleNodeSamplerData_Turbulence", std::span<const FieldDef>(kFields_v29_CParticleNodeSamplerData_Turbulence.data(), kFields_v29_CParticleNodeSamplerData_Turbulence.size())},
}};

const HandlerDef* lookup(const HandlerDef* first, const HandlerDef* last,
                         std::string_view handlerName) noexcept {
    const auto* it = std::lower_bound(
        first, last, handlerName,
        [](const HandlerDef& h, std::string_view name) noexcept { return h.name < name; });
    if (it == last || it->name != handlerName) {
        return nullptr;
    }
    return it;
}

}

const HandlerDef* findHandlerDef(std::string_view handlerName,
                                 HboSchemaVersion schema) noexcept {
    if (schema == HboSchemaVersion::V2_9) {
        return lookup(kHandlerDefs_v29.data(), kHandlerDefs_v29.data() + kHandlerDefs_v29.size(), handlerName);
    }
    return lookup(kHandlerDefs_v25.data(), kHandlerDefs_v25.data() + kHandlerDefs_v25.size(), handlerName);
}

std::size_t handlerDefCount(HboSchemaVersion schema) noexcept {
    return schema == HboSchemaVersion::V2_9 ? kHandlerDefs_v29.size() : kHandlerDefs_v25.size();
}

}
