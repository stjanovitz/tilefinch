#ifndef TILEFINCH_STYLE_CSSOM_H
#define TILEFINCH_STYLE_CSSOM_H

/* The narrow part of the style internals that the CSSOM and computed-style
   bindings (js_dom_bindings.c) use: the bounded math parser behind
   computed-value serialization, @property registration rules, easing and
   font-shorthand validation, container-query units, and the paint stacks.
   Style translation units get it through style_internal.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/style.h"
#include "style_paint_internal.h"

/* Math programs: the instruction set a compiled calc()/min()/max()/clamp()
   runs, and the parser that builds it. */
enum {
    STYLE_MATH_PUSH_AFFINE,
    STYLE_MATH_ADD,
    STYLE_MATH_SUBTRACT,
    STYLE_MATH_SCALE,
    STYLE_MATH_DIVIDE,
    STYLE_MATH_MINIMUM,
    STYLE_MATH_MAXIMUM,
    STYLE_MATH_CLAMP
};

struct StyleMathInstruction {
    int32_t a;
    int32_t b;
    uint8_t op;
    uint8_t reserved[3];
};

#define STYLE_MATH_NODE_LIMIT 32u
#define STYLE_MATH_STACK_LIMIT 8u
#define STYLE_MATH_NESTING_LIMIT 8u
#define STYLE_MATH_ARGUMENT_LIMIT 8u
#define STYLE_MATH_PROGRAM_LIMIT 256u
#define STYLE_MATH_INSTRUCTION_LIMIT 2048u

typedef enum {
    STYLE_MATH_TYPE_NUMBER,
    STYLE_MATH_TYPE_LENGTH
} StyleMathType;

typedef enum {
    STYLE_MATH_NODE_VALUE,
    STYLE_MATH_NODE_ADD,
    STYLE_MATH_NODE_SUBTRACT,
    STYLE_MATH_NODE_SCALE,
    STYLE_MATH_NODE_DIVIDE,
    STYLE_MATH_NODE_MINIMUM,
    STYLE_MATH_NODE_MAXIMUM,
    STYLE_MATH_NODE_CLAMP
} StyleMathNodeOp;

typedef struct {
    int64_t a;
    int64_t b;
    uint8_t op;
    uint8_t type;
    uint8_t left;
    uint8_t right;
    uint8_t third;
    bool folded;
} StyleMathNode;

typedef struct {
    const Stylesheet *sheet;
    const char *text;
    size_t length;
    size_t at;
    unsigned nesting;
    StyleMathNode nodes[STYLE_MATH_NODE_LIMIT];
    size_t node_count;
    int em_basis;
    int rem_basis;
    int ch_basis;
    int viewport_width;
    int viewport_height;
    bool failed;
} StyleMathParser;

typedef struct {
    StyleMathInstruction instructions[STYLE_MATH_NODE_LIMIT];
    size_t count;
    uint8_t stack_depth;
    bool has_percent;
} StyleMathCandidate;

bool style_math_candidate(const Stylesheet *sheet, const char *text,
                          size_t length, StyleMathParser *parser,
                          StyleMathCandidate *candidate, int *root_output,
                          int em_basis, int rem_basis, int ch_basis);

/* The one set of registration rules @property and CSS.registerProperty()
   share. The cascade computes only <length> as a type; any other valid
   syntax keeps its initial value and inheritance as an untyped token
   stream. */
typedef enum {
    STYLE_REGISTRATION_INVALID = 0,
    STYLE_REGISTRATION_UNIVERSAL,
    STYLE_REGISTRATION_LENGTH,
    STYLE_REGISTRATION_UNTYPED
} StyleRegistrationSyntax;
/* `syntax` is the unquoted syntax string; `initial` is NULL when no initial
   value was given. INVALID when the name is not a custom property name, the
   syntax string does not parse, a non-universal syntax has no initial value,
   or a <length> initial value is not computationally independent. */
StyleRegistrationSyntax style_registered_property_check(
    const char *name, size_t name_length,
    const char *syntax, size_t syntax_length,
    const char *initial, size_t initial_length);

/* style_values.c */
/* One CSS <easing-function> (a keyword, cubic-bezier(), steps() or
   linear()) checked against its grammar and serialized as computed values
   are: keywords lower-case, numbers canonical, arguments separated by
   ", ". The one validator behind transition-timing-function, the
   transition shorthand and, through __tilefinchTimingFunction, CSS
   animations and Element.animate(). False for an invalid value or one
   whose serialization does not fit `output_size`. */
bool style_timing_function(const char *text, size_t length,
                           char *output, size_t output_size);
bool style_parse_font_shorthand(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *font);

/* style_queries.c: sets the container-query length units (cqw, cqh, ...)
   for `node` from its nearest size container. */
void style_container_units_for_node(Stylesheet *sheet,
                                    lxb_dom_node_t *node);

#endif /* TILEFINCH_STYLE_CSSOM_H */
