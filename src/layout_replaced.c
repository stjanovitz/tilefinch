/* The intrinsic model of a replaced element (img, svg, video, audio, canvas,
   iframe, or an element replaced by CSS content), shared by the block,
   inline and flex/grid intrinsic paths. Each path keeps its own placement;
   which elements count, their natural size and the 300x150 (audio 300x54)
   default box are answered here once. */

#include "layout_internal.h"

void layout_replaced_info(LayoutContext *context, lxb_dom_node_t *node,
                          const ComputedStyle *style,
                          LayoutReplacedInfo *info)
{
    const ImageResource *image = layout_replaced_image(context, node, style);
    bool available = image_resource_available(image)
                     && image->width > 0 && image->height > 0;
    lxb_tag_id_t tag = node->local_name;
    bool canvas = tag == LXB_TAG_CANVAS;
    bool audio_controls = layout_audio_has_controls(node);
    /* Video, iframe and controlled audio size as two independent default
       lengths; canvas as a 300x150 bitmap with that ratio. */
    bool default_lengths = tag == LXB_TAG_VIDEO || tag == LXB_TAG_IFRAME
                           || audio_controls;
    int width = available ? image_resource_intrinsic_width(image) : 0;
    int height = available ? image_resource_intrinsic_height(image) : 0;
    if ((default_lengths || canvas) && (width <= 0 || height <= 0)) {
        width = 300;
        height = audio_controls ? 54 : 150;
    }
    *info = (LayoutReplacedInfo) {
        .image = image,
        .natural_width = width,
        .natural_height = height,
        .available = available,
        .canvas = canvas,
        .audio_controls = audio_controls,
        .fallback_dimensions = !available && default_lengths
            && !(style->aspect_width > 0 && style->aspect_height > 0)
    };
}

void layout_replaced_note_unresolved(LayoutContext *context,
                                     lxb_dom_node_t *node,
                                     const ComputedStyle *style,
                                     const LayoutReplacedInfo *info)
{
    if (info->available) return;
    if (style->generated_image)
        layout_note_unresolved_external_visual(context, node,
            style->generated_text, IMAGE_PRIORITY_KIND_BACKGROUND, PSEUDO_NONE);
    if (node->local_name != LXB_TAG_IMG) return;
    size_t source_length = 0;
    const char *source = document_attribute(node, "src", &source_length);
    if (source == NULL || source_length == 0) {
        source = document_attribute(node, "srcset", &source_length);
    }
    if (source != NULL && source_length != 0) {
        layout_note_unresolved_external_visual(
            context, node, NULL, IMAGE_PRIORITY_KIND_DOCUMENT, PSEUDO_NONE);
    }
}
