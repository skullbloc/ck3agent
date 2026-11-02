"""The ONE C++ tokenizer every checker in this repo reads source through
(L13 FF3, witness X4/X5).

Seven hand-rolled strippers each re-decided what a comment and a literal are,
and each got one case wrong in its own way: a backslash inside a raw string
read as an escape (R"json({\"ids\":[1]})json" put check_no_va_output out of
phase for 600 lines of query_handlers.cpp, and an address key went unseen); a
digit separator (1'000) read as a char literal; a `//` inside a string read
as a comment. A stripper out of phase reads code as strings and strings as
code, and a checker reading it reports GREENER. So there is one scan here, and
it refuses rather than guesses: an unterminated comment, string, char or raw
string, or a newline inside an ordinary literal, raises CppParseError, which
every caller turns into a red row (fail OPEN), never a pass.

What it knows:
  - `//` comments (a backslash-newline continues one) and `/* */` comments;
  - ordinary strings and char literals, with escapes, and their encoding
    prefixes (u8, u, U, L) as code;
  - raw strings with any prefix (R, u8R, uR, UR, LR) and any delimiter:
    nothing inside one is an escape or a comment;
  - a digit separator: a `'` inside a numeric literal (the token before it
    starts with a digit, or `.digit`) and followed by a digit or letter.
Not modelled: trigraphs, a `\\` line splice outside a // comment, and the
preprocessor (an #include <a'b> header name reads as a char literal and
raises).

API:
  tokens(src)          [(kind, start, end)], kinds: code, line_comment,
                       block_comment, string, char, raw_string. Every
                       character is in exactly one token.
  strip(src, keep_strings=False, keep_comments=False, keep_quotes=True,
        drop_comments=False)
                       the source, same length, newlines kept: comments
                       blanked to spaces, and each literal's contents (between
                       its quotes; for a raw string, between the opening and
                       closing quote) blanked unless keep_strings; with
                       keep_quotes=False the quotes are blanked too. With
                       drop_comments=True comments are REMOVED instead (the
                       text gets shorter; a block comment takes its newlines
                       with it): the shape scripts/cpp_source.strip_comments
                       has always returned, whose readers measure distances
                       in it.
  literals(src)        [(kind, start, end, body)] for string, char, raw_string;
                       body is the text between the quotes (a raw string's
                       without the delimiter and parentheses).
  match_close(src, i)  index of the bracket closing the ( [ { at src[i], in code
                       (brackets in comments and literals do not count), or -1.
  split_args(src, i)   the top-level comma-separated arguments of the call whose
                       '(' is at src[i], as source text (stripped of outer
                       whitespace), or None when unbalanced.
  selftest()           0 when every row below passes.

No dependencies, no host path: ships in the public export beside
inline_safety_static.py (release/manifest.toml).
"""
from __future__ import annotations

import functools
import sys

__all__ = ["CppParseError", "tokens", "strip", "literals", "match_close", "split_args",
           "selftest"]


class CppParseError(ValueError):
    """The scan cannot place a character: the caller reds, never passes."""


_RAW_PREFIXES = ("u8R", "uR", "UR", "LR", "R")


def _ident_char(c: str) -> bool:
    return c.isalnum() or c == "_"


def _raw_prefix_at(src: str, i: int) -> int:
    """Length of a raw-string prefix ending just before src[i] == '"', or 0.
    The prefix must start a token (not follow an identifier character)."""
    for p in _RAW_PREFIXES:
        s = i - len(p)
        if s >= 0 and src[s:i] == p and (s == 0 or not _ident_char(src[s - 1])):
            return len(p)
    return 0


def _is_digit_separator(src: str, i: int) -> bool:
    """src[i] == "'" inside a numeric literal (pp-number) and followed by an
    alphanumeric character."""
    if i + 1 >= len(src) or not src[i + 1].isalnum():
        return False
    j = i
    while j > 0 and (_ident_char(src[j - 1]) or src[j - 1] in ".'"):
        j -= 1
    tok = src[j:i]
    if not tok:
        return False
    return tok[0].isdigit() or (tok[0] == "." and len(tok) > 1 and tok[1].isdigit())


def _line_of(src: str, i: int) -> int:
    return src.count("\n", 0, i) + 1


@functools.lru_cache(maxsize=4096)
def _tokens(src: str) -> tuple:
    out: list[tuple[str, int, int]] = []
    n = len(src)
    i = 0
    code_start = 0

    def flush(upto: int) -> None:
        if upto > code_start:
            out.append(("code", code_start, upto))

    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            flush(i)
            j = i + 2
            while j < n:
                if src[j] == "\n":
                    # a backslash-newline continues the comment
                    k = j - 1
                    if k >= 0 and src[k] == "\\":
                        j += 1
                        continue
                    break
                j += 1
            out.append(("line_comment", i, j))
            i = code_start = j
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            flush(i)
            j = src.find("*/", i + 2)
            if j < 0:
                raise CppParseError(f"line {_line_of(src, i)}: unterminated /* comment")
            out.append(("block_comment", i, j + 2))
            i = code_start = j + 2
            continue
        if c == '"':
            plen = _raw_prefix_at(src, i)
            if plen:
                k = src.find("(", i + 1)
                delim = src[i + 1:k] if k >= 0 else ""
                if k < 0 or len(delim) > 16 or any(ch in delim for ch in ' ()\\\t\n"'):
                    raise CppParseError(f"line {_line_of(src, i)}: malformed raw string delimiter")
                end = src.find(")" + delim + '"', k + 1)
                if end < 0:
                    raise CppParseError(f"line {_line_of(src, i)}: unterminated raw string")
                end += len(delim) + 2
                # the prefix letters are part of the token
                flush(i - plen)
                out.append(("raw_string", i - plen, end))
                i = code_start = end
                continue
            flush(i)
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "\n":
                    raise CppParseError(f"line {_line_of(src, i)}: newline inside a string literal")
                j += 1
            if j >= n:
                raise CppParseError(f"line {_line_of(src, i)}: unterminated string literal")
            out.append(("string", i, j + 1))
            i = code_start = j + 1
            continue
        if c == "'":
            if _is_digit_separator(src, i):
                i += 1
                continue
            flush(i)
            j = i + 1
            while j < n and src[j] != "'":
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "\n":
                    raise CppParseError(f"line {_line_of(src, i)}: newline inside a char literal")
                j += 1
            if j >= n:
                raise CppParseError(f"line {_line_of(src, i)}: unterminated char literal")
            out.append(("char", i, j + 1))
            i = code_start = j + 1
            continue
        i += 1
    flush(n)
    return tuple(out)


def tokens(src: str) -> list[tuple[str, int, int]]:
    return list(_tokens(src))


def _blank(t: str) -> str:
    """Spaces, keeping line ends (\n and \r) so lines stay aligned."""
    return "".join(ch if ch in "\r\n" else " " for ch in t)


def _body_span(src: str, kind: str, s: int, e: int) -> tuple[int, int]:
    """[start, end) of the text between a literal's quotes."""
    if kind == "raw_string":
        q = src.index('"', s)
        k = src.index("(", q)
        delim = src[q + 1:k]
        return k + 1, e - len(delim) - 2
    if kind == "string":
        return s + 1, e - 1
    return s + 1, e - 1          # char


@functools.lru_cache(maxsize=4096)
def strip(src: str, keep_strings: bool = False, keep_comments: bool = False,
          keep_quotes: bool = True, drop_comments: bool = False) -> str:
    parts: list[str] = []
    for kind, s, e in _tokens(src):
        if kind == "code":
            parts.append(src[s:e])
        elif kind in ("line_comment", "block_comment"):
            if drop_comments:
                # a // comment continued by a backslash keeps its line ends
                # (one physical line each); a block comment takes them with it
                if kind == "line_comment":
                    parts.append("\n" * src.count("\n", s, e))
                continue
            parts.append(src[s:e] if keep_comments else _blank(src[s:e]))
        elif keep_strings:
            parts.append(src[s:e])
        else:
            # a raw string's prefix letters stay; the literal from its opening
            # quote is blanked, quotes too unless keep_quotes
            q = src.index('"', s) if kind == "raw_string" else s
            if keep_quotes:
                parts.append(src[s:q + 1] + _blank(src[q + 1:e - 1]) + src[e - 1])
            else:
                parts.append(src[s:q] + _blank(src[q:e]))
    return "".join(parts)


def literals(src: str) -> list[tuple[str, int, int, str]]:
    out = []
    for kind, s, e in _tokens(src):
        if kind in ("string", "char", "raw_string"):
            b0, b1 = _body_span(src, kind, s, e)
            out.append((kind, s, e, src[b0:b1]))
    return out


_PAIRS = {"(": ")", "[": "]", "{": "}"}


def match_close(src: str, i: int) -> int:
    code = strip(src)
    open_c = code[i] if i < len(code) else ""
    close_c = _PAIRS.get(open_c)
    if not close_c:
        return -1
    depth = 0
    for j in range(i, len(code)):
        c = code[j]
        if c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return j
    return -1


def split_args(src: str, open_paren: int) -> list[str] | None:
    code = strip(src)
    end = match_close(src, open_paren)
    if end < 0:
        return None
    args, depth, start = [], 0, open_paren + 1
    for j in range(open_paren + 1, end):
        c = code[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            args.append(src[start:j].strip())
            start = j + 1
    args.append(src[start:end].strip())
    return args


# ── selftest ────────────────────────────────────────────────────────────────
# Each row: (name, source, the code text expected after strip(), with runs of
# whitespace collapsed). The last rows must RAISE.

_ROWS = (
    ("line comment", 'a; // b "c\nd;', "a; d;"),
    ("block comment", "a /* b ' c */ d;", "a d;"),
    ("comment marker inside a string", 'f("http://x/*y*/"); g;', 'f(" "); g;'),
    ("quote inside a comment", "a; // it's\nb;", "a; b;"),
    ("raw string with a backslash before a quote",
     'a = R"json({\\"ids\\":[1]})json"; // it\'s\nb;', 'a = R" "; b;'),
    ("raw string with a quote and a comment marker",
     'a = R"x(" // not a comment)x"; b;', 'a = R" "; b;'),
    ("u8R and LR raw strings", 'a = u8R"(")"; b = LR"d(x)d"; c;', 'a = u8R" "; b = LR" "; c;'),
    ("escaped quote in a string", 'a = "x\\"y"; b;', 'a = " "; b;'),
    ("char literals", "a = '\"'; b = '\\''; c;", "a = ' '; b = ' '; c;"),
    ("an odd digit separator", "int n = 1'0; int m = 'a'; z;", "int n = 1'0; int m = ' '; z;"),
    ("digit separators in hex and float", "x = 0xFF'FF; y = 1'000.5; z = 'q';",
     "x = 0xFF'FF; y = 1'000.5; z = ' ';"),
    ("u8 char literal prefix is not a number", "a = u8'x'; b;", "a = u8' '; b;"),
    ("line comment continued by a backslash", "a; // b \\\nc;\nd;", "a; d;"),
    ("an identifier ending in R is not a raw prefix", 'BAR"x"; y;', 'BAR" "; y;'),
)
_RAISES = (
    ("unterminated block comment", "a /* b"),
    ("unterminated string", 'a = "b;'),
    ("newline inside a string", 'a = "b\nc";'),
    ("unterminated raw string", 'a = R"x(b)y";'),
    ("unterminated char", "a = 'b"),
)


def _norm(t: str) -> str:
    return " ".join(t.split())


def selftest() -> int:
    fails = 0
    for name, src, want in _ROWS:
        try:
            got = strip(src)
            ok = _norm(got) == _norm(want) and len(got) == len(src)
        except CppParseError as e:
            got, ok = f"raised {e}", False
        print(f"{'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f": {got!r}"))
        fails += not ok
    for name, src in _RAISES:
        try:
            strip(src)
            ok = False
        except CppParseError:
            ok = True
        print(f"{'ok  ' if ok else 'FAIL'} raises: {name}")
        fails += not ok
    # every character in exactly one token, in order
    src = 'x = R"(a)" + "b" + \'c\'; /* d */ // e\nf(1\'0);'
    toks = tokens(src)
    cover = all(toks[k][2] == toks[k + 1][1] for k in range(len(toks) - 1)) and \
        toks[0][1] == 0 and toks[-1][2] == len(src)
    print(f"{'ok  ' if cover else 'FAIL'} tokens cover the source in order")
    fails += not cover
    lits = [b for _, _, _, b in literals(src)]
    lok = lits == ["a", "b", "c"]
    print(f"{'ok  ' if lok else 'FAIL'} literal bodies {lits}")
    fails += not lok
    call = 'f(a, "x,)", R"q(,))q", g(b, c) /* ,) */, \'(\') + 1;'
    args = split_args(call, 1)
    aok = args == ['a', '"x,)"', 'R"q(,))q"', "g(b, c) /* ,) */", "'('"] and match_close(call, 1) == call.rindex(")")
    print(f"{'ok  ' if aok else 'FAIL'} split_args / match_close skip literals and comments {args}")
    fails += not aok
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(selftest())
