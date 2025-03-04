SET optimizer TO off;

--
-- setup
--

--start_ignore
DROP VIEW IF EXISTS many_ops;
DROP ROLE r1_opmem_test;
DROP RESOURCE GROUP rg1_opmem_test;
DROP RESOURCE GROUP rg2_opmem_test;
CREATE LANGUAGE plpythonu;
--end_ignore

-- a helper function to run query via SPI
CREATE OR REPLACE FUNCTION f1_opmem_test() RETURNS void AS $$
  plpy.execute("""select * from gp_dist_random('gp_id')""")
$$ LANGUAGE plpythonu;

-- this view contains many operators in the plan, which is used to trigger
-- the issue.  gp_toolkit.gp_resgroup_config is a large JOIN view of many
-- relations, to prevent the source relations being optimized out from the plan
-- we have to keep the columns provided by them in the target list, instead of
-- composing a long SELECT c1,c2,... list we use SELECT * here, but we should
-- not output the groupid as it changes each time.
CREATE OR REPLACE VIEW many_ops AS
      SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
UNION SELECT * FROM gp_toolkit.gp_resgroup_config WHERE groupid=6437
;

-- we must ensure spill to be small enough but still > 0.
-- - rg1's memory quota is 682 * 1% = 6;
-- - per-xact quota is 6/3=2;
-- - spill memory is 2 * 60% = 1;
CREATE RESOURCE GROUP rg1_opmem_test
  WITH (cpu_rate_limit=10, memory_limit=1, memory_shared_quota=0,
        concurrency=3, memory_spill_ratio=60);

CREATE ROLE r1_opmem_test RESOURCE GROUP rg1_opmem_test;
GRANT ALL ON many_ops TO r1_opmem_test;

-- rg1 has very low per-xact memory quota, there will be no enough operator
-- memory reserved, however in resource group mode we assign at least 100KB to
-- each operator, no matter it is memory intensive or not.  As long as there is
-- enough shared memory the query should be executed successfully.
--
-- some operators like HashAgg require more memory to run, the memory quota is
-- also dynamically increased to meet their minimal requirements.
--
-- note: when there is no enough operator memory there should be a warning,
-- however warnings are not displayed in isolation2 tests.

--
-- positive: there is enough global shared memory
--

SET gp_resgroup_memory_policy TO eager_free;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

SET gp_resgroup_memory_policy TO auto;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

--
-- negative: there is not enough shared memory
--

-- rg1 has no group level shared memory, and most memory are granted to rg2,
-- there is only very little global shared memory due to integer rounding.
CREATE RESOURCE GROUP rg2_opmem_test
  WITH (cpu_rate_limit=10, memory_limit=59);

-- this query can execute but will raise OOM error.

SET gp_resgroup_memory_policy TO eager_free;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

SET gp_resgroup_memory_policy TO auto;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

--
-- positive: there is enough group shared memory
--

ALTER RESOURCE GROUP rg2_opmem_test SET memory_limit 40;
ALTER RESOURCE GROUP rg1_opmem_test SET memory_limit 20;
ALTER RESOURCE GROUP rg1_opmem_test SET memory_shared_quota 100;
ALTER RESOURCE GROUP rg1_opmem_test SET memory_spill_ratio 20;

SET gp_resgroup_memory_policy TO eager_free;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

SET gp_resgroup_memory_policy TO auto;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

--
-- positive: the spill memory is large enough, no adjustment is needed
--

DROP RESOURCE GROUP rg2_opmem_test;
ALTER RESOURCE GROUP rg1_opmem_test SET memory_limit 40;
ALTER RESOURCE GROUP rg1_opmem_test SET memory_shared_quota 50;
ALTER RESOURCE GROUP rg1_opmem_test SET memory_spill_ratio 30;
ALTER RESOURCE GROUP rg1_opmem_test SET concurrency 1;

SET gp_resgroup_memory_policy TO eager_free;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

SET gp_resgroup_memory_policy TO auto;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
RESET role;

--
-- positive: when spill memory is zero, work memory is used
--

ALTER RESOURCE GROUP rg1_opmem_test SET memory_spill_ratio 0;

SET gp_resgroup_memory_policy TO eager_free;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
SELECT f1_opmem_test();
RESET role;

SET gp_resgroup_memory_policy TO auto;
SET ROLE TO r1_opmem_test;
SELECT * FROM many_ops;
SELECT f1_opmem_test();
RESET role;

--
-- cleanup
--

DROP VIEW many_ops;
DROP ROLE r1_opmem_test;
DROP RESOURCE GROUP rg1_opmem_test;
