#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Check the public header's thread-safety tags against the code.

Every function that the public header declares must have exactly one of
the Doxygen tags @serialized and @concurrent in its documentation
comment. A serialized function's definition must start by taking the
library-wide API lock and returning kReentryError when the guard doesn't
own it:

    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }

A concurrent function's definition mustn't use ApiGuard at all. Comments
and literals, including raw string literals, don't count for either
check. The "threads" group in hipobj.h explains what the tags mean.

Usage: check_thread_safety_tags.py <hipobj.h> <hipobj.cpp>

Exits with 0 when the tags and the code match. Otherwise, it lists every
problem and exits with 1.
"""

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

TAGS = ("serialized", "concurrent")

# The tokens that can hide quotes or comment markers: a comment, a number,
# or a string or character literal, including a raw string, which can span
# lines. Matching them all in one left-to-right pass means a comment marker
# inside a literal isn't taken for a comment, a quote inside a comment isn't
# taken for a literal, and a digit separator in a number (1'000) isn't taken
# for the start of a character literal. An encoding prefix (u8, u, U, L) is
# only a prefix when it doesn't end a longer identifier. A raw string's
# delimiter is up to 16 characters, any but a space, a parenthesis, a
# backslash, or the control whitespace characters, so it can contain ".
TOKEN = re.compile(
    r"""
      (?P<comment> //[^\n]* | /\*.*?\*/ )
    | (?P<number> (?<![\w.]) \.?\d (?: [eEpP][+-] | '?[\w.] )* )
    | (?P<raw> (?<!\w) (?:u8|[uUL])?
               R"(?P<delim>[^\x20()\\\t\v\f\r\n]{0,16})\( .*? \)(?P=delim)" )
    | (?P<string> (?:(?<!\w)(?:u8|[uUL]))? " (?:\\.|[^"\\\n])* " )
    | (?P<char> (?:(?<!\w)(?:u8|[uUL]))? ' (?:\\.|[^'\\\n])* ' )
    """,
    re.DOTALL | re.VERBOSE,
)

# A public function's declaration in the header, which starts a line
DECLARATION = re.compile(r"^HIPOBJ_API\b[^;(]*?\b(hipObj\w+)\s*\(", re.MULTILINE)

# A public function's definition in the source, whose name starts a line
DEFINITION = re.compile(r"^(hipObj\w+)\s*\(", re.MULTILINE)

# The start of a serialized function, from just after its name's "(": the
# rest of its parameters, the body's "{" (or "try {"), the guard, and the
# check of the guard. \1 makes owns() use the guard the first statement
# declared.
GUARDED_START = re.compile(
    r"[^{;]*\)\s*(?:try\s*)?\{\s*"
    r"(?:hipObj::)?ApiGuard\s+(\w+)\s*;\s*"
    r"if\s*\(\s*!\s*\1\s*\.\s*owns\s*\(\s*\)\s*\)\s*\{\s*"
    r"return\s+(?:hipObj::)?kReentryError\s*;\s*\}"
)

TAG = re.compile(r"[@\\](" + "|".join(TAGS) + r")\b")

IF_ZERO = re.compile(r"#\s*if\s+0\b")
IF_ANY = re.compile(r"#\s*if")  # #if, #ifdef, and #ifndef
ELSE_OR_ELIF = re.compile(r"#\s*(else|elif)\b")
ENDIF = re.compile(r"#\s*endif\b")


@dataclass
class Declaration:
    """A public function declared in the header"""

    name: str
    line: int
    tags: list[str] = field(default_factory=list)
    has_doc_comment: bool = False


@dataclass
class Definition:
    """A public function defined in the source"""

    name: str
    line: int
    starts_guarded: bool  # it starts the way GUARDED_START describes
    mentions_guard: bool  # ApiGuard appears anywhere in it


def line_number(text: str, offset: int) -> int:
    """Return the 1-based line number of offset in text"""
    return text.count("\n", 0, offset) + 1


def drop_disabled_code(text: str) -> str:
    """Blank the lines inside #if 0 blocks, keeping the line numbers.

    The compiler and Doxygen never see these lines, so neither does the
    check.
    """
    out = []
    depth = 0  # how deep inside an #if 0 block, or 0 outside one
    for line in text.splitlines(keepends=True):
        directive = line.strip()
        if depth == 0:
            if IF_ZERO.match(directive):
                depth = 1
                out.append("\n")
            else:
                out.append(line)
            continue
        if IF_ANY.match(directive):
            depth += 1
        elif ENDIF.match(directive):
            depth -= 1
        elif depth == 1 and ELSE_OR_ELIF.match(directive):
            # The rest of the block may be compiled, so check it
            depth = 0
        out.append("\n")
    return "".join(out)


def blank_comments_and_literals(text: str) -> str:
    """Replace each comment and literal with spaces, keeping the newlines.

    What's left is code, so text in a comment or a literal can't look like
    a use of ApiGuard or the start of a definition, and the line numbers
    don't change.
    """

    def blank(match: re.Match[str]) -> str:
        if match.group("number") is not None:
            return match.group(0)
        return re.sub(r"[^\n]", " ", match.group(0))

    return TOKEN.sub(blank, text)


def find_declarations(header: str) -> list[Declaration]:
    """Find the public functions in the header, with their tags"""
    header = drop_disabled_code(header)
    declarations = []
    for match in DECLARATION.finditer(header):
        decl = Declaration(match.group(1), line_number(header, match.start()))
        before = header[: match.start()].rstrip()
        if before.endswith("*/"):
            comment = before[before.rfind("/*") :]
            if comment.startswith(("/*!", "/**")):
                decl.has_doc_comment = True
                decl.tags = TAG.findall(comment)
        declarations.append(decl)
    return declarations


def find_definitions(source: str) -> dict[str, Definition]:
    """Find the public functions in the source, and how each one uses
    the API lock"""
    # Comments and literals go first, as in the compiler, so that a
    # directive inside one is ignored
    source = drop_disabled_code(blank_comments_and_literals(source))
    matches = list(DEFINITION.finditer(source))
    definitions = {}
    for i, match in enumerate(matches):
        # A function's body runs to the start of the next one
        end = matches[i + 1].start() if i + 1 < len(matches) else len(source)
        body = source[match.end() : end]
        definitions[match.group(1)] = Definition(
            match.group(1),
            line_number(source, match.start()),
            GUARDED_START.match(body) is not None,
            re.search(r"\bApiGuard\b", body) is not None,
        )
    return definitions


def check_declaration(
    decl: Declaration, definition: Definition | None, where: str
) -> list[str]:
    """Return the problems with one declaration's tags"""
    prefix = f"{where}:{decl.line}: {decl.name}()"
    if not decl.has_doc_comment:
        return [f"{prefix} has no documentation comment"]
    if len(decl.tags) != 1:
        found = ", ".join("@" + tag for tag in decl.tags) or "none"
        return [
            f"{prefix} needs exactly one of @serialized and @concurrent"
            f" (found: {found})"
        ]
    if definition is None:
        return [f"{prefix} isn't defined in the source"]
    serialized = decl.tags[0] == "serialized"
    if serialized and not definition.starts_guarded:
        return [
            f"{prefix} is @serialized, but its definition doesn't start by"
            " creating an ApiGuard and returning kReentryError when the"
            " guard's owns() is false"
        ]
    if not serialized and definition.mentions_guard:
        return [f"{prefix} is @concurrent, but its definition uses ApiGuard"]
    return []


def check_text(
    header: str, source: str, header_name: str, source_name: str
) -> list[str]:
    """Return every problem found in the header and source text, or an
    empty list if there are none. The names are only for the messages."""
    declarations = find_declarations(header)
    definitions = find_definitions(source)
    if not declarations:
        # So that a change to the header's format can't pass the check
        # without checking anything
        return [f"{header_name}: no HIPOBJ_API function declarations found"]

    problems = []
    for decl in declarations:
        problems += check_declaration(decl, definitions.get(decl.name), header_name)
    declared = {decl.name for decl in declarations}
    for definition in definitions.values():
        if definition.name not in declared:
            problems.append(
                f"{source_name}:{definition.line}: {definition.name}() is"
                " defined but not declared in the header"
            )
    return problems


def check(header_path: Path, source_path: Path) -> list[str]:
    """Return every problem found in the two files, or an empty list if
    there are none"""
    return check_text(
        header_path.read_text(encoding="utf-8"),
        source_path.read_text(encoding="utf-8"),
        str(header_path),
        str(source_path),
    )


def main() -> int:
    """Run the check and report the result"""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("header", type=Path, help="the public header, hipobj.h")
    parser.add_argument("source", type=Path, help="the API's source, hipobj.cpp")
    args = parser.parse_args()

    problems = check(args.header, args.source)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        print(
            f'{len(problems)} problem(s). See the "threads" group in'
            " hipobj.h for what the tags mean.",
            file=sys.stderr,
        )
        return 1
    print("Every public function's thread-safety tag matches its definition.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
