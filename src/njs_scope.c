
/*
 * Copyright (C) Alexander Borisov
 * Copyright (C) NGINX, Inc.
 */


#include <njs_main.h>


static njs_value_t *njs_scope_value_index(njs_vm_t *vm, const njs_value_t *src,
    njs_uint_t runtime, njs_index_t **index);


njs_index_t
njs_scope_temp_index(njs_parser_scope_t *scope)
{
    scope = njs_function_scope(scope);
    if (njs_slow_path(scope == NULL)) {
        return NJS_INDEX_ERROR;
    }

    return njs_scope_index(scope->type, scope->items++, NJS_LEVEL_LOCAL,
                           NJS_VARIABLE_VAR);
}


njs_value_t **
njs_scope_make(njs_vm_t *vm, uint32_t count)
{
    size_t       size;
    njs_value_t  **refs, *values;

    size = (count * sizeof(njs_value_t *)) + (count * sizeof(njs_value_t));

    refs = njs_mp_alloc(vm->mem_pool, size);
    if (njs_slow_path(refs == NULL)) {
        njs_memory_error(vm);
        return NULL;
    }

    values = (njs_value_t *) ((u_char *) refs
                              + (count * sizeof(njs_value_t *)));

    while (count != 0) {
        count--;

        refs[count] = &values[count];

        njs_set_invalid(refs[count]);
    }

    return refs;
}


njs_index_t
njs_scope_global_index(njs_vm_t *vm, const njs_value_t *src, njs_uint_t runtime)
{
    njs_index_t  index, *retval;
    njs_value_t  **values, *value;

    value = njs_scope_value_index(vm, src, runtime, &retval);
    if (njs_slow_path(value == NULL)) {
        return NJS_INDEX_ERROR;
    }

    if (*retval != NJS_INDEX_ERROR) {
        return *retval;
    }

    if (vm->scope_absolute == NULL) {
        vm->scope_absolute = njs_arr_create(vm->mem_pool, 8,
                                            sizeof(njs_value_t *));
        if (njs_slow_path(vm->scope_absolute == NULL)) {
            return NJS_INDEX_ERROR;
        }
    }

    index = vm->scope_absolute->items;

    values = njs_arr_add(vm->scope_absolute);
    if (njs_slow_path(values == NULL)) {
        return NJS_INDEX_ERROR;
    }

    *values = value;

    vm->levels[NJS_LEVEL_STATIC] = vm->scope_absolute->start;

    *retval = njs_scope_index(NJS_SCOPE_GLOBAL, index, NJS_LEVEL_STATIC,
                              NJS_VARIABLE_VAR);

    return *retval;
}


njs_value_t *
njs_scope_value_get(njs_vm_t *vm, njs_index_t index)
{
    return njs_scope_value(vm, index);
}


static njs_int_t
njs_scope_values_hash_test(njs_flathsh_query_t *fhq, void *data)
{
    njs_str_t    string;
    njs_value_t  *value;

    value = *(njs_value_t **) data;

    if (njs_is_string(value)) {
        /* parser strings are always initialized. */
        njs_string_get_unsafe(value, &string);

    } else {
        string.start = (u_char *) value;
        string.length = sizeof(njs_value_t);
    }

    if (fhq->key.length == string.length
        && memcmp(fhq->key.start, string.start, string.length) == 0)
    {
        return NJS_OK;
    }

    return NJS_DECLINED;
}


static const njs_flathsh_proto_t  njs_values_hash_proto
    njs_aligned(64) =
{
    njs_scope_values_hash_test,
    njs_flathsh_proto_alloc,
    njs_flathsh_proto_free,
};


/*
 * Constant values such as njs_value_true are copied to values_hash during
 * code generation when they are used as operands to guarantee aligned value.
 */

static njs_value_t *
njs_scope_value_index(njs_vm_t *vm, const njs_value_t *src, njs_uint_t runtime,
    njs_index_t **index)
{
    u_char               *start;
    uint32_t             value_size, size, length;
    njs_int_t            ret;
    njs_str_t            str;
    njs_bool_t           is_string;
    njs_value_t          *value;
    njs_string_t         *string;
    njs_flathsh_t        *values_hash;
    njs_object_prop_t    *pr;
    njs_flathsh_query_t  fhq;

    is_string = 0;
    value_size = sizeof(njs_value_t);

    if (njs_is_string(src)) {
        /* parser strings are always initialized. */
        njs_string_get_unsafe(src, &str);

        size = (uint32_t) str.length;
        start = str.start;

        is_string = 1;

    } else {
        size = value_size;
        start = (u_char *) src;
    }

    fhq.key_hash = njs_djb_hash(start, size);
    fhq.key.length = size;
    fhq.key.start = start;
    fhq.proto = &njs_values_hash_proto;

    if (njs_flathsh_find(&vm->shared->values_hash, &fhq) == NJS_OK) {
        value = ((njs_object_prop_t *) fhq.value)->u.val;

        *index = (njs_index_t *) ((u_char *) value + sizeof(njs_value_t));

    } else if (runtime && njs_flathsh_find(&vm->values_hash, &fhq) == NJS_OK) {
        value = ((njs_object_prop_t *) fhq.value)->u.val;

        *index = (njs_index_t *) ((u_char *) value + sizeof(njs_value_t));

    } else {
        if (is_string) {
            length = src->string.data->length;

            if (size != length && length > NJS_STRING_MAP_STRIDE) {
                size = njs_string_map_offset(size)
                       + njs_string_map_size(length);
            }

            value_size += sizeof(njs_string_t) + size;
        }

        value_size += sizeof(njs_index_t);

        value = njs_mp_align(vm->mem_pool, sizeof(njs_value_t), value_size);
        if (njs_slow_path(value == NULL)) {
            return NULL;
        }

        *value = *src;

        if (is_string) {
            string = (njs_string_t *) ((u_char *) value + sizeof(njs_value_t)
                                       + sizeof(njs_index_t));

            value->string.data = string;

            string->start = (u_char *) string + sizeof(njs_string_t);
            string->length = src->string.data->length;
            string->size = src->string.data->size;

            memcpy(string->start, start, size);
        }

        *index = (njs_index_t *) ((u_char *) value + sizeof(njs_value_t));
        **index = NJS_INDEX_ERROR;

        fhq.replace = 0;
        fhq.pool = vm->mem_pool;

        values_hash = runtime ? &vm->values_hash : &vm->shared->values_hash;

        ret = njs_flathsh_insert(values_hash, &fhq);
        if (njs_slow_path(ret != NJS_OK)) {
            return NULL;
        }

        pr = fhq.value;
        pr->u.val = value;
    }

    if (start != (u_char *) src) {
        /*
         * The source node value must be updated with the shared value
         * allocated from the permanent memory pool because the node
         * value can be used as a variable initial value.
         */
        *(njs_value_t *) src = *value;
    }

    return value;
}
