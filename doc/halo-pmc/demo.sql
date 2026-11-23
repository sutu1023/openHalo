\set ON_ERROR_STOP on
\ir workload.sql
SET halo_pmc_list = 'cycles,instructions';
SET halo_pmc_sample_period = 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)
SELECT count(*) FROM pmc_probe JOIN pmc_build USING (id);
RESET halo_pmc_list;
SELECT count(*) AS matched_rows FROM pmc_probe JOIN pmc_build USING (id);
