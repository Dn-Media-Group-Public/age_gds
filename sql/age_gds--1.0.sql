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
    name          text        PRIMARY KEY,
    age_graph     text        NOT NULL DEFAULT '',
    node_query    text,
    edge_query    text,
    node_count    bigint,
    edge_count    bigint,
    directed      boolean     NOT NULL DEFAULT false,
    weighted      boolean     NOT NULL DEFAULT false,
    file_path     text        NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now(),
    projected_by  text
);

CREATE FUNCTION age_gds.graph_project(name text, node_query text, edge_query text)
    RETURNS void
    AS 'age_gds', 'age_gds_graph_project'
    LANGUAGE C STRICT;

CREATE FUNCTION age_gds.graph_drop(name text)
    RETURNS void
    AS 'age_gds', 'age_gds_graph_drop'
    LANGUAGE C STRICT;

CREATE FUNCTION age_gds.graph_list()
    RETURNS TABLE(
        name        text,
        age_graph   text,
        node_count  bigint,
        edge_count  bigint,
        directed    boolean,
        weighted    boolean,
        file_path   text,
        created_at  timestamptz
    )
    AS 'age_gds', 'age_gds_graph_list'
    LANGUAGE C;

CREATE FUNCTION age_gds.graph_exists(name text)
    RETURNS boolean
    AS 'age_gds', 'age_gds_graph_exists'
    LANGUAGE C STRICT;
