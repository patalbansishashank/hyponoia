import { useEffect, useState, useCallback, useMemo } from "react";
import { Button } from "@/components/ui/button";
import {
  useGraphData,
  clampNodeBudget,
  GRAPH_RENDER_NODE_LIMIT,
  GRAPH_NODE_BUDGET_STEP,
  GRAPH_NODE_BUDGET_MAX,
} from "../hooks/useGraphData";
import { GraphLoader } from "./GraphLoader";
import { DisplaySettingsMenu } from "./DisplaySettingsMenu";
import {
  useEmbedView,
  buildEmbedScene,
  EMBED_VIEW_CAVEAT_FALLBACK,
} from "../hooks/useEmbedView";
import {
  loadDisplaySettings,
  saveDisplaySettings,
  type DisplaySettings,
} from "../lib/density";
import {
  GraphScene,
  computeCameraTarget,
  type CameraTarget,
} from "./GraphScene";
import { Sidebar } from "./Sidebar";
import { FilterPanel } from "./FilterPanel";
import { NodeDetailPanel } from "./NodeDetailPanel";
import { MissedCallout } from "./MissedCallout";
import { ResizeHandle } from "./ResizeHandle";
import { ErrorBoundary } from "./ErrorBoundary";
import type { GraphNode, GraphData, RepoInfo } from "../lib/types";
import { colorForStatus } from "../lib/colors";

/* Persist panel widths */
function loadWidth(key: string, fallback: number): number {
  try {
    const v = localStorage.getItem(key);
    if (v) return Math.max(150, Math.min(600, parseInt(v, 10)));
  } catch { /* ignore */ }
  return fallback;
}
function saveWidth(key: string, value: number) {
  try { localStorage.setItem(key, String(Math.round(value))); } catch { /* ignore */ }
}

/* Persist the node budget per project */
function budgetKey(project: string): string {
  return `hyp-node-budget:${project}`;
}
function loadNodeBudget(project: string): number {
  try {
    const v = localStorage.getItem(budgetKey(project));
    if (v) return clampNodeBudget(parseInt(v, 10));
  } catch { /* ignore */ }
  return GRAPH_RENDER_NODE_LIMIT;
}
function saveNodeBudget(project: string, value: number) {
  try { localStorage.setItem(budgetKey(project), String(value)); } catch { /* ignore */ }
}

interface GraphTabProps {
  project: string | null;
}

export function formatGraphLimitNotice(data: GraphData | null): string | null {
  if (!data || data.total_nodes <= data.nodes.length) return null;
  return `Showing ${data.nodes.length.toLocaleString("en-US")} of ${data.total_nodes.toLocaleString("en-US")} nodes (${data.edges.length.toLocaleString("en-US")} edges). Raise the node budget or use filters.`;
}

export function GraphTab({ project }: GraphTabProps) {
  const { data, loading, error, progress, fetchOverview } = useGraphData();
  const [highlightedIds, setHighlightedIds] = useState<Set<number> | null>(null);
  const [selectedPath, setSelectedPath] = useState<string | null>(null);
  const [selectedNode, setSelectedNode] = useState<GraphNode | null>(null);
  const [cameraTarget, setCameraTarget] = useState<CameraTarget | null>(null);
  const [repoInfo, setRepoInfo] = useState<RepoInfo | null>(null);
  const [showLabels, setShowLabels] = useState(true);
  const [display, setDisplay] = useState<DisplaySettings>(() =>
    loadDisplaySettings(),
  );
  const updateDisplay = useCallback((next: DisplaySettings) => {
    setDisplay(next);
    saveDisplaySettings(next);
  }, []);
  const [leftWidth, setLeftWidth] = useState(() => loadWidth("hyp-left-w", 260));
  const [rightWidth, setRightWidth] = useState(() => loadWidth("hyp-right-w", 280));
  const limitNotice = formatGraphLimitNotice(data);

  /* Node budget — keyed to its project so switching projects re-reads the
   * persisted value and triggers exactly one fetch. */
  const [budget, setBudget] = useState<{ project: string | null; value: number }>(
    { project: null, value: GRAPH_RENDER_NODE_LIMIT },
  );
  const [budgetDraft, setBudgetDraft] = useState(String(GRAPH_RENDER_NODE_LIMIT));

  const commitBudget = useCallback(() => {
    const parsed = clampNodeBudget(parseInt(budgetDraft, 10));
    setBudgetDraft(String(parsed));
    if (project && parsed !== budget.value) {
      saveNodeBudget(project, parsed);
      setBudget({ project, value: parsed });
    }
  }, [budgetDraft, project, budget.value]);

  /* Filter state — all enabled by default */
  const [enabledLabels, setEnabledLabels] = useState<Set<string>>(new Set());
  const [enabledEdgeTypes, setEnabledEdgeTypes] = useState<Set<string>>(new Set());

  /* Missed skeleton (#963): the file structure of files the indexer could
   * not fully cover, shown as a white satellite cluster beside the code
   * galaxy. Toggle only hides/shows it — the data rides along with every
   * code-graph layout. */
  const [showMissedSkeleton, setShowMissedSkeleton] = useState(true);

  /* Dead-code view: recolor by status + status-based filters */
  const [deadCodeView, setDeadCodeView] = useState(false);
  const [showOnlyDead, setShowOnlyDead] = useState(false);
  const [hideEntryPoints, setHideEntryPoints] = useState(false);
  const [hideTests, setHideTests] = useState(false);

  /* Embedding view (NEXT-STEPS §3.1 step 5): the `ask` lane's vectors
   * projected to 3-D, with a question and its ranked answer overlaid. Same
   * scene, same click handling — only the coordinates come from the vector
   * file instead of the force layout. */
  /* Deep link: `?view=embed&q=<question>` opens the embedding view and asks
   * — so a question-vs-neighbours picture can be handed to someone as a URL,
   * and reproduced (the projection is deterministic; the ranking is the
   * tool's). */
  const [viewMode, setViewMode] = useState<"graph" | "embed">(() => {
    const p = new URLSearchParams(window.location.search);
    return p.get("view") === "embed" ? "embed" : "graph";
  });
  const embed = useEmbedView();
  const [questionDraft, setQuestionDraft] = useState(
    () => new URLSearchParams(window.location.search).get("q") ?? "",
  );
  const [autoAsked, setAutoAsked] = useState(false);
  const embedScene = useMemo(
    () => buildEmbedScene(embed.cloud, embed.overlay),
    [embed.cloud, embed.overlay],
  );

  /* Initialize filters when data loads */
  useEffect(() => {
    if (!data) return;
    const labels = new Set(data.nodes.map((n) => n.label));
    const types = new Set(data.edges.map((e) => e.type));
    for (const lp of data.linked_projects ?? []) {
      for (const n of lp.nodes) labels.add(n.label);
      for (const e of lp.edges) types.add(e.type);
      for (const e of lp.cross_edges) types.add(e.type);
    }
    setEnabledLabels(labels);
    setEnabledEdgeTypes(types);
  }, [data]);

  /* Compute filtered data */
  const filteredData: GraphData | null = useMemo(() => {
    if (!data) return null;

    /* Status-based filters (dead-code view) */
    const statusOk = (n: GraphNode) => {
      if (showOnlyDead && n.status !== "dead") return false;
      if (hideEntryPoints && n.status === "entry") return false;
      if (hideTests && n.status === "test") return false;
      return true;
    };
    /* Recolor by status when the dead-code view is on */
    const paint = (n: GraphNode): GraphNode =>
      deadCodeView ? { ...n, color: colorForStatus(n.status) } : n;
    const keep = (n: GraphNode) => enabledLabels.has(n.label) && statusOk(n);

    const nodes = data.nodes.filter(keep).map(paint);
    const nodeIds = new Set(nodes.map((n) => n.id));
    const edges = data.edges.filter(
      (e) =>
        enabledEdgeTypes.has(e.type) &&
        nodeIds.has(e.source) &&
        nodeIds.has(e.target),
    );

    const linked_projects = data.linked_projects?.map((lp) => {
      const lpNodes = lp.nodes.filter(keep).map(paint);
      const lpIds = new Set(lpNodes.map((n) => n.id));
      const lpEdges = lp.edges.filter(
        (e) =>
          enabledEdgeTypes.has(e.type) && lpIds.has(e.source) && lpIds.has(e.target),
      );
      const crossEdges = lp.cross_edges.filter(
        (e) =>
          enabledEdgeTypes.has(e.type) && nodeIds.has(e.source) && lpIds.has(e.target),
      );
      return { ...lp, nodes: lpNodes, edges: lpEdges, cross_edges: crossEdges };
    });

    return { nodes, edges, total_nodes: data.total_nodes, linked_projects };
  }, [
    data,
    enabledLabels,
    enabledEdgeTypes,
    deadCodeView,
    showOnlyDead,
    hideEntryPoints,
    hideTests,
  ]);

  /* Re-read the persisted budget when the project changes… */
  useEffect(() => {
    if (project) {
      const value = loadNodeBudget(project);
      setBudget({ project, value });
      setBudgetDraft(String(value));
    }
  }, [project]);

  /* …and fetch only once budget and project agree (one fetch per change). */
  useEffect(() => {
    if (project && budget.project === project) {
      fetchOverview(project, budget.value);
      setHighlightedIds(null);
      setSelectedPath(null);
    }
  }, [project, budget, fetchOverview]);

  /* Embedding view: load the cloud when the view is opened or the project
   * changes; drop it when the view is left so a stale cloud is never drawn
   * for another project. */
  const { loadCloud: embedLoadCloud, clear: embedClear } = embed;
  useEffect(() => {
    if (viewMode === "embed" && project) {
      embedLoadCloud(project);
    } else {
      embedClear();
    }
  }, [viewMode, project, embedLoadCloud, embedClear]);

  /* Frame the whole cloud when it arrives, and the question + its neighbours
   * when an answer arrives. */
  useEffect(() => {
    if (viewMode !== "embed" || !embedScene) return;
    const ids = embedScene.highlightedIds ?? new Set(embedScene.data.nodes.map((n) => n.id));
    setCameraTarget(computeCameraTarget(embedScene.data.nodes, ids));
  }, [viewMode, embedScene]);

  const { ask: embedAsk } = embed;
  const submitQuestion = useCallback(() => {
    const q = questionDraft.trim();
    if (project && q) {
      setSelectedNode(null);
      embedAsk(project, q, 10);
    }
  }, [project, questionDraft, embedAsk]);

  /* Deep-linked question: ask once, when the cloud has arrived. */
  useEffect(() => {
    if (!autoAsked && viewMode === "embed" && project && embed.cloud && questionDraft.trim()) {
      setAutoAsked(true);
      embedAsk(project, questionDraft.trim(), 10);
    }
  }, [autoAsked, viewMode, project, embed.cloud, questionDraft, embedAsk]);

  /* Missed skeleton: offset into place and paint white — a ghost of the
   * files the graph could not fully cover, sitting beside the galaxy. */
  const missedSkeleton = useMemo(() => {
    const mg = data?.missed_graph;
    if (!mg || mg.nodes.length === 0) return null;
    const nodes = mg.nodes.map((n) => ({
      ...n,
      x: n.x + mg.offset.x,
      y: n.y + mg.offset.y,
      z: n.z + mg.offset.z,
      color: "#e9eef5",
    }));
    return { nodes, edges: mg.edges, ids: new Set(nodes.map((n) => n.id)) };
  }, [data]);

  /* Overview framing: both clusters (galaxy + skeleton) in one shot. */
  const overviewTarget = useMemo(() => {
    if (!data) return null;
    const all = missedSkeleton ? [...data.nodes, ...missedSkeleton.nodes] : data.nodes;
    return computeCameraTarget(all, new Set(all.map((n) => n.id)));
  }, [data, missedSkeleton]);

  /* With a skeleton beside the galaxy, auto-frame BOTH clusters on load so
   * the side-by-side composition is visible without manual zooming. */
  useEffect(() => {
    if (missedSkeleton && overviewTarget) {
      setCameraTarget(overviewTarget);
    }
  }, [missedSkeleton, overviewTarget]);

  /* Clicking empty space while the skeleton has focus flies back to the
   * overview (the galaxy may be entirely off-screen at that point, so there
   * is no code node to click). No-op during normal galaxy exploration. */
  const handleBackgroundClick = useCallback(() => {
    if (selectedNode && missedSkeleton?.ids.has(selectedNode.id) && overviewTarget) {
      setSelectedNode(null);
      setHighlightedIds(null);
      setSelectedPath(null);
      setCameraTarget(overviewTarget);
    }
  }, [selectedNode, missedSkeleton, overviewTarget]);

  /* Fetch git remote metadata for GitHub deep-links */
  useEffect(() => {
    if (!project) {
      setRepoInfo(null);
      return;
    }
    let cancelled = false;
    fetch(`/api/repo-info?project=${encodeURIComponent(project)}`)
      .then((r) => (r.ok ? r.json() : null))
      .then((d) => {
        if (!cancelled && d && !d.error) setRepoInfo(d as RepoInfo);
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [project]);

  /* What the scene draws: the filtered code graph, or — in the embedding
   * view — the projected cloud with the question overlaid. */
  const inEmbedView = viewMode === "embed";
  const sceneData: GraphData | null =
    inEmbedView ? (embedScene ? embedScene.data : null) : filteredData;
  const sceneHighlight = inEmbedView
    ? (embedScene?.highlightedIds ?? highlightedIds)
    : highlightedIds;

  const handleSelectPath = useCallback(
    (path: string, nodeIds: Set<number>) => {
      const nodes = sceneData?.nodes;
      if (!nodes || !path || nodeIds.size === 0) {
        setHighlightedIds(null);
        setSelectedPath(null);
        setCameraTarget(null);
        return;
      }
      setSelectedPath(path);
      setHighlightedIds(nodeIds);
      setCameraTarget(computeCameraTarget(nodes, nodeIds));
    },
    [sceneData],
  );

  const handleNodeClick = useCallback(
    (node: GraphNode) => {
      if (inEmbedView) {
        /* Embedding view: select and fly to the dot. The question marker and
         * the ranked hits stay highlighted — the overlay is the point. */
        if (!sceneData) return;
        setSelectedNode(node);
        setSelectedPath(node.file_path ?? null);
        setCameraTarget(computeCameraTarget(sceneData.nodes, new Set([node.id])));
        return;
      }
      if (!filteredData) return;

      /* Clicking the missed skeleton re-centers the camera on that whole
       * cluster (it's small — the natural focus unit is the skeleton, not a
       * single node); clicking any code node flies back to the code galaxy
       * via the normal per-node focus below. */
      if (missedSkeleton?.ids.has(node.id)) {
        setSelectedNode(node);
        setHighlightedIds(null);
        setSelectedPath(node.file_path ?? null);
        setCameraTarget(computeCameraTarget(missedSkeleton.nodes, missedSkeleton.ids));
        return;
      }

      setSelectedNode(node);

      /* Highlight the node and its direct connections */
      const connectedIds = new Set([node.id]);
      for (const edge of filteredData.edges) {
        if (edge.source === node.id) connectedIds.add(edge.target);
        if (edge.target === node.id) connectedIds.add(edge.source);
      }
      setHighlightedIds(connectedIds);
      setSelectedPath(node.file_path ?? null);
      setCameraTarget(computeCameraTarget(filteredData.nodes, connectedIds));
    },
    [filteredData, missedSkeleton, inEmbedView, sceneData],
  );

  const handleNavigateToNode = useCallback(
    (node: GraphNode) => {
      handleNodeClick(node);
    },
    [handleNodeClick],
  );

  const toggleLabel = useCallback((label: string) => {
    setEnabledLabels((prev) => {
      const next = new Set(prev);
      if (next.has(label)) next.delete(label);
      else next.add(label);
      return next;
    });
  }, []);

  const toggleEdgeType = useCallback((type: string) => {
    setEnabledEdgeTypes((prev) => {
      const next = new Set(prev);
      if (next.has(type)) next.delete(type);
      else next.add(type);
      return next;
    });
  }, []);

  const enableAll = useCallback(() => {
    if (!data) return;
    const labels = new Set(data.nodes.map((n) => n.label));
    const types = new Set(data.edges.map((e) => e.type));
    for (const lp of data.linked_projects ?? []) {
      for (const n of lp.nodes) labels.add(n.label);
      for (const e of lp.edges) types.add(e.type);
      for (const e of lp.cross_edges) types.add(e.type);
    }
    setEnabledLabels(labels);
    setEnabledEdgeTypes(types);
  }, [data]);

  const disableAll = useCallback(() => {
    setEnabledLabels(new Set());
    setEnabledEdgeTypes(new Set());
  }, []);

  if (!project) {
    return (
      <div className="flex items-center justify-center h-full">
        <p className="text-white/30 text-sm">
          Select a project from the Projects tab
        </p>
      </div>
    );
  }

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full">
        <GraphLoader nodeBudget={budget.value} progress={progress} />
      </div>
    );
  }

  if (error) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="text-center p-8">
          <p className="text-red-400 text-sm mb-2">{error}</p>
          <Button variant="outline" size="sm" onClick={() => fetchOverview(project)}>
            Retry
          </Button>
        </div>
      </div>
    );
  }

  /* No data, or the project genuinely has no nodes — there are no filters to
     interact with, so show a plain full-screen message. The "all filtered out"
     case is handled inside the layout below so the filter sidebar stays put. */
  if (!data || !filteredData || data.nodes.length === 0) {
    return (
      <div className="flex items-center justify-center h-full">
        <p className="text-white/30 text-sm">No nodes in this project</p>
      </div>
    );
  }

  return (
    <div className="h-full flex">
      {/* Left sidebar — resizable */}
      <div
        className="border-r border-border/30 flex flex-col h-full bg-[#0b1920]/90 backdrop-blur-md shrink-0"
        style={{ width: leftWidth }}
      >
        <FilterPanel
          data={data}
          enabledLabels={enabledLabels}
          enabledEdgeTypes={enabledEdgeTypes}
          showLabels={showLabels}
          onToggleLabel={toggleLabel}
          onToggleEdgeType={toggleEdgeType}
          onToggleShowLabels={() => setShowLabels((v) => !v)}
          onEnableAll={enableAll}
          onDisableAll={disableAll}
          deadCodeView={deadCodeView}
          showOnlyDead={showOnlyDead}
          hideEntryPoints={hideEntryPoints}
          hideTests={hideTests}
          onToggleDeadCodeView={() => setDeadCodeView((v) => !v)}
          onToggleShowOnlyDead={() => setShowOnlyDead((v) => !v)}
          onToggleHideEntryPoints={() => setHideEntryPoints((v) => !v)}
          onToggleHideTests={() => setHideTests((v) => !v)}
          missedView={showMissedSkeleton}
          missedCount={data?.missed_graph?.nodes.filter((n) => n.label === "File").length ?? 0}
          onToggleMissedView={() => setShowMissedSkeleton((v) => !v)}
        />
        <Sidebar
          nodes={filteredData.nodes}
          onSelectPath={handleSelectPath}
          selectedPath={selectedPath}
        />
      </div>
      <ResizeHandle
        side="left"
        onResize={(d) => {
          setLeftWidth((w) => {
            const nw = Math.max(150, Math.min(500, w + d));
            saveWidth("hyp-left-w", nw);
            return nw;
          });
        }}
      />

      {/* Graph area */}
      <div className="flex-1 relative overflow-hidden">
        {inEmbedView && !sceneData ? (
          /* Embedding view with nothing to draw: loading, or no view fitted.
           * The reason is said in words — an empty canvas would read as
           * "the project has no embeddings", which may not be the claim. */
          <div className="flex items-center justify-center h-full">
            <div className="text-center max-w-md px-6">
              {embed.loading ? (
                <p className="text-white/40 text-sm">Loading the embedding view...</p>
              ) : embed.error ? (
                <p className="text-red-400 text-sm">{embed.error}</p>
              ) : embed.cloud && !embed.cloud.view.available ? (
                <>
                  <p className="text-white/50 text-sm mb-1">No embedding view for this project</p>
                  <p className="text-white/30 text-xs font-mono">{embed.cloud.view.reason}</p>
                  {embed.cloud.view.remedy && (
                    <p className="text-amber-300/70 text-xs font-mono mt-2">
                      {embed.cloud.view.remedy.replace("<p>", project)}
                    </p>
                  )}
                </>
              ) : (
                <p className="text-white/30 text-sm">No embeddings to draw</p>
              )}
              <Button
                variant="outline"
                size="sm"
                className="mt-4"
                onClick={() => setViewMode("graph")}
              >
                Back to graph
              </Button>
            </div>
          </div>
        ) : !sceneData || sceneData.nodes.length === 0 ? (
          <div className="flex items-center justify-center h-full">
            <div className="text-center">
              <p className="text-white/30 text-sm mb-3">All nodes filtered out</p>
              <Button size="sm" onClick={enableAll}>
                Reset Filters
              </Button>
            </div>
          </div>
        ) : (
          <>
            <ErrorBoundary>
              <GraphScene
                data={sceneData}
                missed={!inEmbedView && showMissedSkeleton ? missedSkeleton : null}
                highlightedIds={sceneHighlight}
                cameraTarget={cameraTarget}
                showLabels={showLabels}
                display={display}
                onNodeClick={handleNodeClick}
                onBackgroundClick={handleBackgroundClick}
              />
            </ErrorBoundary>

            {/* HUD */}
            {inEmbedView && embed.cloud && embedScene ? (
              <div className="absolute top-4 left-4 max-w-[500px] pr-4 text-[11px] text-white/30 font-mono">
                <p>
                  {embedScene.data.nodes.filter((n) => n.id !== -1).length.toLocaleString()}{" "}
                  declarations, embedding view
                </p>
                <p className="text-white/25 mt-0.5">
                  {embed.cloud.view.method ?? "pca"}: {embed.cloud.view.dim ?? "?"}-D to 3-D,{" "}
                  {embed.cloud.view.variance_kept !== undefined
                    ? `${(embed.cloud.view.variance_kept * 100).toFixed(1)}% of variance kept`
                    : "variance kept unknown"}
                  {" "}· deterministic, re-fitted after every embed pass
                  {embed.cloud.view.fitted_at ? ` · fitted ${embed.cloud.view.fitted_at}` : ""}
                </p>
                {/* THE CAVEAT — on the view, not in a tooltip. */}
                <p
                  className="text-amber-300/90 mt-1 pointer-events-none"
                  data-testid="embed-caveat"
                >
                  {embed.cloud.caveat || EMBED_VIEW_CAVEAT_FALLBACK}
                </p>
                {embed.cloud.view.stale && (
                  <p className="text-red-400/90 mt-1">
                    STALE: the index was re-sealed after this view was fitted. Run{" "}
                    <span className="text-red-300">
                      hyponoia embed --project {project} --fit-view
                    </span>
                  </p>
                )}
                {embedScene.unprojected > 0 && (
                  <p className="text-amber-300/70 mt-0.5">
                    {embedScene.unprojected.toLocaleString()} rows have no coordinates yet
                    (re-embedded since the last fit) and are not drawn
                  </p>
                )}
                {embed.asking && (
                  <p className="text-cyan-300/70 mt-1">encoding the question...</p>
                )}
                {embed.error && !embed.asking && (
                  <p className="text-red-400/90 mt-1">{embed.error}</p>
                )}
                {embed.overlay && !embed.asking && (
                  <div className="mt-2 pointer-events-auto">
                    <p className="text-white/70">Q: {embed.overlay.question}</p>
                    {embed.overlay.ask && embed.overlay.ask.available === false ? (
                      <p className="text-amber-300/80 mt-0.5">
                        ask: unavailable — {String(embed.overlay.ask.reason ?? "see the ask tool")}
                      </p>
                    ) : embed.overlay.ask_error ? (
                      <p className="text-red-400/90 mt-0.5">
                        ask: {embed.overlay.ask_text ?? "error"}
                      </p>
                    ) : embed.overlay.hits ? (
                      <>
                        <p className="text-white/40 mt-0.5">
                          ranked by cosine in {embed.cloud.view.dim ?? "?"}-D
                          {embed.overlay.ask?.model ? ` · ${embed.overlay.ask.model}` : ""}
                          {embed.overlay.ask?.language ? ` · ${embed.overlay.ask.language}` : ""}
                          {embed.overlay.ask?.population !== undefined
                            ? ` · population ${embed.overlay.ask.population}`
                            : ""}
                        </p>
                        <ol className="mt-1 space-y-0.5" data-testid="embed-hits">
                          {embed.overlay.hits.map((h) => (
                            <li key={h.rank}>
                              <button
                                className="text-left hover:text-white/90 text-white/60"
                                onClick={() => {
                                  const n = sceneData.nodes.find(
                                    (x) => x.qualified_name === h.qualified_name,
                                  );
                                  if (n) handleNodeClick(n);
                                }}
                                title={h.qualified_name}
                              >
                                <span className="text-cyan-300/90">#{h.rank}</span>{" "}
                                <span className="text-white/40">{h.score.toFixed(3)}</span>{" "}
                                {h.qualified_name.includes(".")
                                  ? h.qualified_name.slice(h.qualified_name.lastIndexOf(".") + 1)
                                  : h.qualified_name}{" "}
                                <span className="text-white/30">
                                  {h.file}:{h.lines}
                                  {h.projected ? "" : " (not drawn: no coordinates)"}
                                </span>
                              </button>
                            </li>
                          ))}
                        </ol>
                        {embed.overlay.ask?.truncation && (
                          <p className="text-white/25 mt-1">{String(embed.overlay.ask.truncation)}</p>
                        )}
                      </>
                    ) : embed.overlay.view.available === false ? (
                      <p className="text-amber-300/80 mt-0.5">
                        {embed.overlay.view.reason} — {embed.overlay.view.remedy?.replace("<p>", project)}
                      </p>
                    ) : null}
                  </div>
                )}
              </div>
            ) : (
              <div className="absolute top-4 left-4 text-[11px] text-white/30 pointer-events-none font-mono">
                <p>
                  {sceneData.nodes.length.toLocaleString()} nodes /{" "}
                  {sceneData.edges.length.toLocaleString()} edges
                </p>
                {data.nodes.length > sceneData.nodes.length && (
                  <p className="text-white/25 mt-0.5">
                    filtered from {data.nodes.length.toLocaleString()}
                  </p>
                )}
                {limitNotice && (
                  <p className="text-amber-300/80 mt-0.5">{limitNotice}</p>
                )}
                {highlightedIds && highlightedIds.size > 0 && (
                  <p className="text-cyan-400/50 mt-0.5">
                    {highlightedIds.size} selected
                  </p>
                )}
              </div>
            )}

            <div className="absolute top-4 right-4 flex gap-2 items-center">
              {inEmbedView && (
                <div className="flex items-center gap-1.5 h-8 px-2 rounded-md border border-border/50 bg-[#0b1920]/80 backdrop-blur-sm">
                  <label
                    htmlFor="embed-question"
                    className="text-[10px] uppercase tracking-wider text-white/40"
                  >
                    Ask
                  </label>
                  <input
                    id="embed-question"
                    type="text"
                    value={questionDraft}
                    onChange={(e) => setQuestionDraft(e.target.value)}
                    onKeyDown={(e) => {
                      if (e.key === "Enter") submitQuestion();
                    }}
                    placeholder="a natural-language question about this code"
                    className="w-56 bg-transparent text-xs font-mono text-cyan-200/90 outline-none placeholder:text-white/20"
                    aria-label="Question for the ask lane; its ranked answer is drawn on the embedding view"
                    disabled={embed.asking}
                  />
                  <Button size="sm" onClick={submitQuestion} disabled={embed.asking || !questionDraft.trim()}>
                    {embed.asking ? "..." : "Ask"}
                  </Button>
                </div>
              )}
              <div
                className="flex items-center h-8 rounded-md border border-border/50 bg-[#0b1920]/80 backdrop-blur-sm overflow-hidden"
                role="group"
                aria-label="View: code graph or embedding space"
              >
                <button
                  className={`px-2 h-full text-[10px] uppercase tracking-wider ${
                    !inEmbedView ? "bg-primary/15 text-primary" : "text-white/40 hover:text-white/70"
                  }`}
                  onClick={() => setViewMode("graph")}
                  aria-pressed={!inEmbedView}
                >
                  Graph
                </button>
                <button
                  className={`px-2 h-full text-[10px] uppercase tracking-wider ${
                    inEmbedView ? "bg-primary/15 text-primary" : "text-white/40 hover:text-white/70"
                  }`}
                  onClick={() => setViewMode("embed")}
                  aria-pressed={inEmbedView}
                  title="The ask lane's vectors, PCA-projected to 3-D. Distances are not real."
                >
                  Embeddings
                </button>
              </div>
              {highlightedIds && !inEmbedView && (
                <Button
                  size="sm"
                  onClick={() => {
                    setHighlightedIds(null);
                    setSelectedPath(null);
                    setSelectedNode(null);
                    setCameraTarget(null);
                  }}
                >
                  Clear selection
                </Button>
              )}
              <div className="flex items-center gap-1.5 h-8 px-2 rounded-md border border-border/50 bg-[#0b1920]/80 backdrop-blur-sm">
                <label
                  htmlFor="node-budget"
                  className="text-[10px] uppercase tracking-wider text-white/40"
                >
                  Nodes
                </label>
                <input
                  id="node-budget"
                  type="number"
                  min={GRAPH_NODE_BUDGET_STEP}
                  max={GRAPH_NODE_BUDGET_MAX}
                  step={GRAPH_NODE_BUDGET_STEP}
                  value={budgetDraft}
                  onChange={(e) => setBudgetDraft(e.target.value)}
                  onBlur={commitBudget}
                  onKeyDown={(e) => {
                    if (e.key === "Enter") {
                      e.currentTarget.blur();
                    }
                  }}
                  className="w-24 bg-transparent text-right text-xs font-mono text-cyan-200/90 outline-none [appearance:textfield] [&::-webkit-outer-spin-button]:appearance-none [&::-webkit-inner-spin-button]:appearance-none"
                  aria-label="Node budget: how many nodes to load"
                  title="How many nodes to load (5,000 steps, edges between loaded nodes follow automatically)"
                />
              </div>
              <DisplaySettingsMenu settings={display} onChange={updateDisplay} />
              <Button
                variant="outline"
                size="sm"
                onClick={() => {
                  setHighlightedIds(null);
                  setSelectedPath(null);
                  setSelectedNode(null);
                  setCameraTarget(null);
                  fetchOverview(project, budget.value);
                }}
              >
                Refresh
              </Button>
            </div>
          </>
        )}
      </div>

      {/* Right detail panel — resizable */}
      {selectedNode && sceneData && (
        <>
          <ResizeHandle
            side="right"
            onResize={(d) => {
              setRightWidth((w) => {
                const nw = Math.max(200, Math.min(500, w + d));
                saveWidth("hyp-right-w", nw);
                return nw;
              });
            }}
          />
          <div
            className="border-l border-border shrink-0 h-full overflow-hidden"
            style={{ width: rightWidth, maxHeight: "100%" }}
          >
            {missedSkeleton?.ids.has(selectedNode.id) ? (
              /* Skeleton node: the standard panel (code snippet, callers) is
               * meaningless for a not-fully-indexed file — show the coverage
               * callout with its report-the-edge-case actions instead. */
              <MissedCallout
                node={selectedNode}
                project={project}
                onClose={() => {
                  setSelectedNode(null);
                  setHighlightedIds(null);
                  setSelectedPath(null);
                }}
              />
            ) : (
              <NodeDetailPanel
                node={selectedNode}
                allNodes={sceneData.nodes}
                allEdges={sceneData.edges}
                project={project}
                repoInfo={repoInfo}
                onClose={() => {
                  setSelectedNode(null);
                  setHighlightedIds(null);
                  setSelectedPath(null);
                }}
                onNavigate={handleNavigateToNode}
              />
            )}
          </div>
        </>
      )}
    </div>
  );
}
