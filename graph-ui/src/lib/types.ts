/* Graph data types matching the C layout3d.c JSON output */

export interface GraphNode {
  id: number;
  x: number;
  y: number;
  z: number;
  label: string;
  name: string;
  file_path?: string;
  qualified_name?: string;
  start_line?: number;
  end_line?: number;
  size: number;
  color: string;
  /* Dead-code classification from the backend layout (layout3d.c). */
  status?: NodeStatus;
  in_calls?: number;
}

export type NodeStatus =
  | "dead"
  | "single"
  | "entry"
  | "test"
  | "exported"
  | "normal"
  | "structural";

/* Git remote metadata for building GitHub deep-links (/api/repo-info). */
export interface RepoInfo {
  root_path: string;
  branch: string;
  remote_url: string;
  web_base: string; /* e.g. github.com/<org>/<repo> */
  blob_base: string; /* e.g. github.com/<org>/<repo>/blob/<branch> */
}

export interface GraphEdge {
  source: number;
  target: number;
  type: string;
}

export interface LinkedProject {
  project: string;
  nodes: GraphNode[];
  edges: GraphEdge[];
  offset: { x: number; y: number; z: number };
  cross_edges: GraphEdge[];
}

/* Missed-graph skeleton (#963): the file structure of files the indexer
 * could not fully cover, laid out as a satellite cluster beside the code
 * galaxy (server-computed offset, same shape as LinkedProject's). */
export interface MissedGraph {
  nodes: GraphNode[];
  edges: GraphEdge[];
  offset: { x: number; y: number; z: number };
}

export interface GraphData {
  nodes: GraphNode[];
  edges: GraphEdge[];
  total_nodes: number;
  linked_projects?: LinkedProject[];
  missed_graph?: MissedGraph;
}

export interface Project {
  name: string;
  root_path: string;
  indexed_at: string;
}

/* ── The `ask` lane's 3-D view (src/ask/ask_view.h, /api/embed-view) ──
 * A PCA projection of every embedded declaration, plus — after a question —
 * where the question landed and which declarations the `ask` tool ranked.
 * The coordinates are a PICTURE, not a metric: `caveat` says so and the view
 * must render it. */
export interface EmbedViewMeta {
  available: boolean;
  method?: string; /* "pca3-v1" */
  dim?: number;
  rows?: number;
  variance_kept?: number; /* fraction of total variance the 3 axes hold */
  eigen?: number[];
  fitted_at?: string;
  stale?: boolean; /* the index was re-sealed since the fit */
  deterministic?: boolean;
  refit?: string;
  reason?: string; /* when !available */
  remedy?: string;
  model?: string;
}

export interface EmbedPoint {
  id: number; /* node_id hint from the vector row */
  qn: string;
  label: string;
  file: string;
  start_line: number;
  end_line: number;
  x: number;
  y: number;
  z: number;
}

export interface EmbedCloud {
  project: string;
  lane: string;
  view: EmbedViewMeta;
  points?: EmbedPoint[];
  unprojected?: number;
  caveat: string;
}

export interface EmbedHit {
  rank: number;
  qualified_name: string;
  node_id: number;
  label: string;
  file: string;
  lines: string;
  score: number;
  projected: boolean;
  x?: number;
  y?: number;
  z?: number;
}

export interface EmbedOverlay {
  project: string;
  question: string;
  lane: string;
  /* The `ask` tool's own JSON result, verbatim (rows: [qn,label,file,lines,score,...]). */
  ask?: {
    available: boolean;
    model?: string;
    language?: string;
    population?: number;
    truncation?: string;
    cols?: string[];
    rows?: (string | number | boolean)[][];
    reason?: string;
    [k: string]: unknown;
  };
  ask_text?: string; /* the tool's prose when it did not render JSON */
  ask_error: boolean;
  view: EmbedViewMeta;
  query?: { x: number; y: number; z: number };
  hits?: EmbedHit[];
  caveat: string;
}

export interface SchemaInfo {
  node_labels: { label: string; count: number }[];
  edge_types: { type: string; count: number }[];
  total_nodes: number;
  total_edges: number;
}

export type TabId = "graph" | "stats" | "control";

export interface ProcessInfo {
  pid: number;
  cpu: number;
  rss_mb: number;
  elapsed: string;
  command: string;
  is_self: boolean;
}
