-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION age_gds" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS age_gds;

-- Returns extension version string; proves C++ entry point reachable
CREATE FUNCTION age_gds.version() RETURNS text
    AS 'age_gds', 'age_gds_version'
    LANGUAGE C STRICT;
