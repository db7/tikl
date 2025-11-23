#ifndef TIKL_SUBST_H
#define TIKL_SUBST_H

#include <stdbool.h>
#include <stddef.h>

typedef const char *(*tikl_subst_lookup_fn)(void *userdata,
                                            const char *key,
                                            size_t len);

char *tikl_expand_placeholders(const char *input,
                               bool allow_expansion,
                               bool helpers_enabled,
                               tikl_subst_lookup_fn lookup,
                               void *lookup_ctx,
                               const char *who,
                               int *status);

#endif /* TIKL_SUBST_H */
