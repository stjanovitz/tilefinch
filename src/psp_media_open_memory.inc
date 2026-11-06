/* Bounded owner-side recovery, shared with host failure/retry tests. */
static bool psp_media_create_playback(PspMediaSession *media,
                                      uint64_t audio_start_us,
                                      char *error, size_t error_size)
{
    unsigned room_stage = 0;
    for (;;) {
        if (psp_media_create_playback_once(
                media, audio_start_us, error, error_size))
            return true;
        if (media->platform.make_room == NULL || room_stage >= 2u
            || !media_failure_playback_memory(error))
            return false;
        size_t working = media->use_swdec
            ? media_psp_swdec_backend_last_working_set() : 0;
        /* The playback record and its preallocated packet come after. */
        size_t playback = PSP_MEDIA_MAXIMUM_PACKET_BYTES + 512u * KIB;
        size_t needed = working == 0 ? 0
            : working > SIZE_MAX - playback ? SIZE_MAX : working + playback;
        /* The Budget can hold the bytes while the heap has no block for
           one of them: free memory anyway, by stage. */
        if (needed != 0 && budget_remaining(media->budget) >= needed)
            needed = 0;
        unsigned previous_stage = room_stage;
        if (!media->platform.make_room(
                media->platform.memory_context, needed, &room_stage)
            || room_stage <= previous_stage)
            return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        printf("tilefinch-media: playback-room stage=%u needed=%u "
               "remaining=%u after=\"%s\"\n",
               room_stage, (unsigned) needed,
               (unsigned) budget_remaining(media->budget), error);
#endif
        error[0] = '\0';
    }
}
