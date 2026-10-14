/* pg_relusage--1.0.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_relusage" to load this file. \quit

-- pg_relusage does not define any SQL objects. Load the library with
-- shared_preload_libraries, session_preload_libraries or LOAD to use it.
