/* jsint.h - die inneren Strukturen des Deuters. */
#ifndef JSINT_H
#define JSINT_H

#include "js.h"

/* ------------------------------------------------------------------ */
/* Syntaxbaum                                                          */
/* ------------------------------------------------------------------ */

enum ast_kind {
    /* Ausdruecke */
    AST_NUMBER, AST_STRING, AST_TEMPLATE, AST_IDENT, AST_BOOL, AST_NULL,
    AST_UNDEFINED, AST_THIS, AST_ARRAY, AST_OBJECT, AST_FUNCTION,
    AST_ARROW, AST_CALL, AST_NEW, AST_MEMBER, AST_INDEX, AST_UNARY,
    AST_UPDATE, AST_BINARY, AST_LOGICAL, AST_ASSIGN, AST_CONDITIONAL,
    AST_SEQUENCE, AST_SPREAD,

    /* Anweisungen */
    AST_PROGRAM, AST_BLOCK, AST_VAR, AST_IF, AST_FOR, AST_FOR_IN,
    AST_FOR_OF, AST_WHILE, AST_DO_WHILE, AST_RETURN, AST_BREAK,
    AST_CONTINUE, AST_EXPRESSION, AST_EMPTY, AST_SWITCH, AST_THROW,
    AST_TRY, AST_LABEL,
};

enum ast_op {
    OP_NONE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_LT, OP_GT, OP_LE, OP_GE,
    OP_EQ, OP_NE, OP_STRICT_EQ, OP_STRICT_NE,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR,
    OP_LOGICAL_AND, OP_LOGICAL_OR, OP_NULLISH,
    OP_NOT, OP_NEGATE, OP_PLUS, OP_BITNOT, OP_TYPEOF, OP_VOID, OP_DELETE,
    OP_IN, OP_INSTANCEOF,
    OP_INCREMENT, OP_DECREMENT,
};

struct ast {
    enum ast_kind kind;
    enum ast_op   op;

    js_num      number;
    char       *text;           /* Name, Zeichenkette, Marke */
    bool        flag;           /* Vorzeichen je nach Knotenart */

    struct ast *a, *b, *c, *d;  /* Kinder */
    struct ast *list;           /* Kette ueber next */
    struct ast *next;

    int32_t     line;
};

/* ------------------------------------------------------------------ */
/* Werte                                                              */
/* ------------------------------------------------------------------ */

enum js_class {
    CLASS_OBJECT,
    CLASS_ARRAY,
    CLASS_FUNCTION,
    CLASS_NATIVE,
    CLASS_NODE,
    CLASS_NODELIST,
    CLASS_ERROR,
    CLASS_STYLE,
    CLASS_CLASSLIST,
};

struct js_prop {
    char           *name;
    struct js_value value;
    bool            enumerable;
    struct js_prop *next;
};

struct js_scope;

/* Eigenschaften, die erst beim Zugriff entstehen - etwa die Felder
 * eines Knotens. */
typedef struct js_value (*js_getter)(struct js_context *ctx,
                                     struct js_object *self,
                                     const char *name, bool *handled);
typedef bool (*js_setter)(struct js_context *ctx, struct js_object *self,
                          const char *name, struct js_value value);

struct js_object {
    enum js_class    klass;
    struct js_prop  *props;
    struct js_object *prototype;

    /* Feld */
    struct js_value *elements;
    size_t           length, capacity;

    /* Funktion */
    struct ast      *params;
    struct ast      *body;
    struct js_scope *closure;
    js_native        native;
    const char      *name;
    int32_t          arity;
    bool             is_arrow;
    struct js_value  bound_this;
    bool             has_bound_this;

    /* Anbindung an das Dokument */
    struct node     *dom;
    js_getter        getter;
    js_setter        setter;
};

/* ------------------------------------------------------------------ */
/* Namensraum                                                          */
/* ------------------------------------------------------------------ */

struct js_binding {
    char              *name;
    struct js_value    value;
    bool               constant;
    struct js_binding *next;
};

struct js_scope {
    struct js_binding *bindings;
    struct js_scope   *parent;
};

/* ------------------------------------------------------------------ */
/* Umgebung                                                            */
/* ------------------------------------------------------------------ */

struct js_arena {
    struct js_arena *next;
    char             data[];
};

enum js_signal {
    SIGNAL_NONE,
    SIGNAL_RETURN,
    SIGNAL_BREAK,
    SIGNAL_CONTINUE,
    SIGNAL_THROW,
};

#define JS_TIMERS 32

struct js_timer {
    bool             active;
    bool             repeating;
    uint64_t         due;
    uint64_t         period;
    struct js_value  callback;
    int32_t          id;
};

#define JS_CONSOLE_SIZE 4096

/* Zu jedem Knoten gehoert hoechstens ein Huellobjekt, damit ein Skript
 * denselben Knoten immer als dasselbe Objekt sieht. Die Tabelle haengt
 * am Zusammenhang, nicht am Modul - sonst wuerden sich zwei Fenster
 * gegenseitig ihre Objekte reichen. */
#define JS_WRAPPERS 512

struct js_context {
    /* Sammelbereich */
    struct js_arena *arenas;
    char            *base;
    size_t           used, capacity, total;
    bool             out_of_memory;

    struct js_scope *global_scope;
    struct js_object *global;

    struct js_object *object_prototype;
    struct js_object *array_prototype;
    struct js_object *string_prototype;
    struct js_object *number_prototype;
    struct js_object *function_prototype;
    struct js_object *node_prototype;

    struct document  *document;
    struct js_object *document_object;

    /* Ablauf */
    enum js_signal   signal;
    struct js_value  signal_value;    /* Rueckgabe oder geworfener Wert */
    char            *signal_label;
    struct js_value  this_value;

    int32_t          depth;
    uint64_t         steps;
    uint64_t         step_limit;

    char             error[256];
    bool             failed;

    char             console[JS_CONSOLE_SIZE];
    size_t           console_used;

    struct js_timer  timers[JS_TIMERS];
    int32_t          next_timer_id;

    struct node      *wrap_nodes[JS_WRAPPERS];
    struct js_object *wrap_objects[JS_WRAPPERS];
    size_t            wrap_count;

    js_change_hook   change_hook;
    void            *change_context;
    js_navigate_hook navigate_hook;
    void            *navigate_context;
    bool             dirty;
};

/* ------------------------------------------------------------------ */
/* jsvalue.c                                                           */
/* ------------------------------------------------------------------ */

void *js_alloc(struct js_context *ctx, size_t size);
char *js_strdup(struct js_context *ctx, const char *s);

struct js_string *js_string_new(struct js_context *ctx, const char *data,
                                size_t length);
struct js_value js_str(struct js_context *ctx, const char *text);
struct js_value js_strn(struct js_context *ctx, const char *text, size_t length);

struct js_value js_undefined(void);
struct js_value js_null(void);
struct js_value js_bool(bool value);
struct js_value js_number(js_num value);
struct js_value js_integer(int64_t value);
struct js_value js_object_value(struct js_object *object);

js_num  js_from_int(int64_t value);
int64_t js_to_int(js_num value);
bool    js_is_nan(js_num value);
bool    js_is_finite(js_num value);
js_num  js_add_num(js_num a, js_num b);
js_num  js_sub_num(js_num a, js_num b);
js_num  js_mul_num(js_num a, js_num b);
js_num  js_div_num(js_num a, js_num b);
js_num  js_mod_num(js_num a, js_num b);

struct js_object *js_new_object(struct js_context *ctx, enum js_class klass);
struct js_object *js_new_array(struct js_context *ctx, size_t length);
bool js_array_push(struct js_context *ctx, struct js_object *array,
                   struct js_value value);
bool js_array_set(struct js_context *ctx, struct js_object *array,
                  size_t index, struct js_value value);

struct js_value *js_own_slot(struct js_object *object, const char *name);
void js_set(struct js_context *ctx, struct js_object *object, const char *name,
            struct js_value value);
void js_set_hidden(struct js_context *ctx, struct js_object *object,
                   const char *name, struct js_value value);
void js_set_native(struct js_context *ctx, struct js_object *object,
                   const char *name, js_native fn, int32_t arity);
struct js_value js_get(struct js_context *ctx, struct js_object *object,
                       const char *name);
bool js_delete(struct js_object *object, const char *name);

/* ------------------------------------------------------------------ */
/* jsparse.c                                                           */
/* ------------------------------------------------------------------ */

struct ast *js_parse(struct js_context *ctx, const char *source, size_t length);

/* ------------------------------------------------------------------ */
/* jsinterp.c                                                          */
/* ------------------------------------------------------------------ */

struct js_value js_eval(struct js_context *ctx, struct ast *node,
                        struct js_scope *scope);
void js_exec_block(struct js_context *ctx, struct ast *list,
                   struct js_scope *scope);

struct js_scope *js_scope_new(struct js_context *ctx, struct js_scope *parent);
void js_declare(struct js_context *ctx, struct js_scope *scope,
                const char *name, struct js_value value, bool constant);
struct js_binding *js_lookup(struct js_scope *scope, const char *name);

struct js_value js_call(struct js_context *ctx, struct js_value callee,
                        struct js_value self, struct js_value *args,
                        size_t count);

void js_throw(struct js_context *ctx, const char *fmt, ...);

/* --- Umwandlungen --- */
bool             js_truthy(struct js_value v);
js_num           js_to_number(struct js_context *ctx, struct js_value v);
struct js_string *js_to_string(struct js_context *ctx, struct js_value v);
const char      *js_typeof(struct js_value v);
bool             js_equals(struct js_context *ctx, struct js_value a,
                           struct js_value b, bool strict);

/* Wandelt eine Festkommazahl in Text. */
size_t js_number_to_text(js_num value, char *out, size_t size);

/* Zugriff mit einem beliebigen Schluessel. */
struct js_value js_get_value(struct js_context *ctx, struct js_value target,
                             const char *key);
void js_set_value(struct js_context *ctx, struct js_value target,
                  const char *key, struct js_value value);

/* ------------------------------------------------------------------ */
/* jsbuiltin.c                                                         */
/* ------------------------------------------------------------------ */

void js_install_builtins(struct js_context *ctx);
void js_console_write(struct js_context *ctx, const char *text);

/* ------------------------------------------------------------------ */
/* jsdom.c                                                             */
/* ------------------------------------------------------------------ */

void js_flush_changes(struct js_context *ctx);

void js_install_dom(struct js_context *ctx);
struct js_object *js_wrap_node(struct js_context *ctx, struct node *node);

#endif /* JSINT_H */
