# Diffing Android Java Heap Dumps

Compares two Android Java heap dumps (before vs after), either Perfetto
`java_heap_profiler` traces or Android `.hprof` dumps.

## Step 1: Load both dumps into warm sessions

Load each dump once into a named session so every query reuses the parsed heap
graph instead of re-parsing the file. Load them in parallel:

```bash
trace_processor server unix --name before_tp --daemonize <before_dump> &
trace_processor server unix --name after_tp --daemonize <after_dump> &
wait
```

A large heap dump can take about a minute to load.

Kill both sessions when finished:
`trace_processor server kill before_tp && trace_processor server kill after_tp`.

## Step 2: Find which classes grew

Diff the two sessions:

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_dumps.py before_tp after_tp
```

The script queries `android_heap_graph_stats` and
`android_heap_graph_class_aggregation` in both sessions, then reports the diffed
snapshots, the heap summary (with reachable Java heap bytes and reachable native
bytes as separate rows), **Top Application Classes by Dominated Bytes
Growth**, **Top Application Classes by Instance Count Growth**, **Application
Classes Added or Removed (0 -> N or N -> 0)**, and **Top
Libcore / Array Classes by Dominated Bytes Growth** (arrays and `libcore` types
are listed separately because they are usually the payload of an app-class leak,
not its cause). The first run builds the dominator tree in both sessions,
which is slow on a large heap; later queries on the same sessions reuse it.

By default, the first matching snapshot in each session is compared. If a
session holds multiple snapshots or processes, select specific ones with
`--before-upid` / `--after-upid` and `--before-ts` / `--after-ts` (the script
lists the available ones when a session holds several processes). Pass
`--top N` to widen the application class tables (default 10), and
`--class-glob '<pattern>'` (repeatable) to limit them to specific packages, for
example the app's own:

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_dumps.py \
  before_tp after_tp --before-upid <BEFORE_UPID> --after-upid <AFTER_UPID> \
  --top 30 --class-glob 'com.example.*'
```

Total heap size fluctuates on its own (e.g. caches, `SoftReference` eviction),
so never dismiss a regression because the heap total looks flat.

- **Compare the growth with what happened between the dumps.** If an action
  was repeated N times, a class that gained about N instances (or a multiple of
  N) is retained once per repetition: a leak. A small fixed gain of one
  top-level object and its children is more likely a one-time state change,
  such as one extra window or cached screen. With a single action between the
  dumps, the counts alone cannot tell the two apart; check whether the grown
  objects should still be alive after that action.
- **Census a small fixed gain before explaining it.** The top-N tables cut off
  the small `+1` classes that show what the extra unit is. Re-run the script
  with a larger `--top` and `--class-glob` patterns for the packages of the
  classes that grew by a fixed amount, and report `before -> after` for every
  class that grew with them.
- **Added and removed classes show code that changed.** When a library
  version or feature flag replaces one implementation with another, its old
  classes go `N -> 0` while the new ones go `0 -> N`. Note such swaps and check
  in Step 3 whether the retention paths pass through the new classes.

## Step 3: Inspect the classes that grew

Step 2 usually flags several related classes (e.g. a leaked object, its inner
classes, and the arrays holding them). They are facets of the same leak, so
investigate them together.

### 3.1 What keeps them alive? (Dominator retention paths)

Run the Phase 1 triage scripts of
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md)
(`triage_retention_hotspots.sql`, `triage_leaked_activities.sql`) on
`after_tp` when the growth is in Activities or a single large hotspot. To
explain a specific class from Step 2, fill in the placeholders of
`explain_retention.sql` and run it on `after_tp` (single quotes keep `$` in
inner class names from being expanded):

```bash
sed -e 's/<class_name>/com.example.LeakedThing$Inner/g' \
    -e 's/<native_size>/-1/g' -e 's/<object_id>/-1/g' \
    $SKILL_ROOT/workflows/android_memory/scripts/explain_retention.sql |
  trace_processor query --remote after_tp -f -
```

It returns three result sets for each dump:

1. **Holder groups**: the class's reachable instances grouped by the classes of
   their two nearest dominators. Old and new instances of a grown class are
   often held by different owners (a few old ones in a long-lived cache, many
   new ones in leaked objects), so an arbitrary instance can explain the wrong
   population. Check that the largest group's count matches the growth; if a
   smaller group matches instead, re-run with `<object_id>` set to its
   `example_object_id`.
2. **Dominator chain** of an instance of the largest group, from the outermost
   dominator down, with the field through which each dominator holds the next
   object and the bytes it retains. The top rows name the retaining container.
   The field is empty where the dominator holds the object only through other
   objects.
3. **Shortest path from a GC root** to the same instance, walked up the
   stdlib's breadth-first tree from the roots, which ignores weak, phantom and
   finalizer references. It names every field on the way, including those
   the dominator chain skips.

Run it once per class. The first run in a session builds the stdlib trees; later
runs reuse them. Pick the top growing application class (if an action was
repeated, one whose gain matches the repetition count), skipping collections,
arrays, and lambdas, which are containers rather than the leaked objects.

> [!CAUTION] Do not write recursive queries or multi-hop self-joins
> (`r1 JOIN r2 ON r2.owned_id = r1.owner_id`) over `heap_graph_reference`, and
> do not walk the heap graph downwards from the roots. The reference table
> holds millions of cyclic edges, so such queries run for minutes or never
> finish. Walking up `heap_graph_dominator_tree.idom_id` or
> `_heap_graph_object_min_depth_tree.parent_id` is cheap, and custom queries
> should look up `heap_graph_reference` once, for a fixed set of ids
> (`WHERE owned_id IN (...)`).

### 3.2 Which instances leaked? (`.hprof` only)

Step 2 and 3.1 treat every instance of a class as interchangeable, yet the
leaked instances and the legitimate ones are usually identical in everything
they expose: same class, same size, same retention path. Only their field values
differ. Reading those values tells you *which* objects were kept, turning, say,
"`ImageRequest` grew by 30 instances" into a reproducible trigger ("every
preview request is retained").

Grouping the reachable instances by field value makes the leaked configuration
(e.g. `is_preview_request = true`) stand out from the baseline population. Run
the queries on both `before_tp` and `after_tp`: the values whose counts grew
identify the leak, and the ones that stayed flat confirm the rest of the
population is healthy.

Field values are only recorded in Android `.hprof` dumps; Perfetto
`java_heap_profiler` traces store the object graph without them, so these
queries return nothing for them.

Values live in two places, so there is one query for each. Use `-f - << 'EOF'`
so `$` in inner class names (e.g. `Foo$Bar`) is not expanded by the shell.

**String fields** are ordinary references: the owner points at a
`java.lang.String` instance through `heap_graph_reference`, and that instance
carries the text in `heap_graph_object_data.value_string`.

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    SELECT r.field_name, d.value_string, COUNT(*) AS instances
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    JOIN heap_graph_reference r ON o.id = r.owner_id
    JOIN heap_graph_object s ON r.owned_id = s.id
    JOIN heap_graph_object_data d ON s.object_data_id = d.id
    WHERE o.reachable = 1
      AND COALESCE(c.deobfuscated_name, c.name) = '<LEAKED_CLASS>'
      AND d.value_string IS NOT NULL
    GROUP BY 1, 2
    ORDER BY instances DESC;
EOF
```

**Primitive fields** hang off the owner itself: `heap_graph_object.object_data_id`
leads to `heap_graph_primitive`, which has one nullable column per primitive type
(`int_value`, `bool_value`, ...) and sets exactly one of them per row, hence the
`COALESCE`.

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    SELECT
      p.field_name,
      p.field_type,
      COALESCE(p.bool_value, p.byte_value, p.char_value, p.short_value,
               p.int_value, p.long_value, p.float_value, p.double_value) AS value,
      COUNT(*) AS instances
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    JOIN heap_graph_object_data d ON o.object_data_id = d.id
    JOIN heap_graph_primitive p ON d.field_set_id = p.field_set_id
    WHERE o.reachable = 1
      AND COALESCE(c.deobfuscated_name, c.name) = '<LEAKED_CLASS>'
      -- Exclude ART's synthetic Object lock-word header (shadow$_monitor_).
      AND p.field_name NOT GLOB 'java.lang.Object.shadow*'
    GROUP BY 1, 2, 3
    ORDER BY instances DESC;
EOF
```

Read the results top-down: a value shared by many instances is a configuration,
and comparing its count between the two sessions tells you whether that
configuration is the one leaking. Identity fields (e.g. `android.os.Binder.mObject`
pointers, hashes) differ per instance and pile up as `instances = 1` rows at the
bottom; ignore them.

### 3.3 Bitmaps: the count is Java, the bytes are native

Since Android 8.0 a `android.graphics.Bitmap` keeps its pixels in native memory
and only references them from the Java object. Its `self_size` stays small
whatever the image size, and the pixel bytes are in
`heap_graph_object.native_size`. Group the reachable Bitmaps by native size in
both sessions to find the size bucket that grew:

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    SELECT o.native_size, COUNT(*) AS bitmaps, SUM(o.native_size) AS native_bytes
    FROM heap_graph_object o
    INNER JOIN heap_graph_class c ON o.type_id = c.id
    WHERE o.reachable = 1 AND c.name = 'android.graphics.Bitmap'
    GROUP BY 1
    ORDER BY native_bytes DESC;
EOF
```

Then explain the bucket that grew with `explain_retention.sql`
(`<class_name>` = `android.graphics.Bitmap`, `<native_size>` = the bucket), and
compare its holder groups with the bucket's growth. If a leaked application
object dominates the Bitmap, the Bitmap growth is a consequence of that leak,
not a separate one. If a newly added UI element dominates it (for example, a
list item, an app widget, or a dialog), the Bitmaps are that element's content:
explain why the element appeared rather than looking for a Bitmap leak.

Read the count and the bytes together:

| Bitmap count | Bitmap native bytes | Most likely cause                         |
| :----------- | :------------------ | :---------------------------------------- |
| up           | up proportionally   | more Bitmaps held alive                   |
| flat         | up                  | bigger Bitmaps: density, resolution,      |
:              :                     : scaling or `Bitmap.Config` change         :
| up           | flat                | more but smaller Bitmaps                  |

## Further investigation and reporting

Once you have identified the leaked classes and their retention paths, follow
Phase 2 Step 4 (inspecting `heap_graph_reference` holders) and Phase 3 (code
search & fix plan) in
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md). For a constant
`+1` step change, look for what differed between the two runs; when system
traces of both runs are available,
[system_trace_memory.md]($SKILL_ROOT/workflows/android_memory/system_trace_memory.md)
compares their memory counters and process states.

## Reporting and query rules

1.  Report before and after counts with the delta (e.g. `12 -> 42 (+30 instances)`),
    the retained (`dominated_bytes`) delta, Java and native byte deltas
    separately, and the retention path holding the objects.
2.  Take class sizes from `android_heap_graph_class_aggregation`. Summing
    `dominated_size_bytes` across instances of a self-nesting class (e.g. a
    `View` inside a `View`) double-counts inner instances.
3.  If Step 2 reported several snapshots in a trace, scope custom queries with
    `o.upid = <upid> AND o.graph_sample_ts = <ts>`. Do not filter on
    `process.name`: in `.hprof` dumps `process.name` is `NULL`.
