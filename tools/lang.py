#!/usr/bin/env python3
"""Translations of the BRODALF window app.

The window app is written in English. Every piece of text the person reads
is marked in gui/main.c with T(L"..."), TN(n, L"one", L"many") for a
sentence with a count, or N_(L"...") in a table; the dialogs' text is in
gui/brodalf.rc. Translations are ordinary gettext files, gui/lang/<code>.po,
which any .po editor (Poedit, for one) opens.

    python3 tools/lang.py pot           writes gui/lang/brodalf.pot from the source
    python3 tools/lang.py update        adds the new English to every .po, marks gone English obsolete
    python3 tools/lang.py check         checks every .po and the format calls in gui/main.c
    python3 tools/lang.py check --strict    also fails when a .po lacks a translation

A translation must keep the %-codes of the English exactly (the same ones,
in the same order), and the spaces and line breaks at its start and end.
The app never shows a translation that does not: it shows the English.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, 'gui', 'main.c')
RC = os.path.join(ROOT, 'gui', 'brodalf.rc')
LANG = os.path.join(ROOT, 'gui', 'lang')

CODE = re.compile(r'%(?:%|[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*)?)?(?:hh|h|ll|l|L|I64|I32|I|z|j|t|w)?[a-zA-Z])')


def codes(s):
    return CODE.findall(s)


# ---- reading C ------------------------------------------------------------

def c_unescape(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != '\\':
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        simple = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}
        if n in simple:
            out.append(simple[n])
            i += 2
        elif n == 'x':
            m = re.match(r'[0-9a-fA-F]+', body[i + 2:])
            out.append(chr(int(m.group(0), 16)))
            i += 2 + len(m.group(0))
        elif n == 'u':
            out.append(chr(int(body[i + 2:i + 6], 16)))
            i += 6
        elif n in '01234567':
            m = re.match(r'[0-7]{1,3}', body[i + 1:])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
        else:
            out.append(n)
            i += 2
    return ''.join(out)


class Source:
    """gui/main.c with its comments, character constants and strings known,
    so calls can be split into their arguments."""

    def __init__(self, path):
        self.path = path
        self.text = open(path, encoding='utf-8').read()
        self.lines = [0]
        for m in re.finditer('\n', self.text):
            self.lines.append(m.end())

    def line(self, pos):
        lo, hi = 0, len(self.lines)
        while lo + 1 < hi:
            mid = (lo + hi) // 2
            if self.lines[mid] <= pos:
                lo = mid
            else:
                hi = mid
        return lo + 1

    def skip(self, i):
        """Past spaces and comments."""
        t = self.text
        while i < len(t):
            if t[i] in ' \t\r\n':
                i += 1
            elif t.startswith('/*', i):
                i = t.index('*/', i) + 2
            elif t.startswith('//', i):
                i = t.index('\n', i)
            else:
                break
        return i

    def literal_end(self, i):
        """i at the opening quote; the position after the closing one."""
        t = self.text
        i += 1
        while t[i] != '"':
            i += 2 if t[i] == '\\' else 1
        return i + 1

    def args(self, open_paren):
        """The arguments of the call whose ( is at open_paren, as (start, end) pairs."""
        t = self.text
        depth, i, start, out = 0, open_paren + 1, open_paren + 1, []
        while i < len(t):
            c = t[i]
            if t.startswith('/*', i):
                i = t.index('*/', i) + 2
                continue
            if t.startswith('//', i):
                i = t.index('\n', i)
                continue
            if c == '"':
                i = self.literal_end(i)
                continue
            if c == "'":
                i += 1
                while t[i] != "'":
                    i += 2 if t[i] == '\\' else 1
                i += 1
                continue
            if c in '([{':
                depth += 1
            elif c in ')]}':
                if depth == 0:
                    out.append((start, i))
                    return out, i
                depth -= 1
            elif c == ',' and depth == 0:
                out.append((start, i))
                start = i + 1
            i += 1
        raise ValueError('unclosed call at line %d' % self.line(open_paren))

    def literal_group(self, start, end):
        """The text of an argument that is only wide literals one after another, else None."""
        t = self.text
        i, parts = self.skip(start), []
        while i < end:
            if not t.startswith('L"', i):
                return None
            j = self.literal_end(i + 1)
            parts.append(c_unescape(t[i + 2:j - 1]))
            i = self.skip(j)
        return ''.join(parts) if parts else None

    def calls(self, names):
        """(name, position of the name, position of its open paren) for every call to one of names."""
        pat = re.compile(r'\b(' + '|'.join(map(re.escape, names)) + r')\s*\(')
        t = self.text
        i = 0
        while True:
            m = pat.search(t, i)
            if not m:
                return
            # Skip what is inside comments and strings.
            prefix = t[self.lines[self.line(m.start()) - 1]:m.start()]
            if '//' in prefix or prefix.count('"') % 2 or self.in_block_comment(m.start()):
                i = m.end()
                continue
            yield m.group(1), m.start(), m.end() - 1
            i = m.end()

    def in_block_comment(self, pos):
        a = self.text.rfind('/*', 0, pos)
        return a >= 0 and self.text.rfind('*/', 0, pos) < a


# ---- what is to be translated ---------------------------------------------

class Entry:
    def __init__(self, msgid, plural=None):
        self.msgid, self.plural, self.refs = msgid, plural, []


def collect():
    entries, order = {}, []

    def add(msgid, plural, ref):
        if not msgid:
            return
        e = entries.get(msgid)
        if e is None:
            e = entries[msgid] = Entry(msgid, plural)
            order.append(e)
        elif plural and not e.plural:
            e.plural = plural
        e.refs.append(ref)

    src = Source(MAIN)
    problems = []
    for name, at, paren in src.calls(['T', 'TN', 'N_']):
        before = src.text[max(0, at - 40):at].rstrip()
        if before.endswith('*') or before.endswith('define'):
            continue  # its definition, not a use
        args, _ = src.args(paren)
        ref = 'gui/main.c:%d' % src.line(at)
        if name == 'TN':
            if len(args) != 3:
                continue
            one, many = src.literal_group(*args[1]), src.literal_group(*args[2])
            if one is None or many is None:
                problems.append('%s: TN needs two pieces of text written out' % ref)
                continue
            if codes(one) != codes(many):
                problems.append('%s: the two forms have different %%-codes: %r and %r' % (ref, one, many))
            add(one, many, ref)
        else:
            if len(args) != 1:
                continue
            text = src.literal_group(*args[0])
            if text is not None:
                add(text, None, ref)

    rc = open(RC, encoding='utf-8').read().split('\n')
    for n, line in enumerate(rc, 1):
        m = re.match(r'\s*(CAPTION|LTEXT|RTEXT|CTEXT|PUSHBUTTON|DEFPUSHBUTTON|AUTOCHECKBOX|CHECKBOX|GROUPBOX|AUTORADIOBUTTON|RADIOBUTTON|CONTROL)\s+"((?:[^"]|"")*)"', line)
        if m:
            add(m.group(2).replace('""', '"'), None, 'gui/brodalf.rc:%d' % n)
    return order, problems, src


# ---- .po files ----------------------------------------------------------------

def po_quote(s):
    s = s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r')
    pieces = s.split('\n')
    lines = [p + '\\n' for p in pieces[:-1]] + ([pieces[-1]] if pieces[-1] else [])
    if len(lines) <= 1 and len(s) < 70:
        return '"%s"' % (lines[0] if lines else '')
    return '""\n' + '\n'.join('"%s"' % l for l in lines)


def po_unquote(parts):
    s = ''.join(parts)
    out, i = [], 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            out.append({'n': '\n', 't': '\t', 'r': '\r'}.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


class PoEntry:
    def __init__(self):
        self.comments, self.msgid, self.plural, self.msgstr = [], None, None, {}
        self.fuzzy = self.obsolete = False


def read_po(path):
    entries, cur, field = [], PoEntry(), None
    raw = {}

    def finish():
        nonlocal cur, raw
        if cur.msgid is not None:
            cur.msgid = po_unquote(raw.get('msgid', []))
            cur.plural = po_unquote(raw['msgid_plural']) if 'msgid_plural' in raw else None
            for k, v in raw.items():
                if k.startswith('msgstr'):
                    idx = int(k[7:-1]) if '[' in k else 0
                    cur.msgstr[idx] = po_unquote(v)
            entries.append(cur)
        cur, raw = PoEntry(), {}

    for line in open(path, encoding='utf-8').read().split('\n'):
        s = line.strip()
        obsolete = s.startswith('#~')
        if obsolete:
            s = s[2:].strip()
        if not s:
            continue
        if s.startswith('#') and not obsolete:
            if cur.msgid is not None and any(k.startswith('msgstr') for k in raw):
                finish()
            cur.comments.append(line)
            if s.startswith('#,') and 'fuzzy' in s:
                cur.fuzzy = True
            continue
        m = re.match(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(".*")$', s)
        if m:
            if m.group(1) in ('msgid', 'msgctxt') and any(k.startswith('msgstr') for k in raw):
                finish()
            field = m.group(1)
            if field == 'msgid' and cur.msgid is None:
                cur.msgid = ''
            cur.obsolete = obsolete
            raw.setdefault(field, []).append(m.group(2)[1:-1])
        elif s.startswith('"') and field:
            raw[field].append(s[1:-1])
    finish()
    return entries


def write_po(path, header, entries, obsolete=()):
    out = [header.rstrip('\n'), '']
    for e in entries:
        out.extend(e.comments)
        if e.fuzzy and not any('fuzzy' in c for c in e.comments):
            out.append('#, fuzzy')
        if '%' in e.msgid and not any('c-format' in c for c in e.comments):
            out.append('#, c-format')
        out.append('msgid ' + po_quote(e.msgid))
        if e.plural is not None:
            out.append('msgid_plural ' + po_quote(e.plural))
            for k in sorted(e.msgstr) or [0, 1]:
                out.append('msgstr[%d] ' % k + po_quote(e.msgstr.get(k, '')))
        else:
            out.append('msgstr ' + po_quote(e.msgstr.get(0, '')))
        out.append('')
    for e in obsolete:
        out.append('#~ msgid ' + po_quote(e.msgid).replace('\n', '\n#~ '))
        if e.plural is not None:
            out.append('#~ msgid_plural ' + po_quote(e.plural).replace('\n', '\n#~ '))
            for k in sorted(e.msgstr):
                out.append(('#~ msgstr[%d] ' % k) + po_quote(e.msgstr[k]).replace('\n', '\n#~ '))
        else:
            out.append('#~ msgstr ' + po_quote(e.msgstr.get(0, '')).replace('\n', '\n#~ '))
        out.append('')
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))


POT_HEADER = r'''# The text of the BRODALF window app, to translate.
# Made by tools/lang.py from gui/main.c and gui/brodalf.rc; do not edit.
msgid ""
msgstr ""
"Project-Id-Version: BRODALF\n"
"MIME-Version: 1.0\n"
"Content-Type: text/plain; charset=UTF-8\n"
"Content-Transfer-Encoding: 8bit\n"
"Plural-Forms: nplurals=2; plural=(n != 1);\n"
'''


def pot_entries(order):
    out = []
    for e in order:
        p = PoEntry()
        p.msgid, p.plural = e.msgid, e.plural
        p.comments = ['#: ' + ' '.join(e.refs[:4]) + (' ...' if len(e.refs) > 4 else '')]
        out.append(p)
    return out


def po_files():
    if not os.path.isdir(LANG):
        return []
    return sorted(os.path.join(LANG, f) for f in os.listdir(LANG) if f.endswith('.po'))


def po_header(path):
    text = open(path, encoding='utf-8').read()
    m = re.match(r'((?:#[^\n]*\n)*msgid ""\nmsgstr ""\n(?:"[^\n]*"\n)*)', text)
    return m.group(1) if m else POT_HEADER


# ---- checking the format calls ----------------------------------------------

FORMAT_CALLS = {'swprintf': 2, 'wadd': 1, 'say': 1, 'vswprintf': None}


def check_calls(src):
    """Every swprintf, wadd and say whose format is written out (or is T or TN
    of text written out) is given as many values as its format has codes."""
    problems, checked = [], 0
    for name, at, paren in src.calls(['swprintf', 'wadd', 'say']):
        args, _ = src.args(paren)
        k = FORMAT_CALLS[name]
        if len(args) <= k:
            continue
        a, b = args[k]
        fmt_src = src.text[a:b].strip()
        forms = []
        text = src.literal_group(a, b)
        if text is not None:
            forms = [text]
        else:
            m = re.match(r'(T|TN)\s*\(', fmt_src)
            if m and src.text[a:b].rstrip().endswith(')'):
                inner, _ = src.args(src.text.index('(', a))
                if m.group(1) == 'T' and len(inner) == 1:
                    t = src.literal_group(*inner[0])
                    forms = [t] if t is not None else []
                elif m.group(1) == 'TN' and len(inner) == 3:
                    forms = [src.literal_group(*inner[1]), src.literal_group(*inner[2])]
                    forms = [f for f in forms if f is not None]
        if not forms:
            continue
        values = len(args) - k - 1
        for f in forms:
            want = sum(1 + c.count('*') for c in codes(f) if c != '%%')  # a * width or precision takes a value too
            if want != values:
                problems.append('gui/main.c:%d: %s has %d %%-code(s) but %d value(s): %r'
                                % (src.line(at), name, want, values, f[:60]))
        checked += 1
    return problems, checked


# ---- commands --------------------------------------------------------------

def cmd_pot():
    order, problems, _ = collect()
    for p in problems:
        print(p)
    os.makedirs(LANG, exist_ok=True)
    write_po(os.path.join(LANG, 'brodalf.pot'), POT_HEADER, pot_entries(order))
    print('gui/lang/brodalf.pot: %d pieces of text' % len(order))
    return 1 if problems else 0


def cmd_update():
    order, problems, _ = collect()
    cmd_pot()
    for path in po_files():
        old = {e.msgid: e for e in read_po(path) if e.msgid and not e.obsolete}
        keep = []
        for p in pot_entries(order):
            o = old.pop(p.msgid, None)
            if o:
                p.msgstr, p.fuzzy = o.msgstr, o.fuzzy
                p.comments = [c for c in o.comments if c.startswith('# ') or c.startswith('#.')] + p.comments
            keep.append(p)
        write_po(path, po_header(path), keep, [e for e in old.values() if any(e.msgstr.values())])
        print('%s: %d gone, kept as obsolete' % (os.path.relpath(path, ROOT), len(old)))
    return 0


def edges(s):
    return (re.match(r'\s*', s).group(0), re.search(r'\s*$', s).group(0))


def cmd_check(strict):
    order, problems, src = collect()
    want = {e.msgid: e for e in order}
    call_problems, checked = check_calls(src)
    problems += call_problems
    print('format calls: %d checked' % checked)
    for path in po_files():
        name = os.path.relpath(path, ROOT)
        entries = read_po(path)
        header = next((e for e in entries if e.msgid == ''), None)
        rule = header.msgstr.get(0, '') if header else ''
        if 'Plural-Forms' not in rule:
            problems.append('%s: no Plural-Forms in the header' % name)
        elif not re.search(r'plural=\(?\s*n\s*(!=\s*1|>\s*1)\s*\)?\s*;', rule):
            problems.append('%s: BRODALF knows only plural=(n != 1) and plural=(n > 1)' % name)
        done, missing, stale = 0, 0, 0
        seen = set()
        for e in entries:
            if not e.msgid or e.obsolete:
                continue
            seen.add(e.msgid)
            w = want.get(e.msgid)
            if w is None:
                stale += 1
                continue
            forms = [e.msgstr.get(0, '')] + ([e.msgstr.get(1, '')] if w.plural else [])
            if e.fuzzy or not all(forms):
                missing += 1
                continue
            english = [w.msgid] + ([w.plural] if w.plural else [])
            for f, en in zip(forms, english):
                if codes(f) != codes(en):
                    problems.append('%s: %%-codes differ for %r: %r' % (name, en[:60], f[:60]))
                if edges(f) != edges(en):
                    problems.append('%s: spaces or line breaks at the start or end differ for %r' % (name, en[:60]))
            done += 1
        missing += sum(1 for m in want if m not in seen)
        print('%s: %d translated, %d missing, %d no longer used' % (name, done, missing, stale))
        if strict and missing:
            problems.append('%s: %d pieces of text are not translated' % (name, missing))
    for p in problems:
        print(p)
    return 1 if problems else 0


def main(argv):
    if len(argv) >= 2 and argv[1] == 'pot':
        return cmd_pot()
    if len(argv) >= 2 and argv[1] == 'update':
        return cmd_update()
    if len(argv) >= 2 and argv[1] == 'check':
        return cmd_check('--strict' in argv[2:])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
