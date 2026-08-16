bool fetch_result_share_body(FetchResult *result)
{
    if (result == NULL) return false;
    if (result->shared_body != NULL) {
        return result->data == (char *) result->shared_body->data
            && result->length == result->shared_body->length;
    }
    if (result->budget == NULL || result->data == NULL
        || result->length == 0) return false;
    /* The shared body outlives the transfer (retained stylesheet and
       image bodies, cache entries), so shed the append path's geometric
       growth slack now, while this is still the sole reference and a
       moving realloc is safe.  A shrink that fails just keeps the
       original buffer. */
    if (result->capacity > result->length + 1) {
        char *trimmed = budget_realloc(result->budget, result->data,
                                       result->length + 1);
        if (trimmed != NULL) {
            result->data = trimmed;
            result->capacity = result->length + 1;
        }
    }
    BrowserSharedBody *body = browser_shared_body_take(
        result->budget, (unsigned char *) result->data, result->length);
    if (body == NULL) return false;
    result->shared_body = body;
    result->data = (char *) body->data;
    /* The allocation is now owned by BrowserSharedBody.  Fetch append paths
       must not attempt to grow it after this ownership transition. */
    result->capacity = 0;
    return true;
}

void fetch_result_destroy(FetchResult *result)
{
    if (result == NULL) return;
    Budget *budget = result->budget;
    if (result->shared_body != NULL) {
        browser_shared_body_release(result->shared_body);
    } else {
        budget_free(budget, result->data);
    }
    for (size_t i = 0; i < FETCH_RESPONSE_COOKIE_CAPACITY; i++) {
        budget_free(budget, result->set_cookies[i]);
        budget_free(budget, result->set_cookie_urls[i]);
    }
    memset(result, 0, sizeof(*result));
    result->budget = budget;
}

void fetch_result_free(FetchResult *result)
{
    if (result == NULL) return;
    Budget *budget = result->budget;
    fetch_result_destroy(result);
    budget_free(budget, result);
}

bool fetch_resolve_url(const char *base_url, const char *reference,
                       char *output, size_t output_size)
{
    return tilefinch_url_resolve(base_url, reference, output, output_size);
}

