# Validate one replay without printing URLs or account data. Compare the
# selected link, not its ordinal: unrelated links can share focus geometry.
function field_value(key,   i) {
    for (i = 2; i <= NF; ++i)
        if (index($i, key "=") == 1) return substr($i, length(key) + 2)
    return ""
}
$1 == "tilefinch-input-page:" {
    m = field_value("mark"); url[m] = field_value("url"); scroll[m] = field_value("scroll")
    loaded[m] = field_value("loaded"); generation[m] = field_value("generation")
}
$1 == "tilefinch-input-focus-target:" {
    m = field_value("mark"); id[m] = field_value("id"); role[m] = field_value("class")
    target[m] = field_value("url")
}
$1 == "tilefinch-focus-probe:" {
    m = field_value("mark"); rect[m] = field_value("rect"); visible[m] = field_value("visible")
}
$1 == "tilefinch-details-preview:" && field_value("press-to-present-us")+0 > 0 {
    if (!previews) pressed_at = field_value("input-us")+0
    if (previews == 2) next_press = field_value("input-us")+0
    previews++
}
$1 == "tilefinch-input-script:" && field_value("native") == "watch-play-ready" {
    ready_at = field_value("at-us")+0; play_ready++
}
$1 == "tilefinch-input-script-media:" && field_value("mark") == "native-player" && field_value("visible") == 1 && (field_value("resolving") == 1 || field_value("playing") == 1) { player = 1 }
$0 == "tilefinch-details-preview: play-before-commit" { early_play++ }
$1 == "tilefinch-media-open:" && field_value("event") == "cancelled" && field_value("stage") == "resolve" && !early_play { retired_resolver = 1 }
$1 == "tilefinch-input-script-media:" && field_value("mark") == "playing" && early_play == 1 && field_value("visible") == 1 && field_value("playing") == 1 && field_value("failed") == 0 && field_value("current")+0 > 0 { advancing = 1 }
$1 == "tilefinch-ui-supervisor:" && field_value("scope") == "interactive" && field_value("cancelled") == 1 && field_value("input-acks")+0 > 0 {
    cancels++; cancelled_at = field_value("cancel-at-us")+0
}
END {
    if (preview_play_run) {
        ok = (retired_resolver && previews == 1 && early_play == 1 && advancing && !play_ready &&
              id["first"] ~ /^yt-result-card-.+/ && scroll["selected"]+0 > 0 &&
              substr(id["first"],length("yt-result-card-")+1) != substr(id["selected"],length("yt-result-details-")+1) &&
              loaded["selected"] == 1 && loaded["returned"] == 1 && url["selected"] == url["returned"] &&
              id["selected"] ~ /^yt-result-details-.+/ && id["selected"] == id["returned"] &&
              target["selected"] == target["returned"] && scroll["selected"] == scroll["returned"] &&
              visible["returned"] == 1 && rect["selected"] == rect["returned"])
        if (!ok) print "FAIL: early Play, advancing native playback, or selected result continuity." > "/dev/stderr"
        exit !ok
    }
    if (switch_run) {
        ok = (loaded["selected"] == 1 && scroll["selected"]+0 > 0 && visible["selected"] == 1 &&
              role["selected"] == "details" && id["selected"] ~ /^yt-result-details-.+/ &&
              play_ready == 1 && pressed_at > 0 && ready_at > pressed_at &&
              player && loaded["from-player"] == 1 &&
              role["from-player"] == "watch-target" && target["selected"] == url["from-player"] &&
              url["returned"] == url["selected"] && scroll["returned"] == scroll["selected"] &&
              id["returned"] == id["selected"] && rect["returned"] == rect["selected"] &&
              role["other"] == "details" && id["other"] != id["selected"] &&
              previews == 3 && cancels == 1 && url["switched"] == url["returned"] &&
              id["switched"] == id["selected"] && target["switched"] == target["selected"] &&
              scroll["switched"] == scroll["selected"] && rect["switched"] == rect["selected"] &&
              loaded["reopened"] == 1 && role["reopened"] == "watch-target" &&
              url["reopened"] == url["from-player"] && generation["reopened"]+0 > generation["switched"]+0)
        if (!ok) { print "FAIL: native Play, cancellation, or selected result continuity." > "/dev/stderr"; exit 1 }
        printf "Details press to actionable Play: %.1f ms\n", (ready_at - pressed_at)/1000
        if (cancelled_at > 0 && next_press > cancelled_at)
            printf "Cancel to different Details press: %.1f ms\n", (next_press - cancelled_at)/1000
        exit 0
    }
    from = deep ? "deep-details" : "details"; watch = deep ? "deep-watch" : "watch"
    back = deep ? "deep-return" : "returned"
    ok = (previews > 0 && loaded[from] == 1 && loaded[watch] == 1 && loaded[back] == 1 &&
          generation[from]+0 < generation[watch]+0 && generation[watch]+0 < generation[back]+0 &&
          url[from] != "" && url[from] == url[back] && url[from] != url[watch] &&
          scroll[from] ~ /^[0-9]+$/ && scroll[from] == scroll[back] &&
          visible[from] == 1 && visible[back] == 1 && rect[from] != "" && rect[from] == rect[back] &&
          role[from] == "details" && role[back] == role[from] &&
          id[from] ~ /^yt-result-details-.+/ && id[from] == id[back] &&
          target[from] == url[watch] && target[from] == target[back] && role[watch] == "watch-target")
    if (deep) ok = (ok && scroll[from]+0 > 0 && loaded["deep-reopen"] == 1 &&
                    generation["deep-reopen"]+0 > generation[back]+0 &&
                    role["deep-reopen"] == "watch-target" && url["deep-reopen"] == url[watch])
    if (cancel) ok = (ok && previews == 2 && cancels == 1 && loaded["restored"] == 1 &&
                      url["restored"] == url[from] && scroll["restored"] == scroll[from] &&
                      visible["restored"] == 1 && rect["restored"] == rect[from] &&
                      role["restored"] == "details" && id["restored"] == id[from] &&
                      target["restored"] == target[from] && generation["restored"]+0 < generation[watch]+0)
    if (!ok) { print "FAIL: Details, restored search position/focus, or reopening did not match." > "/dev/stderr"; exit 1 }
}
