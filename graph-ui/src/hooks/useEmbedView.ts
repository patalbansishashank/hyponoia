import { useCallback, useState } from "react";
import type {
  EmbedCloud,
  EmbedOverlay,
  GraphData,
  GraphEdge,
  GraphNode,
} from "../lib/types";
import { colorForLabel } from "../lib/colors";

/* ── The `ask` lane's 3-D view ─────────────────────────────────────
 *
 * Two fetches: the cloud (every projected declaration) and, per question,
 * the overlay — the `ask` tool's own ranked rows placed in the same 3-space
 * with the question. `buildEmbedScene` turns them into the GraphData the
 * existing scene already knows how to draw, so the view is the same renderer,
 * the same click handling and the same detail panel — nothing new to render.
 *
 * What the picture is NOT: a measurement. The projection keeps three axes of
 * variance and discards the rest; the ranking used cosine in the full space.
 * The server says so in `caveat` and the tab renders it ON the view. */

export const EMBED_VIEW_CAVEAT_FALLBACK =
  "Distances in this view are not real: the projection keeps three of the vector's " +
  "dimensions' worth of variance and discards the rest, and the ranking was computed by " +
  "cosine in the full space, never here. Nearness in the picture is a hint about the " +
  "neighbourhood, not a measurement of it.";

/* Synthetic id for the question marker. Negative so it can never collide with
 * a node id, which the graph mints from 1. */
export const EMBED_QUERY_NODE_ID = -1;
export const EMBED_QUERY_LABEL = "Question";
export const EMBED_NEAREST_EDGE_TYPE = "NEAREST";

/* The stored coordinates are in the vector's own units (a unit-vector cloud
 * spans roughly ±0.7 on its widest axis). The scene's camera, node sizes and
 * label heights are tuned for the force layout's scale of hundreds, so the
 * cloud is scaled to that radius. A uniform scale changes nothing about the
 * picture — and nothing about the picture was a measurement to begin with. */
export const EMBED_SCENE_RADIUS = 600;

export async function fetchEmbedCloud(project: string): Promise<EmbedCloud> {
  const params = new URLSearchParams({ project });
  const res = await fetch(`/api/embed-view?${params}`);
  if (!res.ok) {
    const body = await res.json().catch(() => ({ error: res.statusText }));
    throw new Error(body.error ?? `HTTP ${res.status}`);
  }
  return res.json();
}

export async function askEmbedView(
  project: string,
  question: string,
  limit = 10,
): Promise<EmbedOverlay> {
  const res = await fetch("/api/embed-view/ask", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ project, question, limit }),
  });
  if (!res.ok) {
    const body = await res.json().catch(() => ({ error: res.statusText }));
    throw new Error(body.error ?? `HTTP ${res.status}`);
  }
  return res.json();
}

export interface EmbedScene {
  data: GraphData;
  /* The question marker plus every ranked hit — what the scene highlights. */
  highlightedIds: Set<number> | null;
  /* Uniform scale applied to every coordinate (for the HUD to disclose). */
  scale: number;
  /* Rows in the index that have no coordinates yet (not drawn). */
  unprojected: number;
}

/* Node id for a cloud point. The vector row carries the graph's node id as a
 * hint; a row written without one (0) gets a synthetic negative id below the
 * query marker's, so it is still clickable and never collides. */
function pointId(id: number, index: number): number {
  return id > 0 ? id : -(index + 2);
}

/* Pure: cloud (+ overlay) → the GraphData the scene draws. */
export function buildEmbedScene(
  cloud: EmbedCloud | null,
  overlay: EmbedOverlay | null,
): EmbedScene | null {
  if (!cloud || !cloud.view.available || !cloud.points) return null;

  /* Scale to the scene's radius from the cloud's own extent. */
  let maxR = 0;
  for (const p of cloud.points) {
    const r = Math.sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (r > maxR) maxR = r;
  }
  const scale = maxR > 0 ? EMBED_SCENE_RADIUS / maxR : 1;

  const hitByQn = new Map<string, number>();
  if (overlay?.hits) {
    for (const h of overlay.hits) hitByQn.set(h.qualified_name, h.rank);
  }

  const nodes: GraphNode[] = cloud.points.map((p, i) => {
    const rank = hitByQn.get(p.qn);
    const leaf = p.qn.includes(".") ? p.qn.slice(p.qn.lastIndexOf(".") + 1) : p.qn;
    return {
      id: pointId(p.id, i),
      x: p.x * scale,
      y: p.y * scale,
      z: p.z * scale,
      label: p.label || "Declaration",
      /* Rank labels ride on the name so the existing label sprites show them. */
      name: rank !== undefined ? `#${rank} ${leaf}` : leaf,
      file_path: p.file,
      qualified_name: p.qn,
      start_line: p.start_line,
      end_line: p.end_line,
      size: rank !== undefined ? 14 : 3,
      color: colorForLabel(p.label),
    };
  });

  const edges: GraphEdge[] = [];
  const highlighted = new Set<number>();
  if (overlay?.query && overlay.hits) {
    /* The QUESTION: one bright marker at where the query vector landed. */
    const q = overlay.query;
    nodes.push({
      id: EMBED_QUERY_NODE_ID,
      x: q.x * scale,
      y: q.y * scale,
      z: q.z * scale,
      label: EMBED_QUERY_LABEL,
      name: `Q: ${overlay.question}`,
      size: 18,
      color: "#ffffff",
    });
    highlighted.add(EMBED_QUERY_NODE_ID);
    /* The RANKED ANSWER: the tool's own rows, joined to the cloud by qualified
     * name, each with a line back to the question. A hit the cloud does not
     * carry (unprojected) is listed in the HUD but cannot be drawn. */
    const idByQn = new Map<string, number>();
    for (const n of nodes) if (n.qualified_name) idByQn.set(n.qualified_name, n.id);
    for (const h of overlay.hits) {
      const id = idByQn.get(h.qualified_name);
      if (id === undefined) continue;
      highlighted.add(id);
      edges.push({ source: EMBED_QUERY_NODE_ID, target: id, type: EMBED_NEAREST_EDGE_TYPE });
    }
  }

  return {
    data: { nodes, edges, total_nodes: nodes.length },
    highlightedIds: highlighted.size > 0 ? highlighted : null,
    scale,
    unprojected: cloud.unprojected ?? 0,
  };
}

interface UseEmbedViewResult {
  cloud: EmbedCloud | null;
  overlay: EmbedOverlay | null;
  loading: boolean;
  asking: boolean;
  error: string | null;
  loadCloud: (project: string) => void;
  ask: (project: string, question: string, limit?: number) => void;
  clear: () => void;
}

export function useEmbedView(): UseEmbedViewResult {
  const [cloud, setCloud] = useState<EmbedCloud | null>(null);
  const [overlay, setOverlay] = useState<EmbedOverlay | null>(null);
  const [loading, setLoading] = useState(false);
  const [asking, setAsking] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const loadCloud = useCallback(async (project: string) => {
    setLoading(true);
    setError(null);
    setOverlay(null);
    try {
      setCloud(await fetchEmbedCloud(project));
    } catch (e) {
      setCloud(null);
      setError(e instanceof Error ? e.message : "Failed to fetch the embedding view");
    } finally {
      setLoading(false);
    }
  }, []);

  const ask = useCallback(async (project: string, question: string, limit = 10) => {
    setAsking(true);
    setError(null);
    try {
      setOverlay(await askEmbedView(project, question, limit));
    } catch (e) {
      setOverlay(null);
      setError(e instanceof Error ? e.message : "ask failed");
    } finally {
      setAsking(false);
    }
  }, []);

  const clear = useCallback(() => {
    setCloud(null);
    setOverlay(null);
    setError(null);
  }, []);

  return { cloud, overlay, loading, asking, error, loadCloud, ask, clear };
}
