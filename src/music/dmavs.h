/* reDM: AudioVBScript, dmscript.dll's own scripting language (CAudioVBScriptEngine and its parser and executor):
 * a script's routines and variables behind an IDispatch, as the script object drives it through Active Scripting. */
#ifndef REAL_DMAVS_H
#define REAL_DMAVS_H
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>

typedef struct avs_engine avs_engine;

/* the IID dmscript's objects take in IDispatch::Invoke besides IID_NULL (1fcc43db): its own strings, not BSTRs */
extern const GUID AVS_IID_PRIVATE;
/* dmscript's own conversion of a VARIANT to VT_I4 / VT_DISPATCH / VT_UNKNOWN (0040deec) */
HRESULT avs_convert(VARIANT *d, VARIANT *s, VARTYPE vt);
HRESULT avs_copy(VARIANT *d, const VARIANT *s);

/* what a syntax error reports (IActiveScriptError): the scode, the description, its source, the 0-based line and
 * column and the rest of the line from where the parser stopped */
typedef struct {
    HRESULT scode;
    WCHAR *desc, *source, *text;
    ULONG line, col;
} avs_error;

/* ParseScriptText: the source parsed against the "DirectMusic" item (names it knows become its properties and
 * methods); on a syntax error *err is filled (free with avs_error_free) and the result is the parser's HRESULT */
HRESULT avs_parse(const WCHAR *src, IDispatch *global, avs_engine **out, avs_error *err);
void avs_error_free(avs_error *err);
void avs_free(avs_engine *e);
/* GetScriptDispatch: routines (DISPID 1..n) and the script's variables (1000001 + index) */
HRESULT avs_dispatch(avs_engine *e, IDispatch **out);
/* 0040e4a5: a runtime error's description as "file»line»description", its file and line (when it has none) the
 * ones given (either may be NULL) */
void avs_stamp(EXCEPINFO *ei, const WCHAR *file, const ULONG *line);

#endif
