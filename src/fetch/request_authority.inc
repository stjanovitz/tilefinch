typedef struct {
    uint32_t version;
    TilefinchRequestContext context;
    FetchRequest request;
    char cookie[4096];
    char referrer[TILEFINCH_URL_SERIALIZED_LIMIT];
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
} PreparedPageRequestStorage;

_Static_assert(sizeof(PreparedPageRequestStorage)
                   <= FETCH_PREPARED_PAGE_REQUEST_BYTES,
               "prepared page request exceeds fixed public storage");

static PreparedPageRequestStorage *prepared_page_storage(
    FetchPreparedPageRequest *prepared)
{
    return prepared == NULL ? NULL
        : (PreparedPageRequestStorage *) prepared->bytes;
}

static const PreparedPageRequestStorage *prepared_page_storage_const(
    const FetchPreparedPageRequest *prepared)
{
    return prepared == NULL ? NULL
        : (const PreparedPageRequestStorage *) prepared->bytes;
}

static bool page_transport_options_clean(const FetchRequest *options)
{
    return options == NULL
        || (options->cookie == NULL && options->referer == NULL
            && options->origin == NULL && options->sec_fetch_dest == NULL
            && options->sec_fetch_mode == NULL
            && options->sec_fetch_site == NULL && !options->sec_fetch_user
            && !options->block_private_network
            && options->credential_origin == NULL
            && options->initiator_url == NULL
            && options->referrer_source == NULL
            && options->referrer_policy == NULL
            && !options->enforce_cors
            && options->cookie_session == NULL
            && options->cookie_context == NULL
            && options->page_context == NULL
            && options->content_security_policy == NULL
            && options->content_blocker == NULL
            && options->redirect_policy == FETCH_REDIRECT_DEFAULT
            && options->redirect_url_validator == NULL
            && options->prepared_page_version == 0);
}

static FetchCredentialPolicy page_fetch_credentials(
    TilefinchCredentialsMode credentials)
{
    return credentials == TILEFINCH_CREDENTIALS_OMIT
        ? FETCH_CREDENTIALS_OMIT
        : credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN
            ? FETCH_CREDENTIALS_SAME_ORIGIN : FETCH_CREDENTIALS_INCLUDE;
}

bool fetch_prepare_page_request(
    const FetchPageSecurityContext *security_context,
    const FetchPageRequestDescriptor *descriptor,
    FetchPreparedPageRequest *prepared,
    FetchRequestValidationError *error)
{
    if (error != NULL) *error = FETCH_REQUEST_VALIDATION_CONTEXT;
    PreparedPageRequestStorage *storage = prepared_page_storage(prepared);
    if (storage != NULL) {
        /* Invalidate first so a failed rebuild can never expose a prior
           envelope. The large owned strings are initialized individually
           below; clearing all 6+ KiB on every request was pure write traffic. */
        storage->version = 0;
        storage->request.prepared_page_version = 0;
    }
    if (security_context == NULL || descriptor == NULL || storage == NULL
        || descriptor->target_url == NULL
        || !page_transport_options_clean(&descriptor->transport)) {
        return false;
    }
    storage->cookie[0] = '\0';
    storage->referrer[0] = '\0';
    storage->origin[0] = '\0';
    const char *method = descriptor->method == NULL
        ? "GET" : descriptor->method;
    TilefinchCredentialsMode credentials = descriptor->credentials;
    if (security_context->opaque_origin
        && credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN) {
        credentials = TILEFINCH_CREDENTIALS_OMIT;
    }
    storage->context = (TilefinchRequestContext) {
        .target_url = descriptor->target_url,
        .initiator_url = security_context->document_url,
        .top_level_url = security_context->top_level_url,
        .method = method,
        .mode = descriptor->mode,
        .credentials = credentials,
        .destination = descriptor->destination,
        .initiator_opaque = security_context->opaque_origin,
        .top_level_navigation = descriptor->top_level_navigation,
        .user_activated = descriptor->user_activated
    };
    TilefinchRequestFacts facts;
    if (!tilefinch_request_context_analyze(&storage->context, &facts)) {
        return false;
    }

    if (security_context->session != NULL
        && facts.sends_credentials
        && !browser_session_cookie_header_request_facts(
            security_context->session, &storage->context, &facts,
            storage->cookie, sizeof(storage->cookie))) return false;
    const char *referrer_source = security_context->referrer_source_url != NULL
        ? security_context->referrer_source_url
        : security_context->document_url;
    if (referrer_source != NULL
        && !fetch_compute_referrer(
            referrer_source, descriptor->target_url,
            security_context->referrer_policy,
            storage->referrer, sizeof(storage->referrer))) return false;
    bool sends_origin = !facts.safe_method
        || descriptor->mode == TILEFINCH_REQUEST_MODE_CORS;
    if (sends_origin) {
        if (security_context->opaque_origin) {
            memcpy(storage->origin, "null", sizeof("null"));
        } else if (security_context->document_url == NULL
                   || !tilefinch_url_origin(
                       security_context->document_url, storage->origin,
                       sizeof(storage->origin))) {
            return false;
        }
    }

    storage->request = descriptor->transport;
    storage->request.method = method;
    storage->request.cookie = storage->cookie[0] == '\0'
        ? NULL : storage->cookie;
    storage->request.referer = storage->referrer[0] == '\0'
        ? NULL : storage->referrer;
    storage->request.origin = storage->origin[0] == '\0'
        ? NULL : storage->origin;
    storage->request.sec_fetch_dest =
        tilefinch_request_fetch_destination(&storage->context);
    storage->request.sec_fetch_mode =
        tilefinch_request_fetch_mode(&storage->context);
    storage->request.sec_fetch_site =
        tilefinch_request_facts_fetch_site(&facts);
    storage->request.sec_fetch_user = storage->context.user_activated;
    storage->request.credentials = page_fetch_credentials(credentials);
    storage->request.credential_origin = security_context->document_url == NULL
        ? descriptor->target_url : security_context->document_url;
    storage->request.initiator_url = security_context->document_url;
    storage->request.referrer_source = referrer_source;
    storage->request.referrer_policy = security_context->referrer_policy;
    bool cors = descriptor->mode == TILEFINCH_REQUEST_MODE_CORS;
    storage->request.enforce_cors = cors
        && !storage->request.cors_response_check_deferred;
    if (!cors) storage->request.cors_response_check_deferred = false;
    storage->request.cookie_session = security_context->session;
    storage->request.cookie_context = security_context->session == NULL
        ? NULL : &storage->context;
    storage->request.page_context = &storage->context;
    storage->request.content_security_policy =
        security_context->content_security_policy;
    storage->request.content_blocker = security_context->content_blocker;
    storage->request.prepared_page_version =
        FETCH_PREPARED_PAGE_REQUEST_VERSION;
    storage->version = FETCH_PREPARED_PAGE_REQUEST_VERSION;
    if (!fetch_request_validate_with_facts(
            &storage->request, &facts, error)) {
        storage->version = 0;
        storage->request.prepared_page_version = 0;
        return false;
    }
    return true;
}

bool fetch_prepare_page_request_context(
    const TilefinchRequestContext *context,
    const char *referrer_source_url, const char *referrer_policy,
    const BrowserSession *session,
    const TilefinchContentSecurityPolicy *content_security_policy,
    ContentBlocker *content_blocker,
    const FetchRequest *transport,
    FetchPreparedPageRequest *prepared,
    FetchRequestValidationError *error)
{
    if (context == NULL) {
        if (error != NULL) *error = FETCH_REQUEST_VALIDATION_CONTEXT;
        return false;
    }
    FetchPageSecurityContext security = {
        .document_url = context->initiator_url,
        .top_level_url = context->top_level_url,
        .referrer_source_url = referrer_source_url,
        .referrer_policy = referrer_policy,
        .session = session,
        .content_security_policy = content_security_policy,
        .content_blocker = content_blocker,
        .opaque_origin = context->initiator_opaque
    };
    FetchPageRequestDescriptor descriptor = {
        .target_url = context->target_url,
        .method = context->method,
        .mode = context->mode,
        .credentials = context->credentials,
        .destination = context->destination,
        .top_level_navigation = context->top_level_navigation,
        .user_activated = context->user_activated,
        .transport = transport == NULL ? (FetchRequest) {0} : *transport
    };
    return fetch_prepare_page_request(
        &security, &descriptor, prepared, error);
}

const FetchRequest *fetch_prepared_page_request(
    const FetchPreparedPageRequest *prepared)
{
    const PreparedPageRequestStorage *storage =
        prepared_page_storage_const(prepared);
    return storage != NULL
        && storage->version == FETCH_PREPARED_PAGE_REQUEST_VERSION
        && storage->request.prepared_page_version
               == FETCH_PREPARED_PAGE_REQUEST_VERSION
        && storage->request.page_context == &storage->context
        ? &storage->request : NULL;
}

const TilefinchRequestContext *fetch_prepared_page_request_context(
    const FetchPreparedPageRequest *prepared)
{
    const FetchRequest *request = fetch_prepared_page_request(prepared);
    return request == NULL ? NULL : request->page_context;
}
