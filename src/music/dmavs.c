/* reDM: AudioVBScript (dmscript.dll's CAudioVBScriptEngine, 0x40b7c1..0x40e0a8): a lexer, a parser that compiles a
 * script into statement, expression and reference tables, and an executor that runs them over a stack of VARIANTs.
 *
 * The language: Dim statements, then Subs with no parameters; statements are assignments (Set for objects),
 * calls (a routine of the script, a method of the "DirectMusic" item, or a method at the end of a dotted chain of
 * properties) and If / ElseIf / Else / End If.  Numbers are 32-bit integers ('^' '*' '\' Mod '+' '-', comparisons
 * giving -1 or 0, And Or Not bitwise, Is for objects); strings only appear as a whole argument or value; names the
 * script does not declare become variables of the routine using them, unless the "DirectMusic" item knows them.
 * An If condition is true when it is exactly -1. */
#include "dmavs.h"
#include <ole2.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define E_AVS_SYNTAX ((HRESULT)0x88781223)
#define E_AVS_RUNTIME ((HRESULT)0x88781224)
#define E_AVS_UNSUPPORTED_VARTYPE ((HRESULT)0x88781214)
#define NBUILTIN 3                      /* true, false, nothing: the first three variables (DAT_00402f5c) */
const GUID AVS_IID_PRIVATE = { 0x1fcc43db, 0xbad8, 0x4a88, { 0xbc, 0x77, 0x4e, 0x1a, 0xe0, 0x2d, 0x9c, 0x79 } };
#define VARBASE 1000001                 /* DISPIDs of variables (0xf4241); routines are 1..n */

/* ------------------------------------------------------------------ tokens (the lexer's) */
enum {
    T_EOF = 0, T_SUB, T_DIM, T_IF, T_THEN, T_END, T_ELSEIF, T_ELSE, T_SET, T_CALL,
    T_LPAREN = 10, T_RPAREN, T_COMMA, T_MINUS, T_NOT, T_POW, T_MUL, T_IDIV, T_MOD, T_PLUS,
    T_LT = 20, T_LE, T_GT, T_GE, T_EQ, T_NE, T_IS, T_AND, T_OR, T_NL,
    T_IDENT = 30, T_IDENTDOT, T_STRING, T_NUMBER,
    T_NEG = 1                           /* (in expressions: a unary minus) */
};

/* ------------------------------------------------------------------ growable arrays */
typedef struct { int kind, data; } item;            /* expression RPN / values / infix tokens */
typedef struct { int name, stmt, nlocals; } routine;
typedef struct { int name, dispid; } global;        /* dispid -1: a script variable; else the "DirectMusic" item's */
typedef struct { int kind, line, a, b; } stmt;      /* 0 end, 1 if (a: clause, b: next), 2 assignment, 3 call */
typedef struct { int kind, expr, body; } clause;    /* 0 end, 1 else, 2 if/elseif */
typedef struct { int set, ref, expr; } assign;
typedef struct { int kind, data, args; } call;      /* kind 0: a name, 1: a reference's last property */
typedef struct { int local, chain, index; } ref;

#define VEC(T) struct { T *p; int n, cap; }
static int vgrow(void **p, int *cap, int need, size_t sz)
{
    if (need <= *cap) return 1;
    int c = *cap ? *cap : 4;
    while (c < need) c *= 2;
    void *n = realloc(*p, (size_t)c * sz);
    if (!n) return 0;
    *p = n; *cap = c;
    return 1;
}
#define VPUSH(v, ...) (vgrow((void **)&(v).p, &(v).cap, (v).n + 1, sizeof(*(v).p)) ? \
                       ((v).p[(v).n] = (__typeof__(*(v).p)){ __VA_ARGS__ }, (v).n++, S_OK) : E_OUTOFMEMORY)

typedef struct {
    VEC(routine) routines;          /* +0x00 */
    VEC(global) globals;            /* +0x0c */
    VEC(char) pool;                 /* +0x18 names and strings, by offset */
    VEC(stmt) stmts;                /* +0x28 */
    VEC(int) chain;                 /* +0x34 references' names, each ended by -1 */
    VEC(ref) refs;                  /* +0x40 */
    VEC(item) values;               /* +0x4c 0 number, 1 string, 2 reference */
    VEC(call) calls;                /* +0x58 */
    VEC(item) exprs;                /* +0x64 1 operator, 2 value, 3 call, 4 omitted argument; 0 ends one */
    VEC(clause) clauses;            /* +0x70 */
    VEC(assign) assigns;            /* +0x7c */
} prog;

static int pool_add(prog *g, const char *s, int *off)
{
    int n = (int)strlen(s) + 1;
    if (!vgrow((void **)&g->pool.p, &g->pool.cap, g->pool.n + n, 1)) return 0;
    memcpy(g->pool.p + g->pool.n, s, n);
    *off = g->pool.n;
    g->pool.n += n;
    return 1;
}
static const char *pool_str(const prog *g, int off) { return off >= 0 && off < g->pool.n ? g->pool.p + off : ""; }

/* a name table (dmscript's hash of names, case-insensitive) */
typedef struct { VEC(global) e; } symtab;           /* (name offset, value) */
/* 0040c346: 0 found (value and name out), 1 not found (added when asked: its value is *val) */
static int sym_find(prog *g, symtab *t, int add, const char *name, int *val, int *off)
{
    for (int i = 0; i < t->e.n; i++)
        if (!_stricmp(pool_str(g, t->e.p[i].name), name)) { *val = t->e.p[i].dispid; *off = t->e.p[i].name; return 0; }
    if (!add) return 1;
    if (!pool_add(g, name, off)) return E_OUTOFMEMORY;
    if (FAILED(VPUSH(t->e, *off, *val))) return E_OUTOFMEMORY;
    return 1;
}

/* ------------------------------------------------------------------ the lexer (0040bd83) */
typedef struct {
    const WCHAR *cur, *next;
    int line, col, tok;
    char text[256];                 /* +0x14: an identifier's, a string's or an error's text */
    int num;                        /* +0x114: a number's value, or the error (1..5) */
} lexer;

static const char *const s_lexerr[6] = { "Unexpected error!", "Invalid character", "Identifier too long",
                                        "String too long", "Unterminated string constant", "Number too large" };
static const struct { const WCHAR *w; int tok; } s_keywords[] = {
    { L"sub", T_SUB }, { L"dim", T_DIM }, { L"if", T_IF }, { L"then", T_THEN }, { L"end", T_END },
    { L"elseif", T_ELSEIF }, { L"else", T_ELSE }, { L"set", T_SET }, { L"call", T_CALL }, { L"not", T_NOT },
    { L"mod", T_MOD }, { L"is", T_IS }, { L"and", T_AND }, { L"or", T_OR },
};
static const struct { WCHAR c; int tok; } s_ops[] = {
    { '(', T_LPAREN }, { ')', T_RPAREN }, { ',', T_COMMA }, { '-', T_MINUS }, { '^', T_POW }, { '*', T_MUL },
    { '\\', T_IDIV }, { '+', T_PLUS }, { '<', T_LT }, { '>', T_GT }, { '=', T_EQ },
};
static int is_alpha(WCHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(WCHAR c) { return c >= '0' && c <= '9'; }
static int is_alnum(WCHAR c) { return is_alpha(c) || is_digit(c); }
static WCHAR lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? c + 0x20 : c; }

static void lex_advance(lexer *L)                  /* 0040bd24 */
{
    if (L->next) { L->col += (int)(L->next - L->cur); L->cur = L->next; L->next = NULL; }
    else if (*L->cur) { L->cur++; L->col++; }    /* (never past the text's end) */
}
static void lex_error(lexer *L, int code)          /* 0040bd42 */
{
    if (code < 1 || code > 5) code = 0;
    L->tok = T_EOF;
    L->num = code;
    lstrcpynA(L->text, s_lexerr[code], sizeof(L->text));
}
static void lex_scan(lexer *L)
{
    for (;;) {
        WCHAR c = *L->cur;
        if (!c) { L->tok = T_EOF; return; }
        if (c == '\t' || c == ' ') { lex_advance(L); continue; }
        if (c == '\n') { L->tok = T_NL; return; }
        if (c == '\r') { L->col--; lex_advance(L); continue; }
        if (c == '\'') {                    /* a comment: to the end of the line (one column) */
            while (*L->cur != '\n') { L->cur++; if (!*L->cur) break; }
            L->cur--;
            lex_advance(L);
            continue;
        }
        break;
    }
    const WCHAR *p = L->cur;
    WCHAR c = *p;
    if (c == '"') {
        L->next = p + 1;
        char *o = L->text;
        for (;;) {
            WCHAR d = *L->next;
            if (d > 0x7f) { lex_advance(L); lex_error(L, 1); return; }
            if (d == '\n' || d == '\r' || !d) { lex_error(L, 4); return; }  /* (the original reads on past a NUL) */
            if (d == '"') {
                L->next++;
                if (*L->next != '"') { *o = 0; L->tok = T_STRING; return; }
            }
            *o++ = (char)*L->next;
            L->next++;
            if (o > L->text + 255) { lex_error(L, 3); return; }
        }
    }
    if (is_digit(c)) {
        L->num = 0;
        L->next = p;
        WCHAR d;
        for (;;) {
            L->num = L->num * 10 + (*L->next - '0');
            L->next++;
            d = *L->next;
            if (!is_digit(d)) break;
            if (L->num > 0xccccccb && (L->num != 0xccccccc || d > '7')) break;
        }
        if (is_digit(*L->next)) { lex_error(L, 5); return; }
        L->tok = T_NUMBER;
        return;
    }
    if (!is_alpha(c) && c != '_') {
        for (size_t k = 0; k < sizeof(s_ops) / sizeof(*s_ops); k++) {
            if (c != s_ops[k].c) continue;
            L->tok = s_ops[k].tok;
            if (L->tok == T_LT) {
                if (p[1] == '=') L->tok = T_LE;
                else if (p[1] == '>') L->tok = T_NE;
                else return;
            } else if (L->tok == T_GT) {
                if (p[1] != '=') return;
                L->tok = T_GE;
            } else return;
            L->next = p + 2;
            return;
        }
        lex_error(L, 1);
        return;
    }
    /* a keyword: its letters (any case) and then no letter or digit ('_' may follow) */
    for (size_t k = 0; k < sizeof(s_keywords) / sizeof(*s_keywords); k++) {
        const WCHAR *w = s_keywords[k].w, *q = p;
        while (*w && *q && lower(*q) == *w) { w++; q++; }
        if (!*w && !is_alnum(*q)) { L->tok = s_keywords[k].tok; L->next = q; return; }
    }
    L->next = p + 1;
    while (is_alnum(*L->next) || *L->next == '_') L->next++;
    if ((((L->next - p) * 2) & ~1) >= 0x1ff) { lex_error(L, 2); return; }
    char *o = L->text;
    for (const WCHAR *q = p; q < L->next; q++) *o++ = (char)*q;
    *o = 0;
    if (*L->next == '.') { L->next++; L->tok = T_IDENTDOT; return; }
    L->tok = T_IDENT;
}
static void lex_next(lexer *L)                     /* 0040c08a */
{
    L->text[0] = 0;
    L->num = 0;
    int was = L->tok;
    for (;;) {
        if (was == T_NL) { L->line++; L->col = 1; }
        lex_scan(L);
        if (was != T_NL || L->tok != T_NL) break;
        lex_advance(L);
    }
}

/* ------------------------------------------------------------------ the parser (0040d955) */
typedef struct {
    lexer *L;
    prog *g;
    HRESULT hr;
    avs_error *err;
    IDispatch *named;
    symtab routines, globals, props;    /* +0x14 +0x18 +0x20 */
    VEC(int) proplist;                  /* +0x24 */
    VEC(item) ops;                      /* +0x30 the operator stack while an expression is converted */
    int depth;                          /* nested Ifs and calls in arguments (the parser and executor recurse) */
} parser;
#define MAXNEST 256

static const char *const s_perr[27] = {
    "Unexpected error!", "Expected Sub or Dim statement", "Expected identifier", "Expected line break",
    "Expected End Sub", "Expected Sub", "Expected statement", "Expected '('", "Expected '='",
    "All Dim statements must occur before all Sub statements", "Invalid expression or missing line break",
    "Invalid expression or missing Then", "Invalid expression or missing ')'",
    "Strings may not appear inside numerical expressions", "Invalid expression -- expected a number or variable",
    "A variable with this name already exists", "Another routine with this name already exists",
    "Invalid expression -- expected ')'", "Invalid expression -- encountered ')' without a preceding '('",
    "Expected Then", "Expected End If", "Expected If",
    "Expected line break or ElseIf should be a single word without space before If",
    "The values True, False, and Nothing are read-only and cannot be set",
    "Cannot use parentheses when calling a Sub", "Sub name used where variable was expected", "Unexpected error!",
};
static WCHAR *wdup(const WCHAR *s, int n)
{
    if (n < 0) n = lstrlenW(s);
    WCHAR *d = malloc((n + 1) * sizeof(WCHAR));
    if (d) { memcpy(d, s, n * sizeof(WCHAR)); d[n] = 0; }
    return d;
}
static WCHAR *wfroma(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    WCHAR *d = malloc((n ? n : 1) * sizeof(WCHAR));
    if (d) { if (!n) d[0] = 0; else MultiByteToWideChar(CP_ACP, 0, s, -1, d, n); }
    return d;
}
/* 0040c764: the first error is the one reported (the lexer's message when the lexer failed) */
static void perror_(parser *P, int code)
{
    if (P->hr < 0) return;
    if (code < 0 || code > 0x1a) code = 0;
    P->hr = E_AVS_SYNTAX;
    const char *m = (P->L->tok != T_EOF || P->L->num == 0) ? s_perr[code] : P->L->text;
    if (!P->err) return;
    avs_error_free(P->err);                         /* (the latest error reported is the one kept) */
    P->err->scode = E_AVS_SYNTAX;                   /* 00409f89 */
    P->err->desc = wfroma(m);
    P->err->source = wdup(L"Microsoft AudioVBScript syntax error", -1);
    P->err->line = P->L->line - 1;
    P->err->col = P->L->col - 1;
    const WCHAR *e = P->L->cur;
    while (*e && *e != '\n' && *e != '\r') e++;
    P->err->text = wdup(P->L->cur, (int)(e - P->L->cur));
}
/* 0040c856: the next token; a lexer error stops the parse */
static int p_accept(parser *P)
{
    lex_advance(P->L);
    lex_next(P->L);
    if (P->L->tok == T_EOF && P->L->num != 0) { perror_(P, 0); return 1; }
    return 0;
}
static int expect(parser *P, int tok, int code)     /* 0040c881: this token, then the next */
{
    if (P->L->tok == tok) return p_accept(P);
    perror_(P, code);
    return 1;
}
static int expect_next(parser *P, int tok, int code)    /* 0040c8a1: the next token must be this one */
{
    if (p_accept(P)) return 1;
    if (P->L->tok == tok) return 0;
    perror_(P, code);
    return 1;
}
static int set_hr(parser *P, HRESULT hr) { P->hr = hr; return hr < 0; }

/* 0040cb2d: a reference: a name (the current identifier, or the given one) and, when the identifier has a dot, the
 * properties after it; a name the script does not declare, or a global the "DirectMusic" item knows, is added */
static int dispid_of(IDispatch *d, const char *name)    /* 0040b5c9 */
{
    if (!d) return -1;
    WCHAR *w = wfroma(name);
    if (!w) return -1;
    DISPID id = -1;
    LPOLESTR n = w;
    IDispatch_GetIDsOfNames(d, &IID_NULL, &n, 1, 0x409, &id);
    free(w);
    return id;
}
static int reference(parser *P, int r, symtab *locals, const char *name)
{
    if (P->hr < 0) return 0;
    prog *g = P->g;
    int start = g->chain.n, local = 0, idx = 0, off = 0, val;
    const char *nm = name ? name : P->L->text;
    if (!sym_find(g, &P->routines, 0, nm, &val, &off)) { perror_(P, 0x19); return 0; }
    if (sym_find(g, &P->globals, 0, nm, &idx, &off)) {
        if (!sym_find(g, locals, 0, nm, &idx, &off)) local = 1;
        else {
            int id = dispid_of(P->named, nm);
            if (id == -1) {                 /* a variable of the routine */
                idx = g->routines.p[r].nlocals;
                local = 1;
                int x = sym_find(g, locals, 1, nm, &idx, &off);
                if (x < 0) { P->hr = x; return 0; }
                g->routines.p[r].nlocals++;
            } else {                        /* the "DirectMusic" item's */
                idx = g->globals.n;
                int x = sym_find(g, &P->globals, 1, nm, &idx, &off);
                if (x < 0) { P->hr = x; return 0; }
                if (set_hr(P, VPUSH(g->globals, off, id))) return 0;
            }
        }
    }
    if (set_hr(P, VPUSH(g->chain, off))) return 0;
    if (!name) {
        do {
            if (p_accept(P)) return 0;
            if (P->L->tok != T_IDENT && P->L->tok != T_IDENTDOT) { perror_(P, 2); return 0; }
            int pv = P->proplist.n, po = 0;
            int x = sym_find(g, &P->props, 1, P->L->text, &pv, &po);
            if (x < 0) { P->hr = x; return 0; }
            if (x == 1 && set_hr(P, VPUSH(P->proplist, po))) return 0;
            if (set_hr(P, VPUSH(g->chain, po))) return 0;
        } while (P->L->tok != T_IDENT);
    }
    if (set_hr(P, VPUSH(g->chain, -1))) return 0;
    int n = g->refs.n;
    if (set_hr(P, VPUSH(g->refs, local, start, idx))) return 0;
    return n;
}

/* 0040cd6d / 0040cdbd: what a statement or an operand starts with: a plain name (resolved later, it may be a call)
 * or a dotted reference (parsed now) */
typedef struct { int plain; char *name; int ref; } target;
static void target_parse(parser *P, int r, symtab *locals, target *t)
{
    t->plain = P->L->tok == T_IDENT;
    t->ref = 0;
    t->name = NULL;
    if (t->plain) {
        t->name = _strdup(P->L->text);
        if (!t->name) P->hr = E_OUTOFMEMORY;
    } else t->ref = reference(P, r, locals, NULL);
}
static int target_ref(parser *P, int r, symtab *locals, target *t)
{
    return t->plain ? reference(P, r, locals, t->name) : t->ref;
}

/* 0040b415 / 0040b5a3: the infix tokens to RPN, by precedence (unary operators bind right) */
static int prec(int t)                             /* 0040b351 */
{
    switch (t) {
    case T_LPAREN: return 0;
    case T_NEG: return 9;
    case T_MINUS: case T_PLUS: return 5;
    case T_NOT: return 3;
    case T_POW: return 10;
    case T_MUL: return 8;
    case T_IDIV: return 7;
    case T_MOD: return 6;
    case T_AND: return 2;
    case T_OR: return 1;
    }
    if (t >= T_LT && t <= T_IS) return 4;
    return 0xc;
}
static int is_unary(int t) { return t == T_NEG || t == T_NOT; }
static HRESULT to_rpn(parser *P, item *in, int n, void *outv)
{
    VEC(item) *out = outv;
    P->ops.n = 0;
    for (int k = 0; k < n; k++) {
        item it = in[k];
        if (it.kind == 2 || it.kind == 3) { if (FAILED(VPUSH(*out, it.kind, it.data))) return E_OUTOFMEMORY; continue; }
        int t = it.data;
        if (t == T_RPAREN) {
            for (;;) {
                if (!P->ops.n) return E_FAIL;
                item top = P->ops.p[P->ops.n - 1];
                if (top.data == T_LPAREN) break;
                if (FAILED(VPUSH(*out, top.kind, top.data))) return E_OUTOFMEMORY;
                P->ops.n--;
            }
            P->ops.n--;
            continue;
        }
        if (t != T_LPAREN) {
            int p = prec(t), u = is_unary(t);
            while (P->ops.n) {
                item top = P->ops.p[P->ops.n - 1];
                if (prec(top.data) < p || (u && is_unary(top.data))) break;
                if (FAILED(VPUSH(*out, top.kind, top.data))) return E_OUTOFMEMORY;
                P->ops.n--;
            }
        }
        if (FAILED(VPUSH(P->ops, it.kind, it.data))) return E_OUTOFMEMORY;
    }
    while (P->ops.n) {
        item top = P->ops.p[--P->ops.n];
        if (top.data == T_LPAREN) { P->ops.n = 0; return E_FAIL; }
        if (FAILED(VPUSH(*out, top.kind, top.data))) return E_OUTOFMEMORY;
    }
    return VPUSH(*out, 0, 0);
}

static int call_parse(parser *P, int r, symtab *locals, target *t, int paren);

/* 0040bccc */
static int unary_tok(int t) { return t == T_MINUS || t == T_NOT; }
static int binary_tok(int t) { return t == T_MINUS || (t >= T_POW && t <= T_OR); }

/* 0040cdde: an expression into `out` (the program's expressions, or an argument list); in an argument list in
 * parentheses a ')' may end it; returns where it starts */
static int expression(parser *P, int r, symtab *locals, void *outv, int inparen, int firstnoparen)
{
    VEC(item) *out = outv ? outv : (void *)&P->g->exprs;
    VEC(item) infix = { 0 };
    prog *g = P->g;
    int start = 0;
    if (P->L->tok == T_STRING) {
        int off;
        if (!pool_add(g, P->L->text, &off)) { P->hr = E_OUTOFMEMORY; goto fail; }
        int v = g->values.n;
        if (set_hr(P, VPUSH(g->values, 1, off))) goto fail;
        if (set_hr(P, VPUSH(infix, 2, v))) goto fail;
        if (p_accept(P)) goto fail;
        goto convert;
    }
    int depth = 0, stop;
    do {
        while (!unary_tok(P->L->tok)) {
            int t = P->L->tok;
            if (t == T_LPAREN) {
                if (set_hr(P, VPUSH(infix, 1, T_LPAREN))) goto fail;
                if (p_accept(P)) goto fail;
                depth++;
                continue;
            }
            if (t == T_IDENT || t == T_IDENTDOT) {
                target tg;
                target_parse(P, r, locals, &tg);
                if (P->hr < 0 || p_accept(P)) { free(tg.name); goto fail; }
                if (P->L->tok == T_LPAREN) {
                    /* (dmscript pushes the call whatever its arguments did, losing an error in them: the parse
                     * goes on, and a later error, or none, is what Load reports) */
                    int c = call_parse(P, r, locals, &tg, 1);
                    free(tg.name);
                    if (set_hr(P, VPUSH(infix, 3, c))) goto fail;
                } else {
                    int rf = target_ref(P, r, locals, &tg);
                    free(tg.name);
                    if (P->hr < 0) goto fail;
                    int v = g->values.n;
                    if (set_hr(P, VPUSH(g->values, 2, rf))) goto fail;
                    if (set_hr(P, VPUSH(infix, 2, v))) goto fail;
                }
            } else if (t == T_NUMBER) {
                int v = g->values.n;
                if (set_hr(P, VPUSH(g->values, 0, P->L->num))) goto fail;
                if (set_hr(P, VPUSH(infix, 2, v))) goto fail;
                if (p_accept(P)) goto fail;
            } else {
                perror_(P, t != T_STRING ? 0xe : 0xd);
                goto fail;
            }
            /* after an operand: closing parentheses, then an operator or the end */
            while (P->L->tok == T_RPAREN) {
                if (!depth) {
                    if (!inparen) { perror_(P, 0x12); goto fail; }
                    break;
                }
                if (set_hr(P, VPUSH(infix, 1, T_RPAREN))) goto fail;
                if (p_accept(P)) goto fail;
                depth--;
            }
            if (binary_tok(P->L->tok)) {
                if (set_hr(P, VPUSH(infix, 1, P->L->tok))) goto fail;
                stop = p_accept(P);
                goto next;
            }
            if (!depth) goto convert;
            perror_(P, firstnoparen && depth == 1 && P->L->tok == T_COMMA ? 0x18 : 0x11);
            goto fail;
        }
        {
            int t = P->L->tok == T_MINUS ? T_NEG : P->L->tok;
            if (set_hr(P, VPUSH(infix, 1, t))) goto fail;
            stop = p_accept(P);
        }
next:;
    } while (!stop);
fail:
    free(infix.p);
    return 0;
convert:
    start = out->n;
    {
        HRESULT hr = to_rpn(P, infix.p, infix.n, out);
        free(infix.p);
        if (FAILED(hr)) { P->hr = hr; return 0; }
    }
    return start;
}

/* 0040d0f1: a call's arguments: each an expression ended by 0, an omitted one 4 then 0; 0 ends the list */
static int arguments(parser *P, int r, symtab *locals, int paren)
{
    if (P->hr < 0 || (paren && p_accept(P))) return 0;
    VEC(item) a = { 0 };
    int first = 1, code = paren ? 12 : 10, endtok = paren ? T_RPAREN : T_NL, ret = 0;
    while (P->L->tok != endtok) {
        if (!first && (expect(P, T_COMMA, code) || P->L->tok == T_EOF)) goto out;
        if (P->L->tok == T_COMMA) {
            if (set_hr(P, VPUSH(a, 4, code)) || set_hr(P, VPUSH(a, 0, code))) goto out;
        } else {
            expression(P, r, locals, &a, paren, !paren && first);
            if (P->hr < 0) goto out;
        }
        first = 0;
    }
    if (paren && p_accept(P)) goto out;
    if (set_hr(P, VPUSH(a, 0, code))) goto out;
    ret = P->g->exprs.n;
    for (int k = 0; k < a.n; k++) if (set_hr(P, VPUSH(P->g->exprs, a.p[k].kind, a.p[k].data))) { ret = 0; break; }
out:
    free(a.p);
    return ret;
}

/* 0040d250: a call: a plain name (its routine or method looked up when it runs) or a reference's last property */
static int call_parse(parser *P, int r, symtab *locals, target *t, int paren)
{
    int kind, data;
    if (t->plain) {
        int v = P->proplist.n, off = 0;
        int x = sym_find(P->g, &P->props, 1, t->name, &v, &off);
        if (x < 0) { P->hr = x; return 0; }
        if (x == 1 && set_hr(P, VPUSH(P->proplist, off))) return 0;
        kind = 0; data = off;
    } else { kind = 1; data = t->ref; }
    if (P->depth >= MAXNEST) { perror_(P, 0); return 0; }
    P->depth++;
    int args = arguments(P, r, locals, paren);
    P->depth--;
    if (P->hr < 0) return 0;
    int n = P->g->calls.n;
    if (set_hr(P, VPUSH(P->g->calls, kind, data, args))) return 0;
    return n;
}

/* 0040d301: the value of an assignment (the line ends after it) */
static int assignment(parser *P, int r, symtab *locals, int set, int rf)
{
    if (P->hr < 0) return 0;
    ref *x = &P->g->refs.p[rf];
    if (!x->local && x->index < NBUILTIN) { perror_(P, 0x17); return 0; }
    if (P->L->tok == T_EQ && p_accept(P)) return 0;
    int e = expression(P, r, locals, NULL, 0, 0);
    if (P->hr < 0) return 0;
    if (P->L->tok != T_NL) { perror_(P, 10); return 0; }
    int n = P->g->assigns.n;
    if (set_hr(P, VPUSH(P->g->assigns, set, rf, e))) return 0;
    return n;
}

/* 0040d3bb: a statement starting with a name (after Set or Call when they were there) */
static void statement(parser *P, int r, symtab *locals, int set, int callkw)
{
    if (P->hr < 0) return;
    target t;
    target_parse(P, r, locals, &t);
    if (P->hr < 0) goto out;
    if (callkw ? expect_next(P, T_LPAREN, 7) : p_accept(P)) goto out;
    int kind, idx;
    if (P->L->tok == T_EQ) {
        int rf = target_ref(P, r, locals, &t);
        if (P->hr < 0) goto out;
        idx = assignment(P, r, locals, set, rf);
        if (P->hr < 0) goto out;
        kind = 2;
    } else {
        if (set) { perror_(P, 8); goto out; }
        idx = call_parse(P, r, locals, &t, callkw);
        if (P->hr < 0) goto out;
        kind = 3;
    }
    set_hr(P, VPUSH(P->g->stmts, kind, P->L->line, idx, 0));
out:
    free(t.name);
}

static int block(parser *P, int r, symtab *locals);
/* 0040d581: If ... Then / ElseIf ... Then / Else / End If */
static void if_statement(parser *P, int r, symtab *locals)
{
    if (P->hr < 0) return;
    if (P->depth >= MAXNEST) { perror_(P, 0); return; }
    prog *g = P->g;
    int si = g->stmts.n;
    if (set_hr(P, VPUSH(g->stmts, 1, P->L->line, 0, 0))) return;
    VEC(clause) cl = { 0 };
    int first = 1;
    P->depth++;
    do {
        int cond = 1;
        if (!first) {
            if (P->L->tok == T_EOF) { perror_(P, 0x14); goto out; }  /* (the text ended inside the If) */
            if (P->L->tok == T_ELSE) cond = 0;
        } else first = 0;
        if (p_accept(P)) goto out;
        int e = 0;
        if (cond) {
            e = expression(P, r, locals, NULL, 0, 0);
            if (P->hr < 0 || expect(P, T_THEN, 0x13)) goto out;
        }
        if (P->L->tok != T_NL) { perror_(P, (!cond && P->L->tok == T_IF) ? 0x16 : 3); goto out; }
        int b = block(P, r, locals);
        if (P->hr < 0) goto out;
        if (set_hr(P, cond ? VPUSH(cl, 2, e, b) : VPUSH(cl, 1, 0, b))) goto out;
    } while (P->L->tok != T_END);
    if (p_accept(P) || expect(P, T_IF, 0x15)) goto out;
    int ci = g->clauses.n;
    for (int k = 0; k < cl.n; k++) if (set_hr(P, VPUSH(g->clauses, cl.p[k].kind, cl.p[k].expr, cl.p[k].body))) goto out;
    if (set_hr(P, VPUSH(g->clauses, 0, 0, 0))) goto out;
    g->stmts.p[si].a = ci;
    g->stmts.p[si].b = g->stmts.n;
out:
    P->depth--;
    free(cl.p);
}

/* 0040d4a9: statements, one a line, up to one that cannot start a statement */
static int block(parser *P, int r, symtab *locals)
{
    if (P->hr < 0) return 0;
    if (P->L->tok == T_NL && p_accept(P)) return 0;
    int first = P->g->stmts.n;
    for (;;) {
        int t = P->L->tok;
        if (t == T_IF) if_statement(P, r, locals);
        else if (t == T_SET) { if (p_accept(P)) return 0; statement(P, r, locals, 1, 0); }
        else if (t == T_CALL) { if (p_accept(P)) return 0; statement(P, r, locals, 0, 1); }
        else if (t == T_IDENT || t == T_IDENTDOT) statement(P, r, locals, 0, 0);
        else {
            if (set_hr(P, VPUSH(P->g->stmts, 0, 0, 0, 0))) return 0;
            return first;
        }
        if (P->hr < 0) return 0;
        if (expect(P, T_NL, 3)) return 0;
    }
}

/* 0040ca8b: Dim name */
static void dim(parser *P)
{
    if (P->hr < 0 || expect_next(P, T_IDENT, 2)) return;
    int idx = P->g->globals.n, off = 0;
    int x = sym_find(P->g, &P->globals, 1, P->L->text, &idx, &off);
    if (x < 0) { P->hr = x; return; }
    if (x == 0) { perror_(P, 0xf); return; }
    if (set_hr(P, VPUSH(P->g->globals, off, -1))) return;
    p_accept(P);
}

/* 0040d780: Sub name ... End Sub */
static void sub(parser *P)
{
    if (P->hr < 0 || expect_next(P, T_IDENT, 2)) return;
    prog *g = P->g;
    int v, off;
    if (!sym_find(g, &P->globals, 0, P->L->text, &v, &off)) { perror_(P, 0xf); return; }
    int r = g->routines.n;
    int x = sym_find(g, &P->routines, 1, P->L->text, &r, &off);
    if (x < 0) { P->hr = x; return; }
    if (x == 0) { perror_(P, 0x10); return; }
    if (set_hr(P, VPUSH(g->routines, off, 0, 0))) return;
    if (expect_next(P, T_NL, 3)) return;
    symtab locals = { 0 };
    int b = block(P, r, &locals);
    g->routines.p[r].stmt = b;
    if (P->hr >= 0) {
        if (P->L->tok == T_END) { if (!p_accept(P)) expect(P, T_SUB, 5); }
        else perror_(P, P->L->tok == T_DIM ? 9 : 4);
    }
    free(locals.e.p);
}

/* 0040d8b8 */
static void script(parser *P)
{
    if (P->hr < 0) return;
    if (P->L->tok == T_EOF) { if (P->L->num) perror_(P, 0); return; }
    if (P->L->tok == T_NL && p_accept(P)) return;
    if (P->L->tok == T_EOF) return;
    int subs = 0;
    do {
        if (P->L->tok == T_DIM) { if (subs) perror_(P, 9); else dim(P); }
        else if (P->L->tok != T_SUB) perror_(P, 1);
        else { subs = 1; sub(P); }
    } while (P->hr >= 0 && P->L->tok != T_EOF && !expect(P, T_NL, 3) && P->L->tok != T_EOF);
}

/* ------------------------------------------------------------------ the engine */
struct avs_engine {
    prog g;
    IDispatch *named;
    IDispatch *disp;
};

static void prog_free(prog *g)
{
    free(g->routines.p); free(g->globals.p); free(g->pool.p); free(g->stmts.p); free(g->chain.p);
    free(g->refs.p); free(g->values.p); free(g->calls.p); free(g->exprs.p); free(g->clauses.p); free(g->assigns.p);
}

void avs_error_free(avs_error *e)
{
    if (!e) return;
    free(e->desc); free(e->source); free(e->text);
    memset(e, 0, sizeof(*e));
}

HRESULT avs_parse(const WCHAR *src, IDispatch *global, avs_engine **out, avs_error *err)
{
    *out = NULL;
    if (err) memset(err, 0, sizeof(*err));
    if (!src) return E_POINTER;
    avs_engine *e = calloc(1, sizeof(*e));
    if (!e) return E_OUTOFMEMORY;
    lexer L;
    memset(&L, 0, sizeof(L));
    L.cur = src; L.line = 1; L.col = 1; L.tok = 1;     /* 0040c0cb */
    lex_next(&L);
    parser P;
    memset(&P, 0, sizeof(P));
    P.L = &L; P.g = &e->g; P.err = err; P.named = global;
    static const char *const builtin[NBUILTIN] = { "true", "false", "nothing" };
    for (int k = 0; k < NBUILTIN && P.hr >= 0; k++) {
        int idx = e->g.globals.n, off = 0;
        int x = sym_find(&e->g, &P.globals, 1, builtin[k], &idx, &off);
        if (x < 0) P.hr = x;
        else set_hr(&P, VPUSH(e->g.globals, off, -1));
    }
    script(&P);
    free(P.routines.e.p); free(P.globals.e.p); free(P.props.e.p); free(P.proplist.p); free(P.ops.p);
    if (P.hr < 0) { prog_free(&e->g); free(e); return P.hr; }
    e->named = global;
    if (global) IDispatch_AddRef(global);
    *out = e;
    return S_OK;
}

/* ------------------------------------------------------------------ the executor (00409887 and 0040a73e on) */
/* the script's variables (True, False, Nothing, then its own), shared by the calls of its routines: each call from
 * outside runs on a stack of its own, as threads (the application's, the performance's) may call the script at
 * once, and the variables are only touched under the lock, never held while anything is called */
typedef struct {
    CRITICAL_SECTION cs;
    int init;                       /* (before the script has run a variable reads as empty) */
    VARIANT *v;
    int n;
} vars;
typedef struct {
    vars *vs;
    prog *g;
    IDispatch *named;
    VARIANT *stack;                 /* the routines' own variables, arguments and values */
    int n, cap;
} exec;

static HRESULT stack_push(exec *x, int k)          /* 0040a6c4 */
{
    if (!vgrow((void **)&x->stack, &x->cap, x->n + k, sizeof(VARIANT))) return E_OUTOFMEMORY;
    for (int i = 0; i < k; i++) VariantInit(&x->stack[x->n + i]);
    x->n += k;
    return S_OK;
}
static void stack_pop(exec *x, int to)             /* 0040a423 */
{
    while (x->n > to) VariantClear(&x->stack[--x->n]);
}
static HRESULT avs_clear(VARIANT *v);
static void vars_start(vars *vs)                    /* 0040a794: the variables, True -1, False 0, Nothing */
{
    if (vs->init) return;
    V_VT(&vs->v[0]) = VT_I4; V_I4(&vs->v[0]) = -1;
    V_VT(&vs->v[1]) = VT_I4; V_I4(&vs->v[1]) = 0;
    V_VT(&vs->v[2]) = VT_UNKNOWN; V_UNKNOWN(&vs->v[2]) = NULL;
    vs->init = 1;
}
static HRESULT ex_init(exec *x)
{
    if (x->vs->n < NBUILTIN) return E_UNEXPECTED;
    EnterCriticalSection(&x->vs->cs);
    vars_start(x->vs);
    LeaveCriticalSection(&x->vs->cs);
    return S_OK;
}
static HRESULT var_get(vars *vs, int k, VARIANT *out)          /* 0040a46c */
{
    EnterCriticalSection(&vs->cs);
    HRESULT hr = vs->init ? avs_copy(out, &vs->v[k]) : S_OK;
    LeaveCriticalSection(&vs->cs);
    return hr;
}
static HRESULT var_set(vars *vs, int k, const VARIANT *v)
{
    VARIANT nv, old;
    VariantInit(&nv);
    HRESULT hr = avs_copy(&nv, v);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&vs->cs);
    vars_start(vs);
    old = vs->v[k];
    vs->v[k] = nv;
    LeaveCriticalSection(&vs->cs);
    avs_clear(&old);                /* (outside the lock: letting go of an object may run anything) */
    return S_OK;
}

/* 0040a0e2: a runtime error: DISP_E_EXCEPTION with its description */
static BSTR bstr_cat(const WCHAR *a, const char *b, const WCHAR *c)
{
    WCHAR *wb = wfroma(b ? b : "");
    if (!wb) return NULL;
    int n = lstrlenW(a) + lstrlenW(wb) + lstrlenW(c);
    BSTR s = SysAllocStringLen(NULL, n);
    if (s) { lstrcpyW(s, a); lstrcatW(s, wb); lstrcatW(s, c); }
    free(wb);
    return s;
}
static HRESULT rt_error(EXCEPINFO *ei, int opfail, const WCHAR *a, const char *b, const WCHAR *c)
{
    if (!ei) return DISP_E_EXCEPTION;
    memset(ei, 0, sizeof(*ei));
    ei->bstrSource = SysAllocString(opfail ? L"Microsoft AudioVBScript Operation Failed" : L"Microsoft AudioVBScript Runtime Error");
    ei->bstrDescription = bstr_cat(a ? a : L"", b, c ? c : L"");
    ei->scode = opfail ? (SCODE)0x88781225 : E_AVS_RUNTIME;
    return DISP_E_EXCEPTION;
}
static const char *name_of(exec *x, int off) { return pool_str(x->g, off); }

/* 0040a1e0: a dispatch failure as a runtime error naming what was called or set (mode 1/2 assignment, 3 call) */
static HRESULT map_err(exec *x, int mode, HRESULT hr, int off, EXCEPINFO *ei)
{
    if (hr >= 0 || (hr & 0x1fff0000) != 0x20000 || hr == DISP_E_EXCEPTION) return hr;
    const char *n = name_of(x, off);
    if (hr == DISP_E_BADPARAMCOUNT) return rt_error(ei, 0, L"Wrong number of parameters in call to '", n, L"'");
    if (hr == DISP_E_MEMBERNOTFOUND) {
        if (mode == 3) return rt_error(ei, 0, L"Type mismatch: '", n, L"' is not a routine or method");
        if (mode == 1 || mode == 2) return rt_error(ei, 0, L"Type mismatch: '", n, L"' is not a variable or is a read-only property");
        return hr;
    }
    if (hr == DISP_E_TYPEMISMATCH) {
        if (mode == 3) return rt_error(ei, 0, L"Type mismatch: a parameter in call to '", n, L"' is not of the expected type");
        if (mode == 1 || mode == 2) return rt_error(ei, 0, L"Type mismatch: value assigned to '", n, L"' is not of the expected type");
        return hr;
    }
    if (hr == DISP_E_PARAMNOTOPTIONAL && mode == 3)
        return rt_error(ei, 0, L"A required parameter was omitted in call to '", n, L"'");
    return hr;
}
static int is_obj(const VARIANT *v) { return V_VT(v) == VT_DISPATCH || V_VT(v) == VT_UNKNOWN; }
/* 0040a483: Set with an object, plain assignment with anything else */
static HRESULT check_set(exec *x, const VARIANT *v, int set, int off, EXCEPINFO *ei)
{
    if (set == is_obj(v)) return S_OK;
    if (!set) return rt_error(ei, 0, L"Type mismatch: '", name_of(x, off), L"'.  Likely cause is missing Set statement.");
    return rt_error(ei, 0, L"Object required: '", name_of(x, off), L"'");
}
/* 0040deec (to VT_DISPATCH / VT_UNKNOWN / VT_I4, dmscript's own conversions) */
/* 0040ddac / 0040de35: dmscript's own VariantClear and VariantCopy (no arrays or references) */
static HRESULT avs_clear(VARIANT *v)
{
    if (V_VT(v) & 0x6000) return E_AVS_UNSUPPORTED_VARTYPE;
    if (V_VT(v) == VT_BSTR) SysFreeString(V_BSTR(v));
    else if ((V_VT(v) == VT_DISPATCH || V_VT(v) == VT_UNKNOWN) && V_UNKNOWN(v)) IUnknown_Release(V_UNKNOWN(v));
    V_VT(v) = VT_EMPTY;
    return S_OK;
}
HRESULT avs_copy(VARIANT *d, const VARIANT *s)
{
    if (d == s) return E_INVALIDARG;
    if (V_VT(s) & 0x6000) return E_AVS_UNSUPPORTED_VARTYPE;
    HRESULT hr = avs_clear(d);
    if (FAILED(hr)) return hr;
    if (V_VT(s) == VT_BSTR) {
        V_VT(d) = VT_BSTR;
        V_BSTR(d) = V_BSTR(s) ? SysAllocString(V_BSTR(s)) : NULL;
        return S_OK;
    }
    if ((V_VT(s) == VT_DISPATCH || V_VT(s) == VT_UNKNOWN) && V_UNKNOWN(s)) IUnknown_AddRef(V_UNKNOWN(s));
    *d = *s;
    return S_OK;
}
HRESULT avs_convert(VARIANT *d, VARIANT *s, VARTYPE vt)
{
    VARTYPE from = V_VT(s);
    if (from == vt) { if (d == s) return S_OK; return avs_copy(d, s); }
    if ((vt & 0x6000) || (from & 0x6000)) return E_AVS_UNSUPPORTED_VARTYPE;
    if (vt == VT_I4) {
        LONG v = 0;
        if (from == VT_EMPTY) v = 0;
        else if (from == VT_I2) v = V_I2(s);
        else if (from == VT_BSTR || from == VT_DISPATCH || from == VT_UNKNOWN) return DISP_E_TYPEMISMATCH;
        else return E_AVS_UNSUPPORTED_VARTYPE;
        VariantClear(d);
        V_VT(d) = VT_I4; V_I4(d) = v;
        return S_OK;
    }
    if (vt == VT_DISPATCH || vt == VT_UNKNOWN) {
        if (from == VT_EMPTY || from == VT_I2 || from == VT_I4 || from == VT_BSTR) return DISP_E_TYPEMISMATCH;
        if (from != VT_DISPATCH && from != VT_UNKNOWN) return E_AVS_UNSUPPORTED_VARTYPE;
        IUnknown *u = V_UNKNOWN(s), *o = NULL;
        if (!u) return E_INVALIDARG;
        HRESULT hr = IUnknown_QueryInterface(u, vt == VT_UNKNOWN ? &IID_IUnknown : &IID_IDispatch, (void **)&o);
        if (FAILED(hr)) return hr;
        if (d == s) IUnknown_Release(u); else VariantClear(d);
        V_VT(d) = vt; V_UNKNOWN(d) = o;
        return S_OK;
    }
    return E_AVS_UNSUPPORTED_VARTYPE;
}
static HRESULT to_disp(exec *x, VARIANT *v, int off, EXCEPINFO *ei)    /* 0040a8d9 */
{
    if (FAILED(avs_convert(v, v, VT_DISPATCH))) return rt_error(ei, 0, L"Object required: '", name_of(x, off), L"'");
    return S_OK;
}

/* 0040b68b / 0040b745 / 0040b791 */
/* (dmscript's own IID first, telling the object its strings are not BSTRs; IID_NULL when it does not know it) */
static HRESULT d_invoke(IDispatch *d, DISPID id, WORD flags, DISPPARAMS *p, VARIANT *res, EXCEPINFO *ei)
{
    if (!d) return E_POINTER;
    HRESULT hr = IDispatch_Invoke(d, id, &AVS_IID_PRIVATE, 0x409, flags, p, res, ei, NULL);
    if (hr == DISP_E_UNKNOWNINTERFACE) hr = IDispatch_Invoke(d, id, &IID_NULL, 0x409, flags, p, res, ei, NULL);
    return hr;
}
static HRESULT d_put(IDispatch *d, DISPID id, int set, VARIANT *v, EXCEPINFO *ei)
{
    DISPID named = DISPID_PROPERTYPUT;
    DISPPARAMS p = { v, &named, 1, 1 };
    return d_invoke(d, id, set ? DISPATCH_PROPERTYPUTREF : DISPATCH_PROPERTYPUT, &p, NULL, ei);
}
static HRESULT d_get(IDispatch *d, DISPID id, VARIANT *out, EXCEPINFO *ei)
{
    DISPPARAMS p = { 0 };
    return d_invoke(d, id, DISPATCH_PROPERTYGET | DISPATCH_METHOD, &p, out, ei);
}
static DISPID d_id(IDispatch *d, const char *name) { return dispid_of(d, name); }

/* 0040a95a: a reference read (mode 0), assigned (1, Set 2) or followed to the object whose last property is called
 * (3, the object in *v) */
static HRESULT reference_do(exec *x, int mode, int rf, VARIANT *v, EXCEPINFO *ei, int frame)
{
    prog *g = x->g;
    ref r = g->refs.p[rf];
    const int *c = &g->chain.p[r.chain];
    int props = c[1] != -1;
    VARIANT tmp;
    VariantInit(&tmp);
    HRESULT hr;
    if (!props && mode == 3) return E_UNEXPECTED;
    int pos = r.chain;
    if (!r.local && g->globals.p[r.index].dispid != -1) {
        global gl = g->globals.p[r.index];
        if (!props && (mode == 1 || mode == 2)) {
            hr = d_put(x->named, gl.dispid, mode == 2, v, ei);
            return map_err(x, mode, hr, gl.name, ei);
        }
        hr = d_get(x->named, gl.dispid, &tmp, ei);
        hr = map_err(x, mode, hr, gl.name, ei);
    } else {
        if (!props && (mode == 1 || mode == 2)) {
            hr = check_set(x, v, mode == 2, c[0], ei);
            if (FAILED(hr)) return hr;
            return r.local ? avs_copy(&x->stack[frame + r.index], v) : var_set(x->vs, r.index, v);
        }
        hr = r.local ? avs_copy(&tmp, &x->stack[frame + r.index]) : var_get(x->vs, r.index, &tmp);
    }
    if (FAILED(hr)) goto out;
    if (g->chain.p[pos + 1] == -1) goto done;
    hr = check_set(x, &tmp, 1, g->chain.p[pos], ei);
    if (FAILED(hr)) goto out;
    for (int p = pos + 1;; p++) {
        int last = g->chain.p[p + 1] == -1;
        if (last && mode == 3) goto done;
        hr = to_disp(x, &tmp, g->chain.p[pos], ei);
        if (FAILED(hr)) goto out;
        IDispatch *d = V_DISPATCH(&tmp);
        int noff = g->chain.p[p];
        DISPID id = d_id(d, name_of(x, noff));
        if (id == -1) { hr = rt_error(ei, 0, L"The property '", name_of(x, noff), L"' does not exist"); goto out; }
        if (last && (mode == 1 || mode == 2)) {
            hr = d_put(d, id, mode == 2, v, ei);
            hr = map_err(x, mode, hr, noff, ei);
            goto out;
        }
        VARIANT nv;
        VariantInit(&nv);
        hr = d_get(d, id, &nv, ei);
        VariantClear(&tmp);
        tmp = nv;
        hr = map_err(x, mode, hr, noff, ei);
        if (FAILED(hr)) goto out;
        if (last) goto done;
        hr = check_set(x, &tmp, 1, noff, ei);
        if (FAILED(hr)) goto out;
        pos++;
    }
done:
    VariantClear(v);
    *v = tmp;
    return hr;
out:
    VariantClear(&tmp);
    return hr;
}

static HRESULT run(exec *x, int s, EXCEPINFO *ei, int frame);
static HRESULT eval(exec *x, VARIANT *out, int e, EXCEPINFO *ei, int frame);

/* how deep routines (of any script, the calls between scripts included) run on this thread: a script calling
 * itself for ever stops with a runtime error before the thread's stack runs out */
#define MAXCALLS 1000
static DWORD s_depth_tls = TLS_OUT_OF_INDEXES;
static INIT_ONCE s_depth_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK depth_start(INIT_ONCE *o, void *p, void **c) { s_depth_tls = TlsAlloc(); return TRUE; }

/* 0040adde: a routine of the script, its variables on the stack while it runs */
static HRESULT call_routine(exec *x, int r, EXCEPINFO *ei)
{
    HRESULT hr = ex_init(x);
    if (FAILED(hr)) return hr;
    InitOnceExecuteOnce(&s_depth_once, depth_start, NULL, NULL);
    INT_PTR depth = s_depth_tls != TLS_OUT_OF_INDEXES ? (INT_PTR)TlsGetValue(s_depth_tls) : 0;
    if (depth >= MAXCALLS) return rt_error(ei, 0, L"Out of stack space", NULL, NULL);
    routine rt = x->g->routines.p[r];
    int base = x->n;
    hr = stack_push(x, rt.nlocals);
    if (FAILED(hr)) return hr;
    if (s_depth_tls != TLS_OUT_OF_INDEXES) TlsSetValue(s_depth_tls, (void *)(depth + 1));
    hr = run(x, rt.stmt, ei, base);
    if (s_depth_tls != TLS_OUT_OF_INDEXES) TlsSetValue(s_depth_tls, (void *)depth);
    stack_pop(x, base);
    return hr;
}

/* 0040ae3b: a call (want: its value goes on the stack) */
static HRESULT call_do(exec *x, int ci, int want, EXCEPINFO *ei, int frame)
{
    prog *g = x->g;
    if (ci < 0 || ci >= g->calls.n || g->calls.p[ci].args < 0 || g->calls.p[ci].args >= g->exprs.n)
        return E_FAIL;                              /* (a call whose arguments failed to parse: dmscript crashes) */
    call c = g->calls.p[ci];
    VARIANT obj, res;
    VariantInit(&obj);
    VariantInit(&res);
    IDispatch *d;
    int noff;
    HRESULT hr;
    if (c.kind == 0) {
        noff = c.data;
        const char *n = name_of(x, noff);
        for (int k = 0; k < g->routines.n; k++)
            if (!_stricmp(n, name_of(x, g->routines.p[k].name))) { hr = call_routine(x, k, ei); goto out; }
        d = x->named;
    } else {
        int t = g->refs.p[c.data].chain;
        while (g->chain.p[t] != -1) t++;
        hr = reference_do(x, 3, c.data, &obj, ei, frame);
        if (FAILED(hr)) goto out;
        hr = to_disp(x, &obj, g->chain.p[t - 2], ei);
        if (FAILED(hr)) goto out;
        d = V_DISPATCH(&obj);
        noff = g->chain.p[t - 1];
    }
    {
        DISPID id = d_id(d, name_of(x, noff));
        if (id == -1) { hr = rt_error(ei, 0, L"The routine '", name_of(x, noff), L"' does not exist"); goto out; }
        int nargs = 0;
        for (int i = c.args; g->exprs.p[i].kind != 0; ) {
            nargs++;
            while (g->exprs.p[i].kind != 0) i++;
            i++;
        }
        int base = x->n;
        hr = stack_push(x, nargs > want ? nargs : want);
        if (FAILED(hr)) goto out;
        int i = c.args;
        for (int slot = base + nargs - 1; slot >= base; slot--) {
            if (g->exprs.p[i].kind == 4) {
                V_VT(&x->stack[slot]) = VT_ERROR;
                V_ERROR(&x->stack[slot]) = DISP_E_PARAMNOTFOUND;
            } else {
                VARIANT a;
                VariantInit(&a);
                hr = eval(x, &a, i, ei, frame);
                if (FAILED(hr)) { stack_pop(x, base); goto out; }
                VariantClear(&x->stack[slot]);
                x->stack[slot] = a;
            }
            while (g->exprs.p[i].kind != 0) i++;
            i++;
        }
        DISPPARAMS p = { nargs ? &x->stack[base] : NULL, NULL, (UINT)nargs, 0 };
        hr = d_invoke(d, id, DISPATCH_METHOD, &p, want ? &res : NULL, ei);
        hr = map_err(x, 3, hr, noff, ei);
        if (SUCCEEDED(hr) && want) { VariantClear(&x->stack[base]); x->stack[base] = res; VariantInit(&res); }
        stack_pop(x, base + want);
        if (SUCCEEDED(hr)) hr = S_OK;
    }
out:
    VariantClear(&res);
    VariantClear(&obj);
    return hr;
}

/* 0040ac93: a value: a number, a string, or what a reference reads */
static HRESULT value_do(exec *x, int vi, VARIANT *out, EXCEPINFO *ei, int frame)
{
    item v = x->g->values.p[vi];
    if (v.kind == 0) { V_VT(out) = VT_I4; V_I4(out) = v.data; return S_OK; }
    if (v.kind == 1) {
        WCHAR *w = wfroma(name_of(x, v.data));
        V_VT(out) = VT_BSTR;
        V_BSTR(out) = w ? SysAllocString(w) : NULL;
        free(w);
        return V_BSTR(out) ? S_OK : E_OUTOFMEMORY;
    }
    if (v.kind == 2) return reference_do(x, 0, v.data, out, ei, frame);
    return S_OK;
}

static int ipow(int b, int e)                      /* 0040a3bf */
{
    unsigned u = e < 0 ? -(unsigned)e : (unsigned)e, ub = (unsigned)b, ur = 1;    /* (wrapping, as dmscript's) */
    for (;;) {
        if (u & 1) ur *= ub;
        u >>= 1;
        if (!u) break;
        ub *= ub;
    }
    int r = (int)ur;
    if (e < 0) r = r ? 1 / r : 0;       /* (dmscript divides by zero there) */
    return r;
}
/* 0040a4e9: left op right into left */
static HRESULT binop(int op, VARIANT *right, VARIANT *left, EXCEPINFO *ei)
{
    if (is_obj(right)) {
        if (op != T_IS || !is_obj(left)) return DISP_E_TYPEMISMATCH;
        HRESULT hr = avs_convert(right, right, VT_UNKNOWN);
        if (FAILED(hr)) return hr;
        hr = avs_convert(left, left, VT_UNKNOWN);
        if (FAILED(hr)) return hr;
        int same = V_UNKNOWN(right) == V_UNKNOWN(left);
        VariantClear(left);
        V_VT(left) = VT_I4; V_I4(left) = same ? -1 : 0;
        return S_OK;
    }
    if (V_VT(right) != VT_I4 || V_VT(left) != VT_I4) return DISP_E_TYPEMISMATCH;
    int a = V_I4(left), b = V_I4(right), r;
    switch (op) {
    case T_MINUS: r = (int)((unsigned)a - (unsigned)b); break;
    case T_POW: r = ipow(a, b); break;
    case T_MUL: r = (int)((unsigned)a * (unsigned)b); break;
    case T_IDIV:
        if (!b) return rt_error(ei, 0, L"Division by zero", NULL, NULL);
        r = (a == INT_MIN && b == -1) ? INT_MIN : a / b;
        break;
    case T_MOD:
        if (!b) return rt_error(ei, 0, L"Mod by zero", NULL, NULL);
        r = (b == -1) ? 0 : a % b;
        break;
    case T_PLUS: r = (int)((unsigned)a + (unsigned)b); break;
    case T_LT: r = a < b ? -1 : 0; break;
    case T_LE: r = a <= b ? -1 : 0; break;
    case T_GT: r = a > b ? -1 : 0; break;
    case T_GE: r = a >= b ? -1 : 0; break;
    case T_EQ: r = a == b ? -1 : 0; break;
    case T_NE: r = a != b ? -1 : 0; break;
    case T_AND: r = a & b; break;
    case T_OR: r = a | b; break;
    default: return E_UNEXPECTED;
    }
    V_I4(left) = r;
    return S_OK;
}

/* 0040b1f3: an expression's value */
static HRESULT eval(exec *x, VARIANT *out, int e, EXCEPINFO *ei, int frame)
{
    prog *g = x->g;
    int base = x->n;
    HRESULT hr = S_OK;
    for (int i = e; g->exprs.p[i].kind != 0; i++) {
        item it = g->exprs.p[i];
        if (it.kind == 1) {
            int u = it.data == T_NOT || it.data == T_NEG;
            if (x->n < base + (u ? 1 : 2)) return E_FAIL;
            VARIANT *top = &x->stack[x->n - 1];
            if (u) {
                if (V_VT(top) != VT_I4) hr = DISP_E_TYPEMISMATCH;       /* 0040a30e */
                else V_I4(top) = it.data == T_NOT ? ~V_I4(top) : (int)(0u - (unsigned)V_I4(top));
            } else {
                hr = binop(it.data, top, &x->stack[x->n - 2], ei);
                stack_pop(x, x->n - 1);
            }
        } else if (it.kind == 2) {
            hr = stack_push(x, 1);
            if (SUCCEEDED(hr)) {
                VARIANT *top = &x->stack[x->n - 1];
                hr = value_do(x, it.data, top, ei, frame);
                if (V_VT(top) == VT_EMPTY) { V_VT(top) = VT_I4; V_I4(top) = 0; }
            }
        } else if (it.kind == 3) {
            if (FAILED(hr)) break;
            hr = call_do(x, it.data, 1, ei, frame);
        }
        if (FAILED(hr)) { stack_pop(x, base); return hr; }
    }
    if (x->n != base + 1) return E_FAIL;
    VariantClear(out);
    *out = x->stack[base];
    VariantInit(&x->stack[base]);
    stack_pop(x, base);
    return hr;
}

/* 0040a7f2: the first clause whose condition is exactly True (-1), or Else */
static HRESULT if_do(exec *x, int ci, EXCEPINFO *ei, int frame)
{
    for (;; ci++) {
        clause c = x->g->clauses.p[ci];
        if (c.kind == 0) return S_OK;
        if (c.kind == 2) {
            VARIANT v;
            VariantInit(&v);
            HRESULT hr = eval(x, &v, c.expr, ei, frame);
            if (FAILED(hr)) {                       /* (the error is not the If's: its strings let go of) */
                VariantClear(&v);
                if (hr == DISP_E_EXCEPTION && ei) {
                    SysFreeString(ei->bstrSource); SysFreeString(ei->bstrDescription); SysFreeString(ei->bstrHelpFile);
                    memset(ei, 0, sizeof(*ei));
                }
            }
            if (V_VT(&v) != VT_I4) {
                const WCHAR *m = V_VT(&v) == VT_BSTR ?
                    L"Type mismatch: the condition of an if statement evaluated as a string where a numeric True/False value was expected" :
                    is_obj(&v) ?
                    L"Type mismatch: the condition of an if statement evaluated as an object where a numeric True/False value was expected" :
                    L"Type mismatch: the condition of an if statement did not evaluate to a numeric True/False value";
                VariantClear(&v);
                return rt_error(ei, 0, m, NULL, NULL);
            }
            int t = V_I4(&v) == -1;
            VariantClear(&v);
            if (!t) continue;
        }
        return run(x, c.body, ei, frame);
    }
}

/* 0040e4a5: the line a runtime error happened on, in its description: "file»line»description" (the innermost
 * routine's line is kept) */
void avs_stamp(EXCEPINFO *ei, const WCHAR *deffile, const ULONG *line)
{
    if (!ei || !ei->bstrDescription) return;
    const WCHAR *d = ei->bstrDescription, *file = L"", *ln = L"", *rest = d;
    const WCHAR *a = wcschr(d, 0xbb), *b = a ? wcschr(a + 1, 0xbb) : NULL;
    int nfile = 0, nln = 0;
    if (a && b) { file = d; nfile = (int)(a - d); ln = a + 1; nln = (int)(b - a - 1); rest = b + 1; }
    if (!nfile && deffile) { file = deffile; nfile = lstrlenW(deffile); }
    WCHAR num[16];
    if (!nln && line) { _ultow(*line, num, 10); ln = num; nln = lstrlenW(num); }
    BSTR s = SysAllocStringLen(NULL, nfile + nln + 2 + lstrlenW(rest));
    if (!s) return;
    WCHAR *o = s;
    memcpy(o, file, nfile * sizeof(WCHAR)); o += nfile;
    *o++ = 0xbb;
    memcpy(o, ln, nln * sizeof(WCHAR)); o += nln;
    *o++ = 0xbb;
    lstrcpyW(o, rest);
    SysFreeString(ei->bstrDescription);
    ei->bstrDescription = s;
}

/* 0040ad2f: statements to the end of their block */
static HRESULT run(exec *x, int s, EXCEPINFO *ei, int frame)
{
    HRESULT hr = S_OK;
    stmt st;
    for (;; s++) {
        st = x->g->stmts.p[s];
        if (st.kind == 0) return hr;
        if (st.kind == 1) { hr = if_do(x, st.a, ei, frame); s = st.b - 1; }
        else if (st.kind == 2) {                    /* 0040ac1b */
            assign a = x->g->assigns.p[st.a];
            VARIANT v;
            VariantInit(&v);
            hr = eval(x, &v, a.expr, ei, frame);
            if (SUCCEEDED(hr)) {
                hr = reference_do(x, a.set ? 2 : 1, a.ref, &v, ei, frame);
                if (SUCCEEDED(hr)) hr = S_OK;
            }
            VariantClear(&v);
        } else if (st.kind == 3) hr = call_do(x, st.a, 0, ei, frame);
        if (FAILED(hr)) break;
    }
    if (hr == DISP_E_EXCEPTION) { ULONG l = (ULONG)(st.line - 1); avs_stamp(ei, NULL, &l); }
    return hr;
}

/* ------------------------------------------------------------------ the script's IDispatch (00409887) */
typedef struct {
    IDispatch IDispatch_iface;
    ITypeInfo ITypeInfo_iface;      /* (+0x04: GetTypeInfo hands out the object itself) */
    LONG ref;
    avs_engine *e;
    vars vs;
} sdisp;
static sdisp *sd(IDispatch *i) { return CONTAINING_RECORD(i, sdisp, IDispatch_iface); }
static sdisp *st_(ITypeInfo *i) { return CONTAINING_RECORD(i, sdisp, ITypeInfo_iface); }

static HRESULT WINAPI sd_qi(IDispatch *i, REFIID r, void **o)          /* 0040dae1 */
{
    if (!o) return E_POINTER;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDispatch)) { *o = i; IDispatch_AddRef(i); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI sd_addref(IDispatch *i) { return InterlockedIncrement(&sd(i)->ref); }
static ULONG WINAPI sd_release(IDispatch *i)
{
    sdisp *s = sd(i);
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        for (int k = 0; k < s->vs.n; k++) VariantClear(&s->vs.v[k]);
        free(s->vs.v);
        DeleteCriticalSection(&s->vs.cs);
        free(s);
    }
    return r;
}
static HRESULT WINAPI sd_typecount(IDispatch *i, UINT *n) { if (!n) return E_POINTER; *n = 1; return S_OK; }
static HRESULT WINAPI sd_typeinfo(IDispatch *i, UINT n, LCID l, ITypeInfo **o)     /* 00409836 */
{
    if (!o) return E_POINTER;
    if (n) return DISP_E_BADINDEX;
    *o = &sd(i)->ITypeInfo_iface;
    IDispatch_AddRef(i);
    return S_OK;
}
/* the wide name against a script's name: ASCII letters in any case (004097c7) */
static int name_eq(const WCHAR *w, const char *s)
{
    while (*w < 0x80) {
        int a = tolower((char)*w), b = tolower(*s);
        if ((char)a != (char)b) return 0;
        if (!a) return 1;
        w++; s++;
    }
    return 0;
}
static HRESULT WINAPI sd_ids(IDispatch *i, REFIID r, LPOLESTR *names, UINT n, LCID l, DISPID *ids)  /* 00409910 */
{
    sdisp *s = sd(i);
    prog *g = &s->e->g;
    if (!names || !ids) return E_POINTER;
    if (!IsEqualIID(r, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if (!n) return S_OK;
    for (UINT k = 0; k < n; k++) ids[k] = -1;
    for (int k = 0; k < g->routines.n; k++)
        if (name_eq(names[0], pool_str(g, g->routines.p[k].name))) { ids[0] = k + 1; break; }
    if (ids[0] == -1)
        for (int k = NBUILTIN; k < g->globals.n; k++)
            if (g->globals.p[k].dispid == -1 && name_eq(names[0], pool_str(g, g->globals.p[k].name))) { ids[0] = k + VARBASE; break; }
    if (ids[0] == -1 || n > 1) return DISP_E_UNKNOWNNAME;
    return S_OK;
}
static HRESULT WINAPI sd_invoke(IDispatch *i, DISPID id, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *res,
                                EXCEPINFO *ei, UINT *argerr)               /* 00409a2b */
{
    sdisp *s = sd(i);
    prog *g = &s->e->g;
    /* (dmscript passes its own IID when its strings are not BSTRs; ours always are) */
    if (!p) return E_POINTER;
    if (!IsEqualIID(r, &AVS_IID_PRIVATE) && !IsEqualIID(r, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if (argerr) *argerr = 0;
    if (id < VARBASE) {
        if ((f & DISPATCH_METHOD) && id - 1 >= 0 && id - 1 < g->routines.n) {
            if (p->cArgs) return DISP_E_BADPARAMCOUNT;
            if (p->cNamedArgs) return DISP_E_NONAMEDARGS;
            if (res) return E_UNEXPECTED;
            exec x = { &s->vs, g, s->e->named, NULL, 0, 0 };     /* (a stack of this call's own) */
            HRESULT hr = call_routine(&x, id - 1, ei);
            stack_pop(&x, 0);
            free(x.stack);
            return hr;
        }
        return DISP_E_MEMBERNOTFOUND;
    }
    int k = id - VARBASE;
    if (k >= g->globals.n) return DISP_E_MEMBERNOTFOUND;
    if (f & DISPATCH_PROPERTYGET) {
        if (p->cArgs) return DISP_E_BADPARAMCOUNT;
        if (p->cNamedArgs) return DISP_E_NONAMEDARGS;
        if (res) {
            VariantInit(res);
            var_get(&s->vs, k, res);
        }
        return S_OK;
    }
    if (!(f & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))) return DISP_E_MEMBERNOTFOUND;
    if (p->cArgs != 1 || p->cNamedArgs != 1) return DISP_E_BADPARAMCOUNT;
    if (p->rgdispidNamedArgs[0] != DISPID_PROPERTYPUT) return DISP_E_PARAMNOTFOUND;
    if (res) return E_INVALIDARG;
    exec x = { &s->vs, g, s->e->named, NULL, 0, 0 };
    HRESULT hr = ex_init(&x);                       /* 0040a913 */
    if (FAILED(hr)) return hr;
    hr = check_set(&x, p->rgvarg, (f & DISPATCH_PROPERTYPUTREF) != 0, g->globals.p[k].name, ei);
    if (FAILED(hr)) return hr;
    return var_set(&s->vs, k, p->rgvarg);
}
static const IDispatchVtbl sd_vtbl = { sd_qi, sd_addref, sd_release, sd_typecount, sd_typeinfo, sd_ids, sd_invoke };

/* the type information (00409bdc on): cFuncs routines, cVars script variables; names by MEMBERID */
static HRESULT WINAPI ti_qi(ITypeInfo *i, REFIID r, void **o) { return sd_qi(&st_(i)->IDispatch_iface, r, o); }
static ULONG WINAPI ti_addref(ITypeInfo *i) { return sd_addref(&st_(i)->IDispatch_iface); }
static ULONG WINAPI ti_release(ITypeInfo *i) { return sd_release(&st_(i)->IDispatch_iface); }
static int script_vars(prog *g)
{
    int n = 0;
    for (int k = NBUILTIN; k < g->globals.n; k++) if (g->globals.p[k].dispid == -1) n++;
    return n;
}
static HRESULT WINAPI ti_attr(ITypeInfo *i, TYPEATTR **o)
{
    if (!o) return E_POINTER;
    TYPEATTR *a = calloc(1, sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->cFuncs = (WORD)st_(i)->e->g.routines.n;
    a->cVars = (WORD)script_vars(&st_(i)->e->g);
    *o = a;
    return S_OK;
}
static HRESULT WINAPI ti_comp(ITypeInfo *i, ITypeComp **o) { return E_NOTIMPL; }
static HRESULT WINAPI ti_func(ITypeInfo *i, UINT n, FUNCDESC **o)
{
    if (!o) return E_POINTER;
    if (n >= (UINT)st_(i)->e->g.routines.n) return E_INVALIDARG;
    FUNCDESC *f = calloc(1, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->memid = n + 1; f->funckind = FUNC_DISPATCH; f->invkind = INVOKE_FUNC;
    *o = f;
    return S_OK;
}
static HRESULT WINAPI ti_var(ITypeInfo *i, UINT n, VARDESC **o)
{
    prog *g = &st_(i)->e->g;
    if (!o) return E_POINTER;
    UINT c = 0;
    for (int k = NBUILTIN; k < g->globals.n; k++) {
        if (g->globals.p[k].dispid != -1) continue;
        if (c++ != n) continue;
        VARDESC *v = calloc(1, sizeof(*v));
        if (!v) return E_OUTOFMEMORY;
        v->memid = k + VARBASE; v->varkind = VAR_DISPATCH;
        *o = v;
        return S_OK;
    }
    return E_INVALIDARG;
}
static HRESULT WINAPI ti_names(ITypeInfo *i, MEMBERID id, BSTR *names, UINT max, UINT *got)    /* 00409d61 */
{
    prog *g = &st_(i)->e->g;
    if (id < 1) return E_INVALIDARG;
    if (!names || !got) return E_POINTER;
    if (max != 1) return E_INVALIDARG;
    int off;
    if (id < VARBASE) { if (id - 1 >= g->routines.n) return E_INVALIDARG; off = g->routines.p[id - 1].name; }
    else { if (id - VARBASE >= g->globals.n) return E_INVALIDARG; off = g->globals.p[id - VARBASE].name; }
    WCHAR *w = wfroma(pool_str(g, off));
    *names = w ? SysAllocString(w) : NULL;
    free(w);
    if (!*names) return E_OUTOFMEMORY;
    *got = 1;
    return S_OK;
}
static HRESULT WINAPI ti_reftype(ITypeInfo *i, UINT n, HREFTYPE *o) { return E_NOTIMPL; }
static HRESULT WINAPI ti_impl(ITypeInfo *i, UINT n, INT *o) { return E_NOTIMPL; }
static HRESULT WINAPI ti_ids(ITypeInfo *i, LPOLESTR *n, UINT c, MEMBERID *o) { return E_NOTIMPL; }
static HRESULT WINAPI ti_invoke(ITypeInfo *i, PVOID a, MEMBERID b, WORD c, DISPPARAMS *d, VARIANT *e, EXCEPINFO *f, UINT *g) { return E_NOTIMPL; }
static HRESULT WINAPI ti_doc(ITypeInfo *i, MEMBERID a, BSTR *b, BSTR *c, DWORD *d, BSTR *e) { return E_NOTIMPL; }
static HRESULT WINAPI ti_dll(ITypeInfo *i, MEMBERID a, INVOKEKIND b, BSTR *c, BSTR *d, WORD *e) { return E_NOTIMPL; }
static HRESULT WINAPI ti_refinfo(ITypeInfo *i, HREFTYPE a, ITypeInfo **b) { return E_NOTIMPL; }
static HRESULT WINAPI ti_addr(ITypeInfo *i, MEMBERID a, INVOKEKIND b, PVOID *c) { return E_NOTIMPL; }
static HRESULT WINAPI ti_create(ITypeInfo *i, IUnknown *a, REFIID b, PVOID *c) { return E_NOTIMPL; }
static HRESULT WINAPI ti_mops(ITypeInfo *i, MEMBERID a, BSTR *b) { return E_NOTIMPL; }
static HRESULT WINAPI ti_lib(ITypeInfo *i, ITypeLib **a, UINT *b) { return E_NOTIMPL; }
static void WINAPI ti_rattr(ITypeInfo *i, TYPEATTR *a) { free(a); }
static void WINAPI ti_rfunc(ITypeInfo *i, FUNCDESC *a) { free(a); }
static void WINAPI ti_rvar(ITypeInfo *i, VARDESC *a) { free(a); }
static const ITypeInfoVtbl ti_vtbl = { ti_qi, ti_addref, ti_release, ti_attr, ti_comp, ti_func, ti_var, ti_names,
    ti_reftype, ti_impl, ti_ids, ti_invoke, ti_doc, ti_dll, ti_refinfo, ti_addr, ti_create, ti_mops, ti_lib, ti_rattr,
    ti_rfunc, ti_rvar };

HRESULT avs_dispatch(avs_engine *e, IDispatch **out)     /* 0040b982: made once, kept by the engine */
{
    if (!out) return E_POINTER;
    if (!e->disp) {
        sdisp *s = calloc(1, sizeof(*s));
        if (!s) return E_OUTOFMEMORY;
        s->vs.v = calloc(e->g.globals.n ? e->g.globals.n : 1, sizeof(VARIANT));     /* (all VT_EMPTY) */
        if (!s->vs.v) { free(s); return E_OUTOFMEMORY; }
        s->vs.n = e->g.globals.n;
        InitializeCriticalSection(&s->vs.cs);
        s->IDispatch_iface.lpVtbl = (IDispatchVtbl *)&sd_vtbl;
        s->ITypeInfo_iface.lpVtbl = (ITypeInfoVtbl *)&ti_vtbl;
        s->ref = 1;
        s->e = e;
        e->disp = &s->IDispatch_iface;
    }
    *out = e->disp;
    IDispatch_AddRef(*out);
    return S_OK;
}

void avs_free(avs_engine *e)
{
    if (!e) return;
    if (e->disp) {
        sdisp *s = sd(e->disp);         /* (the variables let go of with the engine; the object may live on) */
        VARIANT *old = malloc((s->vs.n ? s->vs.n : 1) * sizeof(VARIANT));
        EnterCriticalSection(&s->vs.cs);
        for (int k = 0; k < s->vs.n; k++) {
            if (old) old[k] = s->vs.v[k]; else VariantClear(&s->vs.v[k]);
            VariantInit(&s->vs.v[k]);
        }
        s->vs.init = 0;
        LeaveCriticalSection(&s->vs.cs);
        if (old) { for (int k = 0; k < s->vs.n; k++) VariantClear(&old[k]); free(old); }
        IDispatch_Release(e->disp);
    }
    if (e->named) IDispatch_Release(e->named);
    prog_free(&e->g);
    free(e);
}
