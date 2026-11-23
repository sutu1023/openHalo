CREATE TEMP TABLE pmc_build AS SELECT i AS id FROM generate_series(1, 10000) AS g(i);
CREATE TEMP TABLE pmc_probe AS SELECT i AS id FROM generate_series(1, 50000) AS g(i);
ANALYZE pmc_build;
ANALYZE pmc_probe;
SET max_parallel_workers_per_gather = 0;
SET enable_nestloop = off;
SET enable_mergejoin = off;
SET jit = off;
SET work_mem = '16MB';
