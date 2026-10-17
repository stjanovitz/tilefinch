/* The tiny two-master font is authored here, not a captured site resource. */
#include "variable_font_fixture.inc"
static bool test_discard_unconsumed_font_tables(void)
{
#if defined(TILEFINCH_TEST_HAVE_FREETYPE) && defined(TILEFINCH_TEST_WOFF2)
    /* A valid empty GPOS directory with unreachable padding. The bounded
       reader has no kern plan to consume; retain no dead table on the PSP. */
    const size_t padding = 247000;
    size_t old_length = sizeof(variable_font_fixture), length = (old_length+19u)&~(size_t)3u;
    unsigned count = read_be16(variable_font_fixture+4);
    unsigned char *sfnt = calloc(1,length+padding);
    CHECK(sfnt != NULL);
    size_t directory = 12u+count*16u;
    memcpy(sfnt,variable_font_fixture,directory);
    memcpy(sfnt+directory+16,variable_font_fixture+directory,old_length-directory);
    store_be16(sfnt+4,(uint16_t)(count+1));
    for(unsigned i=0;i<count;i++) {
        unsigned char *entry=sfnt+12u+16u*i;
        store_be32(entry+8,read_be32(entry+8)+16u);
    }
    unsigned char *entry=sfnt+directory;
    memcpy(entry,"GPOS",4);store_be32(entry+8,(uint32_t)length);store_be32(entry+12,(uint32_t)padding);
    store_be16(sfnt+length,1);
    store_be16(sfnt+length+4,10);store_be16(sfnt+length+6,12);store_be16(sfnt+length+8,14);
    unsigned char *encoded=NULL;size_t encoded_length=0;
    CHECK(build_null_transform_woff2(sfnt,length+padding,&encoded,&encoded_length));
    Budget budget;budget_init(&budget,4u*MIB);WebFontSet fonts={0};
    CHECK(font_face_load_encoded(&fonts.families[0].faces[0],&budget,encoded,encoded_length,256u*KIB));
    fonts.families[0].weights[0]=400;fonts.families[0].weight_max[0]=700;
    FontFamily web=FONT_SANS;CHECK(font_family_web(0,FONT_SANS,&web));
    const FontFace *face=font_context_face_at_weight(NULL,&fonts,web,false,true,700);
    CHECK(font_text_width_at_size_fixed_mode(face,"AV",2,40*64,false,false)>0);
    CHECK(font_face_backend_bytes(&fonts.families[0].faces[0])<64u*KIB);
    web_font_set_destroy(&fonts);CHECK(budget.current==0);free(sfnt);free(encoded);
#endif
    return true;
}

static bool test_variable_font_weights(void)
{
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    Budget budget; budget_init(&budget, 4u*MIB);
    WebFontSet fonts = {0};
    WebFontFamilyFaces *family = &fonts.families[0];
    CHECK(font_face_load_encoded(&family->faces[0], &budget,
        variable_font_fixture, sizeof(variable_font_fixture), 256u*KIB));
    family->weights[0] = 400; family->weight_max[0] = 700;
    FontFamily web = FONT_SANS; CHECK(font_family_web(0, FONT_SANS, &web));
    const FontFace *normal = font_context_face_at_weight(NULL, &fonts, web, false, false, 400);
    const FontFace *bold = font_context_face_at_weight(NULL, &fonts, web, false, true, 700);
    CHECK(normal != bold && normal->authored_weight == 400 && bold->authored_weight == 700);
    CHECK(!font_face_needs_synthetic_bold(NULL, &fonts, bold, 700));
    int width = font_text_width_at_size_fixed_mode(normal, "AV", 2, 40*64, false, false);
    int bold_width = font_text_width_at_size_fixed_mode(bold, "AV", 2, 40*64, false, false);
    CHECK(width > 0 && bold_width > width);
    int small = font_text_width_at_size_fixed_mode(normal, "AV", 2, 14*64, false, false);
    CHECK(small > 0 && (int64_t)width * 14 < (int64_t)small * 40);
    CHECK(font_text_width_at_size_fixed_mode(normal, "AV", 2, 40*64, false, false) == width);
    for (unsigned i = 0; i < 40; i++) {
        const FontFace *face = font_context_face_at_weight(NULL, &fonts, web, false, false, 400+i*5);
        CHECK(face != NULL && face->authored_weight >= 400 && face->authored_weight <= 700);
        CHECK(font_text_width_at_size_fixed_mode(face, "A", 1, 20*64, false, false) > 0);
    }
    CHECK(font_text_width_at_size_fixed_mode(bold, "AV", 2, 40*64, false, false) == bold_width);
    CHECK(font_text_width_at_size_fixed_mode(normal, "AV", 2, 40*64, false, false) == width);
    CHECK(font_face_backend_bytes(&family->faces[0]) < 256u*KIB);
    web_font_set_destroy(&fonts); CHECK(budget.current == 0);
    CHECK(sweep_font_allocation_failures(variable_font_fixture,
        sizeof(variable_font_fixture), 256u*KIB));
#endif
    return true;
}

static bool test_variable_font_descriptors(void)
{
    static const char html[] = "<style>"
        "@font-face{font-family:Variable;src:url(v.woff2);font-weight:100 900}"
        "@font-face{font-family:Variable;src:url(bad.woff2);font-weight:900 100}"
        "@font-face{font-family:Variable;src:url(bad2.woff2);font-weight:100 900 950}"
        "</style><p>range</p>";
    Budget budget; budget_init(&budget, 4u*MIB);CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0}; Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1, 10)
        && stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(stylesheet_web_font_source_count(&sheet) == 1);
    const WebFontSet *fonts = stylesheet_web_font_set(&sheet);
    CHECK(fonts != NULL && fonts->families[0].weights[0] == 100
        && fonts->families[0].weight_max[0] == 900);
    for (unsigned weight = 100; weight <= 900; weight += 100)
        CHECK(font_family_face_slot_at_weight(&fonts->families[0],weight,false) == 0);
    stylesheet_destroy(&sheet);document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return true;
}

static bool test_variable_weight_refusals(void)
{
#ifdef TILEFINCH_TEST_HAVE_FREETYPE
    bool reached = false;
    for (unsigned failure = 0; failure < 128; failure++) {
        Budget budget; budget_init(&budget, 4u*MIB);
        WebFontSet fonts = {0};
        CHECK(font_face_load_encoded(&fonts.families[0].faces[0], &budget,
            variable_font_fixture, sizeof(variable_font_fixture), 256u*KIB));
        fonts.families[0].weights[0] = 400; fonts.families[0].weight_max[0] = 700;
        FontFamily web = FONT_SANS; CHECK(font_family_web(0, FONT_SANS, &web));
        budget_inject_failure_after(&budget, failure);
        const FontFace *bold = font_context_face_at_weight(NULL,&fonts,web,false,true,700);
        (void)font_text_width_at_size_fixed_mode(bold,"AV",2,40*64,false,false);
        FontGlyph glyph = {0};
        (void)font_glyph_load_at_size(bold,'A',40*64,false,&glyph);
        font_glyph_destroy(bold,&glyph);
        bool injected = budget.injected_failure_count != 0;
        budget_clear_failure_injection(&budget);
        web_font_set_destroy(&fonts);
        if (budget.current) fprintf(stderr,"variation refusal step=%u remaining=%zu\n",failure,budget.current);
        CHECK(budget.current == 0);
        if (!injected) { reached = true; break; }
    }
    CHECK(reached);
#endif
    return true;
}
