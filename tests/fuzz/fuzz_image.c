/* libFuzzer target: raster image probe + decode (stb_image PNG/JPEG/GIF/BMP
   etc. through Tilefinch's scaled JPEG and first-frame GIF paths, libwebp
   through the incremental scaled path) and SVG decode/rasterization.

   Byte 0 selects the mode: bit 0 = SVG, bits 1-2 = target scale divisor
   (1, 2, 3, 4) for raster decodes. The rest is the encoded image. The flow
   mirrors src/image.c: probe, size check, choose a target no larger than
   the source, decode. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "image_decode_internal.h"
#include "image_svg_decode_internal.h"
#include "tilefinch/budget.h"
#include "tilefinch/resources.h"

#define MIB (1024u * 1024u)
/* src/image.c MAX_RASTER_DECODE_WORKING_BYTES */
#define WORKING_LIMIT (8u * MIB)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 512 * 1024) return 0;
    unsigned mode = data[0];
    data++;
    size--;
    unsigned char *encoded = malloc(size == 0 ? 1 : size);
    if (encoded == NULL) return 0;
    if (size != 0) memcpy(encoded, data, size);

    Budget budget;
    budget_init(&budget, 24u * MIB);
    int width = 0, height = 0, components = 0;
    if ((mode & 1u) != 0) {
        unsigned char *pixels = image_svg_decode(encoded, size, &budget,
                                                 2u * MIB, &width, &height);
        if (pixels != NULL) {
            if (width <= 0 || height <= 0
                || (size_t) width * (size_t) height * 4u > 2u * MIB)
                abort();
            /* Touch the last pixel so a short allocation is visible. */
            volatile unsigned char sink =
                pixels[(size_t) width * (size_t) height * 4u - 1u];
            (void) sink;
            budget_free(&budget, pixels);
        }
    } else {
        bool is_webp = false;
        uint64_t checkpoint = budget_checkpoint(&budget);
        ImageDecodeProbeResult probe = image_decode_probe_info(
            &budget, encoded, size, &width, &height, &components, &is_webp);
        /* src/image.c rolls back stb probe scratch the same way. */
        if (budget.current != 0) budget_rollback(&budget, checkpoint);
        if (probe == IMAGE_DECODE_PROBE_SUPPORTED && width > 0 && height > 0
            && (size_t) width <= SIZE_MAX / (size_t) height
            && (size_t) width * (size_t) height <= WORKING_LIMIT / 4u) {
            int divisor = 1 + (int) ((mode >> 1) & 3u);
            ImageResource resource;
            memset(&resource, 0, sizeof(resource));
            resource.encoded = encoded;
            resource.encoded_length = size;
            resource.source_width = width;
            resource.source_height = height;
            resource.width = width / divisor > 0 ? width / divisor : 1;
            resource.height = height / divisor > 0 ? height / divisor : 1;
            unsigned char *pixels = NULL;
            ImageDecodeStatus status = image_resource_decode_checked(
                &resource, &budget, &pixels);
            if (status == IMAGE_DECODE_SUCCEEDED) {
                if (pixels == NULL) abort();
                volatile unsigned char sink = pixels[
                    (size_t) resource.width * (size_t) resource.height * 4u
                    - 1u];
                (void) sink;
                image_resource_free_decoded(&budget, pixels);
            }
        }
    }
    /* Every decode path must return its scratch to the page budget. */
    if (budget.current != 0) abort();
    free(encoded);
    return 0;
}
