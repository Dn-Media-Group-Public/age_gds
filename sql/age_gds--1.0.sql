-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION age_gds" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS age_gds;

-- Returns extension version string; proves C++ entry point reachable
CREATE FUNCTION age_gds.version() RETURNS text
    AS 'age_gds', 'age_gds_version'
    LANGUAGE C STRICT;

-- -------------------------------------------------------------------------
-- Graph projection catalog
-- -------------------------------------------------------------------------

CREATE TABLE age_gds.graphs (
    name             text        PRIMARY KEY,
    age_graph        text        NOT NULL DEFAULT '',
    node_query       text,
    edge_query       text,
    node_count       bigint,
    edge_count       bigint,
    directed         boolean     NOT NULL DEFAULT false,
    weighted         boolean     NOT NULL DEFAULT false,
    file_path        text,                          -- snapshot .bin file (NULL for live)
    created_at       timestamptz NOT NULL DEFAULT now(),
    projected_by     text,
    live             boolean     NOT NULL DEFAULT false,
    nodes_file_path  text,                          -- live index .nodes mmap file
    edges_file_path  text,                          -- live index .edges mmap file
    incremental      boolean     NOT NULL DEFAULT false
                                    -- true  => nodes_file_path/edges_file_path slot/tombstone storage
                                    -- false => file_path flat .bin storage
                                    -- orthogonal to `live` (live implies incremental, not vice versa)
);

-- -------------------------------------------------------------------------
-- Live index trigger catalog
-- -------------------------------------------------------------------------

CREATE TABLE age_gds.index_triggers (
    graph_name   text  NOT NULL REFERENCES age_gds.graphs(name) ON DELETE CASCADE,
    trigger_name text  NOT NULL,
    table_schema text  NOT NULL,
    table_name   text  NOT NULL,
    PRIMARY KEY (graph_name, trigger_name)
);

-- -------------------------------------------------------------------------
-- Snapshot projection functions
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.graph_project(
    name        text,
    age_graph   text,
    node_query  text,
    edge_query  text,
    directed    boolean DEFAULT false,
    weighted    boolean DEFAULT false,
    incremental boolean DEFAULT false
) RETURNS void
    AS 'age_gds', 'age_gds_graph_project'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Live index functions
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.graph_index(
    name        text,
    age_graph   text,
    directed    boolean DEFAULT false,
    weighted    boolean DEFAULT false
) RETURNS void
    AS 'age_gds', 'age_gds_graph_index'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Shared catalog management
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.graph_drop(name text)
    RETURNS void
    AS 'age_gds', 'age_gds_graph_drop'
    LANGUAGE C STRICT;

CREATE FUNCTION age_gds.graph_list()
    RETURNS TABLE(
        name          text,
        age_graph     text,
        node_count    bigint,
        edge_count    bigint,
        directed      boolean,
        weighted      boolean,
        file_path     text,
        created_at    timestamptz,
        "memoryUsage" bigint,
        live          boolean,
        incremental   boolean
    )
    AS 'age_gds', 'age_gds_graph_list'
    LANGUAGE C;

CREATE FUNCTION age_gds.graph_exists(name text)
    RETURNS boolean
    AS 'age_gds', 'age_gds_graph_exists'
    LANGUAGE C STRICT;

CREATE FUNCTION age_gds.graph_reindex(
    name  text,
    force boolean DEFAULT NULL
) RETURNS void
    AS 'age_gds', 'age_gds_graph_reindex'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Session cache
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.cache_stats()
    RETURNS TABLE(entry_count int, total_bytes bigint, limit_bytes bigint)
    AS 'age_gds', 'age_gds_cache_stats'
    LANGUAGE C;

CREATE FUNCTION age_gds.cache_load(graph_name text)
    RETURNS void
    AS 'age_gds', 'age_gds_cache_load'
    LANGUAGE C STRICT;

-- -------------------------------------------------------------------------
-- Internal trigger functions (live index maintenance)
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.index_vertex_trigger()
    RETURNS trigger
    AS 'age_gds', 'age_gds_index_vertex_trigger'
    LANGUAGE C;

CREATE FUNCTION age_gds.index_edge_trigger()
    RETURNS trigger
    AS 'age_gds', 'age_gds_index_edge_trigger'
    LANGUAGE C;

CREATE FUNCTION age_gds.on_new_label()
    RETURNS trigger
    AS 'age_gds', 'age_gds_on_new_label'
    LANGUAGE C;

-- Watch for new AGE labels so live indices auto-extend
CREATE TRIGGER age_gds_label_watch
    AFTER INSERT ON ag_catalog.ag_label
    FOR EACH ROW
    EXECUTE FUNCTION age_gds.on_new_label();

-- -------------------------------------------------------------------------
-- Centrality algorithms
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.pageRank_stream(
    graph_name                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    config                        jsonb  DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_pagerank_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.pageRank_stats(
    graph_name                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    config                        jsonb  DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("ranIterations" int, "didConverge" bool, "computeMillis" bigint,
                "minScore" float8, "maxScore" float8, "meanScore" float8)
    AS 'age_gds', 'age_gds_pagerank_stats'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Additional centrality algorithms
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.betweenness_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    sampling_size                 int    DEFAULT NULL,
    sampling_seed                 int    DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_betweenness_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.closeness_stream(
    graph_name          text    DEFAULT NULL,
    age_graph           text    DEFAULT NULL,
    node_query          text    DEFAULT NULL,
    edge_query          text    DEFAULT NULL,
    use_wasserman_faust boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_closeness_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.harmonicCentrality_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_harmoniccentrality_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.degree_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    orientation                   text   DEFAULT 'NATURAL',
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_degree_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.eigenvector_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_eigenvector_stream'
    LANGUAGE C;

-- HITS: NetworKit has no HITS class; EigenvectorCentrality used for both
-- hub and auth scores (equivalent for undirected graphs).
CREATE FUNCTION age_gds.hits_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, auth float8, hub float8)
    AS 'age_gds', 'age_gds_hits_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.articleRank_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    damping_factor                float8 DEFAULT NULL,
    tolerance                     float8 DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    scaler                        text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_articlerank_stream'
    LANGUAGE C;

-- articulationPoints: uses BiconnectedComponents (no dedicated class in
-- NetworKit); resulting_component_* columns computed via ConnectedComponents
-- on a copy of the graph with the articulation point removed.
CREATE FUNCTION age_gds.articulationPoints_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE(
    "nodeId"                  bigint,
    "resultingComponentCount" int,
    "resultingComponentMin"   int,
    "resultingComponentMax"   int
)
    AS 'age_gds', 'age_gds_articulationpoints_stream'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Community detection algorithms
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.louvain_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    gamma                         float8 DEFAULT 1.0,
    max_iterations                int    DEFAULT 10,
    recurse                       boolean DEFAULT true,
    consecutive_ids               boolean DEFAULT false,
    min_community_size            int    DEFAULT 0
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
    AS 'age_gds', 'age_gds_louvain_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.leiden_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    gamma                         float8 DEFAULT 1.0,
    iterations                    int    DEFAULT 10
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
    AS 'age_gds', 'age_gds_leiden_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.labelPropagation_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    max_iterations                int    DEFAULT NULL,
    seed_property                 text   DEFAULT NULL,
    consecutive_ids               boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, "communityId" bigint)
    AS 'age_gds', 'age_gds_labelpropagation_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.wcc_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    threshold                     float8 DEFAULT NULL,
    consecutive_ids               boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, "componentId" bigint)
    AS 'age_gds', 'age_gds_wcc_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.scc_stream(
    graph_name       text   DEFAULT NULL,
    age_graph        text   DEFAULT NULL,
    node_query       text   DEFAULT NULL,
    edge_query       text   DEFAULT NULL,
    consecutive_ids  boolean DEFAULT false
) RETURNS TABLE("nodeId" bigint, "componentId" bigint)
    AS 'age_gds', 'age_gds_scc_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.kCore_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "coreNumber" bigint)
    AS 'age_gds', 'age_gds_kcore_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.triangleCount_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL,
    max_degree bigint DEFAULT 9223372036854775807
) RETURNS TABLE("nodeId" bigint, "triangleCount" bigint)
    AS 'age_gds', 'age_gds_trianglecount_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.localClusteringCoefficient_stream(
    graph_name              text   DEFAULT NULL,
    age_graph               text   DEFAULT NULL,
    node_query              text   DEFAULT NULL,
    edge_query              text   DEFAULT NULL,
    triangle_count_property text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, coefficient float8)
    AS 'age_gds', 'age_gds_localclusteringcoefficient_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.modularity_stats(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    community_property             text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE(modularity float8)
    AS 'age_gds', 'age_gds_modularity_stats'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Path-finding algorithms
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.bfs_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_bfs_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.shortestPath_dijkstra_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_shortestpath_dijkstra_stream'
    LANGUAGE C;

-- deltaStepping: NetworKit has no DeltaStepping class; Dijkstra used as
-- equivalent for non-negative weight graphs.
CREATE FUNCTION age_gds.deltaStepping_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_deltastepping_stream'
    LANGUAGE C;

-- bellmanFord: NetworKit has no standalone BellmanFord; Dijkstra fallback.
CREATE FUNCTION age_gds.bellmanFord_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_bellmanford_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.dfs_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, depth int)
    AS 'age_gds', 'age_gds_dfs_stream'
    LANGUAGE C;

-- randomWalk: NetworKit has no RandomWalk class; implemented using
-- Aux::Random::choice + NetworKit::Graph::forNeighborsOf.
CREATE FUNCTION age_gds.randomWalk_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    walk_length int    DEFAULT 10,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint)
    AS 'age_gds', 'age_gds_randomwalk_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.spanningTree_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE(src bigint, dst bigint, cost float8)
    AS 'age_gds', 'age_gds_spanningtree_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.allShortestPaths_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_allshortestpaths_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.maxFlow_stats(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    target_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE(flow float8, "computeMillis" bigint)
    AS 'age_gds', 'age_gds_maxflow_stats'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Write-back functions (stage 8)
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.pageRank_write(
    graph_name      text    DEFAULT NULL,
    write_property  text    DEFAULT 'pagerank',
    damping_factor  float8  DEFAULT NULL,
    max_iterations  int     DEFAULT NULL,
    tolerance       float8  DEFAULT NULL,
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS void
    AS 'age_gds', 'age_gds_pagerank_write'
    LANGUAGE C;

CREATE FUNCTION age_gds.betweenness_write(
    graph_name      text    DEFAULT NULL,
    write_property  text    DEFAULT 'betweenness',
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS void
    AS 'age_gds', 'age_gds_betweenness_write'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Node similarity and KNN (stage 8)
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.nodeSimilarity_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, similarity float8)
    AS 'age_gds', 'age_gds_nodesimilarity_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.knn_stream(
    graph_name text DEFAULT NULL,
    top_k      int  DEFAULT 5,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, similarity float8, rank int)
    AS 'age_gds', 'age_gds_knn_stream'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Link prediction (stage 8)
-- Uses NetworKit link predictors; runAll() scores unconnected node pairs.
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.linkprediction_adamicAdar_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_adamicadar_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_commonNeighbors_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_commonneighbors_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_preferentialAttachment_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_preferentialattachment_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_resourceAllocation_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_resourceallocation_stream'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Node embeddings (stage 8)
-- NetworKit::Node2Vec: biased random-walk embeddings.
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.node2vec_stream(
    graph_name      text    DEFAULT NULL,
    embedding_dim   int     DEFAULT 64,
    walk_length     int     DEFAULT 80,
    walks_per_node  int     DEFAULT 10,
    p               float8  DEFAULT 1.0,
    q               float8  DEFAULT 1.0,
    age_graph       text    DEFAULT NULL,
    node_query      text    DEFAULT NULL,
    edge_query      text    DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, embedding float8[])
    AS 'age_gds', 'age_gds_node2vec_stream'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Staleness helper (stage 8)
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.graph_is_stale(name text)
    RETURNS boolean
    AS 'age_gds', 'age_gds_graph_is_stale'
    LANGUAGE C STRICT;
