-- Install in the database where the join will run. Preserve existing definitions.
BEGIN;
CREATE TABLE IF NOT EXISTS public.pmc_name (
    pcm_name varchar(40) PRIMARY KEY,
    pmutype integer NOT NULL,
    pe_config bigint NOT NULL,
    config1 bigint NOT NULL,
    config2 integer NOT NULL CHECK (config2 IN (0, 1))
);
LOCK TABLE public.pmc_name IN SHARE ROW EXCLUSIVE MODE;
INSERT INTO public.pmc_name (pcm_name, pmutype, pe_config, config1, config2)
SELECT v.* FROM (VALUES
    ('cycles', 0, 0, 0, 1),
    ('Instructions', 0, 1, 0, 1),
    ('PERF_COUNT_HW_CACHE_L1D_RD', 3, 0, 0, 1),
    ('PERF_COUNT_HW_CACHE_L1D_MISS_RD', 3, 65536, 0, 1),
    ('MEM_INST_RETIRED_ALL_STORES', 4, 33488, 0, 1),
    ('cpu_clk_unhalted_cycles', 4, 60, 0, 1),
    ('mem_load_retired_l1_miss', 4, 2257, 0, 1),
    ('mem_load_retired_l2_miss', 4, 4305, 0, 1),
    ('mem_load_retired_l3_miss', 4, 8401, 0, 1),
    ('cycle_activity_stalls_mem_any', 4, 335549603, 0, 1),
    ('cycle_activity_stalls_total', 4, 67110051, 0, 1),
    ('cycle_activity_statlls_l1d_miss', 4, 201329827, 0, 1),
    ('cycle_activity_statlls_l2d_miss', 4, 83887523, 0, 1),
    ('cycle_activity_statlls_l3d_miss', 4, 100664995, 0, 1),
    ('UOPS_EXECUTED_1_UOP_EXEC', 4, 16777649, 0, 1),
    ('UOPS_EXECUTED_2_UOP_EXEC', 4, 33554865, 0, 1),
    ('UOPS_EXECUTED_3_UOP_EXEC', 4, 50332081, 0, 1),
    ('UOPS_EXECUTED_4_UOP_EXEC', 4, 67109297, 0, 1),
    ('ICache_Reads', 4, 128, 0, 1),
    ('ICACHE_16B_IFDATA_ACCESS', 4, 896, 0, 1),
    ('ICACHE_64B_IFTAG_HIT', 4, 387, 0, 1),
    ('ICACHE_64B_IFTAG_MISS', 4, 643, 0, 1),
    ('ITLB_MISSES_WALK_COMPLETED', 4, 3717, 0, 1),
    ('ITLB_MISSES_STLB_HIT', 4, 8325, 0, 1)
) AS v(name, pmutype, pe_config, config1, config2)
WHERE NOT EXISTS (SELECT FROM public.pmc_name p WHERE lower(p.pcm_name) = lower(v.name));
COMMIT;
