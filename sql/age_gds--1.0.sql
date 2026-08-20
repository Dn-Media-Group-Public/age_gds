-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION age_gds" to load this file. \quit

-- Ensure age.so is resident before CREATE FUNCTION validates age_gds.so
-- symbols (age_gds.so depends on agtype_in/agtype_out from age.so at
-- dlopen time for the Cypher-callable (agtype, agtype) overloads below).
LOAD '$libdir/age';

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

-- Cypher-callable overload: matches Neo4j's real
-- gds.graph.project(graphName, nodeProjection, relationshipProjection,
-- configuration) shape (not the (graphName, config) 2-arg convention used
-- elsewhere -- there is no graph to select here, graphName IS the new
-- projection's name). nodeProjection/relationshipProjection: '*' or a map
-- of label/type -> {} ('properties' unsupported, errors naming it).
-- config = {ageGraph (required), directed, weighted, incremental}.
CREATE FUNCTION age_gds.graph_project(
    "graphName"              ag_catalog.agtype,
    "nodeProjection"         ag_catalog.agtype,
    "relationshipProjection" ag_catalog.agtype,
    config                   ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_graph_project_cypher'
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

-- Cypher-callable overload: single map {graphName}.
CREATE FUNCTION age_gds.graph_drop("graphName" ag_catalog.agtype)
    RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_graph_drop_cypher'
    LANGUAGE C;

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

-- Cypher-callable overload: no arguments (no graph to select). Array of
-- maps, one per catalog row.
CREATE FUNCTION age_gds.graph_list_cypher()
    RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_graph_list_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.graph_exists(name text)
    RETURNS boolean
    AS 'age_gds', 'age_gds_graph_exists'
    LANGUAGE C STRICT;

-- Cypher-callable overload: bare scalar agtype boolean.
CREATE FUNCTION age_gds.graph_exists("graphName" ag_catalog.agtype)
    RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_graph_exists_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.graph_reindex(
    name  text,
    force boolean DEFAULT NULL
) RETURNS void
    AS 'age_gds', 'age_gds_graph_reindex'
    LANGUAGE C;

-- Cypher-callable overload: config = {force}.
CREATE FUNCTION age_gds.graph_reindex(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_graph_reindex_cypher'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Session cache
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.cache_stats()
    RETURNS TABLE(entry_count int, total_bytes bigint, limit_bytes bigint)
    AS 'age_gds', 'age_gds_cache_stats'
    LANGUAGE C;

-- Cypher-callable overload: no arguments. Single map
-- {entryCount, totalBytes, limitBytes}.
CREATE FUNCTION age_gds.cache_stats_cypher()
    RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_cache_stats_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.cache_load(graph_name text)
    RETURNS void
    AS 'age_gds', 'age_gds_cache_load'
    LANGUAGE C STRICT;

-- Cypher-callable overload: single map {graphName}.
CREATE FUNCTION age_gds.cache_load("graphName" ag_catalog.agtype)
    RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_cache_load_cypher'
    LANGUAGE C;

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

-- Cypher-callable overload: config = {dampingFactor, maxIterations,
-- tolerance, relationshipWeightProperty, scaler}.
CREATE FUNCTION age_gds.pageRank_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_pagerank_stream_cypher'
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

-- Cypher-callable overload: same config keys as pageRank_stream.
CREATE FUNCTION age_gds.pageRank_stats(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_pagerank_stats_cypher'
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

-- Cypher-callable overload: config = {samplingSize, samplingSeed,
-- relationshipWeightProperty}.
CREATE FUNCTION age_gds.betweenness_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_betweenness_stream_cypher'
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

-- Cypher-callable overload: config = {useWassermanFaust}.
CREATE FUNCTION age_gds.closeness_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_closeness_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.harmonicCentrality_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, score float8)
    AS 'age_gds', 'age_gds_harmoniccentrality_stream'
    LANGUAGE C;

-- Cypher-callable overload: no per-algorithm config keys.
CREATE FUNCTION age_gds.harmonicCentrality_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_harmoniccentrality_stream_cypher'
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

-- Cypher-callable overload: config = {orientation, relationshipWeightProperty}.
CREATE FUNCTION age_gds.degree_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_degree_stream_cypher'
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

-- Cypher-callable overload: config = {tolerance, relationshipWeightProperty, scaler}.
CREATE FUNCTION age_gds.eigenvector_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_eigenvector_stream_cypher'
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

-- Cypher-callable overload: no per-algorithm config keys.
CREATE FUNCTION age_gds.hits_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_hits_stream_cypher'
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

-- Cypher-callable overload: config = {dampingFactor, tolerance,
-- relationshipWeightProperty, scaler}.
CREATE FUNCTION age_gds.articleRank_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_articlerank_stream_cypher'
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

-- Cypher-callable overload: no per-algorithm config keys.
CREATE FUNCTION age_gds.articulationPoints_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_articulationpoints_stream_cypher'
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

-- Cypher-callable overload: config = {relationshipWeightProperty, gamma,
-- maxIterations, recurse, consecutiveIds, minCommunitySize}.
CREATE FUNCTION age_gds.louvain_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_louvain_stream_cypher'
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

-- Cypher-callable overload: config = {relationshipWeightProperty, gamma,
-- iterations}.
CREATE FUNCTION age_gds.leiden_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_leiden_stream_cypher'
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

-- Cypher-callable overload: config = {relationshipWeightProperty,
-- maxIterations, seedProperty, consecutiveIds}. seedProperty is refused
-- (same NetworKit::PLP gap as the SQL entry point).
CREATE FUNCTION age_gds.labelPropagation_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_labelpropagation_stream_cypher'
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

-- Cypher-callable overload: config = {relationshipWeightProperty,
-- threshold, consecutiveIds}.
CREATE FUNCTION age_gds.wcc_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_wcc_stream_cypher'
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

-- Cypher-callable overload: config = {consecutiveIds}.
CREATE FUNCTION age_gds.scc_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_scc_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.kCore_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "coreNumber" bigint)
    AS 'age_gds', 'age_gds_kcore_stream'
    LANGUAGE C;

-- Cypher-callable overload: no per-algorithm config keys.
CREATE FUNCTION age_gds.kCore_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_kcore_stream_cypher'
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

-- Cypher-callable overload: config = {maxDegree}.
CREATE FUNCTION age_gds.triangleCount_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_trianglecount_stream_cypher'
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

-- Cypher-callable overload: config = {triangleCountProperty}.
CREATE FUNCTION age_gds.localClusteringCoefficient_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_localclusteringcoefficient_stream_cypher'
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

-- Cypher-callable overload: config = {communityProperty, relationshipWeightProperty}.
CREATE FUNCTION age_gds.modularity_stats(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_modularity_stats_cypher'
    LANGUAGE C;

-- gds.conductance: NetworKit::Conductance::getQuality(), which requires a
-- literal 2-partition (community_property values must be exactly 0 or 1
-- for every node; any other value errors rather than being silently
-- coerced -- see src/algorithms/community.cpp for why plan 02's
-- read_node_property_as_partition() isn't reused directly here).
CREATE FUNCTION age_gds.conductance_stats(
    graph_name                    text   DEFAULT NULL,
    community_property            text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL
) RETURNS TABLE(conductance float8)
    AS 'age_gds', 'age_gds_conductance_stats'
    LANGUAGE C;

-- Cypher-callable overload: config = {communityProperty (required),
-- relationshipWeightProperty}.
CREATE FUNCTION age_gds.conductance_stats(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_conductance_stats_cypher'
    LANGUAGE C;

-- gds.cliqueCounting: NetworKit::MaximalCliques(G, callback), counted and
-- bucketed by clique size as cliques are emitted by the callback. Counts
-- *maximal* cliques only (NetworKit has no all-cliques-of-every-size
-- primitive); SUM(count) over the result rows gives the total maximal
-- clique count.
CREATE FUNCTION age_gds.cliqueCounting_stats(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE(clique_size int, count bigint)
    AS 'age_gds', 'age_gds_cliquecounting_stats'
    LANGUAGE C;

-- Cypher-callable overload: no per-algorithm config keys. Array of
-- {cliqueSize, count} maps (see src/algorithms/community.cpp for why this
-- follows the array-of-maps shape despite the "_stats" name).
CREATE FUNCTION age_gds.cliqueCounting_stats(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_cliquecounting_stats_cypher'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Path-finding algorithms
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.bfs_stream(
    graph_name   text     DEFAULT NULL,
    source_node  bigint   DEFAULT NULL,
    age_graph    text     DEFAULT NULL,
    node_query   text     DEFAULT NULL,
    edge_query   text     DEFAULT NULL,
    max_depth    integer  DEFAULT -1,
    target_nodes bigint[] DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_bfs_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode, maxDepth, targetNodes}
-- (plus ageGraph/nodeQuery/edgeQuery for anonymous mode). Returns a
-- one-element array: {sourceNode, nodeIds}. See docs/api.md for why there
-- is no `path` field.
CREATE FUNCTION age_gds.bfs_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_bfs_stream_cypher'
    LANGUAGE C;

-- BREAKING CHANGE (docs/implemented-plans/03-pathfinding-parity.md): this
-- used to be single-source-to-all (nodeId, distance) rows. It now matches
-- Neo4j's actual split -- single source/target pair, path output. See
-- age_gds.allShortestPaths_dijkstra_stream for the single-source-to-all
-- equivalent.
CREATE FUNCTION age_gds.shortestPath_dijkstra_stream(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    target_node                   bigint DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE(total_cost float8, node_ids bigint[], costs float8[])
    AS 'age_gds', 'age_gds_shortestpath_dijkstra_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode, targetNode,
-- relationshipWeightProperty}. One-element array: {totalCost, nodeIds, costs}.
CREATE FUNCTION age_gds.shortestPath_dijkstra_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_shortestpath_dijkstra_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.allShortestPaths_dijkstra_stream(
    graph_name                    text     DEFAULT NULL,
    source_node                   bigint   DEFAULT NULL,
    age_graph                     text     DEFAULT NULL,
    node_query                    text     DEFAULT NULL,
    edge_query                    text     DEFAULT NULL,
    relationship_weight_property  text     DEFAULT NULL,
    target_nodes                  bigint[] DEFAULT NULL
) RETURNS TABLE("targetNodeId" bigint, total_cost float8, node_ids bigint[], costs float8[])
    AS 'age_gds', 'age_gds_allshortestpaths_dijkstra_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode, relationshipWeightProperty,
-- targetNodes}. Array of maps: {targetNodeId, totalCost, nodeIds, costs}.
CREATE FUNCTION age_gds.allShortestPaths_dijkstra_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_allshortestpaths_dijkstra_stream_cypher'
    LANGUAGE C;

-- deltaStepping: NetworKit has no DeltaStepping class; Dijkstra used as
-- equivalent for non-negative weight graphs. Excluded from plan 03 (no
-- delta parameter to add -- tracked in docs/neo4j-gds-parity-external-libs.md).
CREATE FUNCTION age_gds.deltaStepping_stream(
    graph_name  text   DEFAULT NULL,
    source_node bigint DEFAULT NULL,
    age_graph   text   DEFAULT NULL,
    node_query  text   DEFAULT NULL,
    edge_query  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_deltastepping_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode}.
CREATE FUNCTION age_gds.deltaStepping_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_deltastepping_stream_cypher'
    LANGUAGE C;

-- bellmanFord: now backed by NetworKit::AlgebraicBellmanFord<CSRMatrix>
-- (real Bellman-Ford, supports negative edge weights and reports
-- negative-cycle presence) instead of the previous Dijkstra substitution.
CREATE FUNCTION age_gds.bellmanFord_stream(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8, "isNegativeCycle" boolean)
    AS 'age_gds', 'age_gds_bellmanford_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode, relationshipWeightProperty}.
CREATE FUNCTION age_gds.bellmanFord_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_bellmanford_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.bellmanFord_stats(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("containsNegativeCycle" boolean, "computeMillis" bigint)
    AS 'age_gds', 'age_gds_bellmanford_stats'
    LANGUAGE C;

CREATE FUNCTION age_gds.bellmanFord_stats(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_bellmanford_stats_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.dfs_stream(
    graph_name   text     DEFAULT NULL,
    source_node  bigint   DEFAULT NULL,
    age_graph    text     DEFAULT NULL,
    node_query   text     DEFAULT NULL,
    edge_query   text     DEFAULT NULL,
    max_depth    integer  DEFAULT -1,
    target_nodes bigint[] DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, depth int)
    AS 'age_gds', 'age_gds_dfs_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode, maxDepth, targetNodes}.
CREATE FUNCTION age_gds.dfs_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_dfs_stream_cypher'
    LANGUAGE C;

-- randomWalk: NetworKit has no RandomWalk class; delegates to
-- NetworKit::Embedding::BiasedRandomWalk (the same P/Q-biased walker
-- Node2Vec uses internally) instead of the previous uniform
-- Aux::Random::choice implementation.
--
-- BREAKING CHANGE (docs/implemented-plans/03-pathfinding-parity.md):
-- source_node -> source_nodes bigint[] (NULL = every node, matching
-- Neo4j's default); walk_length default 10 -> 80 (matching Neo4j).
CREATE FUNCTION age_gds.randomWalk_stream(
    graph_name                    text     DEFAULT NULL,
    source_nodes                   bigint[] DEFAULT NULL,
    walk_length                   int      DEFAULT 80,
    age_graph                     text     DEFAULT NULL,
    node_query                    text     DEFAULT NULL,
    edge_query                    text     DEFAULT NULL,
    walks_per_node                int      DEFAULT 10,
    in_out_factor                 float8   DEFAULT 1.0,
    return_factor                 float8   DEFAULT 1.0,
    relationship_weight_property  text     DEFAULT NULL
) RETURNS TABLE(walk_id bigint, "nodeId" bigint)
    AS 'age_gds', 'age_gds_randomwalk_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNodes, walkLength, walksPerNode,
-- inOutFactor, returnFactor, relationshipWeightProperty}.
CREATE FUNCTION age_gds.randomWalk_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_randomwalk_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.spanningTree_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    objective                     text   DEFAULT 'minimum',
    relationship_weight_property  text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL
) RETURNS TABLE(src bigint, dst bigint, cost float8)
    AS 'age_gds', 'age_gds_spanningtree_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {objective, relationshipWeightProperty,
-- sourceNode}.
CREATE FUNCTION age_gds.spanningTree_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_spanningtree_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.allShortestPaths_stream(
    graph_name                    text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_allshortestpaths_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {relationshipWeightProperty}.
CREATE FUNCTION age_gds.allShortestPaths_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_allshortestpaths_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.maxFlow_stats(
    graph_name              text     DEFAULT NULL,
    source_node             bigint   DEFAULT NULL,
    target_node             bigint   DEFAULT NULL,
    age_graph               text     DEFAULT NULL,
    node_query              text     DEFAULT NULL,
    edge_query              text     DEFAULT NULL,
    capacity_property       text     DEFAULT NULL,
    source_nodes            bigint[] DEFAULT NULL,
    target_nodes            bigint[] DEFAULT NULL,
    node_capacity_property  text     DEFAULT NULL
) RETURNS TABLE(flow float8, "computeMillis" bigint)
    AS 'age_gds', 'age_gds_maxflow_stats'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode, targetNode,
-- capacityProperty, sourceNodes, targetNodes, nodeCapacityProperty}.
CREATE FUNCTION age_gds.maxFlow_stats(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_maxflow_stats_cypher'
    LANGUAGE C;

-- gds.shortestPath.astar.stream: haversine-distance heuristic computed from
-- latitude_property/longitude_property node properties.
CREATE FUNCTION age_gds.shortestPath_astar_stream(
    graph_name                    text   DEFAULT NULL,
    source_node                   bigint DEFAULT NULL,
    target_node                   bigint DEFAULT NULL,
    latitude_property             text   DEFAULT NULL,
    longitude_property            text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL
) RETURNS TABLE(total_cost float8, node_ids bigint[], costs float8[])
    AS 'age_gds', 'age_gds_shortestpath_astar_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {sourceNode, targetNode,
-- latitudeProperty, longitudeProperty, relationshipWeightProperty}.
CREATE FUNCTION age_gds.shortestPath_astar_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_shortestpath_astar_stream_cypher'
    LANGUAGE C;

-- gds.minCostMaxFlow: NetworKit::SuccessiveShortestPathMinCostFlow. `alpha`
-- (cost-scaling) is not exposed by this class and is unsupported.
CREATE FUNCTION age_gds.minCostMaxFlow_stats(
    graph_name              text     DEFAULT NULL,
    capacity_property       text     DEFAULT NULL,
    cost_property           text     DEFAULT NULL,
    source_nodes            bigint[] DEFAULT NULL,
    target_nodes            bigint[] DEFAULT NULL,
    age_graph               text     DEFAULT NULL,
    node_query              text     DEFAULT NULL,
    edge_query              text     DEFAULT NULL,
    node_capacity_property  text     DEFAULT NULL
) RETURNS TABLE(total_cost float8, "computeMillis" bigint)
    AS 'age_gds', 'age_gds_mincostmaxflow_stats'
    LANGUAGE C;

-- Cypher-callable overload: config = {capacityProperty, costProperty,
-- sourceNodes, targetNodes, nodeCapacityProperty}.
CREATE FUNCTION age_gds.minCostMaxFlow_stats(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_mincostmaxflow_stats_cypher'
    LANGUAGE C;

-- gds.dag.topologicalSort: NetworKit::TopologicalSort. max_distance is a
-- linear DP pass over the computed order, only populated (non-NULL) when
-- compute_max_distance_from_source is true. Errors if the graph is not a
-- directed acyclic graph.
CREATE FUNCTION age_gds.topologicalSort_stream(
    graph_name                     text    DEFAULT NULL,
    compute_max_distance_from_source boolean DEFAULT false,
    age_graph                      text    DEFAULT NULL,
    node_query                     text    DEFAULT NULL,
    edge_query                     text    DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, "sortIndex" int, "maxDistance" int)
    AS 'age_gds', 'age_gds_topologicalsort_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {computeMaxDistanceFromSource}.
CREATE FUNCTION age_gds.topologicalSort_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_topologicalsort_stream_cypher'
    LANGUAGE C;

-- gds.dag.longestPath: NetworKit::TopologicalSort + weighted DP pass
-- (dist[v] = max over predecessors u of dist[u] + weight(u,v)). Errors if
-- the graph is not a directed acyclic graph.
CREATE FUNCTION age_gds.longestPath_stream(
    graph_name                    text   DEFAULT NULL,
    relationship_weight_property  text   DEFAULT NULL,
    age_graph                     text   DEFAULT NULL,
    node_query                    text   DEFAULT NULL,
    edge_query                    text   DEFAULT NULL
) RETURNS TABLE("nodeId" bigint, distance float8)
    AS 'age_gds', 'age_gds_longestpath_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {relationshipWeightProperty}.
CREATE FUNCTION age_gds.longestPath_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_longestpath_stream_cypher'
    LANGUAGE C;

-- gds.bridges: no dedicated NetworKit class; derived from
-- BiconnectedComponents (same class articulationPoints_stream uses) -- an
-- edge is a bridge iff it is the sole edge of its biconnected component,
-- checked here as "the component's node set has exactly 2 members" (see
-- src/algorithms/pathfinding.cpp for the parallel-edge caveat this implies).
CREATE FUNCTION age_gds.bridges_stream(
    graph_name text   DEFAULT NULL,
    age_graph  text   DEFAULT NULL,
    node_query text   DEFAULT NULL,
    edge_query text   DEFAULT NULL
) RETURNS TABLE(src bigint, dst bigint)
    AS 'age_gds', 'age_gds_bridges_stream'
    LANGUAGE C;

-- Cypher-callable overload (docs/implemented-plans/cypher-callable-interface.md):
-- UNWIND age_gds.bridges_stream(graphName, config) AS row RETURN row.src, row.dst
CREATE FUNCTION age_gds.bridges_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_bridges_stream_cypher'
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

-- Cypher-callable overload: config = {writeProperty, dampingFactor,
-- maxIterations, tolerance}. Returns a single map {nodePropertiesWritten}
-- (Neo4j's own write-procedure convention) instead of void, since a
-- Cypher-callable function must return agtype.
CREATE FUNCTION age_gds.pageRank_write(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_pagerank_write_cypher'
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

-- Cypher-callable overload: config = {writeProperty}. Returns a single map
-- {nodePropertiesWritten}, same convention as pageRank_write above.
CREATE FUNCTION age_gds.betweenness_write(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_betweenness_write_cypher'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Node similarity and KNN (stage 8)
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.nodeSimilarity_stream(
    graph_name                    text     DEFAULT NULL,
    age_graph                     text     DEFAULT NULL,
    node_query                    text     DEFAULT NULL,
    edge_query                    text     DEFAULT NULL,
    similarity_cutoff             float8   DEFAULT 1e-42,
    degree_cutoff                 int      DEFAULT 1,
    top_k                         int      DEFAULT 10,
    top_n                         int      DEFAULT 0,
    relationship_weight_property  text     DEFAULT NULL,
    upper_degree_cutoff           bigint   DEFAULT 9223372036854775807,
    bottom_k                      int      DEFAULT 10,
    bottom_n                      int      DEFAULT 0,
    source_node_filter            bigint[] DEFAULT NULL,
    target_node_filter            bigint[] DEFAULT NULL,
    use_components                boolean  DEFAULT false,
    similarity_metric             text     DEFAULT 'JACCARD'
) RETURNS TABLE(node1 bigint, node2 bigint, similarity float8)
    AS 'age_gds', 'age_gds_nodesimilarity_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {similarityCutoff, degreeCutoff, topK,
-- topN, relationshipWeightProperty, upperDegreeCutoff, bottomK, bottomN,
-- sourceNodeFilter, targetNodeFilter, useComponents, similarityMetric}.
CREATE FUNCTION age_gds.nodeSimilarity_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_nodesimilarity_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.knn_stream(
    graph_name         text   DEFAULT NULL,
    top_k              int    DEFAULT 10,
    age_graph          text   DEFAULT NULL,
    node_query         text   DEFAULT NULL,
    edge_query         text   DEFAULT NULL,
    similarity_cutoff  float8 DEFAULT 0
) RETURNS TABLE("sourceNodeId" bigint, "targetNodeId" bigint, similarity float8, rank int)
    AS 'age_gds', 'age_gds_knn_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {topK, similarityCutoff}.
CREATE FUNCTION age_gds.knn_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_knn_stream_cypher'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Link prediction (stage 8; single-pair scalar functions: plan 04)
-- *_stream uses NetworKit link predictors' runAll() to score every
-- currently-unconnected node pair. The single-pair scalar functions below
-- (no _stream suffix) use the same predictor classes' base run(u, v)
-- method, matching Neo4j GDS's gds.alpha.linkprediction.<name>(node1,
-- node2, {...}) spot-check shape.
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.linkprediction_adamicAdar_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_adamicadar_stream'
    LANGUAGE C;

-- Cypher-callable overload: no per-algorithm config keys.
CREATE FUNCTION age_gds.linkprediction_adamicAdar_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_adamicadar_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_commonNeighbors_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_commonneighbors_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_commonNeighbors_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_commonneighbors_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_preferentialAttachment_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_preferentialattachment_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_preferentialAttachment_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_preferentialattachment_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_resourceAllocation_stream(
    graph_name text DEFAULT NULL,
    age_graph  text DEFAULT NULL,
    node_query text DEFAULT NULL,
    edge_query text DEFAULT NULL
) RETURNS TABLE(node1 bigint, node2 bigint, score float8)
    AS 'age_gds', 'age_gds_linkpred_resourceallocation_stream'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_resourceAllocation_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_resourceallocation_stream_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_adamicAdar(
    node1               bigint,
    node2               bigint,
    graph_name          text DEFAULT NULL,
    relationship_query  text DEFAULT NULL,
    direction           text DEFAULT 'BOTH'
) RETURNS float8
    AS 'age_gds', 'age_gds_linkpred_adamicadar'
    LANGUAGE C;

-- Cypher-callable overload: (node1, node2, graphName, config); config =
-- {relationshipQuery, direction}. Already scalar -- bare agtype number,
-- no array/map packing.
CREATE FUNCTION age_gds.linkprediction_adamicAdar(
    node1       ag_catalog.agtype,
    node2       ag_catalog.agtype,
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_adamicadar_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_commonNeighbors(
    node1               bigint,
    node2               bigint,
    graph_name          text DEFAULT NULL,
    relationship_query  text DEFAULT NULL,
    direction           text DEFAULT 'BOTH'
) RETURNS float8
    AS 'age_gds', 'age_gds_linkpred_commonneighbors'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_commonNeighbors(
    node1       ag_catalog.agtype,
    node2       ag_catalog.agtype,
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_commonneighbors_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_preferentialAttachment(
    node1               bigint,
    node2               bigint,
    graph_name          text DEFAULT NULL,
    relationship_query  text DEFAULT NULL,
    direction           text DEFAULT 'BOTH'
) RETURNS float8
    AS 'age_gds', 'age_gds_linkpred_preferentialattachment'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_preferentialAttachment(
    node1       ag_catalog.agtype,
    node2       ag_catalog.agtype,
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_preferentialattachment_cypher'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_resourceAllocation(
    node1               bigint,
    node2               bigint,
    graph_name          text DEFAULT NULL,
    relationship_query  text DEFAULT NULL,
    direction           text DEFAULT 'BOTH'
) RETURNS float8
    AS 'age_gds', 'age_gds_linkpred_resourceallocation'
    LANGUAGE C;

CREATE FUNCTION age_gds.linkprediction_resourceAllocation(
    node1       ag_catalog.agtype,
    node2       ag_catalog.agtype,
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_linkpred_resourceallocation_cypher'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Node embeddings (stage 8)
-- NetworKit::Node2Vec: biased random-walk embeddings.
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.node2vec_stream(
    graph_name                    text    DEFAULT NULL,
    embedding_dim                 int     DEFAULT 128,
    walk_length                   int     DEFAULT 80,
    walks_per_node                int     DEFAULT 10,
    p                             float8  DEFAULT 1.0,
    q                             float8  DEFAULT 1.0,
    age_graph                     text    DEFAULT NULL,
    node_query                    text    DEFAULT NULL,
    edge_query                    text    DEFAULT NULL,
    relationship_weight_property  text    DEFAULT NULL,
    window_size                   int     DEFAULT 10,
    iterations                    int     DEFAULT 1
) RETURNS TABLE("nodeId" bigint, embedding float8[])
    AS 'age_gds', 'age_gds_node2vec_stream'
    LANGUAGE C;

-- Cypher-callable overload: config = {embeddingDim, walkLength,
-- walksPerNode, p, q, relationshipWeightProperty, windowSize, iterations}.
CREATE FUNCTION age_gds.node2vec_stream(
    "graphName" ag_catalog.agtype,
    config      ag_catalog.agtype DEFAULT NULL
) RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_node2vec_stream_cypher'
    LANGUAGE C;

-- -------------------------------------------------------------------------
-- Staleness helper (stage 8)
-- -------------------------------------------------------------------------

CREATE FUNCTION age_gds.graph_is_stale(name text)
    RETURNS boolean
    AS 'age_gds', 'age_gds_graph_is_stale'
    LANGUAGE C STRICT;

-- Cypher-callable overload: bare scalar agtype boolean.
CREATE FUNCTION age_gds.graph_is_stale("graphName" ag_catalog.agtype)
    RETURNS ag_catalog.agtype
    AS 'age_gds', 'age_gds_graph_is_stale_cypher'
    LANGUAGE C;
