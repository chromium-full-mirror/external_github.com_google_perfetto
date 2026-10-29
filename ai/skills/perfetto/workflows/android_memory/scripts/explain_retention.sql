--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Explains why the reachable instances of one class are alive. Replace:
--   <class_name>   fully qualified class name, e.g. com.example.Foo$Bar
--   <native_size>  only instances with this native_size (e.g. one Bitmap size
--                  bucket), or -1 for all
--   <object_id>    instance to trace, or -1 for one of the largest holder group
--
-- Returns three result sets:
--   1. Holder groups: instances grouped by the classes of their two nearest
--      dominators, so a few old instances in a cache do not hide many new
--      leaked ones (at most 10 groups per dump).
--   2. Dominator chain of the traced instance, outermost dominator first, with
--      the field from the previous row (empty when it holds the object only
--      through other objects) and the bytes each object retains.
--   3. Shortest path from a GC root to the traced instance.

INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

INCLUDE PERFETTO MODULE android.memory.heap_graph.class_tree;

CREATE OR REPLACE PERFETTO TABLE _retention_params AS
SELECT
  '<class_name>' AS class_name,
  CAST('<native_size>' AS INT) AS native_size,
  CAST('<object_id>' AS INT) AS object_id;

CREATE OR REPLACE PERFETTO TABLE _retention_groups AS
SELECT
  o.upid,
  o.graph_sample_ts,
  coalesce(c1.deobfuscated_name, c1.name, '[GC root]') AS dominator,
  coalesce(c2.deobfuscated_name, c2.name, '[GC root]') AS dominator_of_dominator,
  count() AS instances,
  min(o.id) AS example_object_id,
  row_number() OVER (
    PARTITION BY
      o.upid,
      o.graph_sample_ts
    ORDER BY count() DESC, min(o.id)
  ) AS rank
FROM heap_graph_object AS o
CROSS JOIN _retention_params AS params
JOIN heap_graph_class AS c ON c.id = o.type_id
JOIN heap_graph_dominator_tree AS d ON d.id = o.id
LEFT JOIN heap_graph_object AS o1 ON o1.id = d.idom_id
LEFT JOIN heap_graph_class AS c1 ON c1.id = o1.type_id
LEFT JOIN heap_graph_dominator_tree AS d1 ON d1.id = d.idom_id
LEFT JOIN heap_graph_object AS o2 ON o2.id = d1.idom_id
LEFT JOIN heap_graph_class AS c2 ON c2.id = o2.type_id
WHERE
  o.reachable
  AND coalesce(c.deobfuscated_name, c.name) = params.class_name
  AND (params.native_size < 0 OR o.native_size = params.native_size)
GROUP BY
  o.upid,
  o.graph_sample_ts,
  dominator,
  dominator_of_dominator;

CREATE OR REPLACE PERFETTO TABLE _retention_targets AS
SELECT DISTINCT
  iif(params.object_id < 0, g.example_object_id, params.object_id) AS id
FROM _retention_groups AS g
CROSS JOIN _retention_params AS params
WHERE
  g.rank = 1;

-- Walking up a tree is cheap: each object has one parent row. The depth cap
-- stops runaway chains through linked structures (e.g. Message.next).
CREATE OR REPLACE PERFETTO MACRO _retention_walk_up(parents TableOrSubquery)
RETURNS TableOrSubquery
AS (
  WITH RECURSIVE walk(target_id, id, depth) AS (
    SELECT id, id, 0 FROM _retention_targets
    UNION ALL
    SELECT walk.target_id, p.parent_id, walk.depth + 1
    FROM walk
    INNER JOIN $parents AS p ON p.id = walk.id
    WHERE p.parent_id IS NOT NULL AND walk.depth < 200
  )
  SELECT * FROM walk
);

CREATE OR REPLACE PERFETTO TABLE _retention_dominator_chain AS
SELECT *
FROM _retention_walk_up!(
  (SELECT id, idom_id AS parent_id FROM heap_graph_dominator_tree)
);

-- The stdlib's breadth-first tree from the GC roots ignores weak, phantom and
-- finalizer references, so each object's parent chain is a shortest path that
-- keeps it alive.
CREATE OR REPLACE PERFETTO TABLE _retention_gc_root_path AS
SELECT * FROM _retention_walk_up!(_heap_graph_object_min_depth_tree);

-- heap_graph_reference has no index on owned_id, so look it up once for all
-- objects on both paths.
CREATE OR REPLACE PERFETTO TABLE _retention_fields AS
SELECT
  owner_id,
  owned_id,
  min(coalesce(nullif(deobfuscated_field_name, ''), field_name)) AS field_name
FROM heap_graph_reference
WHERE
  owned_id IN (
    SELECT id FROM _retention_dominator_chain
    UNION
    SELECT id FROM _retention_gc_root_path
  )
GROUP BY
  owner_id,
  owned_id;

CREATE OR REPLACE PERFETTO MACRO _retention_rows(walk TableOrSubquery)
RETURNS TableOrSubquery
AS (
  SELECT
    w.target_id,
    w.depth AS step,
    w.id AS object_id,
    coalesce(c.deobfuscated_name, c.name) AS class_name,
    o.root_type,
    f.field_name AS field
  FROM (
    SELECT *, lead(id) OVER (PARTITION BY target_id ORDER BY depth) AS parent_id
    FROM $walk
  ) AS w
  INNER JOIN heap_graph_object AS o ON o.id = w.id
  INNER JOIN heap_graph_class AS c ON c.id = o.type_id
  LEFT JOIN _retention_fields AS f
    ON f.owner_id = w.parent_id AND f.owned_id = w.id
);

SELECT
  coalesce(p.name, 'pid=' || p.pid) AS process_name,
  g.graph_sample_ts,
  g.dominator,
  g.dominator_of_dominator,
  g.instances,
  g.example_object_id
FROM _retention_groups AS g
JOIN process AS p ON p.id = g.upid
WHERE
  g.rank <= 10
ORDER BY
  g.upid,
  g.graph_sample_ts,
  g.rank;

SELECT
  r.*,
  d.dominated_size_bytes + d.dominated_native_size_bytes AS retained_bytes
FROM _retention_rows!(_retention_dominator_chain) AS r
JOIN heap_graph_dominator_tree AS d ON d.id = r.object_id
ORDER BY
  r.target_id,
  r.step DESC;

SELECT *
FROM _retention_rows!(_retention_gc_root_path)
ORDER BY
  target_id,
  step DESC;
