# SPDX-License-Identifier: GPL-3.0-or-later
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_generated_files_up_to_date():
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_schemas.py"), "--check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_report_codec_c_suite_with_sanitizers(tmp_path):
    exe = tmp_path / "report_test"
    srcs = [f"{ROOT}/tests/report_test.c", f"{ROOT}/core/report.c", f"{ROOT}/core/geo.c", f"{ROOT}/core/schemas_gen.c"]
    cc = subprocess.run(["gcc", "-std=gnu11", "-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                         "-Wall", "-Wextra", "-o", str(exe), *srcs, "-lm"], capture_output=True, text=True)
    assert cc.returncode == 0, cc.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "ALL OK" in run.stdout
