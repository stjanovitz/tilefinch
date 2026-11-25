/* Memory study (TILEFINCH_MEMORY_DIAGNOSTICS: host builds and PSP
   validation builds only). Splits the committed page's Budget categories
   into their parts at report time by walking the page's own tables; see
   docs/engineering/VALIDATION_RECORDS.md, "Memory make-up". Every figure is
   a Budget charge (allocation header included) of an allocation charged to
   the category being split, unless the field says otherwise, so each line
   ends with the remainder no named part explains. Allocation-free. */

#include "fnv1a.h"
#include "session_site_storage.h"
#include "tilefinch/frame_lite.h"

/* Shared response bodies are reachable from the HTTP cache, image
   resources and stylesheet pins at once: count each once. */
#define MEMORY_REPORT_SEEN_SLOTS 2048u

typedef struct {
    const void *slots[MEMORY_REPORT_SEEN_SLOTS];
    size_t used;
    bool full;
} MemoryReportSeen;

static MemoryReportSeen memory_report_seen;

static bool memory_report_first(MemoryReportSeen *seen, const void *pointer)
{
    if (pointer == NULL) return false;
    uintptr_t hash = ((uintptr_t) pointer >> 4) * UINT32_C(2654435761);
    for (size_t probe = 0; probe < MEMORY_REPORT_SEEN_SLOTS; probe++) {
        size_t slot = (size_t) ((hash + probe) % MEMORY_REPORT_SEEN_SLOTS);
        if (seen->slots[slot] == pointer) return false;
        if (seen->slots[slot] == NULL) {
            seen->slots[slot] = pointer;
            seen->used++;
            return true;
        }
    }
    seen->full = true;
    return true;
}

/* Adds the allocation's charge to its own category's slot. */
static size_t memory_report_add(size_t totals[BUDGET_CATEGORY_COUNT],
                                const void *pointer)
{
    BudgetCategory category = BUDGET_CATEGORY_UNCATEGORIZED;
    size_t charge = budget_allocation_charge(pointer, &category);
    if (totals != NULL && charge != 0) totals[category] += charge;
    return charge;
}

/* A shared body and its payload, once per report. */
static void memory_report_add_body(size_t totals[BUDGET_CATEGORY_COUNT],
                                   const BrowserSharedBody *body)
{
    if (!memory_report_first(&memory_report_seen, body)) return;
    (void) memory_report_add(totals, body);
    if (memory_report_first(&memory_report_seen, body->data))
        (void) memory_report_add(totals, body->data);
}

static size_t memory_report_charge_in(const void *pointer,
                                      BudgetCategory want)
{
    BudgetCategory category = BUDGET_CATEGORY_UNCATEGORIZED;
    size_t charge = budget_allocation_charge(pointer, &category);
    return category == want ? charge : 0;
}

static size_t memory_report_less(size_t total, size_t part)
{
    return total > part ? total - part : 0;
}

/* The page's realms: the top-level runtime, then every child frame's. */
static size_t memory_report_realms(const BrowserEngine *engine,
                                   const ScriptRuntime **realms,
                                   size_t capacity)
{
    size_t count = 0;
    const NavigationPage *page = &engine->navigation.page;
    if (page->runtime != NULL && count < capacity)
        realms[count++] = page->runtime;
    for (size_t i = 0; i < page->frame_count && count < capacity; i++) {
        const ScriptRuntime *runtime = page->frames[i].runtime;
        bool repeated = runtime == NULL;
        for (size_t j = 0; !repeated && j < count; j++)
            repeated = realms[j] == runtime;
        if (!repeated) realms[count++] = runtime;
    }
    return count;
}

static void memory_report_budget_at_peak(const Budget *budget,
                                         const char *phase,
                                         BrowserMemoryPrint print)
{
    size_t sum = 0;
    for (size_t i = 0; i < BUDGET_CATEGORY_COUNT; i++)
        sum += budget->global_peak_categories[i];
    print("tilefinch-budget-at-peak: phase=%s peak=%zu sum=%zu "
          "uncategorized=%zu dom=%zu javascript=%zu style=%zu "
          "resource=%zu layout=%zu render=%zu session=%zu "
          "navigation=%zu\n",
          phase, budget->peak, sum,
          budget->global_peak_categories[BUDGET_CATEGORY_UNCATEGORIZED],
          budget->global_peak_categories[BUDGET_CATEGORY_DOM],
          budget->global_peak_categories[BUDGET_CATEGORY_JAVASCRIPT],
          budget->global_peak_categories[BUDGET_CATEGORY_STYLE],
          budget->global_peak_categories[BUDGET_CATEGORY_RESOURCE],
          budget->global_peak_categories[BUDGET_CATEGORY_LAYOUT],
          budget->global_peak_categories[BUDGET_CATEGORY_RENDER],
          budget->global_peak_categories[BUDGET_CATEGORY_SESSION],
          budget->global_peak_categories[BUDGET_CATEGORY_NAVIGATION]);
}

static void memory_report_javascript(const BrowserEngine *engine,
                                     const char *phase,
                                     BrowserMemoryPrint print)
{
    const ScriptRuntime *realms[NAVIGATION_FRAME_LIMIT + 1];
    size_t count = memory_report_realms(engine, realms,
                                        sizeof(realms) / sizeof(realms[0]));
    ScriptRuntimeMemorySplit sum = {0};
    size_t runtime_structs = 0;
    for (size_t i = 0; i < count; i++) {
        ScriptRuntimeMemorySplit split;
        if (!script_runtime_memory_split(realms[i], &split)) continue;
        runtime_structs +=
            memory_report_charge_in(realms[i], BUDGET_CATEGORY_JAVASCRIPT);
        sum.heap_used += split.heap_used;
        sum.pool_requested += split.pool_requested;
        sum.pool_budget_bytes += split.pool_budget_bytes;
        sum.pool_header_bytes += split.pool_header_bytes;
        sum.pool_live_blocks += split.pool_live_blocks;
        sum.pool_cached_blocks += split.pool_cached_blocks;
        sum.pool_cached_bytes += split.pool_cached_bytes;
        sum.pool_slack_bytes += split.pool_slack_bytes;
        sum.source_entries += split.source_entries;
        sum.source_text_bytes += split.source_text_bytes;
        sum.source_held_bytes += split.source_held_bytes;
        sum.source_plain_bytes += split.source_plain_bytes;
        sum.net_buffer_bytes += split.net_buffer_bytes;
        sum.classic_queue_count += split.classic_queue_count;
        sum.classic_queue_source_bytes += split.classic_queue_source_bytes;
        sum.classic_queue_bytes += split.classic_queue_bytes;
        sum.bridge_table_bytes += split.bridge_table_bytes;
        sum.wrappers += split.wrappers;
    }
    size_t category =
        engine->budget.categories[BUDGET_CATEGORY_JAVASCRIPT].current;
    size_t named = sum.pool_budget_bytes + sum.source_held_bytes
        + sum.net_buffer_bytes + sum.classic_queue_bytes + runtime_structs
        + sum.bridge_table_bytes;
    /* pool-check: the pool's own ledger against requested + rounding +
       headers + idle caches (0 when they agree). */
    size_t parts = sum.pool_requested + sum.pool_slack_bytes
        + sum.pool_header_bytes + sum.pool_cached_bytes;
    print("tilefinch-js-budget-split: phase=%s javascript=%zu realms=%zu "
          "heap-requested=%zu pool-headers=%zu "
          "pool-blocks=%zu pool-cache=%zu/%zu pool-slack=%zu "
          "pool-total=%zu pool-check=%ld source-store=%zu "
          "source-plain=%zu source-compressed=%zu source-text=%zu "
          "source-entries=%zu net-buffers=%zu classic-queue=%zu/%zu/%zu "
          "runtime-structs=%zu bridge-tables=%zu wrappers=%zu "
          "remainder=%zu\n",
          phase, category, count, sum.pool_requested,
          sum.pool_header_bytes, sum.pool_live_blocks,
          sum.pool_cached_bytes, sum.pool_cached_blocks,
          sum.pool_slack_bytes, sum.pool_budget_bytes,
          (long) sum.pool_budget_bytes - (long) parts,
          sum.source_held_bytes, sum.source_plain_bytes,
          memory_report_less(sum.source_held_bytes, sum.source_plain_bytes),
          sum.source_text_bytes, sum.source_entries, sum.net_buffer_bytes,
          sum.classic_queue_count, sum.classic_queue_source_bytes,
          sum.classic_queue_bytes, runtime_structs,
          sum.bridge_table_bytes, sum.wrappers,
          memory_report_less(category, named));
}

static size_t memory_report_document_bytes(const DocumentMemorySplit *split)
{
    return split->node_arena_bytes + split->text_arena_bytes
        + split->hash_bytes + split->lexbor_object_bytes
        + split->body_text_bytes + split->side_table_bytes;
}

static void memory_report_dom(const BrowserEngine *engine, const char *phase,
                              BrowserMemoryPrint print)
{
    const NavigationPage *page = &engine->navigation.page;
    DocumentMemorySplit top;
    (void) document_memory_split(&page->document, &top);
    size_t frame_documents = 0, frame_bytes = 0;
    for (size_t i = 0; i < page->frame_count; i++) {
        DocumentMemorySplit frame;
        if (page->frames[i].retired
            || !document_memory_split(&page->frames[i].document, &frame))
            continue;
        frame_documents++;
        frame_bytes += memory_report_document_bytes(&frame);
    }
    size_t rescue = 0;
    const ScriptRuntime *realms[NAVIGATION_FRAME_LIMIT + 1];
    size_t count = memory_report_realms(engine, realms,
                                        sizeof(realms) / sizeof(realms[0]));
    for (size_t i = 0; i < count; i++) {
        ScriptRuntimeMemorySplit split;
        if (script_runtime_memory_split(realms[i], &split))
            rescue += split.memory_rescue_bytes;
    }
    size_t category = engine->budget.categories[BUDGET_CATEGORY_DOM].current;
    size_t named = memory_report_document_bytes(&top) + frame_bytes + rescue;
    print("tilefinch-dom-split: phase=%s dom=%zu "
          "node-arena=%zu/%zu/%zu text-arena=%zu/%zu/%zu hashes=%zu "
          "script-text=%zu/%zu/%zu style-text=%zu/%zu/%zu other-text=%zu "
          "comments=%zu attr-values=%zu nodes=%zu elements=%zu "
          "text-nodes=%zu attrs=%zu walk=%s lexbor-objects=%zu "
          "body-text=%zu side-tables=%zu tokenizer=%zu tokenizer-hw=%zu memory-rescue=%zu "
          "frame-documents=%zu/%zu remainder=%zu\n",
          phase, category,
          top.node_arena_bytes, top.node_arena_used, top.node_arena_chunks,
          top.text_arena_bytes, top.text_arena_used, top.text_arena_chunks,
          top.hash_bytes,
          top.script_text_bytes, top.script_text_nodes,
          top.script_text_largest,
          top.style_text_bytes, top.style_text_nodes, top.style_text_largest,
          top.other_text_bytes, top.comment_bytes, top.attribute_value_bytes,
          top.nodes, top.elements, top.text_nodes, top.attributes,
          top.truncated ? "truncated" : "complete",
          top.lexbor_object_bytes, top.body_text_bytes,
          top.side_table_bytes, top.tokenizer_capacity, document_tokenizer_high_water(), rescue,
          frame_documents, frame_bytes,
          memory_report_less(category, named));
}

/* Image tables (top-level page and frames): decoded pixels each resource
   owns (not session-cache leases), canvases, owned encoded bytes. */
typedef struct {
    size_t images, pixel_bytes, pixel_count;
    size_t leased, leased_bytes;
    size_t canvases, canvas_bytes;
    size_t encoded_owned, encoded_shared;
    size_t svg_parse_bytes;
} MemoryReportImages;

static void memory_report_image_table(const ImageResources *table,
                                      MemoryReportImages *images,
                                      size_t totals[BUDGET_CATEGORY_COUNT])
{
    images->svg_parse_bytes += table->svg_parse_bytes;
    for (size_t i = 0; i < table->count; i++) {
        const ImageResource *item = &table->items[i];
        if (item->owns_pixels && item->pixels != NULL) {
            if (item->pixel_body != NULL) {
                images->leased++;
                size_t before = totals[BUDGET_CATEGORY_RESOURCE];
                memory_report_add_body(totals, item->pixel_body);
                images->leased_bytes +=
                    totals[BUDGET_CATEGORY_RESOURCE] - before;
            } else if (memory_report_first(&memory_report_seen,
                                           item->pixels)) {
                size_t charge = memory_report_charge_in(
                    item->pixels, BUDGET_CATEGORY_RESOURCE);
                if (item->is_canvas) {
                    images->canvases++;
                    images->canvas_bytes += charge;
                } else {
                    images->images++;
                    images->pixel_bytes += charge;
                    if (item->width > 0 && item->height > 0)
                        images->pixel_count +=
                            (size_t) item->width * (size_t) item->height;
                }
            }
        }
        if (item->owns_encoded && item->encoded != NULL) {
            if (item->encoded_body != NULL) {
                size_t before = totals[BUDGET_CATEGORY_RESOURCE];
                memory_report_add_body(totals, item->encoded_body);
                images->encoded_shared +=
                    totals[BUDGET_CATEGORY_RESOURCE] - before;
            } else if (memory_report_first(&memory_report_seen,
                                           item->encoded)) {
                images->encoded_owned += memory_report_charge_in(
                    item->encoded, BUDGET_CATEGORY_RESOURCE);
            }
        }
    }
}

/* The application's faces (trusted stb backend): file bytes and the
   parsed font info, both charged resource. */
static size_t memory_report_font_set(const FontSet *fonts)
{
    const FontFace *faces[] = {
        &fonts->sans, &fonts->sans_bold, &fonts->sans_italic, &fonts->serif,
        &fonts->serif_bold, &fonts->metric_sans, &fonts->metric_sans_bold
    };
    size_t total = 0;
    for (size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
        const FontFace *face = faces[i];
        if (!face->loaded || face->backend != FONT_FACE_BACKEND_STB_TRUSTED)
            continue;
        if (memory_report_first(&memory_report_seen, face->data))
            total += memory_report_charge_in(face->data,
                                             BUDGET_CATEGORY_RESOURCE);
        if (memory_report_first(&memory_report_seen, face->implementation))
            total += memory_report_charge_in(face->implementation,
                                             BUDGET_CATEGORY_RESOURCE);
    }
    return total;
}

static void memory_report_resource_and_session(const BrowserEngine *engine,
                                               const char *phase,
                                               BrowserMemoryPrint print)
{
    const NavigationPage *page = &engine->navigation.page;
    const BrowserSession *session = &engine->session;
    memset(&memory_report_seen, 0, sizeof(memory_report_seen));

    /* The HTTP cache first: bodies it shares are its. */
    size_t cache[BUDGET_CATEGORY_COUNT] = {0};
    size_t cache_entries = 0;
    for (size_t i = 0; i < BROWSER_CACHE_ENTRIES; i++) {
        const BrowserCacheEntry *entry = &session->cache[i];
        if (entry->body == NULL) continue;
        cache_entries++;
        memory_report_add_body(cache, entry->body);
        memory_report_add_body(cache, entry->classic_script_bytecode);
        memory_report_add_body(cache, entry->stylesheet_compiled_fragment);
        memory_report_add_body(cache, entry->stylesheet_parsed_ir);
        memory_report_add_body(cache, entry->decoded_image_pixels);
        (void) memory_report_add(cache, entry->response_url);
        (void) memory_report_add(cache, entry->resource_partition_key);
        (void) memory_report_add(cache, entry->resource_initiator_origin);
        (void) memory_report_add(cache, entry->resource_initiator_site);
        (void) memory_report_add(cache, entry->module_request_fragment);
        (void) memory_report_add(cache, entry->module_effective_url);
        (void) memory_report_add(cache, entry->module_initiator_origin);
        (void) memory_report_add(cache, entry->module_partition_key);
    }

    /* Stylesheet bodies the page pins beyond the cache. */
    size_t pins[BUDGET_CATEGORY_COUNT] = {0};
    for (size_t i = 0; i < page->stylesheet_resources.count
         && i < STYLESHEET_DOCUMENT_RESOURCE_LIMIT; i++)
        memory_report_add_body(pins, page->stylesheet_resources.items[i].body);

    MemoryReportImages images = {0};
    size_t image_bodies[BUDGET_CATEGORY_COUNT] = {0};
    memory_report_image_table(&page->images, &images, image_bodies);
    for (size_t i = 0; i < page->frame_count; i++)
        if (!page->frames[i].retired)
            memory_report_image_table(&page->frames[i].images, &images,
                                      image_bodies);

    /* Decoded images the tile caches keep (charged resource). */
    size_t render_decoded = 0;
    const TileCache *caches[] = {
        &engine->render, &engine->candidate_render,
        &engine->provisional_renders[0], &engine->provisional_renders[1]
    };
    for (size_t c = 0; c < sizeof(caches) / sizeof(caches[0]); c++) {
        for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++)
            render_decoded += memory_report_charge_in(
                caches[c]->decoded_images[i].pixels,
                BUDGET_CATEGORY_RESOURCE);
        render_decoded += memory_report_charge_in(
            caches[c]->held_image.pixels, BUDGET_CATEGORY_RESOURCE);
    }

    BudgetConcurrentPoolMetrics curl = {0};
    bool curl_live = fetch_curl_pool_metrics(&curl);
    size_t fetch_reserved = 0, fetch_reserved_max = 0, slots = 0,
        slots_max = 0;
    if (page->resource_scheduler != NULL)
        fetch_scheduler_reservation_state(page->resource_scheduler,
                                          &fetch_reserved,
                                          &fetch_reserved_max, &slots,
                                          &slots_max);
    size_t builtin_fonts = memory_report_font_set(&engine->fonts)
        + memory_report_font_set(&engine->staged_fonts);
    size_t resource =
        engine->budget.categories[BUDGET_CATEGORY_RESOURCE].current;
    size_t named = cache[BUDGET_CATEGORY_RESOURCE]
        + pins[BUDGET_CATEGORY_RESOURCE] + images.pixel_bytes
        + images.canvas_bytes + image_bodies[BUDGET_CATEGORY_RESOURCE]
        + images.encoded_owned
        + render_decoded + page->external_fonts.retained_encoded_bytes
        + builtin_fonts
        + (curl_live ? curl.limit : 0);
    print("tilefinch-resource-split: phase=%s resource=%zu "
          "image-pixels=%zu images=%zu pixels=%zu bpp=4 "
          "image-leases=%zu/%zu canvases=%zu/%zu image-encoded=%zu "
          "svg-parses=%zu render-decoded=%zu stylesheet-pins=%zu "
          "web-fonts=%zu builtin-fonts=%zu cache-bodies=%zu/%zu "
          "curl-pool=%zu/%zu/%zu fetch-reserved=%zu/%zu dedupe=%zu%s "
          "remainder=%zu\n",
          phase, resource, images.pixel_bytes, images.images,
          images.pixel_count, images.leased_bytes, images.leased,
          images.canvas_bytes, images.canvases,
          images.encoded_owned + images.encoded_shared,
          images.svg_parse_bytes, render_decoded,
          pins[BUDGET_CATEGORY_RESOURCE],
          page->external_fonts.retained_encoded_bytes, builtin_fonts,
          cache[BUDGET_CATEGORY_RESOURCE], cache_entries,
          curl.limit, curl.current, curl.peak,
          fetch_reserved, fetch_reserved_max,
          memory_report_seen.used,
          memory_report_seen.full ? "-full" : "",
          memory_report_less(resource, named));

    size_t bytecode[BROWSER_SCRIPT_BYTECODE_KINDS];
    for (size_t kind = 0; kind < BROWSER_SCRIPT_BYTECODE_KINDS; kind++)
        bytecode[kind] = browser_session_script_bytecode_bytes(
            session, (BrowserScriptBytecodeKind) kind);
    size_t tables = memory_report_charge_in(session->module_bytecode,
                                            BUDGET_CATEGORY_SESSION)
        + memory_report_charge_in(session->classic_bytecode,
                                  BUDGET_CATEGORY_SESSION)
        + memory_report_charge_in(session->lazy_bundle_records,
                                  BUDGET_CATEGORY_SESSION);
    size_t site = site_storage_budget_bytes(session);
    size_t disk_queue = browser_session_script_disk_queue_bytes(session);
    size_t session_bytes =
        engine->budget.categories[BUDGET_CATEGORY_SESSION].current;
    size_t bytecode_sum = 0;
    for (size_t kind = 0; kind < BROWSER_SCRIPT_BYTECODE_KINDS; kind++)
        bytecode_sum += bytecode[kind];
    size_t session_named = session->accounting_bytes + bytecode_sum + tables
        + session->cookie_bytes + session->cookie_long_path_bytes + site
        + cache[BUDGET_CATEGORY_SESSION] + pins[BUDGET_CATEGORY_SESSION]
        + image_bodies[BUDGET_CATEGORY_SESSION] + disk_queue;
    print("tilefinch-session-split: phase=%s session=%zu fixed=%zu "
          "module-bytecode=%zu classic-bytecode=%zu lazy-bundles=%zu "
          "bytecode-tables=%zu cookies=%zu cookie-long-paths=%zu "
          "site-storage=%zu cache=%zu shared-body-headers=%zu "
          "disk-queue=%zu remainder=%zu\n",
          phase, session_bytes, session->accounting_bytes,
          bytecode[BROWSER_SCRIPT_BYTECODE_MODULE],
          bytecode[BROWSER_SCRIPT_BYTECODE_CLASSIC],
          bytecode[BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE], tables,
          session->cookie_bytes, session->cookie_long_path_bytes, site,
          cache[BUDGET_CATEGORY_SESSION],
          pins[BUDGET_CATEGORY_SESSION]
              + image_bodies[BUDGET_CATEGORY_SESSION],
          disk_queue, memory_report_less(session_bytes, session_named));
}

static void memory_report_render(const BrowserEngine *engine,
                                 const char *phase, BrowserMemoryPrint print)
{
    const TileCache *caches[] = {
        &engine->render, &engine->candidate_render,
        &engine->provisional_renders[0], &engine->provisional_renders[1]
    };
    size_t base_tiles = 0, base_bytes = 0, ahead_tiles = 0, ahead_bytes = 0;
    size_t tables = 0, glyph = 0, glyph_entries = 0, scaled = 0;
    size_t overflow = 0, fixed = 0;
    for (size_t c = 0; c < sizeof(caches) / sizeof(caches[0]); c++) {
        const TileCache *cache = caches[c];
        for (size_t i = 0; cache->tiles != NULL && i < cache->tile_capacity;
             i++) {
            size_t charge = memory_report_charge_in(
                cache->tiles[i], BUDGET_CATEGORY_RENDER);
            if (charge == 0) continue;
            if (i < RENDER_TILE_BASE_CAPACITY) {
                base_tiles++;
                base_bytes += charge;
            } else {
                ahead_tiles++;
                ahead_bytes += charge;
            }
        }
        tables += memory_report_charge_in(cache->tiles,
                                          BUDGET_CATEGORY_RENDER)
            + memory_report_charge_in(cache->frame_scratch_tile,
                                      BUDGET_CATEGORY_RENDER)
            + memory_report_charge_in(cache->glyph_cache,
                                      BUDGET_CATEGORY_RENDER);
        for (size_t i = 0; cache->glyph_cache != NULL
             && i < cache->glyph_cache_capacity; i++) {
            const GlyphCacheEntry *entry = &cache->glyph_cache[i];
            if (!entry->valid) continue;
            glyph_entries++;
            glyph += memory_report_charge_in(entry->glyph.pixels,
                                             BUDGET_CATEGORY_RENDER)
                + memory_report_charge_in(entry->glyph.colors,
                                          BUDGET_CATEGORY_RENDER);
        }
        for (size_t i = 0; i < TILEFINCH_IMAGE_CACHE_ENTRIES; i++)
            scaled += memory_report_charge_in(cache->scaled_images[i].pixels,
                                              BUDGET_CATEGORY_RENDER);
        overflow += cache->overflow_cache_bytes;
        fixed += memory_report_charge_in(cache->fixed_pixels,
                                         BUDGET_CATEGORY_RENDER)
            + memory_report_charge_in(cache->fixed_alpha,
                                      BUDGET_CATEGORY_RENDER)
            + memory_report_charge_in(cache->fixed_row_first,
                                      BUDGET_CATEGORY_RENDER)
            + memory_report_charge_in(cache->fixed_row_last,
                                      BUDGET_CATEGORY_RENDER);
    }
    size_t framebuffer = memory_report_charge_in(engine->framebuffer,
                                                 BUDGET_CATEGORY_RENDER);
    size_t provisional = memory_report_charge_in(engine->provisional_frame,
                                                 BUDGET_CATEGORY_RENDER);
    size_t render = engine->budget.categories[BUDGET_CATEGORY_RENDER].current;
    size_t named = framebuffer + provisional + base_bytes + ahead_bytes
        + tables + glyph + scaled + overflow + fixed;
    print("tilefinch-render-split: phase=%s render=%zu framebuffer=%zu "
          "provisional-frame=%zu tiles=%zu/%zu reserved-tiles=%zu/%zu "
          "paint-ahead=%zu/%zu tile-tables=%zu glyph-cache=%zu/%zu "
          "glyph-pixels=%zu scaled-images=%zu overflow=%zu fixed-layer=%zu "
          "remainder=%zu\n",
          phase, render, framebuffer, provisional,
          base_bytes + ahead_bytes, base_tiles + ahead_tiles,
          base_bytes, base_tiles, ahead_bytes, ahead_tiles, tables,
          glyph_entries, engine->render.glyph_cache_bytes, glyph, scaled,
          overflow, fixed, memory_report_less(render, named));
}

/* An origin (scheme://host[:port]) hash: the log never names the page. */
static uint32_t memory_report_origin_hash(const char *url)
{
    if (url == NULL) return 0;
    const char *scheme = strstr(url, "://");
    size_t length = strlen(url);
    if (scheme != NULL) {
        const char *end = strpbrk(scheme + 3, "/?#");
        length = end == NULL ? length : (size_t) (end - url);
    }
    return style_fnv1a(STYLE_FNV1A_SEED, url, length, true);
}

/* Everything one realm holds in the JavaScript category that the split
   names: pool, source store, network and queue buffers, DOM handle tables
   and the ScriptRuntime itself. */
static size_t memory_report_realm_bytes(const ScriptRuntime *runtime,
                                        const ScriptRuntimeMemorySplit *split)
{
    return split->pool_budget_bytes + split->source_held_bytes
        + split->net_buffer_bytes + split->classic_queue_bytes
        + split->bridge_table_bytes
        + memory_report_charge_in(runtime, BUDGET_CATEGORY_JAVASCRIPT);
}

static void memory_report_frame_line(BrowserMemoryPrint print,
                                     const char *phase, const char *index,
                                     uint32_t origin, bool loaded,
                                     const ScriptRuntime *runtime,
                                     bool shares_page_realm,
                                     const PocDocument *document)
{
    /* A frame running in the page's realm reports realm=page and no heap
       of its own (the top line already counts that realm). */
    ScriptRuntimeMemorySplit split;
    bool live = !shares_page_realm
        && script_runtime_memory_split(runtime, &split);
    DocumentMemorySplit dom;
    bool parsed = document_memory_split(document, &dom);
    print("tilefinch-frame-realm: phase=%s index=%s origin-hash=%08x "
          "loaded=%d realm=%s heap-after-bootstrap=%zu heap-now=%zu "
          "heap-limit=%zu budget-javascript-bytes=%zu "
          "budget-after-bootstrap=%zu pool-cache=%zu source-store=%zu "
          "wrappers=%zu document-dom-bytes=%zu document-nodes=%zu\n",
          phase, index, (unsigned) origin, loaded ? 1 : 0,
          live ? "own" : shares_page_realm ? "page" : "none",
          live ? split.heap_after_bootstrap : 0,
          live ? split.heap_used : 0, live ? split.heap_limit : 0,
          live ? memory_report_realm_bytes(runtime, &split) : 0,
          live ? split.budget_after_bootstrap : 0,
          live ? split.pool_cached_bytes : 0,
          live ? split.source_held_bytes : 0, live ? split.wrappers : 0,
          parsed ? memory_report_document_bytes(&dom) : 0,
          parsed ? dom.nodes : 0);
}

static void memory_report_frames(const BrowserEngine *engine,
                                 const char *phase, BrowserMemoryPrint print)
{
    const NavigationSession *navigation = &engine->navigation;
    const NavigationPage *page = &navigation->page;
    size_t realms = 0, live = 0, heap = 0, after = 0, budget = 0;
    const NavigationEntry *entry = navigation_current(navigation);
    memory_report_frame_line(print, phase, "top",
                             memory_report_origin_hash(
                                 entry == NULL ? NULL : entry->url),
                             page->loaded, page->runtime, false,
                             &page->document);
    for (size_t i = 0; i < page->frame_count; i++) {
        const NavigationFrame *frame = &page->frames[i];
        if (frame->retired) continue;
        live++;
        char index[24];
        snprintf(index, sizeof(index), "%zu", i);
        memory_report_frame_line(print, phase, index,
                                 memory_report_origin_hash(frame->url),
                                 frame->loaded, frame->runtime,
                                 frame->runtime != NULL
                                     && frame->runtime == page->runtime,
                                 &frame->document);
        ScriptRuntimeMemorySplit split;
        if (frame->runtime != NULL
            && frame->runtime != page->runtime
            && script_runtime_memory_split(frame->runtime, &split)) {
            realms++;
            heap += split.heap_used;
            after += split.heap_after_bootstrap;
            budget += memory_report_realm_bytes(frame->runtime, &split);
        }
    }
    print("tilefinch-frame-realms: phase=%s frames=%zu live=%zu realms=%zu "
          "heap-after-bootstrap=%zu heap-now=%zu budget-javascript-bytes=%zu "
          "discovered=%zu loaded=%zu failed=%zu\n",
          phase, page->frame_count, live, realms, after, heap, budget,
          navigation->frames_discovered, navigation->frames_loaded,
          navigation->frames_failed);
}

void browser_engine_memory_census(const BrowserEngine *engine,
                                  const char *reason)
{
    if (engine == NULL || !engine->navigation_ready) return;
    const ScriptRuntime *realms[NAVIGATION_FRAME_LIMIT + 1];
    size_t count = memory_report_realms(engine, realms,
                                        sizeof(realms) / sizeof(realms[0]));
    for (size_t i = 0; i < count; i++)
        script_runtime_heap_detail_census((ScriptRuntime *) realms[i],
                                          reason);
}

void browser_engine_memory_report(const BrowserEngine *engine,
                                  const char *phase, bool heap_detail,
                                  BrowserMemoryPrint print)
{
    if (engine == NULL || print == NULL || !engine->navigation_ready) return;
    if (phase == NULL) phase = "unknown";
    memory_report_budget_at_peak(&engine->budget, phase, print);
    memory_report_javascript(engine, phase, print);
    memory_report_dom(engine, phase, print);
    memory_report_resource_and_session(engine, phase, print);
    memory_report_render(engine, phase, print);
    memory_report_frames(engine, phase, print);
#if defined(TILEFINCH_FRAME_LITE)
    tilefinch_frame_lite_print(print, phase);
#endif
    if (heap_detail) browser_engine_memory_census(engine, phase);
}
