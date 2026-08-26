/* jsmain.c - Anlegen, Ausfuehren und Abraeumen einer Skriptumgebung. */

#include "jsint.h"
#include "kstring.h"
#include "mm.h"

struct js_context *js_create(void)
{
    struct js_context *ctx = kmalloc(sizeof(*ctx));

    if (!ctx)
        return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->step_limit = 40000000ULL;

    ctx->object_prototype = js_new_object(ctx, CLASS_OBJECT);
    if (!ctx->object_prototype) {
        kfree(ctx);
        return NULL;
    }
    ctx->object_prototype->prototype = NULL;

    ctx->array_prototype = js_new_object(ctx, CLASS_OBJECT);
    ctx->string_prototype = js_new_object(ctx, CLASS_OBJECT);
    ctx->number_prototype = js_new_object(ctx, CLASS_OBJECT);
    ctx->function_prototype = js_new_object(ctx, CLASS_OBJECT);

    ctx->global = js_new_object(ctx, CLASS_OBJECT);
    ctx->global_scope = js_scope_new(ctx, NULL);
    ctx->this_value = js_object_value(ctx->global);

    if (!ctx->global || !ctx->global_scope) {
        js_destroy(ctx);
        return NULL;
    }

    js_install_builtins(ctx);
    js_install_dom(ctx);
    return ctx;
}

void js_destroy(struct js_context *ctx)
{
    if (!ctx)
        return;

    struct js_arena *block = ctx->arenas;

    while (block) {
        struct js_arena *next = block->next;

        kfree(block);
        block = next;
    }
    kfree(ctx);
}

void js_on_change(struct js_context *ctx, js_change_hook hook, void *context)
{
    if (!ctx)
        return;
    ctx->change_hook = hook;
    ctx->change_context = context;
}

void js_on_navigate(struct js_context *ctx, js_navigate_hook hook,
                    void *context)
{
    if (!ctx)
        return;
    ctx->navigate_hook = hook;
    ctx->navigate_context = context;
}

const char *js_error(const struct js_context *ctx)
{
    return ctx && ctx->error[0] ? ctx->error : NULL;
}

const char *js_console(const struct js_context *ctx)
{
    return ctx ? ctx->console : "";
}

void js_console_clear(struct js_context *ctx)
{
    if (!ctx)
        return;
    ctx->console[0] = '\0';
    ctx->console_used = 0;
}

/* Meldet dem Browser, dass sich der Baum geaendert hat. Jeder Weg, auf
 * dem ein Skript laufen kann - Seitenskript, Ereignis, Zeitgeber -
 * endet hier, sonst bliebe die Aenderung unsichtbar. */
void js_flush_changes(struct js_context *ctx)
{
    if (!ctx || !ctx->dirty)
        return;
    ctx->dirty = false;
    if (ctx->change_hook)
        ctx->change_hook(ctx->change_context);
}

bool js_run(struct js_context *ctx, const char *source, size_t length)
{
    if (!ctx || !source)
        return false;

    ctx->error[0] = '\0';
    ctx->failed = false;
    ctx->signal = SIGNAL_NONE;
    ctx->signal_label = NULL;
    ctx->steps = 0;
    ctx->depth = 0;

    struct ast *program = js_parse(ctx, source, length);

    if (!program) {
        if (!ctx->error[0])
            strlcpy(ctx->error, "Das Skript konnte nicht gelesen werden.",
                    sizeof(ctx->error));
        return false;
    }

    js_exec_block(ctx, program->list, ctx->global_scope);

    bool thrown = ctx->signal == SIGNAL_THROW;

    ctx->signal = SIGNAL_NONE;
    js_flush_changes(ctx);

    if (ctx->out_of_memory) {
        strlcpy(ctx->error, "Dem Skript ist der Speicher ausgegangen.",
                sizeof(ctx->error));
        return false;
    }
    return !thrown && !ctx->failed;
}

bool js_run_handler(struct js_context *ctx, const char *source,
                    struct node *self)
{
    if (!ctx || !source)
        return false;

    ctx->error[0] = '\0';
    ctx->failed = false;
    ctx->signal = SIGNAL_NONE;
    ctx->steps = 0;
    ctx->depth = 0;

    struct ast *program = js_parse(ctx, source, strlen(source));

    if (!program)
        return false;

    struct js_value saved = ctx->this_value;
    struct js_object *wrapper = js_wrap_node(ctx, self);

    if (wrapper)
        ctx->this_value = js_object_value(wrapper);

    struct js_scope *scope = js_scope_new(ctx, ctx->global_scope);

    if (wrapper) {
        js_declare(ctx, scope, "this", js_object_value(wrapper), false);
        js_declare(ctx, scope, "event", js_undefined(), false);
    }
    js_exec_block(ctx, program->list, scope);

    ctx->this_value = saved;

    bool thrown = ctx->signal == SIGNAL_THROW;

    ctx->signal = SIGNAL_NONE;
    js_flush_changes(ctx);
    return !thrown && !ctx->failed;
}
