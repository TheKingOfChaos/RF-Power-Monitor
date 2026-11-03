"""Minimal KiCad s-expression reader/writer: lists of str tokens; quoted strings keep their quotes."""
import re

_TOK = re.compile(r'\s*(\(|\)|"(?:\\.|[^"\\])*"|[^\s()"]+)', re.S)


def parse(text):
    stack, cur = [], []
    pos = 0
    n = len(text)
    while pos < n:
        m = _TOK.match(text, pos)
        if not m:
            if text[pos:].strip() == "":
                break
            raise ValueError(f"bad token at {pos}: {text[pos:pos + 40]!r}")
        tok = m.group(1)
        pos = m.end()
        if tok == "(":
            stack.append(cur)
            cur = []
        elif tok == ")":
            done = cur
            cur = stack.pop()
            cur.append(done)
        else:
            cur.append(tok)
    return cur[0] if len(cur) == 1 else cur


def dump(node):
    if isinstance(node, list):
        return "(" + " ".join(dump(x) for x in node) + ")"
    return node


def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def uq(s):
    if isinstance(s, str) and s.startswith('"'):
        return s[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return s


def find(node, key):
    """first child list whose head is key"""
    for x in node:
        if isinstance(x, list) and x and x[0] == key:
            return x
    return None


def findall(node, key):
    return [x for x in node if isinstance(x, list) and x and x[0] == key]


def walk(node):
    yield node
    for x in node:
        if isinstance(x, list):
            yield from walk(x)
