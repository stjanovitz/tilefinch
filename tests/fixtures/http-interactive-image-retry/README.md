# Deferred image retry

The deferred image timeout journey, with one more record: the hero's one
retry succeeds. The first hero request still times out after twenty pumps,
so the page sees the hero's error event and then, for the retried request,
its load event. Synthetic URLs only; the v10 request policy fields remain
enforced.
