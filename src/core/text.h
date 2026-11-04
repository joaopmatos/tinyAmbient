#ifndef WN_CORE_TEXT_H
#define WN_CORE_TEXT_H

#include <stdarg.h>
#include <stddef.h>

/* Small text helpers shared by the layers that parse or format command text.
 * They live in core/ because ui/ may depend on core/ but not the reverse, and
 * because the same case-folding rule has to apply to a noise name, a flag, and
 * a file extension. */

/* Case-insensitive compare over ASCII letters. Both strings must be
 * NUL-terminated. Locale is deliberately ignored: command names, noise types,
 * and extensions are ASCII, and a Turkish locale must not fold 'I' differently
 * from the parser's own view of the same text. */
int wn_text_equal_ci(const char *a, const char *b);

/* vsnprintf into `buf`. A NULL buffer or a zero capacity is a no-op, and
 * truncation is silent: every caller either owns a buffer large enough for the
 * message or only needs a best-effort diagnostic. */
void wn_text_vset(char *buf, size_t cap, const char *fmt, va_list args);

/* Variadic form of wn_text_vset. The format attribute lets the compiler check
 * the arguments where it understands the syntax. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 4)))
#endif
void wn_text_set(char *buf, size_t cap, const char *fmt, ...);

#endif /* WN_CORE_TEXT_H */
