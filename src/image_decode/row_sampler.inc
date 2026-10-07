/* Streaming row sampler, included by image_decode.c.

   Reduces exactly as the previous decode-then-reduce path did, one point
   sample per target pixel at source (x * width / target_width,
   y * height / target_height), but consumes source rows one at a time, top
   to bottom, and writes each target row as soon as its source row arrives.
   No source-sized surface exists, and a decoder can ask whether a row is
   sampled at all before converting it. Working memory is one column table. */

typedef struct {
    int source_width;
    int source_height;
    int target_width;
    int target_height;
    unsigned char *output;
    uint32_t *columns;
    int source_y;
    int target_y;
} ImageRowSampler;

static void image_row_sampler_release(ImageRowSampler *sampler)
{
    if (sampler == NULL) return;
    image_free(sampler->columns);
    memset(sampler, 0, sizeof(*sampler));
}

static bool image_row_sampler_init(ImageRowSampler *sampler,
                                   int source_width, int source_height,
                                   int target_width, int target_height,
                                   unsigned char *output)
{
    if (sampler == NULL) return false;
    memset(sampler, 0, sizeof(*sampler));
    if (output == NULL || source_width <= 0 || source_height <= 0
        || target_width <= 0 || target_height <= 0
        || target_width > source_width || target_height > source_height
        || (size_t) target_width > SIZE_MAX / sizeof(uint32_t))
        return false;
    *sampler = (ImageRowSampler) {
        .source_width = source_width,
        .source_height = source_height,
        .target_width = target_width,
        .target_height = target_height,
        .output = output
    };
    sampler->columns = image_malloc(
        (size_t) target_width * sizeof(uint32_t));
    if (sampler->columns == NULL) return false;
    for (int x = 0; x < target_width; x++) {
        sampler->columns[x] = (uint32_t) image_point_sample_index(
            x, source_width, target_width);
    }
    return true;
}

/* Whether the next source row is the sample of the next target row; heights
   only reduce, so each source row is sampled at most once. */
static bool image_row_sampler_wants(const ImageRowSampler *sampler)
{
    return sampler->target_y < sampler->target_height
        && image_point_sample_index(sampler->target_y,
                                    sampler->source_height,
                                    sampler->target_height)
           == sampler->source_y;
}

/* Consumes the next source row; rgba may be NULL when it is not wanted. */
static bool image_row_sampler_push(ImageRowSampler *sampler,
                                   const unsigned char *rgba)
{
    if (sampler->source_y >= sampler->source_height) return false;
    if (image_row_sampler_wants(sampler)) {
        if (rgba == NULL) return false;
        uint32_t *out = (uint32_t *) (void *) (sampler->output
            + (size_t) sampler->target_y
                * (size_t) sampler->target_width * 4u);
        for (int x = 0; x < sampler->target_width; x++) {
            memcpy(out + x, rgba + (size_t) sampler->columns[x] * 4u, 4u);
        }
        sampler->target_y++;
    }
    sampler->source_y++;
    return true;
}

static bool image_row_sampler_finished(const ImageRowSampler *sampler)
{
    return sampler->source_y == sampler->source_height
        && sampler->target_y == sampler->target_height;
}
