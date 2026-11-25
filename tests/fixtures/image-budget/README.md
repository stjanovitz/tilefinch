# Image budget fixtures

`photo-1024x768.jpg` is a flat-colour 1024x768 baseline JPEG (4.8 KB),
generated locally with FFmpeg's lavfi `color` source. Drawn at a small
size it takes the decode worker's reduced decode, whose working memory is
a few MB: `tests/test_browser_engine_image_budget.c` serves it from a replay
it writes itself.
