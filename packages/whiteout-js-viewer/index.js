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
    HD_DEBUG_MODES,
    TEAM_COLORS,
    TEAM_COLOR_NAMES,
    Instance,
    Model,
    Scene,
} from './wf-viewer.js';

export { WebAudioBridge } from './web-audio.js';

// Load-table helpers — JSON manifest of model URLs + per-asset overrides.
// Useful both inside HiveApp (which consumes them automatically) and for
// callers who use the raw WhiteoutViewer with their own UI.
export {
    makeLoadTableSolver,
    buildOverrideMap,
    tableModels,
    isAbsoluteUrl,
} from './load-table.js';
