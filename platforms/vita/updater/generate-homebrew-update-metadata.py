#!/usr/bin/env python3

import argparse
import hashlib
import json
import re
from pathlib import Path


TITLE_ID = "MLVITA002"
TITLE = "Smash Melee Vita"
CONTENT_ID = "EP9000-MLVITA002_00-0000000000000000"
SYSTEM_VERSION = "56623104"


def current_release_notes(path: Path, release: str) -> str:
    notes = path.read_text(encoding="utf-8")
    heading = f"## Changes in Vita v{release}"
    start = notes.find(heading)
    if start < 0:
        raise ValueError(f"missing release notes section: {heading}")
    body_start = start + len(heading)
    end = notes.find("\n## ", body_start)
    body = notes[body_start:] if end < 0 else notes[body_start:end]
    body = body.strip()
    if not body:
        raise ValueError(f"empty release notes section: {heading}")
    return body


def cdata(text: str) -> str:
    return text.replace("]]>", "]]]]><![CDATA[>")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate Homebrew Update feed metadata for a Vita VPK."
    )
    parser.add_argument("--version-file", type=Path, required=True)
    parser.add_argument("--release-notes", type=Path, required=True)
    parser.add_argument("--vpk", type=Path, required=True)
    parser.add_argument("--repository", default="zm2283145/melee-Vita")
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()

    version = json.loads(args.version_file.read_text(encoding="utf-8"))
    release = version.get("release", "")
    app_version = version.get("app", "")
    if re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", release) is None:
        raise ValueError("invalid release version")
    if re.fullmatch(r"[0-9]{2}\.[0-9]{2}", app_version) is None:
        raise ValueError("invalid Vita APP_VER")
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) is None:
        raise ValueError("invalid GitHub repository")

    expected_name = f"SmashMeleevita-{release}.vpk"
    if args.vpk.name != expected_name:
        raise ValueError(f"VPK must be named {expected_name}")
    package = args.vpk.read_bytes()
    if not package:
        raise ValueError("VPK is empty")

    base_url = f"https://github.com/{args.repository}/releases/latest/download"
    package_url = f"{base_url}/{expected_name}"
    changeinfo_name = f"{TITLE_ID}-changeinfo.xml"
    changeinfo_url = f"{base_url}/{changeinfo_name}"
    sha1 = hashlib.sha1(package).hexdigest()
    notes = current_release_notes(args.release_notes, release)

    update_xml = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<titlepatch status="alive" titleid="{TITLE_ID}">\n'
        f'  <tag name="{TITLE_ID}_T0" signoff="true">\n'
        f'    <package version="{app_version}" size="{len(package)}"\n'
        f'        sha1sum="{sha1}" url="{package_url}"\n'
        f'        psp2_system_ver="{SYSTEM_VERSION}" content_id="{CONTENT_ID}">\n'
        f'      <paramsfo><title>{TITLE}</title></paramsfo>\n'
        f'      <changeinfo url="{changeinfo_url}"/>\n'
        "    </package>\n"
        "  </tag>\n"
        "</titlepatch>\n"
    )
    changeinfo_xml = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        "<changeinfo><changes><![CDATA[\n"
        f"{cdata(notes)}\n"
        "]]></changes></changeinfo>\n"
    )

    args.output_directory.mkdir(parents=True, exist_ok=True)
    (args.output_directory / f"{TITLE_ID}-ver.xml").write_text(
        update_xml, encoding="utf-8", newline="\n"
    )
    (args.output_directory / changeinfo_name).write_text(
        changeinfo_xml, encoding="utf-8", newline="\n"
    )


if __name__ == "__main__":
    main()
