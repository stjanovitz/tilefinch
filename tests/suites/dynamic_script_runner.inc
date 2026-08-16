int main(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    if (!budget_install_lexbor(&budget)) {
        fprintf(stderr, "could not install test Lexbor allocator\n");
        return 1;
    }

    size_t failures = 0;
    if (!test_dynamic_classic_and_error(&budget)) failures++;
    if (!test_dynamic_classic_ordering(&budget)) failures++;
    if (!test_dynamic_module_ordering(&budget)) failures++;
    if (!test_default_dynamic_as_ready(&budget)) failures++;
    if (!test_dynamic_module_dependency(&budget)) failures++;
    if (!test_static_module_referrer_policy(&budget)) failures++;
    if (!test_repeated_static_module_roots_share_admission(&budget)) {
        failures++;
    }
    if (!test_deferred_bootstrap_keeps_bounded_admission(&budget)) {
        failures++;
    }
    if (!test_dynamic_script_cancellation(&budget)) failures++;
    if (!test_remove_pending_still_executes(&budget)) failures++;
    if (!test_cancel_discard_survives_allocation_failure(&budget)) failures++;
    if (!test_connected_insertion_paths(&budget)) failures++;
    if (!test_connected_src_mutation_and_namespaces(&budget)) failures++;
    if (!test_deep_tree_traversal_bounds(&budget)) failures++;
    if (!test_parser_time_dynamic_insertion(&budget)) failures++;
    if (!test_inert_script_types_do_not_consume_quota(&budget)) failures++;
    if (!test_inline_json_assignments_avoid_compiler(&budget)) failures++;
    if (!test_script_realm_quota_caps_and_defaults(&budget)) failures++;
    if (!test_dynamic_byte_reservation_waits_for_actual_length(&budget)) {
        failures++;
    }
    if (!test_dynamic_scheduler_backpressure_retries(&budget)) failures++;
    if (!test_oversized_resource_loader_envelope_is_segmented(&budget)) {
        failures++;
    }
    if (!test_concurrent_dynamic_module_quota_progress(&budget)) failures++;
    size_t module_quota_bytes = strlen(quota_module_source)
        + strlen(quota_module_dependency_source);
    if (!run_dynamic_module_quota_case(
            &budget, "dynamic module exact count and byte boundary",
            2, module_quota_bytes, true, 2)) failures++;
    if (!run_dynamic_module_quota_case(
            &budget, "dynamic module transitive count rejection",
            1, module_quota_bytes, false, 1)) failures++;
    if (!run_dynamic_module_quota_case(
            &budget, "dynamic module transitive byte rejection",
            2, module_quota_bytes - 1u, false, 2)) failures++;
    if (!test_cached_source_rejected_by_quota(&budget)) failures++;
    if (!test_failed_fetch_releases_byte_reservation(&budget)) failures++;
    if (!run_static_dynamic_shared_quota_case(
            &budget, "static and dynamic scripts share count quota",
            true)) failures++;
    if (!run_static_dynamic_shared_quota_case(
            &budget, "static and dynamic scripts share byte quota",
            false)) failures++;
    if (!test_native_script_element_state(&budget)) failures++;
    if (!test_external_start_and_async_state(&budget)) failures++;
    if (!test_inline_module_deferred_current_script(&budget)) failures++;
    if (!test_script_state_capacity_is_explicit(&budget)) failures++;
    if (!test_pending_dynamic_element_destruction(&budget)) failures++;
    if (!test_initial_script_snapshot_mutation(&budget)) failures++;
    if (!test_external_script_self_destruction(&budget)) failures++;
    if (!test_deep_initial_script_snapshot(&budget)) failures++;
    bool clean = budget.current == 0 && budget_uninstall_lexbor(&budget);
    if (!clean) {
        fprintf(stderr, "dynamic script tests left %zu owned bytes\n",
                budget.current);
        failures++;
    }
    if (failures != 0) {
        fprintf(stderr, "tilefinch-dynamic-script-async-tests: %zu failure(s)\n",
                failures);
        return 1;
    }
    puts("tilefinch-dynamic-script-async-tests: all checks passed");
    return 0;
}
