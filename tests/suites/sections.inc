static int test_sections(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);

    puts("test: experimental independently compressed section store");
    SectionStoreStreamBuilder rejected_builder;
    memset(&rejected_builder, 0xa5, sizeof(rejected_builder));
    CHECK(!section_store_stream_begin(
              &rejected_builder, NULL, &budget, 4096, 8192)
          && rejected_builder.store == NULL
          && !rejected_builder.active);
    section_store_stream_abort(&rejected_builder);
    static const char section_page[] =
        "<!doctype html><html><head><title>Section Store</title>"
        "<style>h2{color:#123456}</style></head><body>Prelude"
        "<!-- <h1>comment decoy</h1> -->"
        "<script>const decoy='<h2>script decoy</h2>';</script>"
        "<h1 id=alpha>Alpha</h1><p>first section first section first section</p>"
        "<h2 id=beta>Beta</h2><p class='remote compound' data-mode=wide>"
        "second section second section second section</p>"
        "<h2 id=gamma>Gamma</h2><p>third section third section third section</p>"
        "</body></html>";
    CompressedSectionStore section_store;
    CHECK(section_store_build(&section_store, &budget, section_page,
                              strlen(section_page), 4096, 8192));
    CHECK(section_store.section_count == 4
          && section_store.compressed_blocks == 1
          && section_store.stored_length < section_store.source_length
          && section_store.anchor_count == 3
          && sizeof(SectionStoreAnchor) == 16
          && section_store.index_work_units == 1
          && section_store.index_cooperative_yields == 2);
    for (size_t i = 0; i < section_store.block_count; i++) {
        CHECK(section_store.blocks[i].data != NULL
              && section_store.blocks[i].stored_length != 0);
    }
    Budget progress_budget;
    budget_init(&progress_budget, 4 * MIB);
    CompressedSectionStore progress_store = {0};
    SectionStoreStreamBuilder progress_builder = {0};
    TestSectionProgress progress = {.monotonic = true};
    CHECK(section_store_stream_begin(
              &progress_builder, &progress_store, &progress_budget,
              4096, 8192));
    section_store_stream_set_progress(
        &progress_builder, test_section_progress, &progress);
    CHECK(section_store_stream_append(
              &progress_builder, (const unsigned char *) section_page,
              strlen(section_page))
          && section_store_stream_finish(&progress_builder)
          && progress.monotonic && progress.calls == 2
          && progress.ready_calls == 2 && progress.final_calls == 1
          && progress.last_indexed_bytes == strlen(section_page)
          && progress_store.index_work_units == 1
          && progress_store.index_cooperative_yields == 2);
    section_store_destroy(&progress_store);
    CHECK(progress_budget.current == 0);

    Budget progress_cancel_budget;
    budget_init(&progress_cancel_budget, 4 * MIB);
    CompressedSectionStore progress_cancel_store = {0};
    SectionStoreStreamBuilder progress_cancel_builder = {0};
    TestSectionProgress progress_cancel = {
        .cancel_at = 1, .monotonic = true
    };
    CHECK(section_store_stream_begin(
              &progress_cancel_builder, &progress_cancel_store,
              &progress_cancel_budget, 4096, 8192));
    section_store_stream_set_progress(
        &progress_cancel_builder, test_section_progress, &progress_cancel);
    CHECK(section_store_stream_append(
              &progress_cancel_builder,
              (const unsigned char *) section_page, strlen(section_page))
          && !section_store_stream_finish(&progress_cancel_builder)
          && !progress_cancel_builder.active
          && progress_cancel.calls == 1
          && progress_cancel.final_calls == 0
          && progress_cancel_budget.current == 0);
    size_t anchor_section = SIZE_MAX;
    char anchor_tag_name[32];
    CHECK(section_store_find_anchor(&section_store, "alpha", 5,
                                    &anchor_section)
          && anchor_section == 1
          && section_store_find_anchor(&section_store, "beta", 4,
                                       &anchor_section)
          && anchor_section == 2
          && section_store_find_anchor(&section_store, "gamma", 5,
                                       &anchor_section)
          && anchor_section == 3
          && section_store_find_anchor_element(
                 &section_store, "beta", 4, &anchor_section,
                 anchor_tag_name)
          && anchor_section == 2
          && strcmp(anchor_tag_name, "h2") == 0
          && !section_store_find_anchor(&section_store, "missing", 7,
                                        &anchor_section));
    size_t selector_section = SIZE_MAX;
    char selector_tag_name[32];
    CHECK(section_store_selector_is_simple("p.remote[data-mode=wide]", 24)
          && section_store_selector_is_simple(".missing", 8)
          && !section_store_selector_is_simple("h2 + p", 6)
          && !section_store_selector_is_simple("h1, h2", 6)
          && section_store_find_simple_selector(
              &section_store, "p.remote[data-mode=wide]", 24,
              &selector_section, selector_tag_name, NULL)
          && selector_section == 2
          && strcmp(selector_tag_name, "p") == 0
          && section_store_find_simple_selector(
                 &section_store, "[data-mode]", 11, &selector_section,
                 selector_tag_name, NULL)
          && selector_section == 2
          && section_store_find_simple_selector(
                 &section_store, "#gamma", 6, &selector_section,
                 selector_tag_name, NULL)
          && selector_section == 3
          && strcmp(selector_tag_name, "h2") == 0
          && !section_store_find_simple_selector(
                 &section_store, "h2 + p", 6, &selector_section,
                 selector_tag_name, NULL)
          && !section_store_find_simple_selector(
                 &section_store, ".missing", 8, &selector_section,
                 selector_tag_name, NULL)
          && !section_store_simple_selector_maybe_matches_section(
                 &section_store, 1, "p.remote[data-mode=wide]", 24)
          && section_store_simple_selector_maybe_matches_section(
                 &section_store, 2, "p.remote[data-mode=wide]", 24)
          && !section_store_simple_selector_maybe_matches_section(
                 &section_store, 3, ".missing", 8));
    TilefinchRemoteSelectorMatch selector_matches[4];
    size_t selector_match_count = 0;
    CHECK(section_store_collect_simple_selector(
              &section_store, "[id]", 4, selector_matches, 4,
              &selector_match_count)
          && selector_match_count == 3
          && selector_matches[0].section_index == 1
          && strcmp(selector_matches[0].identifier, "alpha") == 0
          && selector_matches[1].section_index == 2
          && strcmp(selector_matches[1].identifier, "beta") == 0
          && selector_matches[2].section_index == 3
          && strcmp(selector_matches[2].identifier, "gamma") == 0
          && section_store_collect_simple_selector(
                 &section_store, "p", 1, selector_matches, 4,
                 &selector_match_count)
          && selector_match_count == 3
          && !section_store_collect_simple_selector(
                 &section_store, "[id]", 4, selector_matches, 2,
                 &selector_match_count));
    char *section_fragment = NULL;
    size_t section_fragment_length = 0;
    CHECK(section_store_extract_html(&section_store, 2, &section_fragment,
                                     &section_fragment_length));
    CHECK(strstr(section_fragment, "Section Store") != NULL
          && strstr(section_fragment, "Beta") != NULL
          && strstr(section_fragment, "Alpha") == NULL
          && strstr(section_fragment, "Gamma") == NULL);
    PocDocument section_document;
    CHECK(document_parse(&section_document, &budget, section_fragment,
                         section_fragment_length, 512));
    CHECK(strcmp(section_document.title, "Section Store") == 0);
    document_destroy(&section_document);
    budget_free(&budget, section_fragment);
    CHECK(section_store_extract_query_html(
              &section_store, 2, &section_fragment,
              &section_fragment_length));
    const char *query_start = strstr(
        section_fragment, "<!--tilefinch-semantic-section-start-->");
    const char *query_beta = strstr(section_fragment, "Beta");
    const char *query_end = strstr(
        section_fragment, "<!--tilefinch-semantic-section-end-->");
    CHECK(query_start != NULL && query_beta != NULL && query_end != NULL
          && query_start < query_beta && query_beta < query_end
          && strstr(section_fragment, "Alpha") == NULL
          && strstr(section_fragment, "Gamma") == NULL
          && document_parse(&section_document, &budget, section_fragment,
                            section_fragment_length, 512));
    document_destroy(&section_document);
    budget_free(&budget, section_fragment);
    CHECK(!section_store_extract_html(&section_store,
                                      section_store.section_count,
                                      &section_fragment,
                                      &section_fragment_length));
    SectionPager section_pager;
    CHECK(section_pager_init(&section_pager, &section_store, 0, 1));
    puts("test: section pager candidate selection is transactional");
    char *original_section_html = section_pager.current_html;
    size_t original_section_length = section_pager.current_length;
    size_t pager_bytes = budget.current;
    SectionPagerSelection pager_selection = {0};
    budget_inject_failure_after(&budget, 0);
    CHECK(!section_pager_prepare(
              &section_pager, 1, 1, &pager_selection)
          && section_pager.current_section == 0
          && section_pager.current_html == original_section_html
          && section_pager.current_length == original_section_length);
    budget_clear_failure_injection(&budget);
    CHECK(section_pager_prepare(&section_pager, 1, 1, &pager_selection)
          && pager_selection.active
          && strstr(pager_selection.html, "Alpha") != NULL
          && section_pager.current_section == 0
          && section_pager.current_html == original_section_html);
    section_pager_abort(&pager_selection);
    CHECK(!pager_selection.active
          && section_pager.current_section == 0
          && section_pager.current_html == original_section_html
          && section_pager.current_length == original_section_length
          && strstr(section_pager.current_html, "Prelude") != NULL
          && budget.current == pager_bytes);
    PocDocument retained_section = {0};
    CHECK(document_parse(
              &retained_section, &budget, section_pager.current_html,
              section_pager.current_length, 512)
          && strcmp(retained_section.title, "Section Store") == 0);
    document_destroy(&retained_section);
    CHECK(budget.current == pager_bytes);
    DocumentBacking section_backing;
    DocumentBackingSelection backing_selection = {0};
    CHECK(document_backing_init_sections(
              &section_backing, &section_store, &section_pager, NULL, NULL)
          && document_backing_prepare_section(
              &section_backing, 1, 1, &backing_selection)
          && document_backing_current_section(&section_backing) == 0);
    document_backing_abort_section(&backing_selection);
    CHECK(document_backing_current_section(&section_backing) == 0
          && section_backing.revision == 1
          && budget.current == pager_bytes);
    section_pager_record_height(&section_pager, 10);
    CHECK(section_pager_move(&section_pager, 1)
          && section_pager.current_section == 1
          && strstr(section_pager.current_html, "Alpha") != NULL);
    CHECK(section_pager_prefetch_pending(&section_pager));
    section_pager_record_height(&section_pager, 20);
    CHECK(section_pager_move(&section_pager, 1)
          && section_pager.current_section == 2
          && strstr(section_pager.current_html, "Beta") != NULL);
    CHECK(section_pager_prefetch_pending(&section_pager));
    char *current_before_prefetch_cancel = section_pager.current_html;
    char *prefetch_before_cancel = section_pager.prefetched_html;
    CHECK(section_pager.has_prefetch
          && section_pager.prefetched_section == 3
          && section_pager_prepare(
              &section_pager, 3, 1, &pager_selection)
          && pager_selection.uses_prefetch
          && pager_selection.html == prefetch_before_cancel
          && section_pager.current_html == current_before_prefetch_cancel);
    section_pager_abort(&pager_selection);
    CHECK(section_pager.current_section == 2
          && section_pager.current_html == current_before_prefetch_cancel
          && strstr(section_pager.current_html, "Beta") != NULL
          && section_pager.has_prefetch
          && section_pager.prefetched_html == prefetch_before_cancel
          && section_pager.prefetched_section == 3);
    section_pager_record_height(&section_pager, 30);
    size_t logical_offset = 0;
    CHECK(section_pager_known_logical_offset(&section_pager, 3,
                                             &logical_offset)
          && logical_offset == 60
          && section_pager.prefetches == 1
          && section_pager.prefetch_hits == 0);
    CHECK(section_pager_move(&section_pager, -1)
          && section_pager.current_section == 1
          && section_pager.prefetch_hits == 0);
    section_pager_destroy(&section_pager);
    section_store_destroy(&section_store);
    CHECK(budget.current == 0);

    puts("test: adaptive section routing delegates bounded documents");
    SectionRouteStream route = {0};
    SectionStoreStreamBuilder route_builder = {0};
    CHECK(section_route_stream_begin(
              &route, &route_builder, &section_store, &budget,
              4096, 16384, false)
          && section_route_stream_body(
                 &route, (const unsigned char *) section_page,
                 strlen(section_page))
          && section_route_stream_finish(&route)
          && !route.sectioned
          && route.length == strlen(section_page)
          && memcmp(route.buffer, section_page, route.length) == 0);
    SectionRouteStream split_route = {
        .budget = &budget,
        .byte_threshold = section_route_stream_threshold(&budget)
    };
    for (size_t i = 0; i < strlen(section_page); i++) {
        section_route_stream_scan(
            &split_route, (const unsigned char *) section_page + i, 1);
    }
    CHECK(split_route.element_count == route.element_count
          && split_route.attribute_count == route.attribute_count
          && split_route.maximum_depth == route.maximum_depth
          && split_route.depth == route.depth);

    static const char route_raw_page[] =
        "<html><head><script>const x='<fake data-x=1>';if(a < b)a++;"
        "</script><style>.x:before{content:'<fake data-y=2>'}</style>"
        "<title><fake data-z=3></title></head><body>"
        "<!-- <fake data-c=4> --><img><br><input><p>ok</p>"
        "<textarea><fake data-t=5></textarea>"
        "<iframe><fake data-f=6></iframe></body></html>";
    SectionRouteStream raw_route = {
        .budget = &budget,
        .byte_threshold = section_route_stream_threshold(&budget)
    };
    SectionRouteStream raw_split_route = {
        .budget = &budget,
        .byte_threshold = section_route_stream_threshold(&budget)
    };
    section_route_stream_scan(
        &raw_route, (const unsigned char *) route_raw_page,
        strlen(route_raw_page));
    for (size_t i = 0; i < strlen(route_raw_page); i++) {
        section_route_stream_scan(
            &raw_split_route,
            (const unsigned char *) route_raw_page + i, 1);
    }
    CHECK(raw_route.element_count == 12
          && raw_route.attribute_count == 0
          && raw_route.maximum_depth == 3
          && raw_route.depth == 0
          && raw_route.element_count == raw_split_route.element_count
          && raw_route.attribute_count == raw_split_route.attribute_count
          && raw_route.maximum_depth == raw_split_route.maximum_depth
          && raw_route.depth == raw_split_route.depth);
    section_route_stream_abort(&route);
    CHECK(budget.current == 0);

    size_t routed_length = 900u * 1024u;
    char *routed_page = malloc(routed_length);
    CHECK(routed_page != NULL);
    memcpy(routed_page, "<html><body>", 12);
    memset(routed_page + 12, 'r', routed_length - 26);
    memcpy(routed_page + routed_length - 14, "</body></html>", 14);
    CHECK(section_route_stream_begin(
              &route, &route_builder, &section_store, &budget,
              4096, 16384, false)
          && section_route_stream_body(
                 &route, (const unsigned char *) routed_page,
                 routed_length)
          && section_route_stream_finish(&route)
          && route.sectioned
          && section_store.source_length == routed_length
          && section_store.section_count > 1);
    free(routed_page);
    section_route_stream_abort(&route);
    section_store_destroy(&section_store);
    CHECK(budget.current == 0);

    size_t long_page_length = 96u * 1024u;
    char *long_page = malloc(long_page_length + 1);
    CHECK(long_page != NULL);
    memcpy(long_page, "<html><body>", 12);
    memset(long_page + 12, 'x', long_page_length - 26);
    memcpy(long_page + long_page_length - 14, "</body></html>", 14);
    long_page[long_page_length] = '\0';
    CHECK(section_store_build(&section_store, &budget, long_page,
                              long_page_length, 4096, 16384)
          && section_store.section_count == 6);
    for (size_t i = 0; i < section_store.section_count; i++) {
        CHECK(section_store.sections[i].source_length <= 16384);
    }
    CHECK(section_store_extract_html(&section_store, 5, &section_fragment,
                                     &section_fragment_length)
          && section_fragment_length < 17u * 1024u);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    free(long_page);
    CHECK(budget.current == 0);

    puts("test: dense markup receives a bounded element split");
    static const char dense_prefix[] = "<html><body><main>";
    static const char dense_item[] = "<span>x</span>";
    static const char dense_suffix[] = "</main></body></html>";
    const size_t dense_items = 2500;
    size_t dense_length = sizeof(dense_prefix) - 1
        + dense_items * (sizeof(dense_item) - 1)
        + sizeof(dense_suffix) - 1;
    char *dense_page = malloc(dense_length + 1);
    CHECK(dense_page != NULL);
    size_t dense_used = 0;
    memcpy(dense_page + dense_used, dense_prefix, sizeof(dense_prefix) - 1);
    dense_used += sizeof(dense_prefix) - 1;
    for (size_t i = 0; i < dense_items; i++) {
        memcpy(dense_page + dense_used, dense_item,
               sizeof(dense_item) - 1);
        dense_used += sizeof(dense_item) - 1;
    }
    memcpy(dense_page + dense_used, dense_suffix, sizeof(dense_suffix) - 1);
    dense_used += sizeof(dense_suffix) - 1;
    dense_page[dense_used] = '\0';
    CHECK(dense_used == dense_length
          && section_store_build(&section_store, &budget, dense_page,
                                 dense_length, 4096, 512 * 1024)
          && section_store.section_count == 5
          && section_store.sections[1].continuation
          && section_store.sections[2].continuation
          && section_store_extract_html(
                 &section_store, 1, &section_fragment,
                 &section_fragment_length)
          && strstr(section_fragment, "<main>") != NULL
          && strstr(section_fragment, "<span>x</span>") != NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    free(dense_page);
    CHECK(budget.current == 0);

    puts("test: dense layout island receives a finite element split");
    static const char dense_island_prefix[] =
        "<html><body><main style='display:flex'>";
    size_t dense_island_length = sizeof(dense_island_prefix) - 1
        + dense_items * (sizeof(dense_item) - 1)
        + sizeof(dense_suffix) - 1;
    char *dense_island_page = malloc(dense_island_length + 1);
    CHECK(dense_island_page != NULL);
    size_t dense_island_used = 0;
    memcpy(dense_island_page + dense_island_used, dense_island_prefix,
           sizeof(dense_island_prefix) - 1);
    dense_island_used += sizeof(dense_island_prefix) - 1;
    for (size_t i = 0; i < dense_items; i++) {
        memcpy(dense_island_page + dense_island_used, dense_item,
               sizeof(dense_item) - 1);
        dense_island_used += sizeof(dense_item) - 1;
    }
    memcpy(dense_island_page + dense_island_used, dense_suffix,
           sizeof(dense_suffix) - 1);
    dense_island_used += sizeof(dense_suffix) - 1;
    dense_island_page[dense_island_used] = '\0';
    CHECK(dense_island_used == dense_island_length
          && section_store_build(
                 &section_store, &budget, dense_island_page,
                 dense_island_length, 4096, 512 * 1024)
          && section_store.section_count == 5
          && section_store.sections[1].continuation
          && section_store_extract_html(
                 &section_store, 1, &section_fragment,
                 &section_fragment_length)
          && strstr(section_fragment, "display:flex") != NULL
          && strstr(section_fragment, "<span>x</span>") != NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    free(dense_island_page);
    CHECK(budget.current == 0);

    puts("test: forced section splits retain live ancestor context");
    static const char forced_prefix[] =
        "<html><body><main class=outer><section data-scope=forced>";
    static const char forced_middle[] =
        "<div class=late><p class=copy>";
    static const char forced_suffix[] =
        "</p></div></section></main></body></html>";
    size_t forced_length = sizeof(forced_prefix) - 1 + 6000
        + sizeof(forced_middle) - 1 + 14000 + sizeof(forced_suffix) - 1;
    char *forced_page = malloc(forced_length + 1);
    CHECK(forced_page != NULL);
    size_t forced_used = 0;
    memcpy(forced_page + forced_used, forced_prefix,
           sizeof(forced_prefix) - 1);
    forced_used += sizeof(forced_prefix) - 1;
    memset(forced_page + forced_used, 'a', 6000);
    forced_used += 6000;
    memcpy(forced_page + forced_used, forced_middle,
           sizeof(forced_middle) - 1);
    forced_used += sizeof(forced_middle) - 1;
    memset(forced_page + forced_used, 'b', 14000);
    forced_used += 14000;
    memcpy(forced_page + forced_used, forced_suffix,
           sizeof(forced_suffix) - 1);
    forced_used += sizeof(forced_suffix) - 1;
    forced_page[forced_used] = '\0';
    CHECK(forced_used == forced_length
          && section_store_build(&section_store, &budget, forced_page,
                                 forced_length, 4096, 8192)
          && section_store.section_count == 3
          && section_store.sections[0].source_length < 8192
          && section_store.sections[1].continuation
          && section_store.sections[2].continuation);
    size_t forced_end = section_store.prefix_length;
    for (size_t i = 0; i < section_store.section_count; i++) {
        CHECK(section_store.sections[i].source_offset == forced_end
              && section_store.sections[i].source_length <= 8192);
        forced_end += section_store.sections[i].source_length;
    }
    CHECK(forced_end == section_store.source_length
          && section_store_extract_html(&section_store, 1,
                                        &section_fragment,
                                        &section_fragment_length)
          && strstr(section_fragment, "<main class=outer>") != NULL
          && strstr(section_fragment,
                    "<section data-scope=forced>") != NULL
          && strstr(section_fragment, "<div class=late>") != NULL
          && strstr(section_fragment, "<p class=copy>") != NULL);
    budget_free(&budget, section_fragment);
    CHECK(section_store_extract_html(&section_store, 2,
                                     &section_fragment,
                                     &section_fragment_length)
          && strstr(section_fragment, "<main class=outer>") != NULL
          && strstr(section_fragment,
                    "<section data-scope=forced>") != NULL
          && strstr(section_fragment, "<div class=late>") != NULL
          && strstr(section_fragment, "<p class=copy>") != NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    free(forced_page);
    CHECK(budget.current == 0);

    puts("test: late layout island receives a fresh bounded section");
    static const char island_prefix[] = "<html><body><main><p>";
    static const char island_open[] =
        "</p><div class=island style='display:flex'>"
        "<span class=first>";
    static const char island_middle[] =
        "</span><span class=second>";
    static const char island_suffix[] =
        "</span></div></main></body></html>";
    size_t island_length = sizeof(island_prefix) - 1 + 5000
        + sizeof(island_open) - 1 + 2500 + sizeof(island_middle) - 1
        + 2500 + sizeof(island_suffix) - 1;
    char *late_island_page = malloc(island_length + 1);
    CHECK(late_island_page != NULL);
    size_t island_used = 0;
    memcpy(late_island_page + island_used, island_prefix,
           sizeof(island_prefix) - 1);
    island_used += sizeof(island_prefix) - 1;
    memset(late_island_page + island_used, 'p', 5000);
    island_used += 5000;
    memcpy(late_island_page + island_used, island_open,
           sizeof(island_open) - 1);
    island_used += sizeof(island_open) - 1;
    memset(late_island_page + island_used, 'a', 2500);
    island_used += 2500;
    memcpy(late_island_page + island_used, island_middle,
           sizeof(island_middle) - 1);
    island_used += sizeof(island_middle) - 1;
    memset(late_island_page + island_used, 'b', 2500);
    island_used += 2500;
    memcpy(late_island_page + island_used, island_suffix,
           sizeof(island_suffix) - 1);
    island_used += sizeof(island_suffix) - 1;
    late_island_page[island_used] = '\0';
    CHECK(island_used == island_length
          && section_store_build(&section_store, &budget, late_island_page,
                                 island_length, 4096, 8192)
          && section_store.section_count == 2
          && !section_store.sections[0].continuation
          && !section_store.sections[1].continuation
          && section_store.sections[0].source_length < 8192
          && section_store.sections[1].source_length < 8192
          && section_store_extract_html(&section_store, 1,
                                        &section_fragment,
                                        &section_fragment_length)
          && strstr(section_fragment, "display:flex") != NULL
          && strstr(section_fragment, "class=first") != NULL
          && strstr(section_fragment, "class=second") != NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    free(late_island_page);
    CHECK(budget.current == 0);

    puts("test: moderately oversized layout island stays intact");
    static const char oversized_island_prefix[] =
        "<html><body><div style='display:grid'><span id=island-start>";
    static const char oversized_island_middle[] =
        "</span><span id=island-end>";
    static const char oversized_island_suffix[] =
        "</span></div><h2 id=after-island>After</h2>"
        "<p>Tail</p></body></html>";
    size_t oversized_island_length = sizeof(oversized_island_prefix) - 1
        + 12000 + sizeof(oversized_island_middle) - 1 + 10000
        + sizeof(oversized_island_suffix) - 1;
    char *oversized_island_page = malloc(oversized_island_length + 1);
    CHECK(oversized_island_page != NULL);
    size_t oversized_island_used = 0;
    memcpy(oversized_island_page + oversized_island_used,
           oversized_island_prefix, sizeof(oversized_island_prefix) - 1);
    oversized_island_used += sizeof(oversized_island_prefix) - 1;
    memset(oversized_island_page + oversized_island_used, 'a', 12000);
    oversized_island_used += 12000;
    memcpy(oversized_island_page + oversized_island_used,
           oversized_island_middle, sizeof(oversized_island_middle) - 1);
    oversized_island_used += sizeof(oversized_island_middle) - 1;
    memset(oversized_island_page + oversized_island_used, 'b', 10000);
    oversized_island_used += 10000;
    memcpy(oversized_island_page + oversized_island_used,
           oversized_island_suffix, sizeof(oversized_island_suffix) - 1);
    oversized_island_used += sizeof(oversized_island_suffix) - 1;
    oversized_island_page[oversized_island_used] = '\0';
    CHECK(oversized_island_used == oversized_island_length
          && section_store_build(
                 &section_store, &budget, oversized_island_page,
                 oversized_island_length, 4096, 8192)
          && section_store.section_count == 2
          && section_store.sections[0].source_length > 8192
          && section_store.sections[0].source_length <= 4 * 8192
          && section_store_extract_html(
                 &section_store, 0, &section_fragment,
                 &section_fragment_length)
          && strstr(section_fragment, "id=island-start") != NULL
          && strstr(section_fragment, "id=island-end") != NULL
          && strstr(section_fragment, "id=after-island") == NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    free(oversized_island_page);
    CHECK(budget.current == 0);

    puts("test: pathological layout island remains hard bounded");
    size_t pathological_island_length = sizeof(oversized_island_prefix) - 1
        + 40000 + sizeof(oversized_island_middle) - 1 + 40000
        + sizeof(oversized_island_suffix) - 1;
    char *pathological_island_page = malloc(
        pathological_island_length + 1);
    CHECK(pathological_island_page != NULL);
    size_t pathological_island_used = 0;
    memcpy(pathological_island_page + pathological_island_used,
           oversized_island_prefix, sizeof(oversized_island_prefix) - 1);
    pathological_island_used += sizeof(oversized_island_prefix) - 1;
    memset(pathological_island_page + pathological_island_used, 'a', 40000);
    pathological_island_used += 40000;
    memcpy(pathological_island_page + pathological_island_used,
           oversized_island_middle, sizeof(oversized_island_middle) - 1);
    pathological_island_used += sizeof(oversized_island_middle) - 1;
    memset(pathological_island_page + pathological_island_used, 'b', 40000);
    pathological_island_used += 40000;
    memcpy(pathological_island_page + pathological_island_used,
           oversized_island_suffix, sizeof(oversized_island_suffix) - 1);
    pathological_island_used += sizeof(oversized_island_suffix) - 1;
    pathological_island_page[pathological_island_used] = '\0';
    CHECK(pathological_island_used == pathological_island_length
          && section_store_build(
                 &section_store, &budget, pathological_island_page,
                 pathological_island_length, 4096, 8192)
          && section_store.section_count >= 4);
    bool saw_pathological_continuation = false;
    for (size_t i = 0; i < section_store.section_count; i++) {
        CHECK(section_store.sections[i].source_length <= 4 * 8192);
        saw_pathological_continuation |=
            section_store.sections[i].continuation;
    }
    CHECK(saw_pathological_continuation);
    section_store_destroy(&section_store);
    free(pathological_island_page);
    CHECK(budget.current == 0);

    puts("test: forced section split avoids nearby raw text");
    static const char raw_prefix[] =
        "<html><body><main><p>";
    static const char raw_open[] = "</p><script>const payload='";
    static const char raw_close[] =
        "<h2 id=raw-decoy>not markup</h2>';</script>"
        "<h2 id=raw-real>Real heading</h2><p>Tail</p></main></body></html>";
    size_t raw_length = sizeof(raw_prefix) - 1 + 6000
        + sizeof(raw_open) - 1 + 3000 + sizeof(raw_close) - 1;
    char *raw_page = malloc(raw_length + 1);
    CHECK(raw_page != NULL);
    size_t raw_used = 0;
    memcpy(raw_page + raw_used, raw_prefix, sizeof(raw_prefix) - 1);
    raw_used += sizeof(raw_prefix) - 1;
    memset(raw_page + raw_used, 't', 6000);
    raw_used += 6000;
    memcpy(raw_page + raw_used, raw_open, sizeof(raw_open) - 1);
    raw_used += sizeof(raw_open) - 1;
    memset(raw_page + raw_used, 'r', 3000);
    raw_used += 3000;
    memcpy(raw_page + raw_used, raw_close, sizeof(raw_close) - 1);
    raw_used += sizeof(raw_close) - 1;
    raw_page[raw_used] = '\0';
    CHECK(raw_used == raw_length
          && section_store_build(&section_store, &budget, raw_page,
                                 raw_length, 4096, 8192)
          && section_store.section_count == 3
          && section_store.anchor_count == 1
          && section_store.sections[0].source_length < 8192
          && section_store_extract_html(&section_store, 1,
                                        &section_fragment,
                                        &section_fragment_length)
          && strstr(section_fragment, "<script>const payload='") != NULL
          && strstr(section_fragment, "</script>") != NULL
          && strstr(section_fragment, "id=raw-real") == NULL
          && section_store_find_anchor(&section_store, "raw-real", 8,
                                       &anchor_section)
          && anchor_section == 2
          && !section_store_find_anchor(&section_store, "raw-decoy", 9,
                                        &anchor_section)
          && section_store_find_simple_selector(
                 &section_store, "#raw-real", 9, &selector_section,
                 selector_tag_name, NULL)
          && selector_section == 2
          && !section_store_find_simple_selector(
                 &section_store, "#raw-decoy", 10, &selector_section,
                 selector_tag_name, NULL));
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    free(raw_page);
    CHECK(budget.current == 0);

    puts("test: compressed section stream abort and allocation recovery");
    Budget section_abort_budget;
    budget_init(&section_abort_budget, 4 * MIB);
    SectionStoreStreamBuilder aborted_builder = {0};
    CompressedSectionStore aborted_store = {0};
    CHECK(section_store_stream_begin(
              &aborted_builder, &aborted_store, &section_abort_budget,
              4096, 8192)
          && section_store_stream_append(
              &aborted_builder, (const unsigned char *) section_page,
              strlen(section_page) / 2));
    section_store_stream_abort(&aborted_builder);
    section_store_stream_abort(&aborted_builder);
    section_store_destroy(&aborted_store);
    CHECK(section_abort_budget.current == 0);
    bool section_fault_sweep_completed = false;
    for (size_t failure_at = 0; failure_at < 128; failure_at++) {
        Budget fault_budget;
        budget_init(&fault_budget, 4 * MIB);
        CompressedSectionStore fault_store = {0};
        budget_inject_failure_after(&fault_budget, failure_at);
        bool built = section_store_build(
            &fault_store, &fault_budget, section_page, strlen(section_page),
            4096, 8192);
        section_store_destroy(&fault_store);
        CHECK(fault_budget.current == 0);
        if (built) {
            section_fault_sweep_completed = true;
            break;
        }
    }
    CHECK(section_fault_sweep_completed);

    puts("test: compressed section lexical block boundaries");
    size_t boundary_page_capacity = 9000;
    char *boundary_page = malloc(boundary_page_capacity);
    CHECK(boundary_page != NULL);
    size_t boundary_length = 0;
    static const char boundary_prefix[] = "<html><body>";
    memcpy(boundary_page, boundary_prefix, sizeof(boundary_prefix) - 1);
    boundary_length = sizeof(boundary_prefix) - 1;
    memset(boundary_page + boundary_length, 'x', 4094 - boundary_length);
    boundary_length = 4094;
    static const char boundary_comment[] =
        "<!-- <h2 id=comment-decoy>Comment decoy</h2> -->";
    memcpy(boundary_page + boundary_length, boundary_comment,
           sizeof(boundary_comment) - 1);
    boundary_length += sizeof(boundary_comment) - 1;
    memset(boundary_page + boundary_length, 'y', 8190 - boundary_length);
    boundary_length = 8190;
    static const char boundary_tail[] =
        "<script>const decoy='<h2 id=script-decoy>Script decoy</h2>';"
        "</script><h2 id=boundary>Boundary</h2></body></html>";
    memcpy(boundary_page + boundary_length, boundary_tail,
           sizeof(boundary_tail) - 1);
    boundary_length += sizeof(boundary_tail) - 1;
    CHECK(boundary_length < boundary_page_capacity
          && section_store_build(&section_store, &budget, boundary_page,
                                 boundary_length, 4096, 16384)
          && section_store.section_count == 2
          && section_store.anchor_count == 1
          && section_store_find_anchor(&section_store, "boundary", 8,
                                       &anchor_section)
          && anchor_section == 1
          && !section_store_find_anchor(&section_store, "comment-decoy", 13,
                                        &anchor_section)
          && !section_store_find_anchor(&section_store, "script-decoy", 12,
                                        &anchor_section));
    section_store_destroy(&section_store);
    free(boundary_page);
    CHECK(budget.current == 0);

    puts("test: compressed section ancestor style context");
    static const char context_page[] =
        "<!doctype html><html><head><style>"
        ".article-theme h2{color:#2468ac}"
        "</style></head><body><div class=article-theme data-scope=retained>"
        "<h1 id=context-first>First</h1><p>First section</p>"
        "<h2 id=context-target>Target</h2><p>Target section</p>"
        "</div></body></html>";
    CHECK(section_store_build(&section_store, &budget, context_page,
                              strlen(context_page), 4096, 8192)
          && section_store.section_count == 2
          && section_store.context_tag_count >= 1
          && section_store_extract_html(&section_store, 1,
                                        &section_fragment,
                                        &section_fragment_length)
          && strstr(section_fragment,
                    "<div class=article-theme data-scope=retained>") != NULL);
    PocDocument context_document;
    Stylesheet context_sheet;
    CHECK(document_parse(&context_document, &budget, section_fragment,
                         section_fragment_length, 128)
          && stylesheet_build(&context_sheet, &budget, &context_document, 480));
    lxb_dom_node_t *context_target = find_id(
        lxb_dom_interface_node(context_document.html), "context-target");
    ComputedStyle context_target_style = style_for_node(
        &context_sheet, context_target, NULL);
    CHECK(context_target != NULL && context_target->parent != NULL
          && attribute_equals(context_target->parent, "class",
                              "article-theme")
          && context_target_style.color == 0x2468ac);
    stylesheet_destroy(&context_sheet);
    document_destroy(&context_document);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    CHECK(budget.current == 0);

    puts("test: compressed section keeps structural layout islands intact");
    static const char island_page[] =
        "<!doctype html><html><body>"
        "<div style='display:flex'><h1 id=flex-a>Flex A</h1>"
        "<h2 id=flex-b>Flex B</h2></div>"
        "<table><tr><td><h3 id=table-a>Table A</h3></td></tr></table>"
        "<h4 id=outside>Outside</h4><p>Tail</p></body></html>";
    CHECK(section_store_build(&section_store, &budget, island_page,
                              strlen(island_page), 4096, 8192)
          && section_store.section_count == 2
          && section_store_extract_html(&section_store, 0,
                                        &section_fragment,
                                        &section_fragment_length)
          && strstr(section_fragment, "id=flex-a") != NULL
          && strstr(section_fragment, "id=flex-b") != NULL
          && strstr(section_fragment, "id=table-a") != NULL
          && strstr(section_fragment, "id=outside") == NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    CHECK(budget.current == 0);

    puts("test: compressed section keeps embedded-CSS layout islands intact");
    static const char styled_island_page[] =
        "<!doctype html><html><head><style>"
        ".cards, #utility, nav { display: grid; }"
        "@media (max-width:600px){.responsive{display:flex}}"
        "section.compound.one[data-mode=wide]{display:grid}"
        ".unrelated:hover{display:flex}"
        "</style></head><body>"
        "<div class='wide cards'><h1 id=css-a>CSS A</h1>"
        "<h2 id=css-b>CSS B</h2></div>"
        "<section id=utility><h3 id=css-c>CSS C</h3>"
        "<h4 id=css-d>CSS D</h4></section>"
        "<aside class=responsive><h4 id=css-e>CSS E</h4>"
        "<h4 id=css-f>CSS F</h4></aside>"
        "<section class='compound one' data-mode=wide>"
        "<h4 id=css-g>CSS G</h4><h4 id=css-h>CSS H</h4></section>"
        "<h5 id=css-outside>Outside</h5><p>Tail</p></body></html>";
    CHECK(section_store_build(&section_store, &budget, styled_island_page,
                              strlen(styled_island_page), 4096, 8192)
          && section_store.section_count == 2
          && section_store_extract_html(&section_store, 0,
                                        &section_fragment,
                                        &section_fragment_length)
          && strstr(section_fragment, "id=css-a") != NULL
          && strstr(section_fragment, "id=css-b") != NULL
          && strstr(section_fragment, "id=css-c") != NULL
          && strstr(section_fragment, "id=css-d") != NULL
          && strstr(section_fragment, "id=css-e") != NULL
          && strstr(section_fragment, "id=css-f") != NULL
          && strstr(section_fragment, "id=css-g") != NULL
          && strstr(section_fragment, "id=css-h") != NULL
          && strstr(section_fragment, "id=css-outside") == NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    CHECK(budget.current == 0);

    puts("test: compressed section reindexes loaded layout selectors");
    static const char external_island_page[] =
        "<!doctype html><html><body><div class=external-flex>"
        "<h1 id=external-a>External A</h1><p>First</p>"
        "<h2 id=external-b>External B</h2><p>Second</p></div>"
        "<h2 id=external-outside>Outside</h2><p>Tail</p></body></html>";
    static const char *const external_layout_selectors[] = {
        ".external-flex"
    };
    CHECK(section_store_build(
              &section_store, &budget, external_island_page,
              strlen(external_island_page), 4096, 8192)
          && section_store.section_count == 3
          && section_store_reindex_layout_selectors(
                 &section_store, external_layout_selectors, 1)
          && section_store.section_count == 2
          && section_store_find_anchor(
                 &section_store, "external-b", 10, &anchor_section)
          && anchor_section == 0
          && section_store_find_anchor(
                 &section_store, "external-outside", 16, &anchor_section)
          && anchor_section == 1
          && section_store_extract_html(
                 &section_store, 0, &section_fragment,
                 &section_fragment_length)
          && strstr(section_fragment, "id=external-a") != NULL
          && strstr(section_fragment, "id=external-b") != NULL
          && strstr(section_fragment, "id=external-outside") == NULL);
    budget_free(&budget, section_fragment);
    section_store_destroy(&section_store);
    CHECK(budget.current == 0);

    puts("test: pager teardown survives a growing store reindex");
    static const char growing_prefix[] =
        "<!doctype html><html><body><p>";
    static const char growing_suffix[] =
        "</p><div class=growing-island><span>island</span></div>"
        "<p>tail</p></body></html>";
    size_t growing_length = sizeof(growing_prefix) - 1 + 4400
        + sizeof(growing_suffix) - 1;
    char *growing_page = malloc(growing_length + 1);
    CHECK(growing_page != NULL);
    memcpy(growing_page, growing_prefix, sizeof(growing_prefix) - 1);
    memset(growing_page + sizeof(growing_prefix) - 1, 'x', 4400);
    memcpy(growing_page + sizeof(growing_prefix) - 1 + 4400,
           growing_suffix, sizeof(growing_suffix));
    static const char *const growing_layout_selectors[] = {
        ".growing-island"
    };
    SectionPager growing_pager;
    SectionPagerSelection stale_selection = {0};
    CHECK(section_store_build(
              &section_store, &budget, growing_page, growing_length, 4096, 8192)
          && section_store.section_count == 1
          && section_pager_init(&growing_pager, &section_store, 0, 0)
          && growing_pager.state_capacity == 1
          && section_pager_record_focus(&growing_pager, "focus", 5)
          && section_store_reindex_layout_selectors(
                 &section_store, growing_layout_selectors, 1)
          && section_store.section_count == 2
          && growing_pager.state_capacity == 1
          && section_pager_focus(&growing_pager, 0) == NULL
          && !section_pager_prepare(
                 &growing_pager, 1, 1, &stale_selection));
    section_pager_abort(&stale_selection);
    section_pager_destroy(&growing_pager);
    section_store_destroy(&section_store);
    free(growing_page);
    CHECK(budget.current == 0);

    puts("test: same-count reindex and pager ABA fail closed");
    static const char revision_prefix[] =
        "<!doctype html><html><body><div class=one>"
        "<h1>A</h1><h2>B</h2></div><p>";
    static const char revision_suffix[] =
        "</p><div class=two><h2>C</h2><h2>D</h2></div>"
        "<h2>E</h2></body></html>";
    size_t revision_length = sizeof(revision_prefix) - 1 + 4500
        + sizeof(revision_suffix) - 1;
    char *revision_page = malloc(revision_length + 1);
    CHECK(revision_page != NULL);
    memcpy(revision_page, revision_prefix, sizeof(revision_prefix) - 1);
    memset(revision_page + sizeof(revision_prefix) - 1, 'x', 4500);
    memcpy(revision_page + sizeof(revision_prefix) - 1 + 4500,
           revision_suffix, sizeof(revision_suffix));
    static const char *const revision_one[] = {".one"};
    static const char *const revision_two[] = {".two"};
    SectionPager revision_pager;
    DocumentBacking revision_backing;
    SectionPagerSelection stale_same = {0}, newer_same = {0};
    CHECK(section_store_build(
              &section_store, &budget, revision_page, revision_length,
              4096, 8192)
          && section_store_reindex_layout_selectors(
                 &section_store, revision_one, 1)
          && section_store.section_count == 4
          && section_pager_init(
                 &revision_pager, &section_store, 1, 0)
          && document_backing_init_sections(
                 &revision_backing, &section_store, &revision_pager,
                 NULL, NULL)
          && section_pager_prepare(
                 &revision_pager, 1, 1, &stale_same)
          && section_pager_prepare(
                 &revision_pager, 1, -1, &newer_same)
          && section_pager_commit(&newer_same)
          && !section_pager_commit(&stale_same));
    section_pager_abort(&stale_same);
    uint64_t prior_index_revision = section_store.index_revision;
    CHECK(section_pager_record_focus(&revision_pager, "focus", 5)
          && section_store_reindex_layout_selectors(
                 &section_store, revision_two, 1)
          && section_store.section_count == 4
          && section_store.index_revision != prior_index_revision
          && !section_pager_valid(&revision_pager)
          && !document_backing_valid(&revision_backing)
          && section_pager_focus(&revision_pager, 1) == NULL
          && !section_pager_prepare(
                 &revision_pager, 1, 0, &stale_same));
    section_pager_abort(&stale_same);
    section_pager_destroy(&revision_pager);
    section_store_destroy(&section_store);
    free(revision_page);
    CHECK(budget.current == 0);

    puts("test: compressed section transport cancellation matrix");
    static const size_t cancellation_points[] = {0, 1, 31, 4095, 4096};
    for (size_t i = 0;
         i < sizeof(cancellation_points) / sizeof(cancellation_points[0]);
         i++) {
        Budget cancel_budget;
        budget_init(&cancel_budget, 4 * MIB);
        SectionStoreStreamBuilder cancel_builder = {0};
        CompressedSectionStore cancel_store = {0};
        size_t cut = cancellation_points[i];
        CHECK(section_store_stream_begin(
                  &cancel_builder, &cancel_store, &cancel_budget, 4096, 8192)
              && section_store_stream_append(
                  &cancel_builder, (const unsigned char *) section_page,
                  cut < strlen(section_page) ? cut : strlen(section_page)));
        section_store_stream_abort(&cancel_builder);
        section_store_stream_abort(&cancel_builder);
        section_store_destroy(&cancel_store);
        CHECK(cancel_budget.current == 0);
    }
    CHECK(test_section_store_transport_fault(23, 0));
    CHECK(test_section_store_transport_fault(0, 29));

    Budget tiny;
    puts("test: tiny-budget rejection");
    budget_init(&tiny, 1 * MIB);
