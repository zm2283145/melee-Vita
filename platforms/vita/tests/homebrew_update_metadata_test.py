#!/usr/bin/env python3

import hashlib
import json
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[3]
    generator = root / "platforms/vita/updater/generate-homebrew-update-metadata.py"
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        vpk = temporary / "SmashMeleevita-0.8.13.vpk"
        payload = b"synthetic Vita package"
        vpk.write_bytes(payload)
        version = temporary / "version.json"
        version.write_text(
            json.dumps({"release": "0.8.13", "app": "00.91"}),
            encoding="utf-8",
        )
        notes = temporary / "RELEASE_NOTES.md"
        notes.write_text(
            "## Changes in Vita v0.8.13\n\n"
            "- Added support for the upcoming Homebrew Update plugin.\n\n"
            "## Changes in Vita v0.8.12\n\n- Previous release.\n",
            encoding="utf-8",
        )
        output = temporary / "dist"
        subprocess.run(
            [
                sys.executable,
                str(generator),
                "--version-file",
                str(version),
                "--release-notes",
                str(notes),
                "--vpk",
                str(vpk),
                "--output-directory",
                str(output),
            ],
            check=True,
        )

        feed = ET.parse(output / "MLVITA002-ver.xml").getroot()
        assert feed.attrib == {"status": "alive", "titleid": "MLVITA002"}
        package = feed.find("./tag/package")
        assert package is not None
        assert package.attrib["version"] == "00.91"
        assert package.attrib["size"] == str(len(payload))
        assert package.attrib["sha1sum"] == hashlib.sha1(payload).hexdigest()
        assert package.attrib["url"].endswith(
            "/releases/latest/download/SmashMeleevita-0.8.13.vpk"
        )
        assert package.attrib["content_id"] == (
            "EP9000-MLVITA002_00-0000000000000000"
        )
        changeinfo = ET.parse(output / "MLVITA002-changeinfo.xml").getroot()
        assert "upcoming Homebrew Update plugin" in "".join(
            changeinfo.itertext()
        )
        assert "Previous release" not in "".join(changeinfo.itertext())

    print("PASS: Homebrew Update release metadata generation")


if __name__ == "__main__":
    main()
