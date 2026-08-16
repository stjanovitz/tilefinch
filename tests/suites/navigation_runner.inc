int main(void)
{
#define RUN_TEST(test) do {                                                  \
    if (!(test)()) {                                                         \
        fprintf(stderr, "navigation load test failed: %s\n", #test);      \
        return 1;                                                            \
    }                                                                        \
} while (0)
    RUN_TEST(test_pump_and_cancel);
    RUN_TEST(test_progressive_preview_is_bounded_and_transient);
    RUN_TEST(test_failed_candidate_rolls_back_presented_preview);
    RUN_TEST(test_progressive_preview_waits_for_external_visuals);
    RUN_TEST(test_markup_priority_survives_preview_pressure);
    RUN_TEST(test_background_images_continue_after_useful_paint);
    RUN_TEST(test_background_image_failure_retains_visible_prefix);
    RUN_TEST(test_web_fonts_begin_after_fallback_layout);
    RUN_TEST(test_streaming_preview_precedes_eof);
    RUN_TEST(test_streaming_preview_skips_inert_prefix_layout);
    RUN_TEST(test_streaming_preview_pressure_preserves_final_commit);
    RUN_TEST(test_streaming_stylesheet_continuation_converges);
    RUN_TEST(test_streaming_preview_rejects_incomplete_external_css);
    RUN_TEST(test_streaming_preview_cancel_is_transient);
    RUN_TEST(test_parser_blocking_script_classification);
    RUN_TEST(test_valid_meta_referrer_overrides_response_policy);
    RUN_TEST(test_invalid_meta_referrer_preserves_response_policy);
    RUN_TEST(test_rebuild_without_history_keeps_external_css);
    RUN_TEST(test_finalize_cancellation_is_transactional);
    RUN_TEST(test_streamed_candidate_remote_binding_isolation);
    RUN_TEST(test_candidate_cancel_preserves_late_incumbent_dom);
    RUN_TEST(test_bounded_finalize_preserves_history_pruning);
    RUN_TEST(test_relayout_damage_tracks_paint_only_fields);
    RUN_TEST(test_candidate_hook_set_is_transactional);
    RUN_TEST(test_script_oom_retains_usable_target);
    RUN_TEST(test_low_memory_replacement_success);
    RUN_TEST(test_streaming_dynamic_stylesheets_are_not_adopted);
    RUN_TEST(test_dynamic_stylesheet_event_rescan_is_reentrant_safe);
    RUN_TEST(test_stylesheet_event_microtask_exception_is_nonfatal);
    RUN_TEST(test_dynamic_stylesheet_href_base_and_reinsert_lifecycle);
    RUN_TEST(test_low_memory_external_script_incumbent_falls_back);
    RUN_TEST(test_low_memory_replacement_failure_boundaries);
    RUN_TEST(test_sectioned_path);
    RUN_TEST(test_authoritative_layout_resumes_across_finalize_ticks);
    RUN_TEST(test_resumable_layout_cancel_preserves_incumbent);
    RUN_TEST(test_response_headers_precede_parser_script_and_viewport);
    RUN_TEST(test_response_csp_gates_parser_and_resource_paths);
    RUN_TEST(test_frame_response_policy_precedes_child_execution);
    RUN_TEST(test_preload_settlement_releases_scheduler_slots);
    RUN_TEST(test_raw_scanner_preloads_parser_blocking_script);
    RUN_TEST(test_compiled_stylesheet_reuse_stays_in_navigation_ram);
    RUN_TEST(test_compiled_stylesheet_cache_rechecks_inline_nonce);
    RUN_TEST(test_compiled_stylesheet_cache_rechecks_link_authority);
    RUN_TEST(test_critical_ch_body_is_not_page_visible);
    RUN_TEST(test_redirected_critical_ch_retries_final_origin);
    RUN_TEST(test_safe_section_before_eof);
    RUN_TEST(test_mismatched_close_is_not_sealed);
    RUN_TEST(test_script_frontier_snapshot_survives_author_mutation);
    RUN_TEST(test_script_frontier_skips_destroyed_snapshot_nodes);
    RUN_TEST(test_script_frontier_snapshot_capacity_is_explicit);
    RUN_TEST(test_preserved_runtime_retires_replaced_document);
    RUN_TEST(test_transient_document_survives_active_rebind_until_detach);
    RUN_TEST(test_preserved_runtime_dynamic_import_owns_scheduler);
    RUN_TEST(test_delayed_import_after_module_pipeline_failure);
#undef RUN_TEST
    puts("navigation load tests passed");
    return 0;
}
