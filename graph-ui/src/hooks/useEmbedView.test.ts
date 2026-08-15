import { describe, expect, it } from "vitest";
import {
  buildEmbedScene,
  EMBED_NEAREST_EDGE_TYPE,
  EMBED_QUERY_NODE_ID,
  EMBED_SCENE_RADIUS,
  EMBED_VIEW_CAVEAT_FALLBACK,
} from "./useEmbedView";
import type { EmbedCloud, EmbedOverlay } from "../lib/types";

/* The shape /api/embed-view serves (see src/mcp/mcp.c hyp_mcp_ask_view_points_json). */
const cloud: EmbedCloud = {
  project: "p",
  lane: "local",
  view: {
    available: true,
    method: "pca3-v1",
    dim: 1024,
    rows: 3,
    variance_kept: 0.26,
    fitted_at: "2026-08-15T17:38:42Z",
    stale: false,
    deterministic: true,
  },
  points: [
    { id: 11, qn: "p.Writer.sortSections", label: "Function", file: "Writer.cpp", start_line: 1, end_line: 9, x: 0.6, y: 0.0, z: 0.0 },
    { id: 12, qn: "p.Writer.compareSections", label: "Function", file: "Writer.cpp", start_line: 20, end_line: 30, x: 0.0, y: 0.3, z: 0.0 },
    { id: 0, qn: "p.Config.Ctx", label: "Class", file: "Config.h", start_line: 5, end_line: 50, x: 0.0, y: 0.0, z: -0.3 },
  ],
  unprojected: 2,
  caveat: "Distances in this view are not real: ...",
};

/* The shape /api/embed-view/ask serves (hyp_mcp_ask_view_overlay). */
const overlay: EmbedOverlay = {
  project: "p",
  question: "how are sections ordered",
  lane: "local",
  ask: {
    available: true,
    model: "voyage-4-nano-Q8_0@75e62c7dba5e",
    cols: ["qn", "label", "file", "lines", "score"],
    rows: [
      ["p.Writer.sortSections", "Function", "Writer.cpp", "1-9", 0.66],
      ["p.Config.Ctx", "Class", "Config.h", "5-50", 0.58],
    ],
  },
  ask_error: false,
  view: { available: true, method: "pca3-v1", dim: 1024, rows: 3 },
  query: { x: 0.3, y: 0.1, z: 0.0 },
  hits: [
    { rank: 1, qualified_name: "p.Writer.sortSections", node_id: 11, label: "Function", file: "Writer.cpp", lines: "1-9", score: 0.66, projected: true, x: 0.6, y: 0, z: 0 },
    { rank: 2, qualified_name: "p.Config.Ctx", node_id: 0, label: "Class", file: "Config.h", lines: "5-50", score: 0.58, projected: true, x: 0, y: 0, z: -0.3 },
  ],
  caveat: "Distances in this view are not real: ...",
};

describe("buildEmbedScene", () => {
  it("returns null without an available view", () => {
    expect(buildEmbedScene(null, null)).toBeNull();
    expect(
      buildEmbedScene({ ...cloud, view: { available: false, reason: "not fitted" }, points: undefined }, null),
    ).toBeNull();
  });

  it("draws the cloud alone, scaled to the scene radius, with nothing highlighted", () => {
    const scene = buildEmbedScene(cloud, null);
    expect(scene).not.toBeNull();
    const s = scene!;
    expect(s.data.nodes).toHaveLength(3);
    expect(s.data.edges).toHaveLength(0);
    expect(s.highlightedIds).toBeNull();
    /* Widest point (0.6) lands on the scene radius; the rest scale with it. */
    expect(s.scale).toBeCloseTo(EMBED_SCENE_RADIUS / 0.6, 6);
    expect(s.data.nodes[0].x).toBeCloseTo(EMBED_SCENE_RADIUS, 6);
    expect(s.data.nodes[1].y).toBeCloseTo(0.3 * s.scale, 6);
    /* A row with no node id hint still gets a unique, negative, non-query id. */
    expect(s.data.nodes[2].id).toBeLessThan(0);
    expect(s.data.nodes[2].id).not.toBe(EMBED_QUERY_NODE_ID);
    expect(s.data.nodes[0].qualified_name).toBe("p.Writer.sortSections");
    expect(s.data.nodes[0].name).toBe("sortSections");
    expect(s.unprojected).toBe(2);
  });

  it("overlays the question and the tool's ranked rows, rank-labelled and joined by lines", () => {
    const s = buildEmbedScene(cloud, overlay)!;
    /* 3 declarations + 1 question marker. */
    expect(s.data.nodes).toHaveLength(4);
    const q = s.data.nodes.find((n) => n.id === EMBED_QUERY_NODE_ID)!;
    expect(q).toBeDefined();
    expect(q.x).toBeCloseTo(0.3 * s.scale, 6);
    expect(q.name).toContain("how are sections ordered");
    /* The hits carry their rank in the label the scene draws, in the tool's
     * order: #1 is the tool's first row, #2 its second. */
    const first = s.data.nodes.find((n) => n.qualified_name === "p.Writer.sortSections")!;
    const second = s.data.nodes.find((n) => n.qualified_name === "p.Config.Ctx")!;
    const other = s.data.nodes.find((n) => n.qualified_name === "p.Writer.compareSections")!;
    expect(first.name).toBe("#1 sortSections");
    expect(second.name).toBe("#2 Ctx");
    expect(other.name).toBe("compareSections");
    /* Highlighted: the question and both hits — and nothing else. */
    expect(s.highlightedIds).toEqual(new Set([EMBED_QUERY_NODE_ID, first.id, second.id]));
    /* One pointer per hit, from the question. */
    expect(s.data.edges).toHaveLength(2);
    for (const e of s.data.edges) {
      expect(e.source).toBe(EMBED_QUERY_NODE_ID);
      expect(e.type).toBe(EMBED_NEAREST_EDGE_TYPE);
    }
    expect(s.data.edges.map((e) => e.target)).toEqual([first.id, second.id]);
  });

  it("keeps the two ends in step: hit order is the tool's row order, never re-ranked", () => {
    const s = buildEmbedScene(cloud, overlay)!;
    const rowQns = overlay.ask!.rows!.map((r) => r[0]);
    const edgeQns = s.data.edges.map(
      (e) => s.data.nodes.find((n) => n.id === e.target)!.qualified_name,
    );
    expect(edgeQns).toEqual(rowQns);
    expect(overlay.hits!.map((h) => h.qualified_name)).toEqual(rowQns);
  });

  it("carries the caveat: the picture is not a measurement", () => {
    expect(EMBED_VIEW_CAVEAT_FALLBACK).toContain("not real");
    expect(cloud.caveat).toContain("not real");
  });
});
