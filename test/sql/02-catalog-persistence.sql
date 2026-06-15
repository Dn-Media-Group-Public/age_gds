\c postgres
DROP EXTENSION IF EXISTS age_gds CASCADE;
CREATE EXTENSION age_gds;

-- Project a graph (stub loader)
SELECT age_gds.graph_project('g1', 'stub_nodes', 'stub_edges');

-- Verify catalog row
SELECT name, node_count, edge_count FROM age_gds.graph_list();
-- expected: g1 | 3 | 3

-- Exists
SELECT age_gds.graph_exists('g1');      -- must return true
SELECT age_gds.graph_exists('noexist'); -- must return false

-- Round-trip: drop and re-project
SELECT age_gds.graph_drop('g1');
SELECT age_gds.graph_exists('g1');      -- must return false

SELECT age_gds.graph_project('g1', 'stub_nodes', 'stub_edges');
SELECT age_gds.graph_exists('g1');      -- must return true

-- Transactional rollback test
BEGIN;
SELECT age_gds.graph_project('g2', 'stub_nodes', 'stub_edges');
ROLLBACK;
SELECT age_gds.graph_exists('g2');      -- must return false

-- Duplicate name error (expected to raise ERROR)
\set ON_ERROR_STOP off
SELECT age_gds.graph_project('g1', 'stub_nodes', 'stub_edges');
\set ON_ERROR_STOP on

-- Cleanup
SELECT age_gds.graph_drop('g1');
DROP EXTENSION age_gds;
