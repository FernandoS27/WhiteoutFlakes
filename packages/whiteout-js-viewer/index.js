// Public surface for whiteout-js-viewer.
//
// Two consumption modes:
//   1. HiveApp     — drop-in shell that wires a viewer to a set of DOM
//                    elements you provide; mimics hiveworkshop.com's
//                    "View in 3D" panel.
//   2. wf-* modules — raw building blocks (WhiteoutViewer, Instance,
//                    Scene, WebAudioBridge, …) for callers building
//                    their own UI.

export { HiveApp } from './hive-app.js';

export {
    WhiteoutViewer,
    DEBUG_VIEWS,
    debugViewsFor,
    HD_DEBUG_MODES,
    TEAM_COLORS,
    TEAM_COLOR_NAMES,
    Instance,
    Model,
    Scene,
    // `.pkb` / `.pkfx` detection — hosts that build their own model list
    // need it to decide what is listable and what a row will spawn as.
    EFFECT_EXTENSIONS,
    isEffectPath,
    // Same, for the formats that spawn as a model: `.mdx` / `.mdl` and
    // StarCraft II / Heroes `.m3`.
    MODEL_EXTENSIONS,
    isModelPath,
} from './wf-viewer.js';

export { WebAudioBridge } from './web-audio.js';

// Hive CASC mirror URL policy. Exported because a host that supplies its
// own pathSolver still wants the same two-route chain the built-in one
// uses, rather than a third private copy of the rules.
export {
    hiveCandidates,
    cascContentsUrl,
    directUrl,
    mirrorName,
    requestName,
    normalizePath,
} from './hive-resolve.js';

// Load-table helpers — JSON manifest of model URLs + per-asset overrides.
// Useful both inside HiveApp (which consumes them automatically) and for
// callers who use the raw WhiteoutViewer with their own UI.
export {
    makeLoadTableSolver,
    buildOverrideMap,
    tableModels,
    isAbsoluteUrl,
} from './load-table.js';
