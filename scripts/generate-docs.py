#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import argparse
import html
import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
ENV_SOURCE = ROOT / "include" / "uib" / "env.h"
ENV_OUTPUT = ROOT / "docs" / "environment-variables.md"
API_OUTPUT = ROOT / "docs" / "public-api.md"
MAN_OUTPUT = ROOT / "man" / "uib.7"

DOC_FORMAT_SOURCE = ROOT / "docs" / "generate-docs.json"
DOC_FORMAT = json.loads(DOC_FORMAT_SOURCE.read_text())
ENV_TEMPLATE = ROOT / "docs" / "environment-variables.md.in"
API_TEMPLATE = ROOT / "docs" / "public-api.md.in"
CONFIGURE_SOURCE = ROOT / "configure.ac"
MAN_TEMPLATE = ROOT / "man" / "uib.7.in"


def render_template(path, replacements):
    content = path.read_text()

    for marker, value in replacements.items():
        if content.count(marker) != 1:
            raise ValueError(f"{path}: expected one {marker} placeholder")
        content = content.replace(marker, value)
    return content


def project_version():
    match = re.search(r"AC_INIT\(\[uib\], \[([^]]+)\]", CONFIGURE_SOURCE.read_text())

    if not match:
        raise ValueError(f"{CONFIGURE_SOURCE}: cannot determine project version")
    return match.group(1)


def parse_fields(block):
    fields = {}
    current = None

    for raw in block.splitlines():
        line = re.sub(r"^\s*\* ?", "", raw).rstrip()
        tagged = re.match(r"@(\w[\w-]*):\s*(.*)", line)
        section = re.match(r"(Example):\s*(.*)", line)
        if tagged:
            current = tagged.group(1)
            fields[current] = tagged.group(2)
        elif section:
            current = section.group(1).lower()
            fields[current] = section.group(2)
        elif current == "example" and line:
            if fields[current]:
                fields[current] += " "
            fields[current] += line.strip()
        elif not line:
            current = None
    return fields


def parse_environment_annotations():
    entries = []
    pattern = re.compile(
        r'/\*\*(.*?)\*/\s*#define\s+UIB_ENV_[A-Z0-9_]+\s+"(UIB_[A-Z0-9_]+)"',
        re.S)

    text = ENV_SOURCE.read_text()
    for match in pattern.finditer(text):
        block = match.group(1)
        fields = parse_fields(block)
        prose = []

        for raw in block.splitlines():
            line = re.sub(r"^\s*\* ?", "", raw).strip()
            if not line or line.startswith("DOC:") or line.startswith("@"):
                continue
            if line.startswith("Example:"):
                break
            prose.append(line)
        fields["description"] = " ".join(prose)
        fields["symbol"] = match.group(2)
        entries.append(fields)
    return entries


def parse_api_annotations():
    entries = []
    pattern = re.compile(
        r"/\*\*(.*?)\*/\s*[^;{}]*?\b(uibv?_[a-z0-9_]+)\s*"
        r"\([^;{}]*\)\s*\{", re.S)

    for path in sorted((ROOT / "src").glob("*.c")):
        text = path.read_text()
        for match in pattern.finditer(text):
            block = match.group(1)
            symbol = match.group(2)
            heading = re.search(
                rf"^\s*\*\s*{re.escape(symbol)}\s+-\s+(.+)$", block,
                re.M)
            if not heading:
                continue
            fields = parse_fields(block)
            fields["description"] = heading.group(1).strip()
            fields["symbol"] = symbol
            entries.append(fields)
    return entries


def require_fields(entries, fields, source):
    seen = set()

    for entry in entries:
        symbol = entry["symbol"]
        missing = [field for field in fields if not entry.get(field)]

        if missing:
            message = f"{source}: {symbol}: missing {', '.join(missing)}"

            raise ValueError(message)
        if symbol in seen:
            raise ValueError(f"{source}: duplicate annotation for {symbol}")
        seen.add(symbol)


def markdown_code(value):
    return "<code>" + html.escape(value, quote=False) + "</code>"


def markdown_text(value):
    return html.escape(value, quote=False).replace("|", "&#124;")


def roff_text(value):
    value = value.replace("`", "")
    value = value.replace("\\", r"\e")
    value = value.replace("-", r"\-")
    if value.startswith((".", "'")):
        value = r"\&" + value
    return value


def roff_bold(value):
    return r"\fB" + roff_text(value) + r"\fP"


def environment_inputs():
    inputs = set()
    paths = [ROOT / "Makefile"]

    for directory in ("src", "examples", "tests"):
        for path in (ROOT / directory).rglob("*"):
            if path.suffix in (".c", ".h", ".sh") or path.name == "Makefile":
                paths.append(path)
    for path in paths:
        text = path.read_text(errors="ignore")
        inputs.update(re.findall(
            r'(?:getenv|setenv|env_u32)\(\s*"(UIB_[A-Z0-9_]+)"',
            text))
        inputs.update(re.findall(r"\$\{?(UIB_[A-Z0-9_]+)", text))
        inputs.update(re.findall(
            r"(?:^|[\s;])(?:env\s+)?(UIB_[A-Z0-9_]+)=", text, re.M))
    return inputs


def header_apis():
    declarations = {}

    for path in sorted((ROOT / "include" / "uib").glob("*.h")):
        text = re.sub(r"/\*.*?\*/", "", path.read_text(), flags=re.S)
        for match in re.finditer(r"\b(uibv?_[a-z0-9_]+)\s*\(", text):
            name = match.group(1)
            start = text.rfind("\n", 0, match.start()) + 1
            end = text.find(";", match.end())

            if end < 0:
                continue
            declaration = " ".join(text[start:end + 1].split())
            if declaration.startswith("#define"):
                continue
            declarations[name] = (declaration, path.relative_to(ROOT))
    return declarations


def render_environment(entries):
    lines = []
    scopes = []

    for entry in entries:
        if entry["uib-env"] not in scopes:
            scopes.append(entry["uib-env"])
    for scope in scopes:
        lines.extend([
            f"## {scope}",
            "",
            DOC_FORMAT["markdown"]["environment_table_header"],
            DOC_FORMAT["markdown"]["environment_table_rule"],
        ])
        for entry in entries:
            if entry["uib-env"] != scope:
                continue
            lines.append(
                "| " + " | ".join((
                    markdown_code(entry["symbol"]),
                    markdown_text(entry["default"]),
                    markdown_text(entry["values"]),
                    markdown_text(entry["description"]),
                    markdown_code(entry["example"]),
                )) + " |"
            )
        lines.append("")
    return render_template(
        ENV_TEMPLATE, {"@ENVIRONMENT_SECTIONS@": "\n".join(lines).rstrip("\n")})


def render_api(entries, declarations):
    lines = []
    areas = []

    for entry in entries:
        if entry["uib-api"] not in areas:
            areas.append(entry["uib-api"])
    for area in areas:
        lines.extend([
            f"## {area}",
            "",
            DOC_FORMAT["markdown"]["api_table_header"],
            DOC_FORMAT["markdown"]["api_table_rule"],
        ])
        for entry in entries:
            if entry["uib-api"] != area:
                continue
            declaration, path = declarations[entry["symbol"]]
            function = f"[`{entry['symbol']}`](../{path})"
            lines.append(
                "| " + " | ".join((
                    function,
                    markdown_code(declaration),
                    markdown_text(entry["description"]),
                    markdown_code(entry["example"]),
                )) + " |"
            )
        lines.append("")
    return render_template(
        API_TEMPLATE, {"@API_SECTIONS@": "\n".join(lines).rstrip("\n")})


def render_man_environment(lines, entries):
    scopes = []

    for entry in entries:
        if entry["uib-env"] not in scopes:
            scopes.append(entry["uib-env"])
    for scope in scopes:
        lines.append(f'.SS "{roff_text(scope)}"')
        for entry in entries:
            if entry["uib-env"] != scope:
                continue
            lines.extend([
                ".TP",
                f".B {entry['symbol']}",
                roff_text(entry["description"]),
                ".br",
                DOC_FORMAT["man"]["default_label"] + roff_text(entry["default"]),
                ".br",
                DOC_FORMAT["man"]["accepted_values_label"] + roff_text(entry["values"]),
                ".br",
                DOC_FORMAT["man"]["example_label"] + roff_bold(entry["example"]),
            ])


def render_man_api(lines, entries, declarations):
    areas = []

    for entry in entries:
        if entry["uib-api"] not in areas:
            areas.append(entry["uib-api"])
    for area in areas:
        lines.append(f'.SS "{roff_text(area)}"')
        for entry in entries:
            if entry["uib-api"] != area:
                continue
            declaration, _path = declarations[entry["symbol"]]
            lines.extend([
                ".TP",
                f".B {entry['symbol']}",
                roff_text(declaration),
                ".br",
                roff_text(entry["description"]),
                ".br",
                DOC_FORMAT["man"]["example_label"] + roff_bold(entry["example"]),
            ])


def render_man(env_entries, api_entries, declarations):
    api_lines = []
    environment_lines = []

    render_man_api(api_lines, api_entries, declarations)
    render_man_environment(environment_lines, env_entries)
    return render_template(MAN_TEMPLATE, {
        "@PUBLIC_API@": "\n".join(api_lines).rstrip("\n"),
        "@ENVIRONMENT@": "\n".join(environment_lines).rstrip("\n"),
        "@VERSION@": project_version(),
    })


def update_or_check(path, content, check):
    current = path.read_text() if path.exists() else None

    if check:
        if current != content:
            print(f"out of date: {path.relative_to(ROOT)}", file=sys.stderr)
            return False
        return True
    path.write_text(content)
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Generate UIB reference documentation")
    parser.add_argument("--check", action="store_true",
                        help="fail when generated documentation is out of date")
    args = parser.parse_args()
    env_entries = parse_environment_annotations()
    api_entries = parse_api_annotations()
    declarations = header_apis()

    require_fields(env_entries,
                   ("uib-env", "description", "default", "values", "example"),
                   ENV_SOURCE)
    require_fields(api_entries, ("uib-api", "description", "example"),
                   ROOT / "src")

    documented_env = {entry["symbol"] for entry in env_entries}
    missing_env = environment_inputs() - documented_env
    if missing_env:
        raise ValueError("undocumented environment variables: " +
                         ", ".join(sorted(missing_env)))

    documented_api = {entry["symbol"] for entry in api_entries}
    declared_api = set(declarations)
    missing_api = declared_api - documented_api
    stale_api = documented_api - declared_api
    if missing_api:
        raise ValueError("undocumented public APIs: " +
                         ", ".join(sorted(missing_api)))
    if stale_api:
        raise ValueError("annotations without public declarations: " +
                         ", ".join(sorted(stale_api)))

    env_doc = render_environment(env_entries)
    api_doc = render_api(api_entries, declarations)
    man_page = render_man(env_entries, api_entries, declarations)
    ok = update_or_check(ENV_OUTPUT, env_doc, args.check)
    ok = update_or_check(API_OUTPUT, api_doc, args.check) and ok
    ok = update_or_check(MAN_OUTPUT, man_page, args.check) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
