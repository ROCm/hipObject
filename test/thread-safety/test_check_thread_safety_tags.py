#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Tests for check_thread_safety_tags.py.

Each test runs the checker on a small header and source, which declare
and define one serialized function, hipObjLocked(), and one concurrent
function, hipObjFree(), and changes one thing about them. Most of the
changes are mistakes that the checker has to report.

Usage: test_check_thread_safety_tags.py [unittest options]
"""

import unittest

from check_thread_safety_tags import check_text

HEADER = """\
/*!
 * @brief A serialized function
 * @serialized
 */
HIPOBJ_API hipObjError_t hipObjLocked(int x);

/*!
 * @brief A concurrent function
 * @concurrent
 */
HIPOBJ_API hipObjError_t hipObjFree(int x);
"""

GUARD = """\
    hipObj::ApiGuard guard;
    if (!guard.owns()) {
        return hipObj::kReentryError;
    }
"""

RETURN = "    return {hipObjSuccess, 0};\n"


def definition(name: str, body: str) -> str:
    """Return the definition of a public function with the given body"""
    return (
        f"hipObjError_t\n{name}(int x)\ntry {{\n{body}}}\n"
        "catch (...) {\n    return hipObj::handleException();\n}\n\n"
    )


def source(locked: str = GUARD + RETURN, free: str = RETURN) -> str:
    """Return a source file that defines both functions"""
    return definition("hipObjLocked", locked) + definition("hipObjFree", free)


def problems(header: str = HEADER, src: str | None = None) -> list[str]:
    """Run the checker on the header and source text"""
    return check_text(header, source() if src is None else src, "h", "c")


class MatchingCodeTest(unittest.TestCase):
    """Code that matches its tags passes"""

    def test_matching_tags_pass(self) -> None:
        """The unchanged header and source pass"""
        self.assertEqual(problems(), [])

    def test_body_without_try_passes(self) -> None:
        """A body that opens with a plain brace, not try, passes"""
        src = source().replace("(int x)\ntry {\n", "(int x)\n{\n", 1)
        self.assertEqual(problems(src=src), [])

    def test_concurrent_literal_naming_guard_passes(self) -> None:
        """A concurrent function may put "ApiGuard" in a string"""
        src = source(free='    puts("ApiGuard");\n' + RETURN)
        self.assertEqual(problems(src=src), [])

    def test_concurrent_comment_naming_guard_passes(self) -> None:
        """A concurrent function may mention ApiGuard in a comment"""
        src = source(free="    // doesn't need an ApiGuard\n" + RETURN)
        self.assertEqual(problems(src=src), [])

    def test_disabled_declaration_is_ignored(self) -> None:
        """An untagged declaration inside #if 0 isn't checked"""
        header = HEADER + "#if 0\nHIPOBJ_API hipObjError_t hipObjOff(void);\n#endif\n"
        self.assertEqual(problems(header=header), [])


class SerializedDefinitionTest(unittest.TestCase):
    """A serialized function that doesn't start with the guard fails"""

    def assert_unguarded(self, locked: str) -> None:
        """Check that hipObjLocked() with this body is reported"""
        found = problems(src=source(locked=locked))
        self.assertEqual(len(found), 1, found)
        self.assertIn("hipObjLocked() is @serialized", found[0])

    def test_no_guard(self) -> None:
        """No guard at all"""
        self.assert_unguarded(RETURN)

    def test_code_before_guard(self) -> None:
        """A statement before the guard"""
        self.assert_unguarded("    (void)x;\n" + GUARD + RETURN)

    def test_state_used_before_guard(self) -> None:
        """Library state used before the guard"""
        self.assert_unguarded(
            "    if (!getState().initialized) {\n" + RETURN + "    }\n" + GUARD + RETURN
        )

    def test_conditional_guard(self) -> None:
        """The guard inside an if"""
        self.assert_unguarded("    if (x) {\n" + GUARD + "    }\n" + RETURN)

    def test_guard_in_nested_block(self) -> None:
        """The guard inside a block of its own, which ends its lifetime"""
        self.assert_unguarded("    {\n" + GUARD + "    }\n" + RETURN)

    def test_owns_not_checked(self) -> None:
        """A guard whose owns() isn't checked"""
        self.assert_unguarded("    hipObj::ApiGuard guard;\n" + RETURN)

    def test_owns_of_other_variable(self) -> None:
        """owns() called on something other than the guard"""
        self.assert_unguarded(GUARD.replace("!guard.owns()", "!other.owns()") + RETURN)

    def test_wrong_return(self) -> None:
        """Something other than kReentryError returned"""
        self.assert_unguarded(
            GUARD.replace("hipObj::kReentryError", "{hipObjSuccess, 0}") + RETURN
        )

    def test_guard_in_string_literal(self) -> None:
        """The guard's code in a string literal"""
        literal = GUARD.replace("\n", " ")
        self.assert_unguarded(f'    puts("{literal}");\n' + RETURN)

    def test_guard_in_comment(self) -> None:
        """The guard's code in a comment"""
        self.assert_unguarded("    /*\n" + GUARD + "    */\n" + RETURN)


class TagTest(unittest.TestCase):
    """Problems with the tags and declarations are reported"""

    def assert_one_problem(self, found: list[str], text: str) -> None:
        """Check that found is one problem, which contains text"""
        self.assertEqual(len(found), 1, found)
        self.assertIn(text, found[0])

    def test_concurrent_uses_guard(self) -> None:
        """A concurrent function that takes the lock"""
        found = problems(src=source(free=GUARD + RETURN))
        self.assert_one_problem(found, "hipObjFree() is @concurrent")

    def test_missing_tag(self) -> None:
        """A doc comment without a tag"""
        found = problems(header=HEADER.replace(" * @concurrent\n", ""))
        self.assert_one_problem(found, "(found: none)")

    def test_both_tags(self) -> None:
        """A doc comment with both tags"""
        header = HEADER.replace(" * @concurrent\n", " * @concurrent\n * @serialized\n")
        found = problems(header=header)
        self.assert_one_problem(found, "(found: @concurrent, @serialized)")

    def test_backslash_tag(self) -> None:
        """A tag written with a backslash counts"""
        header = HEADER.replace("@concurrent", "\\concurrent")
        self.assertEqual(problems(header=header), [])

    def test_no_doc_comment(self) -> None:
        """A plain comment, not a doc comment, before the declaration"""
        header = HEADER.replace(
            "/*!\n * @brief A concurrent", "/*\n * @brief A concurrent"
        )
        found = problems(header=header)
        self.assert_one_problem(found, "hipObjFree() has no documentation comment")

    def test_declared_but_not_defined(self) -> None:
        """A declaration with no definition"""
        found = problems(src=definition("hipObjLocked", GUARD + RETURN))
        self.assert_one_problem(found, "hipObjFree() isn't defined")

    def test_defined_but_not_declared(self) -> None:
        """A definition with no declaration"""
        found = problems(src=source() + definition("hipObjExtra", RETURN))
        self.assert_one_problem(found, "hipObjExtra() is defined but not declared")

    def test_no_declarations(self) -> None:
        """A header with no declarations at all"""
        found = problems(header="")
        self.assert_one_problem(found, "no HIPOBJ_API function declarations")


if __name__ == "__main__":
    unittest.main()
