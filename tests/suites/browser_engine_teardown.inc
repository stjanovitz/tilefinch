static int test_headless_repeated_teardown(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 8u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.tile_capacity = 0;
    static const char page[] = "<!doctype html><title>Headless</title><p>ok";
    for (size_t iteration = 0; iteration < 3; iteration++) {
        char error[128] = {0};
        BrowserEngine *engine = browser_engine_create(
            &config, error, sizeof(error));
        CHECK(engine != NULL
              && browser_engine_commit_html(
                  engine, "https://headless.test/", page,
                  sizeof(page) - 1, false)
              && browser_engine_controller(engine) != NULL
              && browser_engine_render_shell(engine) == NULL
              && browser_engine_shutdown(engine));
        BrowserEngineMetrics metrics;
        CHECK(browser_engine_metrics(engine, &metrics)
              && metrics.budget_current == 0
              && metrics.budget_active_allocations == 0);
        browser_engine_destroy(engine);
    }
    return 0;
}

static int test_single_active_engine(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 8u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.tile_capacity = 0;
    char error[128] = {0};
    BrowserEngine *first = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(first != NULL);

    BrowserEngine *rejected = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(rejected == NULL && strstr(error, "already active") != NULL);
    static const char first_page[] =
        "<!doctype html><title>First owner</title><p>still usable";
    CHECK(browser_engine_commit_html(
        first, "https://first.test/", first_page,
        sizeof(first_page) - 1, true));

    /* Releasing the first engine's process-global Lexbor ownership must make a
       later facade safe even while the shut-down control object still exists. */
    CHECK(browser_engine_shutdown(first));
    BrowserEngine *second = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(second != NULL);
    static const char second_page[] =
        "<!doctype html><title>Second owner</title><p>fresh allocator";
    CHECK(browser_engine_commit_html(
              second, "https://second.test/", second_page,
              sizeof(second_page) - 1, true)
          && strcmp(browser_engine_navigation(second)->page.document.title,
                    "Second owner") == 0
          && browser_engine_shutdown(second));
    browser_engine_destroy(second);
    browser_engine_destroy(first);
    return 0;
}

static int test_late_candidate_failure_transaction(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 2u * MIB;
    config.tile_capacity = 2;
    static const char stable_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Stable incumbent</title>"
        "<style>body{margin:3px;background:#fff;color:#111}"
        "#stable{display:flex;gap:4px;padding:5px}</style>"
        "<body><main id=stable><a href='#stable'>incumbent</a>"
        "<span>must survive</span></main>";
    static const char candidate_page[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Uncommitted candidate</title>"
        "<style>body{margin:11px;background:#123;color:#fed}"
        ".grid{display:flex;flex-wrap:wrap;gap:7px}.grid article{width:42%;"
        "padding:9px;border:2px solid #abc}.grid strong{display:block}</style>"
        "<body><section class=grid>"
        "<article><strong>candidate one</strong><p>different layout</p></article>"
        "<article><strong>candidate two</strong><p>must not leak</p></article>"
        "<article><strong>candidate three</strong><p>rollback probe</p></article>"
        "</section>";
    bool saw_style_failure = false;
    bool saw_layout_failure = false;
    bool saw_render_shell_failure = false;
    bool saw_history_failure = false;
    size_t failed_candidates = 0;
    for (size_t countdown = 0; countdown < 1024; countdown++) {
        char error[128] = {0};
        BrowserEngine *engine = browser_engine_create(
            &config, error, sizeof(error));
        CHECK(engine != NULL
              && browser_engine_commit_html(
                  engine, "https://stable.test/original", stable_page,
                  sizeof(stable_page) - 1, true)
              && browser_engine_render_frame(engine, NULL));
        NavigationSession *navigation = browser_engine_navigation(engine);
        Budget *budget = browser_engine_budget(engine);
        size_t pixel_count = 0;
        uint16_t *frame = browser_engine_framebuffer(engine, &pixel_count);
        const NavigationEntry *entry = navigation_current(navigation);
        CHECK(navigation != NULL && budget != NULL && frame != NULL
              && entry != NULL
              && strcmp(entry->url, "https://stable.test/original") == 0
              && strcmp(entry->title, "Stable incumbent") == 0);
        uint64_t stable_checksum = frame_checksum(frame, pixel_count);
        size_t stable_bytes = budget->current;
        size_t stable_commits = navigation->loads_committed;

        size_t injected_before = budget->injected_failure_count;
        budget_inject_failure_after(budget, countdown);
        bool committed = browser_engine_commit_html(
            engine, "https://candidate.test/rejected", candidate_page,
            sizeof(candidate_page) - 1, true);
        bool injected = budget->injected_failure_count != injected_before;
        budget_clear_failure_injection(budget);

        /* Some bounded indexes are optional and may tolerate their own failed
           allocation. Recreate the incumbent for every countdown so those
           successful candidates do not hide later mandatory failure sites. */
        if (committed) {
            CHECK(browser_engine_shutdown(engine));
            browser_engine_destroy(engine);
            if (!injected) break;
            continue;
        }
        CHECK(injected);
        failed_candidates++;
        saw_style_failure = saw_style_failure
            || strstr(navigation->last_error, "stylesheet") != NULL;
        saw_layout_failure = saw_layout_failure
            || strstr(navigation->last_error, "layout") != NULL;
        saw_render_shell_failure = saw_render_shell_failure
            || strstr(navigation->last_error,
                      "candidate commit preparation failed") != NULL;
        saw_history_failure = saw_history_failure
            || strstr(navigation->last_error, "history entry") != NULL;

        entry = navigation_current(navigation);
        const DocumentBacking *backing =
            browser_engine_document_backing(engine);
        CHECK(navigation->page.loaded
              && navigation->loads_committed == stable_commits
              && navigation->history_count == 1
              && navigation->history_index == 0
              && entry != NULL
              && strcmp(entry->url, "https://stable.test/original") == 0
              && strcmp(entry->title, "Stable incumbent") == 0
              && strcmp(navigation->page.document.title,
                        "Stable incumbent") == 0
              && document_backing_kind(backing) == DOCUMENT_BACKING_FULL
              && backing->full_document == &navigation->page.document
              && browser_engine_controller(engine) != NULL
              && browser_engine_render_shell(engine) != NULL
              && browser_engine_render_frame(engine, NULL)
              && frame_checksum(frame, pixel_count) == stable_checksum
              && budget->current == stable_bytes);
        CHECK(browser_engine_shutdown(engine));
        browser_engine_destroy(engine);
        if (saw_style_failure && saw_layout_failure
            && saw_render_shell_failure && saw_history_failure) {
            break;
        }
    }
    CHECK(failed_candidates != 0 && saw_style_failure
          && saw_layout_failure && saw_render_shell_failure
          && saw_history_failure);

    return 0;
}

static int test_allocator_owner_conflict_does_not_poison_engine(void)
{
    Budget external;
    budget_init(&external, 8u * MIB);
    CHECK(budget_install_lexbor(&external));
    static const char page[] =
        "<!doctype html><title>External owner</title><body>live";
    PocDocument document = {0};
    CHECK(document_parse(&document, &external, page, sizeof(page) - 1, 32));

    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 8u * MIB;
    config.tile_capacity = 0;
    char error[160] = {0};
    CHECK(browser_engine_create(&config, error, sizeof(error)) == NULL
          && strstr(error, "Lexbor") != NULL);

    document_destroy(&document);
    CHECK(external.current == 0 && budget_uninstall_lexbor(&external));
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_incumbent_teardown_cannot_run_author_javascript(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    char error[160] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char incumbent[] =
        "<!doctype html><title>Incumbent</title><body>old"
        "<script>globalThis.__tilefinchDescribeTimers=()=>{"
        "document.body.setAttribute('data-old-teardown','ran');"
        "return 'author override ran'}</script>";
    static const char candidate[] =
        "<!doctype html><title>Candidate</title><body>new";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://old-origin.test/", incumbent,
                 sizeof(incumbent) - 1, true)
          && browser_engine_commit_html(
                 engine, "https://new-origin.test/", candidate,
                 sizeof(candidate) - 1, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.runtime != NULL
          && script_runtime_evaluate_diagnostic(
                 navigation->page.runtime,
                 "globalThis.pocSummary="
                 "document.body.getAttribute('data-old-teardown')||'clean'",
                 "<teardown-author-code-probe>",
                 &navigation->page.script_result)
          && strcmp(navigation->page.script_result.summary, "clean") == 0
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}
