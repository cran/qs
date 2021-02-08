#ifndef EXPAND_BINDING_VALUE_H
#define EXPAND_BINDING_VALUE_H

int get_bndcell_tag(SEXP e);
void R_expand_binding_value(SEXP b);
SEXP R_make_binding_value(SEXP val);

#endif
